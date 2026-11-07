package server_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"partyup/internal/protocol"
)

// waitForMembersState 等一条满足谓词的 members.state 推送。
func (c *wsClient) waitForMembersState(pred func([]protocol.MemberRosterEntry) bool) []protocol.MemberRosterEntry {
	c.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting for members.state: %v", err)
		}
		if env.Type != protocol.TypeMembersState {
			continue
		}
		var p protocol.MemberListOK
		if err := env.DecodePayload(&p); err != nil {
			c.t.Fatalf("decode members.state: %v", err)
		}
		if pred(p.Members) {
			return p.Members
		}
	}
}

// rosterByID 在名册里找成员；找不到返回 false。
func rosterByID(members []protocol.MemberRosterEntry, id string) (protocol.MemberRosterEntry, bool) {
	for _, m := range members {
		if m.ID == id {
			return m, true
		}
	}
	return protocol.MemberRosterEntry{}, false
}

// getRoster 经 REST 拉全服名册。
func (e *testEnv) getRoster(token string) (int, []byte) {
	e.t.Helper()
	return e.doJSON(http.MethodGet, "/api/members", token, nil)
}

// TestMemberRosterIncludesOfflineAndAggregatesConnections 锁定全服名册：
//   - GET /api/members 需认证；返回 DB 全部成员（含从未连接的离线成员）；
//   - online 按 member_id 聚合多条 WebSocket，一条断开不会让成员离线；
//   - 空闲 = 在线但未入语音；文字频道订阅不等于语音位置；
//   - voice_channel_ids 是实际语音频道集合（同一成员可同时在多个频道）；
//   - 响应不含 password_hash / token 等敏感字段。
func TestMemberRosterIncludesOfflineAndAggregatesConnections(t *testing.T) {
	env := newTestEnv(t)
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	// 未认证拒绝。
	status, body := env.getRoster("")
	if status != http.StatusUnauthorized {
		t.Fatalf("GET /api/members without token: status=%d body=%s, want 401", status, body)
	}

	// 只建了 HTTP 会话、没有 WS 连接：两个成员都算离线，但都必须在名册里。
	status, body = env.getRoster(aliceToken)
	if status != http.StatusOK {
		t.Fatalf("GET /api/members: status=%d body=%s", status, body)
	}
	if strings.Contains(string(body), "password_hash") || strings.Contains(string(body), `"token"`) {
		t.Fatalf("member roster leaked sensitive fields: %s", body)
	}
	var roster protocol.MemberListOK
	if err := json.Unmarshal(body, &roster); err != nil {
		t.Fatalf("decode roster: %v", err)
	}
	if len(roster.Members) != 2 {
		t.Fatalf("roster size = %d, want 2 (含未连接的 bob)", len(roster.Members))
	}
	aliceEntry, ok := rosterByID(roster.Members, alice.ID)
	if !ok {
		t.Fatalf("roster missing alice: %+v", roster.Members)
	}
	bobEntry, ok := rosterByID(roster.Members, bob.ID)
	if !ok {
		t.Fatalf("roster missing bob: %+v", roster.Members)
	}
	for _, m := range []protocol.MemberRosterEntry{aliceEntry, bobEntry} {
		if m.Online || m.State != "offline" {
			t.Fatalf("member %s before WS = online:%v state:%q, want offline", m.Name, m.Online, m.State)
		}
		if m.VoiceChannelIDs == nil || len(m.VoiceChannelIDs) != 0 {
			t.Fatalf("member %s voice_channel_ids = %v, want []", m.Name, m.VoiceChannelIDs)
		}
	}

	// alice 连上：广播推送应显示 online / idle（在线但不在语音）。
	aliceWS := dialWS(t, env.http.URL, aliceToken)
	state := aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		m, ok := rosterByID(members, alice.ID)
		return ok && m.Online && m.State == "idle"
	})
	if entry, _ := rosterByID(state, bob.ID); entry.Online {
		t.Fatalf("bob should remain offline before connecting: %+v", entry)
	}

	// bob 连上：两人都 online/idle。
	bobWS := dialWS(t, env.http.URL, bobToken)
	aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		a, okA := rosterByID(members, alice.ID)
		b, okB := rosterByID(members, bob.ID)
		return okA && okB && a.Online && b.Online && b.State == "idle"
	})

	ch1 := aliceWS.createChannel("roster-one")
	ch2 := aliceWS.createChannel("roster-two")
	aliceWS.subscribe(ch1.ID)
	bobWS.subscribe(ch1.ID)

	// bob 只在 ch1 语音；alice 只订阅 ch1 文字，不因此被算进语音。
	bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch1.ID})
	aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		b, ok := rosterByID(members, bob.ID)
		return ok && b.State == "voice" && len(b.VoiceChannelIDs) == 1 && b.VoiceChannelIDs[0] == ch1.ID
	})

	// bob 的第二条连接进 ch2 语音：同一成员聚合为一项，两个语音频道都要出现。
	bobWS2 := dialWS(t, env.http.URL, bobToken)
	bobWS2.subscribe(ch2.ID)
	bobWS2.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch2.ID})
	state = aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		if len(members) != 2 {
			return false
		}
		b, ok := rosterByID(members, bob.ID)
		return ok && b.Online && b.State == "voice" && len(b.VoiceChannelIDs) == 2
	})
	if b, _ := rosterByID(state, bob.ID); len(b.VoiceChannelIDs) != 2 ||
		b.VoiceChannelIDs[0] != ch1.ID || b.VoiceChannelIDs[1] != ch2.ID {
		t.Fatalf("bob voice_channel_ids = %v, want sorted [%s %s]", b.VoiceChannelIDs, ch1.ID, ch2.ID)
	}
	if a, _ := rosterByID(state, alice.ID); a.State != "idle" {
		t.Fatalf("alice state = %q, want idle (文字订阅不算语音)", a.State)
	}

	// 断开 bob 的第一条连接：成员仍在线，ch1 语音清掉，ch2 保留。
	_ = bobWS.conn.Close()
	state = aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		b, ok := rosterByID(members, bob.ID)
		return ok && b.Online && len(b.VoiceChannelIDs) == 1 && b.VoiceChannelIDs[0] == ch2.ID
	})
	if b, _ := rosterByID(state, bob.ID); b.State != "voice" {
		t.Fatalf("bob state after first conn close = %q, want voice (ch2)", b.State)
	}

	// 断开最后一条：离线、无语音频道。
	_ = bobWS2.conn.Close()
	state = aliceWS.waitForMembersState(func(members []protocol.MemberRosterEntry) bool {
		b, ok := rosterByID(members, bob.ID)
		return ok && !b.Online && b.State == "offline" && len(b.VoiceChannelIDs) == 0
	})
	if b, _ := rosterByID(state, bob.ID); b.State != "offline" {
		t.Fatalf("bob state after all conns closed = %q, want offline", b.State)
	}

	// WS member.list 与 REST 返回同一份数据。
	env2 := aliceWS.request(protocol.TypeMemberList, nil)
	var wsRoster protocol.MemberListOK
	if err := env2.DecodePayload(&wsRoster); err != nil {
		t.Fatalf("decode member.list.ok: %v", err)
	}
	if len(wsRoster.Members) != 2 {
		t.Fatalf("member.list size = %d, want 2", len(wsRoster.Members))
	}
	if b, _ := rosterByID(wsRoster.Members, bob.ID); b.Online {
		t.Fatalf("member.list bob should be offline: %+v", b)
	}

	// 最终 REST 与推送一致：bob 离线且不在任何语音频道。
	status, body = env.getRoster(aliceToken)
	if status != http.StatusOK {
		t.Fatalf("final GET /api/members: status=%d", status)
	}
	if err := json.Unmarshal(body, &roster); err != nil {
		t.Fatalf("decode final roster: %v", err)
	}
	if b, _ := rosterByID(roster.Members, bob.ID); b.Online || len(b.VoiceChannelIDs) != 0 {
		t.Fatalf("final roster bob = %+v, want offline/no voice", b)
	}
}
