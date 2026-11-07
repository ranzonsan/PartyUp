package server_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"

	"partyup/internal/protocol"
)

// TestDisplayNameRegistrationAndSessionContract 锁定 display_name 的 HTTP 契约：
//   - bootstrap / invite 注册都接受可选 display_name；未提供或空白时回退登录名；
//   - login / me 原样带回 display_name，且 name（唯一登录名）语义不变；
//   - 非法显示名 400，且不会消费邀请码。
func TestDisplayNameRegistrationAndSessionContract(t *testing.T) {
	env := newTestEnv(t)

	status, body := env.doJSON(http.MethodPost, "/api/bootstrap", "", map[string]string{
		"token":        env.bootstrapToken,
		"username":     "alice",
		"password":     "secret123",
		"display_name": "Alice 🐱",
	})
	if status != http.StatusCreated {
		t.Fatalf("bootstrap with display_name: status=%d body=%s", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		t.Fatalf("decode bootstrap: %v", err)
	}
	if sess.Member.Name != "alice" {
		t.Fatalf("bootstrap member.name = %q, want alice (登录名不被显示名替换)", sess.Member.Name)
	}
	if sess.Member.DisplayName != "Alice 🐱" {
		t.Fatalf("bootstrap member.display_name = %q, want Alice 🐱", sess.Member.DisplayName)
	}
	if sess.Member.ID == sess.Member.DisplayName {
		t.Fatalf("display_name must not be used as id: %+v", sess.Member)
	}

	// 登录（旧客户端请求体没有 display_name）仍工作，并带回已存显示名。
	status, body = env.doJSON(http.MethodPost, "/api/login", "", map[string]string{
		"username": "alice", "password": "secret123",
	})
	if status != http.StatusOK {
		t.Fatalf("login: status=%d body=%s", status, body)
	}
	if err := json.Unmarshal(body, &sess); err != nil {
		t.Fatalf("decode login: %v", err)
	}
	if sess.Member.DisplayName != "Alice 🐱" {
		t.Fatalf("login display_name = %q, want Alice 🐱", sess.Member.DisplayName)
	}
	token := sess.Token

	status, body = env.doJSON(http.MethodGet, "/api/me", token, nil)
	if status != http.StatusOK {
		t.Fatalf("me: status=%d body=%s", status, body)
	}
	var me struct {
		Member protocol.Member `json:"member"`
	}
	if err := json.Unmarshal(body, &me); err != nil {
		t.Fatalf("decode me: %v", err)
	}
	if me.Member.DisplayName != "Alice 🐱" || me.Member.Name != "alice" {
		t.Fatalf("me member = %+v, want name=alice display_name=Alice 🐱", me.Member)
	}

	// 未提供 display_name → 回退 username；空白 display_name 同样回退。
	bob, _ := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	if bob.DisplayName != "bob" {
		t.Fatalf("bob display_name = %q, want fallback bob", bob.DisplayName)
	}

	code := env.newInvite()
	status, body = env.doJSON(http.MethodPost, "/api/invite/"+code, "", map[string]string{
		"username": "carol", "password": "secret123", "display_name": "   ",
	})
	if status != http.StatusCreated {
		t.Fatalf("register with blank display_name: status=%d body=%s", status, body)
	}
	var carol sessionResponse
	if err := json.Unmarshal(body, &carol); err != nil {
		t.Fatalf("decode carol: %v", err)
	}
	if carol.Member.DisplayName != "carol" {
		t.Fatalf("blank display_name fallback = %q, want carol", carol.Member.DisplayName)
	}

	// 非法显示名：控制字符与超长都必须 400，且邀请码不被消费。
	for _, bad := range []string{"bad\nname", strings.Repeat("x", 33)} {
		invite := env.newInvite()
		status, body = env.doJSON(http.MethodPost, "/api/invite/"+invite, "", map[string]string{
			"username": "dave", "password": "secret123", "display_name": bad,
		})
		if status != http.StatusBadRequest {
			t.Fatalf("invalid display_name %q: status=%d body=%s, want 400", bad, status, body)
		}
		var errResp errorResponse
		if err := json.Unmarshal(body, &errResp); err != nil {
			t.Fatalf("decode invalid display error: %v", err)
		}
		if errResp.Code != protocol.CodeInvalidArgument {
			t.Fatalf("invalid display_name code = %q, want %q", errResp.Code, protocol.CodeInvalidArgument)
		}
		inv, err := env.store.InviteByCode(t.Context(), invite)
		if err != nil {
			t.Fatalf("invite lookup after invalid display: %v", err)
		}
		if inv.UsedBy != "" {
			t.Fatalf("invalid display_name consumed invite: %+v", inv)
		}
	}
}

// TestDisplayNameInMessageAndPresence 锁定展示契约：
//   - text.new / text.history 的 member_name 仍是唯一登录名，member_display_name 是展示名；
//   - presence 同时给 name 与 display_name（旧字段语义不变）。
func TestDisplayNameInMessageAndPresence(t *testing.T) {
	env := newTestEnv(t)
	status, body := env.doJSON(http.MethodPost, "/api/bootstrap", "", map[string]string{
		"token": env.bootstrapToken, "username": "alice", "password": "secret123",
		"display_name": "Alice A",
	})
	if status != http.StatusCreated {
		t.Fatalf("bootstrap: status=%d body=%s", status, body)
	}
	var sess sessionResponse
	if err := json.Unmarshal(body, &sess); err != nil {
		t.Fatalf("decode bootstrap: %v", err)
	}
	aliceToken := sess.Token
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	// registerViaInvite 不带 display_name，bob 应回退登录名。
	if bob.DisplayName != "bob" {
		t.Fatalf("bob display_name = %q, want bob", bob.DisplayName)
	}

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)
	ch := aliceWS.createChannel("display-contract")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	presence := aliceWS.waitForPresenceWith(ch.ID, 2)
	var alicePresence *protocol.PresenceMember
	for i := range presence.Members {
		if presence.Members[i].MemberID == sess.Member.ID {
			alicePresence = &presence.Members[i]
		}
	}
	if alicePresence == nil {
		t.Fatalf("presence members = %+v, missing alice", presence.Members)
	}
	if alicePresence.Name != "alice" || alicePresence.DisplayName != "Alice A" {
		t.Fatalf("presence alice = %+v, want name=alice display_name=Alice A", *alicePresence)
	}

	aliceWS.sendText(ch.ID, "hello")
	textEnv := bobWS.waitFor(protocol.TypeTextNew)
	var msg protocol.TextMessage
	if err := textEnv.DecodePayload(&msg); err != nil {
		t.Fatalf("decode text.new: %v", err)
	}
	if msg.MemberName != "alice" || msg.MemberDisplayName != "Alice A" {
		t.Fatalf("text.new author = name:%q display:%q, want alice / Alice A",
			msg.MemberName, msg.MemberDisplayName)
	}
	if msg.MemberID == "" || msg.MemberID == msg.MemberDisplayName {
		t.Fatalf("text.new member_id = %q (display name must not replace id)", msg.MemberID)
	}

	histEnv := bobWS.request(protocol.TypeTextHistory, protocol.TextHistoryRequest{ChannelID: ch.ID, Limit: 50})
	var hist protocol.TextHistoryOK
	if err := histEnv.DecodePayload(&hist); err != nil {
		t.Fatalf("decode text.history: %v", err)
	}
	if len(hist.Messages) != 1 {
		t.Fatalf("history size = %d, want 1", len(hist.Messages))
	}
	if hist.Messages[0].MemberName != "alice" || hist.Messages[0].MemberDisplayName != "Alice A" {
		t.Fatalf("history author = name:%q display:%q, want alice / Alice A",
			hist.Messages[0].MemberName, hist.Messages[0].MemberDisplayName)
	}
}
