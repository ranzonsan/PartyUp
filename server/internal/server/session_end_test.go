package server_test

// 会话终止原因的区分（2026-11-07 小批次）：
//   login_elsewhere → 4001（batch 1 已验收，语义不变）
//   kicked          → 4002
//   banned          → 4003
// 应用层 session.replaced.reason 与关闭码必须同时正确：客户端按 reason 显示文案，
// 关闭码用于诊断与兜底。三者都由真机 + 单测双重覆盖。

import (
	"encoding/json"
	"net/http"
	"testing"

	"partyup/internal/protocol"
)

// TestKickSessionReasonAndCloseCode：管理员踢下线 → reason=kicked + 4002。
func TestKickSessionReasonAndCloseCode(t *testing.T) {
	env, _, aliceToken, _, _, carol, carolToken := setupRoles(t)
	ws := dialWS(t, env.http.URL, carolToken)

	status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/kick", aliceToken, nil)
	if status != http.StatusOK {
		t.Fatalf("kick: status=%d body=%s", status, body)
	}

	msg := ws.waitFor(protocol.TypeSessionReplaced)
	var p protocol.SessionReplacedPayload
	if err := msg.DecodePayload(&p); err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if p.Reason != protocol.SessionReasonKicked {
		t.Fatalf("session.replaced reason = %q, want %q", p.Reason, protocol.SessionReasonKicked)
	}
	assertWSClosedWith(t, ws, protocol.CloseSessionKicked)

	// 被踢后旧 token 已撤销。
	if status, _ := env.doJSON(http.MethodGet, "/api/me", carolToken, nil); status != http.StatusUnauthorized {
		t.Fatalf("kicked token /api/me status = %d, want 401", status)
	}
}

// TestBanSessionReasonAndCloseCode：封禁 → reason=banned + 4003，且之后登录被拒。
func TestBanSessionReasonAndCloseCode(t *testing.T) {
	env, _, aliceToken, _, _, carol, carolToken := setupRoles(t)
	ws := dialWS(t, env.http.URL, carolToken)

	status, body := env.doJSON(http.MethodPost, "/api/admin/members/"+carol.ID+"/ban", aliceToken,
		map[string]string{"reason": "unit test"})
	if status != http.StatusOK {
		t.Fatalf("ban: status=%d body=%s", status, body)
	}

	msg := ws.waitFor(protocol.TypeSessionReplaced)
	var p protocol.SessionReplacedPayload
	if err := msg.DecodePayload(&p); err != nil {
		t.Fatalf("decode session.replaced: %v", err)
	}
	if p.Reason != protocol.SessionReasonBanned {
		t.Fatalf("session.replaced reason = %q, want %q", p.Reason, protocol.SessionReasonBanned)
	}
	assertWSClosedWith(t, ws, protocol.CloseSessionBanned)

	// 封禁后登录被拒（账号级），错误码 MEMBER_BANNED。
	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "carol", "password": "carolpass123",
	})
	if status != http.StatusForbidden {
		t.Fatalf("banned login status = %d, want 403 (body=%s)", status, body)
	}
	var e errorResponse
	if err := json.Unmarshal(body, &e); err != nil || e.Code != protocol.CodeMemberBanned {
		t.Fatalf("banned login code = %q, want %q", e.Code, protocol.CodeMemberBanned)
	}
}
