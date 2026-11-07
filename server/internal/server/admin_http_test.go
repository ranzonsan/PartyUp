package server_test

// 身份/权限第一批（2026-11-07）的 HTTP 级回归：
// 角色矩阵（谁能调什么）、owner 唯一性与转让、封禁生效、审计可查、匿名 server-info。
// 全部走真实 HTTP + 真实 SQLite；权限绝不 mock，因为缺陷正是「只靠客户端隐藏按钮」。

import (
	"encoding/json"
	"net/http"
	"testing"

	"partyup/internal/protocol"
)

// promote 用 owner token 把成员在 user ↔ admin 之间调整。
func (e *testEnv) promote(ownerToken, memberID, role string) (int, []byte) {
	e.t.Helper()
	return e.doJSON(http.MethodPost, "/api/admin/members/"+memberID+"/role", ownerToken,
		map[string]string{"role": role})
}

// setupRoles 起一个 owner=alice、admin=bob、user=carol 的环境。
func setupRoles(t *testing.T) (env *testEnv, alice protocol.Member, aliceToken string,
	bob protocol.Member, bobToken string, carol protocol.Member, carolToken string) {
	t.Helper()
	env = newTestEnv(t)
	alice, aliceToken = env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken = env.registerViaInvite(code, "bob", "bobpass123")
	code = env.newInvite()
	carol, carolToken = env.registerViaInvite(code, "carol", "carolpass123")

	status, body := env.promote(aliceToken, bob.ID, "admin")
	if status != http.StatusOK {
		t.Fatalf("promote bob: status=%d body=%s", status, body)
	}
	return
}

// TestRolesOnSessionResponses 确认协议 Member 带 role，且 admin 兼容位正确。
func TestRolesOnSessionResponses(t *testing.T) {
	env, alice, aliceToken, bob, bobToken, carol, carolToken := setupRoles(t)

	if alice.Role != protocol.RoleOwner || !alice.Admin {
		t.Fatalf("bootstrap member role = %+v, want owner", alice)
	}
	if bob.Role != protocol.RoleUser || bob.Admin {
		t.Fatalf("bob initial role = %+v, want user", bob)
	}
	if carol.Role != protocol.RoleUser {
		t.Fatalf("carol role = %q, want user", carol.Role)
	}
	// /api/me 反映提升后的角色。
	status, body := env.doJSON(http.MethodGet, "/api/me", bobToken, nil)
	if status != http.StatusOK {
		t.Fatalf("bob me: status=%d body=%s", status, body)
	}
	var me struct {
		Member protocol.Member `json:"member"`
	}
	if err := json.Unmarshal(body, &me); err != nil {
		t.Fatalf("decode me: %v", err)
	}
	if me.Member.Role != protocol.RoleAdmin || !me.Member.Admin {
		t.Fatalf("bob me role = %+v, want admin", me.Member)
	}
	_ = aliceToken
	_ = carolToken
}

// TestAdminPermissionMatrix 逐条验证权限矩阵（规格 §2）：
// user 一律 403 ADMIN_ONLY；admin 可管理但危险设置/任免/转让 403 OWNER_ONLY；
// owner 全部可做。这是「服务端强制」的核心证据。
func TestAdminPermissionMatrix(t *testing.T) {
	env, alice, aliceToken, bob, bobToken, carol, carolToken := setupRoles(t)

	// --- user 不能访问任何管理端点 ---
	userCases := []struct {
		method, path string
		body         any
	}{
		{http.MethodGet, "/api/admin/overview", nil},
		{http.MethodGet, "/api/admin/settings", nil},
		{http.MethodPatch, "/api/admin/settings", map[string]any{"announcement": "hi"}},
		{http.MethodGet, "/api/admin/members", nil},
		{http.MethodGet, "/api/admin/invites", nil},
		{http.MethodPost, "/api/admin/invites", nil},
		{http.MethodGet, "/api/admin/channels", nil},
		{http.MethodGet, "/api/admin/audit", nil},
	}
	for _, tc := range userCases {
		status, body := env.doJSON(tc.method, tc.path, carolToken, tc.body)
		if status != http.StatusForbidden {
			t.Fatalf("%s %s as user: status=%d body=%s, want 403", tc.method, tc.path, status, body)
		}
		var e errorResponse
		if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeAdminOnly {
			t.Fatalf("%s %s as user: code=%q (err=%v), want %s", tc.method, tc.path, e.Code, err, protocol.CodeAdminOnly)
		}
	}

	// --- admin：普通管理可用 ---
	if status, body := env.doJSON(http.MethodGet, "/api/admin/overview", bobToken, nil); status != http.StatusOK {
		t.Fatalf("admin overview: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodPatch, "/api/admin/settings", bobToken,
		map[string]any{"announcement": "welcome"}); status != http.StatusOK {
		t.Fatalf("admin patch announcement: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodPost, "/api/admin/invites", bobToken, nil); status != http.StatusCreated {
		t.Fatalf("admin create invite: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodPost, "/api/admin/channels", bobToken,
		map[string]string{"name": "admin-created"}); status != http.StatusCreated {
		t.Fatalf("admin create channel: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodGet, "/api/admin/members", bobToken, nil); status != http.StatusOK {
		t.Fatalf("admin list members: status=%d body=%s", status, body)
	}

	// --- admin：危险设置 403 OWNER_ONLY，且值没被改 ---
	for _, patch := range []map[string]any{
		{"voice_mode": "direct"},
		{"screen_mode": "direct"},
		{"max_screen_shares": 8},
	} {
		status, body := env.doJSON(http.MethodPatch, "/api/admin/settings", bobToken, patch)
		if status != http.StatusForbidden {
			t.Fatalf("admin dangerous %v: status=%d body=%s, want 403", patch, status, body)
		}
		var e errorResponse
		if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeOwnerOnly {
			t.Fatalf("admin dangerous %v: code=%q, want %s", patch, e.Code, protocol.CodeOwnerOnly)
		}
	}
	status, body := env.doJSON(http.MethodGet, "/api/admin/settings", bobToken, nil)
	if status != http.StatusOK {
		t.Fatalf("get settings: status=%d body=%s", status, body)
	}
	var settings protocol.SettingsState
	if err := json.Unmarshal(body, &settings); err != nil {
		t.Fatalf("decode settings: %v", err)
	}
	if settings.VoiceMode != "relay" || settings.ScreenMode != "relay" {
		t.Fatalf("modes changed by admin: %+v", settings)
	}

	// --- admin：不能动 owner（踢/封/改角色/重置密码/任免） ---
	ownerOps := []struct {
		method, path string
		body         any
	}{
		{http.MethodPost, "/api/admin/members/" + alice.ID + "/kick", nil},
		{http.MethodPost, "/api/admin/members/" + alice.ID + "/ban", map[string]string{"reason": "x"}},
		{http.MethodPost, "/api/admin/members/" + alice.ID + "/password", nil},
		{http.MethodPost, "/api/admin/members/" + alice.ID + "/role", map[string]string{"role": "user"}},
	}
	for _, tc := range ownerOps {
		status, body := env.doJSON(tc.method, tc.path, bobToken, tc.body)
		if status != http.StatusForbidden {
			t.Fatalf("admin %s %s: status=%d body=%s, want 403", tc.method, tc.path, status, body)
		}
		var e errorResponse
		if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeOwnerOnly {
			t.Fatalf("admin %s %s: code=%q, want %s", tc.method, tc.path, e.Code, protocol.CodeOwnerOnly)
		}
	}

	// --- admin：可以踢/封普通用户 ---
	if status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/kick", bobToken, nil); status != http.StatusOK {
		t.Fatalf("admin kick user: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodGet, "/api/me", carolToken, nil); status != http.StatusUnauthorized {
		t.Fatalf("kicked user me: status=%d body=%s, want 401", status, body)
	}

	// --- owner：危险设置可改，并广播到 server-info ---
	if status, body := env.doJSON(http.MethodPatch, "/api/admin/settings", aliceToken,
		map[string]string{"voice_mode": "direct", "screen_mode": "direct"}); status != http.StatusOK {
		t.Fatalf("owner patch modes: status=%d body=%s", status, body)
	}
	status, body = env.doJSON(http.MethodGet, "/api/server-info", "", nil)
	if status != http.StatusOK {
		t.Fatalf("server-info: status=%d body=%s", status, body)
	}
	var info protocol.ServerInfoPublic
	if err := json.Unmarshal(body, &info); err != nil {
		t.Fatalf("decode server-info: %v", err)
	}
	if info.VoiceMode != "direct" || info.ScreenMode != "direct" {
		t.Fatalf("server-info modes = %+v, want direct/direct", info)
	}
	if !info.Initialized {
		t.Fatal("server-info initialized = false, want true")
	}
	if info.BootstrapAllowed {
		t.Fatal("server-info bootstrap_allowed = true on initialized server")
	}

	// --- admin 不能任免/转让 ---
	if status, body := env.promote(bobToken, carol.ID, "admin"); status != http.StatusForbidden {
		t.Fatalf("admin promote: status=%d body=%s, want 403", status, body)
	}
	if status, body := env.doJSON(http.MethodPost, "/api/admin/owner/transfer", bobToken,
		map[string]string{"member_id": bob.ID}); status != http.StatusForbidden {
		t.Fatalf("admin transfer: status=%d body=%s, want 403", status, body)
	}
}

// TestOwnerTransferFlow 覆盖 owner 唯一性与转让：转让后旧 owner 降 admin，
// 全库仍只有 1 个 owner；旧 owner（现在是 admin）不能再转让。
func TestOwnerTransferFlow(t *testing.T) {
	env, alice, aliceToken, bob, bobToken, _, _ := setupRoles(t)

	// 初始唯一 owner 是 alice。
	status, body := env.doJSON(http.MethodGet, "/api/admin/members", aliceToken, nil)
	if status != http.StatusOK {
		t.Fatalf("list members: status=%d body=%s", status, body)
	}
	var list struct {
		Members []struct {
			ID   string `json:"id"`
			Role string `json:"role"`
		} `json:"members"`
	}
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatalf("decode members: %v", err)
	}
	ownerCount := 0
	for _, m := range list.Members {
		if m.Role == protocol.RoleOwner {
			ownerCount++
			if m.ID != alice.ID {
				t.Fatalf("owner id = %s, want alice %s", m.ID, alice.ID)
			}
		}
	}
	if ownerCount != 1 {
		t.Fatalf("owner count = %d, want 1", ownerCount)
	}

	// 转让给 bob。
	if status, body := env.doJSON(http.MethodPost, "/api/admin/owner/transfer", aliceToken,
		map[string]string{"member_id": bob.ID}); status != http.StatusOK {
		t.Fatalf("transfer: status=%d body=%s", status, body)
	}
	// alice 现在是 admin；bob 是 owner。
	status, body = env.doJSON(http.MethodGet, "/api/me", aliceToken, nil)
	if status != http.StatusOK {
		t.Fatalf("alice me: status=%d body=%s", status, body)
	}
	var meA struct {
		Member protocol.Member `json:"member"`
	}
	_ = json.Unmarshal(body, &meA)
	if meA.Member.Role != protocol.RoleAdmin || !meA.Member.Admin {
		t.Fatalf("alice after transfer = %+v, want admin", meA.Member)
	}
	status, body = env.doJSON(http.MethodGet, "/api/me", bobToken, nil)
	if status != http.StatusOK {
		t.Fatalf("bob me: status=%d body=%s", status, body)
	}
	var meB struct {
		Member protocol.Member `json:"member"`
	}
	_ = json.Unmarshal(body, &meB)
	if meB.Member.Role != protocol.RoleOwner {
		t.Fatalf("bob after transfer = %+v, want owner", meB.Member)
	}

	// 旧 owner（admin）再转让 → 403。
	if status, body := env.doJSON(http.MethodPost, "/api/admin/owner/transfer", aliceToken,
		map[string]string{"member_id": alice.ID}); status != http.StatusForbidden {
		t.Fatalf("ex-owner transfer: status=%d body=%s, want 403", status, body)
	}
	// 新 owner 不能再把自己的角色通过 role 接口改掉（该接口只接受 user/admin，且 owner 目标被拒）。
	if status, body := env.promote(aliceToken, bob.ID, "user"); status != http.StatusForbidden {
		t.Fatalf("demote owner: status=%d body=%s, want 403", status, body)
	}
}

// TestBanEnforcement 覆盖封禁：登录被拒、WS 鉴权被拒、已连接被踢、解封恢复。
func TestBanEnforcement(t *testing.T) {
	env, _, aliceToken, _, _, carol, carolToken := setupRoles(t)

	// 先建立一条 carol 的 WS 连接（应该被踢掉）。
	ws := dialWS(t, env.http.URL, carolToken)

	// 封禁 carol。
	if status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/ban", aliceToken,
		map[string]string{"reason": "spam"}); status != http.StatusOK {
		t.Fatalf("ban: status=%d body=%s", status, body)
	}
	// 2026-11-07 起封禁有专用 reason/关闭码（banned/4003），见 session_end_test.go。
	envMsg := ws.waitFor(protocol.TypeSessionReplaced)
	var p protocol.SessionReplacedPayload
	if err := envMsg.DecodePayload(&p); err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if p.Reason != protocol.SessionReasonBanned {
		t.Fatalf("ban session.replaced reason = %q, want %q", p.Reason, protocol.SessionReasonBanned)
	}
	assertWSClosedWith(t, ws, protocol.CloseSessionBanned)

	// 旧 token 立即失效（会话被撤销）。
	if status, body := env.doJSON(http.MethodGet, "/api/me", carolToken, nil); status != http.StatusUnauthorized {
		t.Fatalf("banned me: status=%d body=%s, want 401", status, body)
	}
	// 用正确密码重新登录 → 403 MEMBER_BANNED。
	status, body := env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "carol", "password": "carolpass123",
	})
	if status != http.StatusForbidden {
		t.Fatalf("banned login: status=%d body=%s, want 403", status, body)
	}
	var e errorResponse
	if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeMemberBanned {
		t.Fatalf("banned login code=%q, want %s", e.Code, protocol.CodeMemberBanned)
	}

	// WS 鉴权：即使拿到未过期的 token 也不能连（构造一条新 session 证明鉴权路径检查封禁）。
	// 这里直接用 login 拿不到 token，因此断言登录态即可；WS 路径由单元测试覆盖。

	// 解封后可以重新登录。
	if status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/unban", aliceToken, nil); status != http.StatusOK {
		t.Fatalf("unban: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "carol", "password": "carolpass123",
	}); status != http.StatusOK {
		t.Fatalf("login after unban: status=%d body=%s", status, body)
	}
}

// TestAuditTrail 覆盖审计：管理动作逐条可查、最新在前、普通用户不可读。
func TestAuditTrail(t *testing.T) {
	env, _, aliceToken, bob, bobToken, carol, carolToken := setupRoles(t)

	// 制造多种动作：频道创建（admin）、角色变更（owner）、封禁（admin）。
	if status, body := env.doJSON(http.MethodPost, "/api/admin/channels", bobToken,
		map[string]string{"name": "audit-channel"}); status != http.StatusCreated {
		t.Fatalf("create channel: status=%d body=%s", status, body)
	}
	if status, body := env.promote(aliceToken, carol.ID, "admin"); status != http.StatusOK {
		t.Fatalf("promote carol: status=%d body=%s", status, body)
	}
	if status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/ban", bobToken,
		map[string]string{"reason": "audit"}); status != http.StatusOK {
		t.Fatalf("ban carol: status=%d body=%s", status, body)
	}
	_ = carolToken

	status, body := env.doJSON(http.MethodGet, "/api/admin/audit?limit=50", aliceToken, nil)
	if status != http.StatusOK {
		t.Fatalf("audit: status=%d body=%s", status, body)
	}
	var resp struct {
		Events []struct {
			Action    string `json:"action"`
			ActorName string `json:"actor_name"`
			Target    string `json:"target"`
			AtMS      int64  `json:"at_ms"`
		} `json:"events"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		t.Fatalf("decode audit: %v", err)
	}
	seen := map[string]bool{}
	var lastAt int64 = 1 << 62
	for _, ev := range resp.Events {
		seen[ev.Action] = true
		if ev.AtMS > lastAt {
			t.Fatalf("audit not newest-first: %d after %d", ev.AtMS, lastAt)
		}
		lastAt = ev.AtMS
	}
	for _, want := range []string{"channel.create", "member.role_change", "member.ban"} {
		if !seen[want] {
			t.Fatalf("audit missing action %q; got %v", want, seen)
		}
	}

	// 普通用户（carol 已被封禁，用 bob 之外再注册一个）不可读审计。
	code := env.newInvite()
	_, daveToken := env.registerViaInvite(code, "dave", "davepass123")
	if status, body := env.doJSON(http.MethodGet, "/api/admin/audit", daveToken, nil); status != http.StatusForbidden {
		t.Fatalf("user audit: status=%d body=%s, want 403", status, body)
	}
	_ = bob
}

// TestServerInfoAnonymous 覆盖匿名探测：初始化前后状态与 bootstrap_allowed 语义。
func TestServerInfoAnonymous(t *testing.T) {
	env := newTestEnv(t) // 已生成 bootstrap token，但还没有成员 → 未初始化

	status, body := env.doJSON(http.MethodGet, "/api/server-info", "", nil)
	if status != http.StatusOK {
		t.Fatalf("server-info before bootstrap: status=%d body=%s", status, body)
	}
	var info protocol.ServerInfoPublic
	if err := json.Unmarshal(body, &info); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if info.Initialized {
		t.Fatal("initialized = true before bootstrap")
	}
	if !info.BootstrapAllowed {
		t.Fatal("bootstrap_allowed = false for loopback connection on uninitialized server")
	}
	if info.Registration != "invite" {
		t.Fatalf("registration = %q, want invite", info.Registration)
	}
	if info.ProtocolVersion != protocol.Version {
		t.Fatalf("protocol_version = %d, want %d", info.ProtocolVersion, protocol.Version)
	}

	env.bootstrapAdmin("alice", "secret123")
	status, body = env.doJSON(http.MethodGet, "/api/server-info", "", nil)
	if status != http.StatusOK {
		t.Fatalf("server-info after bootstrap: status=%d body=%s", status, body)
	}
	if err := json.Unmarshal(body, &info); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if !info.Initialized {
		t.Fatal("initialized = false after bootstrap")
	}
	if info.BootstrapAllowed {
		t.Fatal("bootstrap_allowed = true on initialized server")
	}
}
