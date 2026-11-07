package server_test

// 认证链路验收（2026-11-07）：退出登录、会话过期、单端登录、登录限速。
//
// 全部走真实 HTTP + 真实 WebSocket + 真实 SQLite（t.TempDir），不 mock 任何层：
// 这几条缺陷的共同点是「只在真实链路上才暴露」，单测替换掉一层就失去意义。

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/server"
	"partyup/internal/store"
)

// login 走 POST /api/login 并返回 token；非 200 直接 fatal。
func (e *testEnv) login(username, password string) string {
	e.t.Helper()
	status, body := e.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": username, "password": password,
	})
	if status != http.StatusOK {
		e.t.Fatalf("login %s: status=%d body=%s", username, status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		e.t.Fatalf("decode login response: %v", err)
	}
	if sess.Token == "" {
		e.t.Fatal("login returned empty token")
	}
	return sess.Token
}

// doJSONWithHeaders 与 doJSON 相同，但把响应头一起返回（限速测试要读 Retry-After）。
func (e *testEnv) doJSONWithHeaders(method, path, bearer string, body any) (int, http.Header, []byte) {
	e.t.Helper()

	var reader io.Reader
	if body != nil {
		raw, err := json.Marshal(body)
		if err != nil {
			e.t.Fatalf("marshal request body: %v", err)
		}
		reader = bytes.NewReader(raw)
	}
	req, err := http.NewRequest(method, e.http.URL+path, reader)
	if err != nil {
		e.t.Fatalf("build request: %v", err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	resp, err := e.http.Client().Do(req)
	if err != nil {
		e.t.Fatalf("%s %s: %v", method, path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		e.t.Fatalf("read response body: %v", err)
	}
	return resp.StatusCode, resp.Header.Clone(), data
}

// sessionActive 用 store 的真实判据检查 token 是否仍可用（= 库里该会话 active）。
func (e *testEnv) sessionActive(token string) bool {
	e.t.Helper()
	_, err := e.store.MemberBySession(context.Background(), auth.HashToken(token))
	switch {
	case err == nil:
		return true
	case errors.Is(err, store.ErrNotFound):
		return false
	default:
		e.t.Fatalf("member by session: %v", err)
		return false
	}
}

// activeSessionsByMember 从库的运维视图统计每个成员的 active 会话数。
func (e *testEnv) activeSessionsByMember() map[string]int {
	e.t.Helper()
	sessions, err := e.store.ListSessions(context.Background())
	if err != nil {
		e.t.Fatalf("list sessions: %v", err)
	}
	out := make(map[string]int, 4)
	for _, s := range sessions {
		if s.Active {
			out[s.MemberID]++
		}
	}
	return out
}

// dialAndAuth 建一条 WS 并发 auth，返回**第一条**响应（成功或失败都由调用方判）。
// dialWS 在失败时直接 fatal，这里需要观察 auth.err 的具体错误码。
func dialAndAuth(t *testing.T, httpURL, token string) (*websocket.Conn, protocol.Envelope) {
	t.Helper()
	wsURL := "ws" + strings.TrimPrefix(httpURL, "http") + "/ws"
	conn, resp, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		status := "no response"
		if resp != nil {
			status = resp.Status
		}
		t.Fatalf("dial ws: %v (%s)", err, status)
	}
	t.Cleanup(func() { _ = conn.Close() })

	req, err := protocol.New(protocol.TypeAuth, protocol.AuthRequest{Token: token})
	if err != nil {
		t.Fatalf("build auth request: %v", err)
	}
	raw, err := req.Marshal()
	if err != nil {
		t.Fatalf("marshal auth request: %v", err)
	}
	_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := conn.WriteMessage(websocket.TextMessage, raw); err != nil {
		t.Fatalf("write auth: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	_, data, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read auth response: %v", err)
	}
	env, err := protocol.Decode(data)
	if err != nil {
		t.Fatalf("decode auth response: %v", err)
	}
	return conn, env
}

// assertWSClosedWith 断言下一条读结果是给定关闭码的 Close 帧。
func assertWSClosedWith(t *testing.T, c *wsClient, code int) {
	t.Helper()
	_ = c.conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	_, _, err := c.conn.ReadMessage()
	if err == nil {
		t.Fatal("expected connection close, got another message")
	}
	var closeErr *websocket.CloseError
	if !errors.As(err, &closeErr) {
		t.Fatalf("read error = %v (%T), want websocket.CloseError", err, err)
	}
	if closeErr.Code != code {
		t.Fatalf("close code = %d, want %d", closeErr.Code, code)
	}
}

// assertAuthError 断言一条 auth 响应是带指定错误码的 auth.err。
func assertAuthError(t *testing.T, env protocol.Envelope, wantCode string) {
	t.Helper()
	if env.Type != protocol.TypeError(protocol.TypeAuth) {
		t.Fatalf("auth response type = %q, want %q", env.Type, protocol.TypeError(protocol.TypeAuth))
	}
	var p protocol.ErrorPayload
	if err := env.DecodePayload(&p); err != nil {
		t.Fatalf("decode auth.err: %v", err)
	}
	if p.Code != wantCode {
		t.Fatalf("auth.err code = %q (%s), want %q", p.Code, p.Message, wantCode)
	}
}

// TestLogoutRevokesSession 覆盖 A：
//   - POST /api/logout 204 且无 body；撤销后 /api/me 401；
//   - 幂等：重复注销仍 204；未知 token 401；
//   - 该 token 的活动 WS 先收到 session.replaced(logout)，再被 4001 关闭。
func TestLogoutRevokesSession(t *testing.T) {
	env := newTestEnv(t)
	_, token := env.bootstrapAdmin("alice", "secret123")

	if status, body := env.doJSON(http.MethodGet, "/api/me", token, nil); status != http.StatusOK {
		t.Fatalf("me before logout: status=%d body=%s", status, body)
	}
	ws := dialWS(t, env.http.URL, token)

	status, body := env.doJSON(http.MethodPost, "/api/logout", token, nil)
	if status != http.StatusNoContent {
		t.Fatalf("logout: status=%d want 204 body=%s", status, body)
	}
	if len(body) != 0 {
		t.Fatalf("logout body = %q, want empty (204 无 body)", body)
	}

	// 旧 token 立即失效。
	status, body = env.doJSON(http.MethodGet, "/api/me", token, nil)
	if status != http.StatusUnauthorized {
		t.Fatalf("me after logout: status=%d want 401 body=%s", status, body)
	}
	var errResp errorResponse
	if err := json.Unmarshal(body, &errResp); err != nil {
		t.Fatalf("decode me error: %v", err)
	}
	if errResp.Code != protocol.CodeUnauthenticated {
		t.Fatalf("me after logout code = %q, want %q", errResp.Code, protocol.CodeUnauthenticated)
	}
	if env.sessionActive(token) {
		t.Fatal("session still active in DB after logout")
	}

	// 幂等：同一 token 再注销一次仍 204。
	status, body = env.doJSON(http.MethodPost, "/api/logout", token, nil)
	if status != http.StatusNoContent {
		t.Fatalf("second logout: status=%d want 204 body=%s", status, body)
	}

	// 未知 token → 401；没有 Authorization → 401。
	if status, body = env.doJSON(http.MethodPost, "/api/logout", "not-a-real-token", nil); status != http.StatusUnauthorized {
		t.Fatalf("logout unknown token: status=%d want 401 body=%s", status, body)
	}
	if status, body = env.doJSON(http.MethodPost, "/api/logout", "", nil); status != http.StatusUnauthorized {
		t.Fatalf("logout without bearer: status=%d want 401 body=%s", status, body)
	}

	// WS：应用层通知先到，随后是 4001 Close。
	envMsg := ws.waitFor(protocol.TypeSessionReplaced)
	var p protocol.SessionReplacedPayload
	if err := envMsg.DecodePayload(&p); err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if p.Reason != protocol.SessionReasonLogout {
		t.Fatalf("session.replaced reason = %q, want %q", p.Reason, protocol.SessionReasonLogout)
	}
	assertWSClosedWith(t, ws, protocol.CloseSessionReplaced)
}

// TestLoginReplacesPreviousSession 覆盖 C 的主路径：
// 新登录踢掉旧会话 + 旧 WS 收到 session.replaced(login_elsewhere) 后关闭；
// 任一时刻该成员最多一条有效 session。
func TestLoginReplacesPreviousSession(t *testing.T) {
	env := newTestEnv(t)
	member, token1 := env.bootstrapAdmin("alice", "secret123")
	oldWS := dialWS(t, env.http.URL, token1)

	token2 := env.login("alice", "secret123")
	if token1 == token2 {
		t.Fatal("login did not rotate the token")
	}

	// 旧连接：先收到应用层消息，再被 4001 关闭。
	envMsg := oldWS.waitFor(protocol.TypeSessionReplaced)
	var p protocol.SessionReplacedPayload
	if err := envMsg.DecodePayload(&p); err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if p.Reason != protocol.SessionReasonLoginElsewhere {
		t.Fatalf("session.replaced reason = %q, want %q", p.Reason, protocol.SessionReasonLoginElsewhere)
	}
	assertWSClosedWith(t, oldWS, protocol.CloseSessionReplaced)

	// 旧 token 失效，新 token 可用。
	if env.sessionActive(token1) {
		t.Fatal("old session still active after new login")
	}
	if !env.sessionActive(token2) {
		t.Fatal("new session is not active after login")
	}
	if status, body := env.doJSON(http.MethodGet, "/api/me", token1, nil); status != http.StatusUnauthorized {
		t.Fatalf("me with replaced token: status=%d want 401 body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodGet, "/api/me", token2, nil); status != http.StatusOK {
		t.Fatalf("me with new token: status=%d want 200 body=%s", status, body)
	}

	// 库层面：该成员恰好 1 条 active 会话。
	if n := env.activeSessionsByMember()[member.ID]; n != 1 {
		t.Fatalf("active sessions for %s = %d, want exactly 1", member.Name, n)
	}
}

// TestAllLoginEntryPointsAreSingleSession 覆盖 C 的入口完整性：
// bootstrap / login / invite 注册（注册即登录）都必须走「撤销其它会话 + 签发新会话」。
func TestAllLoginEntryPointsAreSingleSession(t *testing.T) {
	env := newTestEnv(t)

	// 入口 1：bootstrap 建管理员并直接签发会话。
	alice, bootstrapToken := env.bootstrapAdmin("alice", "secret123")
	if !env.sessionActive(bootstrapToken) {
		t.Fatal("bootstrap session should be active right after bootstrap")
	}
	// 入口 2：/api/login —— 必须撤销 bootstrap 签发的会话。
	aliceLogin := env.login("alice", "secret123")
	if env.sessionActive(bootstrapToken) {
		t.Fatal("bootstrap session still active after /api/login")
	}
	if !env.sessionActive(aliceLogin) {
		t.Fatal("/api/login session should be active")
	}

	// 入口 3：邀请注册即登录。bob 无旧会话 → 首次只有 1 条；
	// 再 /api/login 一次必须撤销注册时签发的会话。
	bob, bobRegister := env.registerViaInvite(env.newInvite(), "bob", "bobpass123")
	if !env.sessionActive(bobRegister) {
		t.Fatal("invite registration session should be active")
	}
	bobLogin := env.login("bob", "bobpass123")
	if env.sessionActive(bobRegister) {
		t.Fatal("invite-registration session still active after /api/login")
	}
	if !env.sessionActive(bobLogin) {
		t.Fatal("bob login session should be active")
	}

	// 全库每个成员恰好 1 条 active 会话。
	active := env.activeSessionsByMember()
	for _, m := range []struct {
		id   string
		name string
	}{{alice.ID, "alice"}, {bob.ID, "bob"}} {
		if active[m.id] != 1 {
			t.Fatalf("active sessions for %s = %d, want exactly 1", m.name, active[m.id])
		}
	}
}

// TestSessionTTLExpiry 覆盖 B 的端到端行为：TTL 到点后 /api/me 与 WS auth 都拒绝。
func TestSessionTTLExpiry(t *testing.T) {
	const ttl = 150 * time.Millisecond
	env := newTestEnvWithOptions(t, server.Options{SessionTTL: ttl})
	_, token := env.bootstrapAdmin("alice", "secret123")

	// 未过期前可用。注意 /api/me 会尝试滑动续期，但 TTL(150ms) < 节流粒度(5m)
	// 时续期条件永不成立（剩余永远不少于 TTL-5m），所以这里不会把过期时间推后。
	if status, body := env.doJSON(http.MethodGet, "/api/me", token, nil); status != http.StatusOK {
		t.Fatalf("me before ttl expiry: status=%d body=%s", status, body)
	}

	time.Sleep(ttl + 200*time.Millisecond)

	if status, body := env.doJSON(http.MethodGet, "/api/me", token, nil); status != http.StatusUnauthorized {
		t.Fatalf("me after ttl expiry: status=%d want 401 body=%s", status, body)
	}
	if env.sessionActive(token) {
		t.Fatal("expired session still active in DB")
	}

	// WS 鉴权：过期 token → auth.err UNAUTHENTICATED（不是 SESSION_REPLACED）。
	_, authEnv := dialAndAuth(t, env.http.URL, token)
	assertAuthError(t, authEnv, protocol.CodeUnauthenticated)
}

// TestWSAuthDistinguishesReplacedFromInvalid 覆盖 WS 鉴权错误码语义：
// 被新登录替换 → SESSION_REPLACED；乱填/过期 → UNAUTHENTICATED。
func TestWSAuthDistinguishesReplacedFromInvalid(t *testing.T) {
	env := newTestEnv(t)
	_, token1 := env.bootstrapAdmin("alice", "secret123")
	_ = env.login("alice", "secret123") // 撤销 token1

	_, replacedEnv := dialAndAuth(t, env.http.URL, token1)
	assertAuthError(t, replacedEnv, protocol.CodeSessionReplaced)

	_, invalidEnv := dialAndAuth(t, env.http.URL, "definitely-not-a-token")
	assertAuthError(t, invalidEnv, protocol.CodeUnauthenticated)
}

// TestLoginRateLimit 覆盖 D：连续失败触发 429 + Retry-After；窗口过后恢复；
// 成功登录清零。用 2 次/400ms 的小配置，避免测试等待 15 分钟。
func TestLoginRateLimit(t *testing.T) {
	const maxFailures = 2
	env := newTestEnvWithOptions(t, server.Options{
		LoginRateLimit: server.LoginRateLimit{MaxFailures: maxFailures, Window: 400 * time.Millisecond},
	})
	env.bootstrapAdmin("alice", "secret123") // bootstrap 不计入登录限速

	wrong := map[string]string{"username": "alice", "password": "wrong-password"}
	for i := 0; i < maxFailures; i++ {
		status, body := env.doJSON(http.MethodPost, "/api/login", "", wrong)
		if status != http.StatusUnauthorized {
			t.Fatalf("wrong password #%d: status=%d want 401 body=%s", i+1, status, body)
		}
	}

	// 第 maxFailures+1 次：429 + Retry-After + RATE_LIMITED。
	status, header, body := env.doJSONWithHeaders(http.MethodPost, "/api/login", "", wrong)
	if status != http.StatusTooManyRequests {
		t.Fatalf("rate-limited attempt: status=%d want 429 body=%s", status, body)
	}
	retryAfter := header.Get("Retry-After")
	if retryAfter == "" {
		t.Fatal("429 response missing Retry-After header")
	}
	secs, err := strconv.Atoi(retryAfter)
	if err != nil || secs < 1 {
		t.Fatalf("Retry-After = %q, want positive integer seconds", retryAfter)
	}
	var errResp errorResponse
	if err := json.Unmarshal(body, &errResp); err != nil {
		t.Fatalf("decode 429 body: %v", err)
	}
	if errResp.Code != protocol.CodeRateLimited {
		t.Fatalf("429 code = %q, want %q", errResp.Code, protocol.CodeRateLimited)
	}

	// 被限速期间，**正确密码也不行**（按尝试计费，防止「先猜错耗尽额度再试对的」）。
	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "secret123",
	})
	if status != http.StatusTooManyRequests {
		t.Fatalf("correct password while limited: status=%d want 429 body=%s", status, body)
	}

	// 窗口过后恢复：桶回满 → 正确密码 200。
	time.Sleep(500 * time.Millisecond)
	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "secret123",
	})
	if status != http.StatusOK {
		t.Fatalf("login after window: status=%d want 200 body=%s", status, body)
	}

	// 成功登录清零：紧接着一次错误密码仍按「首次」处理（401 而不是 429），
	// 随后正确密码又能成功（说明桶被清空后重新计数）。
	if status, body = env.doJSON(http.MethodPost, "/api/login", "", wrong); status != http.StatusUnauthorized {
		t.Fatalf("wrong after reset: status=%d want 401 body=%s", status, body)
	}
	if status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "secret123",
	}); status != http.StatusOK {
		t.Fatalf("login after reset: status=%d want 200 body=%s", status, body)
	}
}

// TestLoginRateLimitDoesNotAffectExistingSessions 覆盖 D 的边界：
// 限速只作用于登录尝试，已签发的 token 与 WS 连接不受影响。
func TestLoginRateLimitDoesNotAffectExistingSessions(t *testing.T) {
	env := newTestEnvWithOptions(t, server.Options{
		LoginRateLimit: server.LoginRateLimit{MaxFailures: 1, Window: time.Minute},
	})
	_, token := env.bootstrapAdmin("alice", "secret123")
	ws := dialWS(t, env.http.URL, token)

	// 打满限速。
	if status, _ := env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "wrong-password",
	}); status != http.StatusUnauthorized {
		t.Fatalf("first wrong password: status=%d want 401", status)
	}
	if status, _ := env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "wrong-password",
	}); status != http.StatusTooManyRequests {
		t.Fatalf("second wrong password: status=%d want 429", status)
	}

	// 既有 token 仍然可用，既有 WS 仍然活着（能正常回请求）。
	if status, body := env.doJSON(http.MethodGet, "/api/me", token, nil); status != http.StatusOK {
		t.Fatalf("me while rate limited: status=%d want 200 body=%s", status, body)
	}
	env0 := ws.request(protocol.TypeChannelList, struct{}{})
	if env0.Type != protocol.TypeOK(protocol.TypeChannelList) {
		t.Fatalf("channel.list on existing ws: type = %q", env0.Type)
	}
}
