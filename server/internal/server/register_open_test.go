package server_test

// 开放注册（registration=open，2026-11-07 小批次）：
// open 下匿名 POST /api/register 可直接注册（role=user）；invite/closed 下拒绝；
// member_limit 生效；重名 409。注册与邀请注册共用同样的校验与限速。

import (
	"encoding/json"
	"net/http"
	"testing"

	"partyup/internal/protocol"
)

// registerCreds 造一份注册请求体。
func registerCreds(name string) map[string]string {
	return map[string]string{"username": name, "password": "pass-" + name + "-123"}
}

// setRegistration 用 owner token 改注册策略。
func setRegistration(t *testing.T, env *testEnv, ownerToken, value string) {
	t.Helper()
	status, body := env.doJSON(http.MethodPatch, "/api/admin/settings", ownerToken,
		map[string]string{"registration": value})
	if status != http.StatusOK {
		t.Fatalf("set registration=%s: status=%d body=%s", value, status, body)
	}
}

// TestRegistrationOpenFlow 覆盖 open 注册主路径与错误分支。
func TestRegistrationOpenFlow(t *testing.T) {
	env, _, aliceToken, _, _, carol, _ := setupRoles(t)

	// 默认 invite：匿名 /api/register 被拒（需要邀请码）。
	status, body := env.doJSON(http.MethodPost, "/api/register", "", registerCreds("dave"))
	if status != http.StatusForbidden {
		t.Fatalf("register under invite: status=%d want 403 body=%s", status, body)
	}
	var e errorResponse
	if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeForbidden {
		t.Fatalf("register under invite code=%q want %q", e.Code, protocol.CodeForbidden)
	}

	// owner 打开开放注册。
	setRegistration(t, env, aliceToken, "open")

	// 注册成功：201 + role=user。
	status, body = env.doJSON(http.MethodPost, "/api/register", "", registerCreds("dave"))
	if status != http.StatusCreated {
		t.Fatalf("open register: status=%d body=%s", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		t.Fatalf("decode register: %v", err)
	}
	if sess.Token == "" {
		t.Fatal("register returned empty token")
	}
	if sess.Member.Role != protocol.RoleUser || sess.Member.Admin {
		t.Fatalf("registered member role = %+v, want user", sess.Member)
	}

	// 重名 → 409。
	status, body = env.doJSON(http.MethodPost, "/api/register", "", registerCreds("dave"))
	if status != http.StatusConflict {
		t.Fatalf("duplicate register: status=%d want 409 body=%s", status, body)
	}

	// 成员上限：现有 4 人（alice/bob/carol/dave），把上限设为 4 后注册被拒。
	status, body = env.doJSON(http.MethodPatch, "/api/admin/settings", aliceToken,
		map[string]int{"member_limit": 4})
	if status != http.StatusOK {
		t.Fatalf("set member_limit: status=%d body=%s", status, body)
	}
	status, body = env.doJSON(http.MethodPost, "/api/register", "", registerCreds("eve"))
	if status != http.StatusConflict {
		t.Fatalf("register over member_limit: status=%d want 409 body=%s", status, body)
	}

	// closed：拒绝。
	setRegistration(t, env, aliceToken, "closed")
	status, body = env.doJSON(http.MethodPost, "/api/register", "", registerCreds("frank"))
	if status != http.StatusForbidden {
		t.Fatalf("register under closed: status=%d want 403 body=%s", status, body)
	}

	// 被上限拒绝的 eve 没有账号：可以用她的凭据登录失败（确认没被创建）。
	status, _ = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "eve", "password": "pass-eve-123",
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("login for not-created user: status=%d want 401", status)
	}
	_ = carol
}
