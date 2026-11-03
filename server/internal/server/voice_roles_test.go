package server_test

import (
	"testing"

	"partyup/internal/protocol"
)

// TestVoiceRolesAssignedByServer 锁定「直连角色由服务端裁决」：
//   - 频道里只有自己 → answerer（先等着，别对着空气发 offer）；
//   - 第二个人加入 → 新来者 offerer、先来的 answerer（两端不会都判 answerer）；
//   - relay 模式 → 全员 answerer（服务端 SFU 才是 offerer）。
func TestVoiceRolesAssignedByServer(t *testing.T) {
	t.Run("direct", func(t *testing.T) {
		env := newTestEnvWithModes(t, "direct", "direct")
		alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
		code := env.newInvite()
		bob, bobToken := env.registerViaInvite(code, "bob", "secret123")
		aliceWS := dialWS(t, env.http.URL, aliceToken)
		bobWS := dialWS(t, env.http.URL, bobToken)
		ch := aliceWS.createChannel("roles")
		aliceWS.subscribe(ch.ID)
		bobWS.subscribe(ch.ID)
		aliceWS.waitForPresenceWith(ch.ID, 2)

		// 1) 单独加入：role=answerer。
		aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
		st := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 1 })
		if st.Members[0].MemberID != alice.ID || st.Members[0].Role != "answerer" {
			t.Fatalf("solo member = %+v, want alice/answerer", st.Members[0])
		}
		if st.Members[0].JoinedSeq != 1 {
			t.Fatalf("solo joined_seq = %d, want 1", st.Members[0].JoinedSeq)
		}

		// 2) 第二个人加入：先来者 answerer，新来者 offerer —— 两人里恰好一个 offerer。
		bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
		st = waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 2 })
		roles := map[string]string{}
		for _, m := range st.Members {
			roles[m.MemberID] = m.Role
		}
		if roles[alice.ID] != "answerer" || roles[bob.ID] != "offerer" {
			t.Fatalf("roles = %v, want alice=answerer bob=offerer", roles)
		}

		// 3) 第三个人加入：不能因为新来者把已在通话的两人角色改掉（mesh 暂不支持）。
		carol, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")
		carolWS := dialWS(t, env.http.URL, carolToken)
		carolWS.subscribe(ch.ID)
		aliceWS.waitForPresenceWith(ch.ID, 3)
		carolWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
		st = waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 3 })
		roles = map[string]string{}
		for _, m := range st.Members {
			roles[m.MemberID] = m.Role
		}
		if roles[alice.ID] != "answerer" || roles[bob.ID] != "offerer" || roles[carol.ID] != "answerer" {
			t.Fatalf("roles after third join = %v, want alice=answerer bob=offerer carol=answerer", roles)
		}
	})

	t.Run("relay", func(t *testing.T) {
		env := newTestEnvWithModes(t, "relay", "direct")
		_, aliceToken := env.bootstrapAdmin("alice", "secret123")
		code := env.newInvite()
		_, bobToken := env.registerViaInvite(code, "bob", "secret123")
		aliceWS := dialWS(t, env.http.URL, aliceToken)
		bobWS := dialWS(t, env.http.URL, bobToken)
		ch := aliceWS.createChannel("roles-relay")
		aliceWS.subscribe(ch.ID)
		bobWS.subscribe(ch.ID)
		aliceWS.waitForPresenceWith(ch.ID, 2)

		aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
		waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 1 })
		bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
		st := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 2 })
		for _, m := range st.Members {
			if m.Role != "answerer" {
				t.Fatalf("relay member %s role = %q, want answerer", m.MemberID, m.Role)
			}
		}
	})
}
