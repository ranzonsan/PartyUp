package server_test

// 端到端测试：真实 HTTP + 真实 WebSocket + 真实 SQLite 文件（t.TempDir）。
// 覆盖验收要的三件事：邀请码能建账号、两个客户端消息互通、历史可用。
// 不 mock 任何一层——要的就是「跑起来真能通」。

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/auth"
	"partyup/internal/protocol"
	"partyup/internal/server"
	"partyup/internal/store"
)

// ------------------------------------------------------------------
// 测试环境
// ------------------------------------------------------------------

type testEnv struct {
	t              *testing.T
	store          *store.Store
	srv            *server.Server
	http           *httptest.Server
	bootstrapToken string
}

func newTestEnv(t *testing.T) *testEnv {
	t.Helper()
	return newTestEnvWith(t, "relay")
}

// newTestEnvWith 允许测试覆盖服务端选项（目前用于语音传输策略：relay / direct）。
// 屏幕共享策略独立，默认 direct；需要覆盖时用 newTestEnvWithModes。
func newTestEnvWith(t *testing.T, voiceMode string) *testEnv {
	t.Helper()
	return newTestEnvWithModes(t, voiceMode, "")
}

// newTestEnvWithModes 同时指定语音与屏幕共享策略（两者互相独立，见 ADR-0002）。
func newTestEnvWithModes(t *testing.T, voiceMode, screenMode string) *testEnv {
	t.Helper()

	st, err := store.Open(t.TempDir())
	if err != nil {
		t.Fatalf("open store: %v", err)
	}

	logHandler := io.Discard
	var logOut io.Writer = logHandler
	if os.Getenv("PARTYUP_TEST_LOG") != "" {
		logOut = os.Stderr
	}
	logger := slog.New(slog.NewTextHandler(logOut, &slog.HandlerOptions{Level: slog.LevelDebug}))
	srv := server.New(st, server.Options{
		Name:       "test-server",
		Version:    "0.0.0-test",
		Logger:     logger,
		VoiceMode:  voiceMode,
		ScreenMode: screenMode,
	})

	info, err := srv.EnsureFirstRun(context.Background())
	if err != nil {
		t.Fatalf("ensure first run: %v", err)
	}
	if info.Token == "" {
		t.Fatal("expected a bootstrap token on a fresh server")
	}
	if info.InviteCode == "" {
		t.Fatal("expected an invite code on a fresh server")
	}

	hs := httptest.NewServer(srv.Handler())
	env := &testEnv{t: t, store: st, srv: srv, http: hs, bootstrapToken: info.Token}
	t.Cleanup(func() {
		srv.Hub().Close()
		hs.Close()
		_ = st.Close()
	})
	return env
}

type sessionResponse struct {
	Member protocol.Member `json:"member"`
	Token  string          `json:"token"`
}

type errorResponse struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// doJSON 发一个 JSON 请求，返回状态码与响应体。
func (e *testEnv) doJSON(method, path string, bearer string, body any) (int, []byte) {
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
	return resp.StatusCode, data
}

// bootstrapAdmin 用 bootstrap token 建管理员并返回成员与 token。
func (e *testEnv) bootstrapAdmin(username, password string) (protocol.Member, string) {
	e.t.Helper()
	status, body := e.doJSON(http.MethodPost, "/api/bootstrap", "", map[string]string{
		"token": e.bootstrapToken, "username": username, "password": password,
	})
	if status != http.StatusCreated {
		e.t.Fatalf("bootstrap: status=%d body=%s", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		e.t.Fatalf("decode bootstrap response: %v", err)
	}
	if sess.Token == "" {
		e.t.Fatal("bootstrap returned empty token")
	}
	return sess.Member, sess.Token
}

// newInvite 直接经 store 造一个邀请码（CLI 路径的等价物）。
func (e *testEnv) newInvite() string {
	e.t.Helper()
	code, err := auth.NewInviteCode()
	if err != nil {
		e.t.Fatalf("generate invite: %v", err)
	}
	if _, err := e.store.CreateInvite(context.Background(), code, ""); err != nil {
		e.t.Fatalf("create invite: %v", err)
	}
	return code
}

// registerViaInvite 用邀请码注册成员并返回成员与 token。
func (e *testEnv) registerViaInvite(code, username, password string) (protocol.Member, string) {
	e.t.Helper()
	status, body := e.doJSON(http.MethodPost, "/api/invite/"+code, "", map[string]string{
		"username": username, "password": password,
	})
	if status != http.StatusCreated {
		e.t.Fatalf("register via invite: status=%d body=%s", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		e.t.Fatalf("decode invite response: %v", err)
	}
	return sess.Member, sess.Token
}

// ------------------------------------------------------------------
// WebSocket 测试客户端
// ------------------------------------------------------------------

type wsClient struct {
	t    *testing.T
	conn *websocket.Conn
}

// dialWS 连接 /ws 并用 token 完成 auth 握手，返回已鉴权客户端。
func dialWS(t *testing.T, httpURL, token string) *wsClient {
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
	c := &wsClient{t: t, conn: conn}
	t.Cleanup(func() { _ = conn.Close() })

	authEnv := c.request(protocol.TypeAuth, protocol.AuthRequest{Token: token})
	if authEnv.Type != protocol.TypeOK(protocol.TypeAuth) {
		t.Fatalf("auth failed: type=%s payload=%s", authEnv.Type, authEnv.Payload)
	}
	return c
}

func (c *wsClient) sendRaw(env protocol.Envelope) {
	c.t.Helper()
	data, err := env.Marshal()
	if err != nil {
		c.t.Fatalf("marshal envelope: %v", err)
	}
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := c.conn.WriteMessage(websocket.TextMessage, data); err != nil {
		c.t.Fatalf("write ws message: %v", err)
	}
}

func (c *wsClient) recv(deadline time.Time) (protocol.Envelope, error) {
	_ = c.conn.SetReadDeadline(deadline)
	_, data, err := c.conn.ReadMessage()
	if err != nil {
		return protocol.Envelope{}, err
	}
	return protocol.Decode(data)
}

// request 发一个请求并等到 id 相同的 .ok / .err（推送消息会被顺带跳过）。
func (c *wsClient) request(msgType string, payload any) protocol.Envelope {
	c.t.Helper()
	req, err := protocol.New(msgType, payload)
	if err != nil {
		c.t.Fatalf("build request %s: %v", msgType, err)
	}
	c.sendRaw(req)

	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting response for %s: %v", msgType, err)
		}
		if env.Type == protocol.TypePing {
			continue
		}
		if env.ID == req.ID && (strings.HasSuffix(env.Type, ".ok") || strings.HasSuffix(env.Type, ".err")) {
			if strings.HasSuffix(env.Type, ".err") {
				var p protocol.ErrorPayload
				_ = env.DecodePayload(&p)
				c.t.Fatalf("%s failed: code=%s message=%s", msgType, p.Code, p.Message)
			}
			return env
		}
	}
}

// waitFor 等待一条指定类型的推送消息。
func (c *wsClient) waitFor(msgType string) protocol.Envelope {
	c.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting for push %s: %v", msgType, err)
		}
		if env.Type == msgType {
			return env
		}
	}
}

// waitForPresenceWith 等待某频道 presence 中出现 expected 个成员。
func (c *wsClient) waitForPresenceWith(channelID string, expected int) protocol.PresencePayload {
	c.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting presence with %d members: %v", expected, err)
		}
		if env.Type != protocol.TypePresence {
			continue
		}
		var p protocol.PresencePayload
		if err := env.DecodePayload(&p); err != nil {
			c.t.Fatalf("decode presence: %v", err)
		}
		if p.ChannelID == channelID && len(p.Members) >= expected {
			return p
		}
	}
}

// expectError 发一个请求并断言收到指定错误码。
func (c *wsClient) expectError(msgType string, payload any, wantCode string) {
	c.t.Helper()
	req, err := protocol.New(msgType, payload)
	if err != nil {
		c.t.Fatalf("build request %s: %v", msgType, err)
	}
	c.sendRaw(req)

	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting error for %s: %v", msgType, err)
		}
		if env.Type != protocol.TypeError(msgType) {
			continue
		}
		if env.ID != req.ID {
			c.t.Fatalf("error response id mismatch: req=%s resp=%s", req.ID, env.ID)
		}
		var p protocol.ErrorPayload
		if err := env.DecodePayload(&p); err != nil {
			c.t.Fatalf("decode error payload: %v", err)
		}
		if p.Code != wantCode {
			c.t.Fatalf("%s: error code = %s, want %s (message: %s)", msgType, p.Code, wantCode, p.Message)
		}
		return
	}
}

// createChannel 经 WS 建频道并返回协议对象。
func (c *wsClient) createChannel(name string) protocol.Channel {
	c.t.Helper()
	env := c.request(protocol.TypeChannelCreate, protocol.ChannelCreateRequest{Name: name})
	var out protocol.ChannelCreateOK
	if err := env.DecodePayload(&out); err != nil {
		c.t.Fatalf("decode channel.create.ok: %v", err)
	}
	if out.Channel.ID == "" {
		c.t.Fatal("channel.create.ok returned empty channel id")
	}
	if out.Channel.Voice == nil || out.Channel.Shares == nil {
		c.t.Fatal("channel arrays must be empty arrays, not null (protocol: voice/shares are arrays)")
	}
	return out.Channel
}

// subscribe 订阅频道。
func (c *wsClient) subscribe(channelID string) {
	c.t.Helper()
	env := c.request(protocol.TypeChannelSub, protocol.ChannelSubscribeRequest{ChannelID: channelID})
	var out protocol.ChannelSubscribeOK
	if err := env.DecodePayload(&out); err != nil {
		c.t.Fatalf("decode channel.subscribe.ok: %v", err)
	}
	if out.ChannelID != channelID {
		c.t.Fatalf("subscribe ack channel_id = %q, want %q", out.ChannelID, channelID)
	}
}

// sendText 发文字并断言收到 text.send.ok，返回分配的 seq 与消息 id。
func (c *wsClient) sendText(channelID, body string) protocol.TextSendOK {
	c.t.Helper()
	env := c.request(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID:   channelID,
		ClientMsgID: protocol.NewID(),
		Body:        body,
	})
	var out protocol.TextSendOK
	if err := env.DecodePayload(&out); err != nil {
		c.t.Fatalf("decode text.send.ok: %v", err)
	}
	return out
}

// ------------------------------------------------------------------
// 测试
// ------------------------------------------------------------------

func TestHealthEndpoint(t *testing.T) {
	env := newTestEnv(t)
	status, body := env.doJSON(http.MethodGet, "/api/health", "", nil)
	if status != http.StatusOK {
		t.Fatalf("health status = %d, want 200 (body=%s)", status, body)
	}
	var health map[string]any
	if err := json.Unmarshal(body, &health); err != nil {
		t.Fatalf("decode health: %v", err)
	}
	if health["ok"] != true {
		t.Fatalf("health ok = %v, want true", health["ok"])
	}
}

func TestBootstrapFlow(t *testing.T) {
	env := newTestEnv(t)

	// 错误 token 必须被拒。
	status, body := env.doJSON(http.MethodPost, "/api/bootstrap", "", map[string]string{
		"token": "not-the-token", "username": "alice", "password": "secret123",
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("bootstrap with bad token: status = %d, want 401 (body=%s)", status, body)
	}

	member, token := env.bootstrapAdmin("alice", "secret123")
	if member.Name != "alice" || !member.Admin {
		t.Fatalf("bootstrap member = %+v, want name=alice admin=true", member)
	}

	// 已经初始化过：再过一次 bootstrap 应该失败（一次性凭据已焚毁）。
	status, body = env.doJSON(http.MethodPost, "/api/bootstrap", "", map[string]string{
		"token": env.bootstrapToken, "username": "mallory", "password": "secret123",
	})
	if status != http.StatusForbidden {
		t.Fatalf("second bootstrap: status = %d, want 403 (body=%s)", status, body)
	}

	// /api/me 带 token 可用。
	status, body = env.doJSON(http.MethodGet, "/api/me", token, nil)
	if status != http.StatusOK {
		t.Fatalf("me: status = %d, want 200 (body=%s)", status, body)
	}
	var me struct {
		Member protocol.Member `json:"member"`
	}
	if err := json.Unmarshal(body, &me); err != nil {
		t.Fatalf("decode me: %v", err)
	}
	if me.Member.ID != member.ID {
		t.Fatalf("me.member.id = %q, want %q", me.Member.ID, member.ID)
	}

	// 无 token 必须 401。
	status, _ = env.doJSON(http.MethodGet, "/api/me", "", nil)
	if status != http.StatusUnauthorized {
		t.Fatalf("me without token: status = %d, want 401", status)
	}
}

func TestLoginFlow(t *testing.T) {
	env := newTestEnv(t)
	env.bootstrapAdmin("alice", "secret123")

	status, body := env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "secret123",
	})
	if status != http.StatusOK {
		t.Fatalf("login: status = %d, want 200 (body=%s)", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		t.Fatalf("decode login: %v", err)
	}
	if sess.Token == "" {
		t.Fatal("login returned empty token")
	}

	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "wrong-password",
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("wrong password: status = %d, want 401 (body=%s)", status, body)
	}
	var errResp errorResponse
	if err := json.Unmarshal(body, &errResp); err != nil {
		t.Fatalf("decode error: %v", err)
	}
	if errResp.Code != protocol.CodeUnauthenticated {
		t.Fatalf("wrong password code = %q, want %q", errResp.Code, protocol.CodeUnauthenticated)
	}

	// 不存在的账号与错误密码返回同一句话，避免账号枚举。
	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "nobody", "password": "secret123",
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("unknown user: status = %d, want 401 (body=%s)", status, body)
	}
	var unknownErr errorResponse
	_ = json.Unmarshal(body, &unknownErr)
	if unknownErr.Message != errResp.Message {
		t.Fatalf("unknown user message = %q, want same as wrong password %q", unknownErr.Message, errResp.Message)
	}
}

func TestInviteRegistration(t *testing.T) {
	env := newTestEnv(t)
	env.bootstrapAdmin("alice", "secret123")

	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "bobpass123")
	if bob.Name != "bob" || bob.Admin {
		t.Fatalf("invited member = %+v, want name=bob admin=false", bob)
	}

	// 同一个邀请码不能再用。
	status, body := env.doJSON(http.MethodPost, "/api/invite/"+code, "", map[string]string{
		"username": "carol", "password": "carolpass123",
	})
	if status != http.StatusConflict {
		t.Fatalf("reuse invite: status = %d, want 409 (body=%s)", status, body)
	}

	// 不存在的码 → 404 NOT_FOUND。
	status, body = env.doJSON(http.MethodPost, "/api/invite/NOPE1234", "", map[string]string{
		"username": "dave", "password": "davepass123",
	})
	if status != http.StatusNotFound {
		t.Fatalf("unknown invite: status = %d, want 404 (body=%s)", status, body)
	}

	// bob 的 token 能连 WS（说明邀请注册出来的会话是有效的）。
	c := dialWS(t, env.http.URL, bobToken)
	env0 := c.request(protocol.TypeChannelList, struct{}{})
	if env0.Type != protocol.TypeOK(protocol.TypeChannelList) {
		t.Fatalf("channel.list type = %q", env0.Type)
	}
}

// TestTwoClientsExchangeMessages 是本次骨架的核心验收：
// 两个独立 WebSocket 连接（两个成员）在同一频道里互相收到对方的文字消息。
func TestTwoClientsExchangeMessages(t *testing.T) {
	env := newTestEnv(t)
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "bobpass123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)

	ch := aliceWS.createChannel("general")
	if ch.Name != "general" {
		t.Fatalf("channel name = %q, want general", ch.Name)
	}

	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)

	presence := aliceWS.waitForPresenceWith(ch.ID, 2)
	got := map[string]bool{}
	for _, m := range presence.Members {
		got[m.MemberID] = true
	}
	if !got[alice.ID] || !got[bob.ID] {
		t.Fatalf("presence members = %+v, want both alice(%s) and bob(%s)", presence.Members, alice.ID, bob.ID)
	}

	// alice → bob
	aliceOK := aliceWS.sendText(ch.ID, "hello from alice")
	if aliceOK.Seq != 1 {
		t.Fatalf("first message seq = %d, want 1", aliceOK.Seq)
	}
	aliceEcho := aliceWS.waitFor(protocol.TypeTextNew)
	bobRecv := bobWS.waitFor(protocol.TypeTextNew)

	var aliceMsg, bobMsg protocol.TextMessage
	if err := aliceEcho.DecodePayload(&aliceMsg); err != nil {
		t.Fatalf("decode alice text.new: %v", err)
	}
	if err := bobRecv.DecodePayload(&bobMsg); err != nil {
		t.Fatalf("decode bob text.new: %v", err)
	}
	if bobMsg.Body != "hello from alice" || bobMsg.MemberID != alice.ID {
		t.Fatalf("bob received %+v, want body=hello from alice member=alice", bobMsg)
	}
	if bobMsg.ID != aliceOK.ID || bobMsg.Seq != aliceOK.Seq {
		t.Fatalf("bob message id/seq = %s/%d, want %s/%d", bobMsg.ID, bobMsg.Seq, aliceOK.ID, aliceOK.Seq)
	}
	if aliceMsg.ID != bobMsg.ID {
		t.Fatalf("echo id %s != peer id %s", aliceMsg.ID, bobMsg.ID)
	}

	// bob → alice
	bobOK := bobWS.sendText(ch.ID, "hi alice, bob here")
	if bobOK.Seq != 2 {
		t.Fatalf("second message seq = %d, want 2", bobOK.Seq)
	}
	aliceRecv := aliceWS.waitFor(protocol.TypeTextNew)
	bobEcho := bobWS.waitFor(protocol.TypeTextNew)

	var aliceGot, bobGot protocol.TextMessage
	if err := aliceRecv.DecodePayload(&aliceGot); err != nil {
		t.Fatalf("decode alice received: %v", err)
	}
	if err := bobEcho.DecodePayload(&bobGot); err != nil {
		t.Fatalf("decode bob echo: %v", err)
	}
	if aliceGot.Body != "hi alice, bob here" || aliceGot.MemberID != bob.ID {
		t.Fatalf("alice received %+v, want bob's reply", aliceGot)
	}
	if aliceGot.Seq != 2 || bobGot.Seq != 2 {
		t.Fatalf("seqs = alice:%d bob:%d, want 2/2", aliceGot.Seq, bobGot.Seq)
	}

	// 历史：两条都在，按 seq 升序。
	histEnv := bobWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{ChannelID: ch.ID, Limit: 10})
	var hist protocol.TextHistoryOK
	if err := histEnv.DecodePayload(&hist); err != nil {
		t.Fatalf("decode history: %v", err)
	}
	if len(hist.Messages) != 2 {
		t.Fatalf("history size = %d, want 2", len(hist.Messages))
	}
	if hist.Messages[0].Seq != 1 || hist.Messages[1].Seq != 2 {
		t.Fatalf("history seqs = %d,%d want ascending 1,2", hist.Messages[0].Seq, hist.Messages[1].Seq)
	}
	if hist.Messages[0].Body != "hello from alice" || hist.Messages[1].Body != "hi alice, bob here" {
		t.Fatalf("history bodies = %q,%q", hist.Messages[0].Body, hist.Messages[1].Body)
	}
}

func TestHistoryPagination(t *testing.T) {
	env := newTestEnv(t)
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ch := aliceWS.createChannel("history")
	aliceWS.subscribe(ch.ID)

	for i := 1; i <= 5; i++ {
		ok := aliceWS.sendText(ch.ID, "msg-"+string(rune('0'+i)))
		if ok.Seq != int64(i) {
			t.Fatalf("message %d seq = %d", i, ok.Seq)
		}
		aliceWS.waitFor(protocol.TypeTextNew)
	}

	// 最近两条：seq 4,5（升序返回）。
	env1 := aliceWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{ChannelID: ch.ID, Limit: 2})
	var page1 protocol.TextHistoryOK
	if err := env1.DecodePayload(&page1); err != nil {
		t.Fatalf("decode page1: %v", err)
	}
	if len(page1.Messages) != 2 || page1.Messages[0].Seq != 4 || page1.Messages[1].Seq != 5 {
		t.Fatalf("recent page = %+v, want seq 4,5", page1.Messages)
	}

	// before_seq=4 → seq 1,2,3。
	env2 := aliceWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{
		ChannelID: ch.ID, BeforeSeq: 4, Limit: 10,
	})
	var page2 protocol.TextHistoryOK
	if err := env2.DecodePayload(&page2); err != nil {
		t.Fatalf("decode page2: %v", err)
	}
	if len(page2.Messages) != 3 {
		t.Fatalf("older page size = %d, want 3", len(page2.Messages))
	}
	for i, m := range page2.Messages {
		if m.Seq != int64(i+1) {
			t.Fatalf("older page[%d].Seq = %d, want %d", i, m.Seq, i+1)
		}
	}
}

func TestChannelErrorsAndAuthGates(t *testing.T) {
	env := newTestEnv(t)
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	aliceWS := dialWS(t, env.http.URL, aliceToken)

	ch := aliceWS.createChannel("dup")

	// 同名频道 → CONFLICT。
	aliceWS.expectError(protocol.TypeChannelCreate, protocol.ChannelCreateRequest{Name: "dup"}, protocol.CodeConflict)

	// 不订阅就发消息 → FORBIDDEN。
	aliceWS.expectError(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID: ch.ID, Body: "should fail",
	}, protocol.CodeForbidden)

	// 未知频道 → NOT_FOUND。
	aliceWS.expectError(protocol.TypeChannelSub, protocol.ChannelSubscribeRequest{
		ChannelID: "01HZZZZZZZZZZZZZZZZZZZZZZZ",
	}, protocol.CodeNotFound)

	// 空消息 → INVALID_ARGUMENT。
	aliceWS.subscribe(ch.ID)
	aliceWS.expectError(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID: ch.ID, Body: "   ",
	}, protocol.CodeInvalidArgument)

	// 未知消息类型 → INVALID_ARGUMENT。
	aliceWS.expectError("nope.nope", struct{}{}, protocol.CodeInvalidArgument)
}

// TestUnauthenticatedWebSocket 验证 auth 之前不能做任何业务操作。
func TestUnauthenticatedWebSocket(t *testing.T) {
	env := newTestEnv(t)

	wsURL := "ws" + strings.TrimPrefix(env.http.URL, "http") + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial ws: %v", err)
	}
	defer conn.Close()

	req, err := protocol.New(protocol.TypeChannelList, struct{}{})
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	data, _ := req.Marshal()
	if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
		t.Fatalf("write: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, respData, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	envResp, err := protocol.Decode(respData)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if envResp.Type != protocol.TypeError(protocol.TypeChannelList) {
		t.Fatalf("type = %q, want %q", envResp.Type, protocol.TypeError(protocol.TypeChannelList))
	}
	var p protocol.ErrorPayload
	if err := envResp.DecodePayload(&p); err != nil {
		t.Fatalf("decode error payload: %v", err)
	}
	if p.Code != protocol.CodeUnauthenticated {
		t.Fatalf("code = %q, want %q", p.Code, protocol.CodeUnauthenticated)
	}
}

// TestBadTokenClosesConnection 验证 auth.err 之后服务端主动断开。
func TestBadTokenClosesConnection(t *testing.T) {
	env := newTestEnv(t)
	env.bootstrapAdmin("alice", "secret123")

	wsURL := "ws" + strings.TrimPrefix(env.http.URL, "http") + "/ws"
	conn, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial ws: %v", err)
	}
	defer conn.Close()

	req, _ := protocol.New(protocol.TypeAuth, protocol.AuthRequest{Token: "bogus-token"})
	data, _ := req.Marshal()
	if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
		t.Fatalf("write auth: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, respData, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read auth.err: %v", err)
	}
	authErr, err := protocol.Decode(respData)
	if err != nil {
		t.Fatalf("decode auth.err: %v", err)
	}
	if authErr.Type != protocol.TypeError(protocol.TypeAuth) {
		t.Fatalf("type = %q, want auth.err", authErr.Type)
	}

	// 下一帧读到的应该是关闭错误（服务端在 auth.err 之后 close）。
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			return // 连接已关闭，符合预期
		}
	}
}

// TestTextMessagesCarryMemberName 锁定「text.new 与 text.history 都带 member_name」。
// 这是把 B 方向（产品功能）协议缺口补上后的回归判据：历史消息里的离线成员必须
// 能显示名字，而不是 id 前缀。
func TestTextMessagesCarryMemberName(t *testing.T) {
	env := newTestEnv(t)
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)

	ch := aliceWS.createChannel("name-check")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	sent := aliceWS.sendText(ch.ID, "hello from alice")

	// 广播包含发送者自己：用 alice 的连接取 text.new，断言作者名。
	push := aliceWS.waitFor(protocol.TypeTextNew)
	var live protocol.TextMessage
	if err := push.DecodePayload(&live); err != nil {
		t.Fatalf("decode text.new: %v", err)
	}
	if live.MemberID != alice.ID {
		t.Fatalf("text.new member_id = %q, want %q", live.MemberID, alice.ID)
	}
	if live.MemberName != "alice" {
		t.Fatalf("text.new member_name = %q, want %q", live.MemberName, "alice")
	}
	if live.Seq != sent.Seq {
		t.Fatalf("text.new seq = %d, want %d", live.Seq, sent.Seq)
	}

	// 历史（联 members 表）同样要带名字。
	histEnv := aliceWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{ChannelID: ch.ID, Limit: 50})
	var hist protocol.TextHistoryOK
	if err := histEnv.DecodePayload(&hist); err != nil {
		t.Fatalf("decode text.history.ok: %v", err)
	}
	if len(hist.Messages) != 1 {
		t.Fatalf("history has %d messages, want 1", len(hist.Messages))
	}
	if hist.Messages[0].MemberName != "alice" {
		t.Fatalf("history member_name = %q, want %q", hist.Messages[0].MemberName, "alice")
	}

	// bob 发一条，历史里两个人的名字都要对。
	bobWS.sendText(ch.ID, "hello from bob")
	_ = aliceWS.waitFor(protocol.TypeTextNew)

	histEnv = aliceWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{ChannelID: ch.ID, Limit: 50})
	if err := histEnv.DecodePayload(&hist); err != nil {
		t.Fatalf("decode text.history.ok: %v", err)
	}
	if len(hist.Messages) != 2 {
		t.Fatalf("history has %d messages, want 2", len(hist.Messages))
	}
	if hist.Messages[0].MemberName != "alice" || hist.Messages[1].MemberName != "bob" {
		t.Fatalf("history authors = [%q %q], want [alice bob]",
			hist.Messages[0].MemberName, hist.Messages[1].MemberName)
	}
	if hist.Messages[1].MemberID != bob.ID {
		t.Fatalf("second history member_id = %q, want %q", hist.Messages[1].MemberID, bob.ID)
	}
}

// TestMediaSignalRelayAndScreenState 锁定屏幕共享的控制面：
// screen.start/stop → screen.state 广播；media.signal 的广播（offer）与定向投递
// （answer/candidate 发给指定成员），以及非法 kind / 未订阅频道的拒绝。
func TestMediaSignalRelayAndScreenState(t *testing.T) {
	env := newTestEnv(t)
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)

	ch := aliceWS.createChannel("share")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	// 1) screen.start：bob 必须收到 screen.state，含 sharer 身份与源分辨率。
	aliceWS.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Source: protocol.ScreenSource{
			ID:     "42",
			Title:  "DISPLAY1 2560x1440",
			Width:  2560,
			Height: 1440,
			Kind:   "screen",
		},
	})
	stateEnv := bobWS.waitFor(protocol.TypeScreenState)
	var state protocol.ScreenStatePayload
	if err := stateEnv.DecodePayload(&state); err != nil {
		t.Fatalf("decode screen.state: %v", err)
	}
	if state.ChannelID != ch.ID {
		t.Fatalf("screen.state channel_id = %q, want %q", state.ChannelID, ch.ID)
	}
	if len(state.Shares) != 1 {
		t.Fatalf("screen.state shares = %d, want 1", len(state.Shares))
	}
	share := state.Shares[0]
	if share.SharerID != alice.ID || share.MemberName != "alice" {
		t.Fatalf("share sharer = (%q,%q), want (%q,alice)", share.SharerID, share.MemberName, alice.ID)
	}
	if share.Source.Width != 2560 || share.Source.Height != 1440 || share.Source.Kind != "screen" {
		t.Fatalf("share source = %dx%d kind=%q, want 2560x1440 screen",
			share.Source.Width, share.Source.Height, share.Source.Kind)
	}
	if share.Mode != "direct" {
		t.Fatalf("share mode = %q, want direct (default)", share.Mode)
	}

	// 2) offer 广播：alice → bob，payload 原样透传。
	const offerPayload = `{"type":"offer","sdp":"v=0 fake offer"}`
	aliceWS.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID,
		Media:     "screen",
		Kind:      "offer",
		Payload:   offerPayload,
	})
	sigEnv := bobWS.waitFor(protocol.TypeMediaSignal)
	var sig protocol.MediaSignalPush
	if err := sigEnv.DecodePayload(&sig); err != nil {
		t.Fatalf("decode media.signal: %v", err)
	}
	if sig.From != alice.ID || sig.FromName != "alice" {
		t.Fatalf("media.signal from = (%q,%q), want (%q,alice)", sig.From, sig.FromName, alice.ID)
	}
	if sig.Kind != "offer" || sig.Media != "screen" {
		t.Fatalf("media.signal kind/media = %q/%q, want offer/screen", sig.Kind, sig.Media)
	}
	if sig.Payload != offerPayload {
		t.Fatalf("media.signal payload was rewritten by the server: %q", sig.Payload)
	}

	// 3) 定向 answer：bob → alice，只有 alice 能收到。
	const answerPayload = `{"type":"answer","sdp":"v=0 fake answer"}`
	bobWS.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID,
		To:        alice.ID,
		Media:     "screen",
		Kind:      "answer",
		Payload:   answerPayload,
	})
	sigEnv = aliceWS.waitFor(protocol.TypeMediaSignal)
	if err := sigEnv.DecodePayload(&sig); err != nil {
		t.Fatalf("decode media.signal answer: %v", err)
	}
	if sig.From != bob.ID || sig.Kind != "answer" || sig.Payload != answerPayload {
		t.Fatalf("answer relay = from:%q kind:%q payload:%q", sig.From, sig.Kind, sig.Payload)
	}

	// 4) 非法参数与未订阅频道被拒（错误码稳定）。
	aliceWS.expectError(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID, Media: "screen", Kind: "bogus", Payload: "{}",
	}, protocol.CodeInvalidArgument)
	aliceWS.expectError(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: "01NOTSUBSCRIBED000000000000", Media: "screen", Kind: "offer", Payload: "{}",
	}, protocol.CodeForbidden)
	aliceWS.expectError(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID, Media: "screen", Kind: "offer", Payload: strings.Repeat("x", protocol.MaxSignalPayload+1),
	}, protocol.CodeInvalidArgument)

	// 5) screen.stop：bob 收到空列表。
	aliceWS.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: ch.ID})
	stateEnv = bobWS.waitFor(protocol.TypeScreenState)
	if err := stateEnv.DecodePayload(&state); err != nil {
		t.Fatalf("decode screen.state after stop: %v", err)
	}
	if len(state.Shares) != 0 {
		t.Fatalf("screen.state shares after stop = %d, want 0", len(state.Shares))
	}
}
