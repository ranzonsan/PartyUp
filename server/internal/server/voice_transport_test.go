package server_test

import (
	"testing"

	"partyup/internal/protocol"
)

// TestVoiceStateTransportIsPerMember 锁定 PRODUCT.md 的硬要求：
// transport **按成员下发**，不是频道级单字段。
//
// v1 服务端给所有人填同一个值（等价于服务端策略），但协议结构必须 per-member——
// 将来支持逐成员混合路径时只改服务端填充逻辑，客户端与协议都不用重设计。
func TestVoiceStateTransportIsPerMember(t *testing.T) {
	cases := []struct {
		name      string
		voiceMode string
		want      string
	}{
		{"relay policy", "relay", "relay"},
		{"direct policy", "direct", "direct"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			env := newTestEnvWith(t, tc.voiceMode)
			alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
			_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
			aliceWS := dialWS(t, env.http.URL, aliceToken)
			bobWS := dialWS(t, env.http.URL, bobToken)
			ch := aliceWS.createChannel("transport-per-member")
			aliceWS.subscribe(ch.ID)
			bobWS.subscribe(ch.ID)
			aliceWS.waitForPresenceWith(ch.ID, 2)

			aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
			bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})

			state := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool {
				return len(s.Members) == 2
			})
			if state.VoiceMode != tc.voiceMode {
				t.Fatalf("voice_mode = %q, want %q (策略字段必须保留)", state.VoiceMode, tc.voiceMode)
			}
			seen := map[string]bool{}
			for _, m := range state.Members {
				if m.Transport != tc.want {
					t.Fatalf("member %s transport = %q, want %q (per-member 字段)", m.MemberID, m.Transport, tc.want)
				}
				if m.TransportReason != "server_policy" {
					t.Fatalf("member %s transport_reason = %q, want server_policy",
						m.MemberID, m.TransportReason)
				}
				seen[m.MemberID] = true
			}
			if len(seen) != 2 {
				t.Fatalf("voice.state members = %v, want 2 members", seen)
			}
			_ = alice.ID
		})
	}
}
