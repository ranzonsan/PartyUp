package server_test

import (
	"testing"
	"time"

	"partyup/internal/protocol"
)

// TestVoiceTransportFailedDowngradesChannel 验收 3c 的服务端联动（PROTOCOL.md §4.4）：
//
//	direct 频道里某个成员上报「我这边直连失败」→ 服务端把**该频道所有成员**的
//	transport 降级为 relay 并广播 → 成员重新 voice.join 时按 relay 建 SFU 会话。
//
// 同时锁定四条硬要求：幂等、防刷（30s 冷却）、单向、日志可复盘（trigger_count）。
func TestVoiceTransportFailedDowngradesChannel(t *testing.T) {
	env := newTestEnvWith(t, "direct")
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)
	ch := aliceWS.createChannel("downgrade")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool {
		return len(s.Members) == 2 && allTransport(s, "direct", "server_policy")
	})

	// 1) alice 上报直连失败 → 服务端降级（degraded=true，trigger_count=1）。
	resp := aliceWS.request(protocol.TypeVoiceTransportFailed, protocol.VoiceTransportFailedRequest{
		ChannelID: ch.ID, Media: "voice", Reason: "ice_failed",
	})
	var ack protocol.VoiceTransportFailedOK
	if err := resp.DecodePayload(&ack); err != nil {
		t.Fatalf("decode voice.transport_failed.ok: %v", err)
	}
	if !ack.Degraded || ack.Transport != "relay" || ack.Reason != "downgraded" || ack.TriggerCount != 1 {
		t.Fatalf("downgrade ack = %+v, want degraded=true transport=relay reason=downgraded trigger_count=1", ack)
	}

	// 2) 所有成员都收到新 voice.state：transport=relay、reason=direct_failed_fallback。
	st := waitVoiceState(t, bobWS, func(s protocol.VoiceStatePayload) bool {
		return len(s.Members) == 2 && allTransport(s, "relay", "direct_failed_fallback")
	})
	if st.VoiceMode != "direct" {
		t.Fatalf("voice_mode = %q, want direct（策略字段不变，只有事实变了）", st.VoiceMode)
	}

	// 3) 幂等：另一个成员再上报 → 不重复降级（already_relay）。
	resp2 := bobWS.request(protocol.TypeVoiceTransportFailed, protocol.VoiceTransportFailedRequest{
		ChannelID: ch.ID, Media: "voice", Reason: "ice_timeout",
	})
	var ack2 protocol.VoiceTransportFailedOK
	if err := resp2.DecodePayload(&ack2); err != nil {
		t.Fatalf("decode second ack: %v", err)
	}
	if ack2.Degraded || ack2.Reason != "already_relay" {
		t.Fatalf("second ack = %+v, want degraded=false reason=already_relay", ack2)
	}

	// 4) 防刷：同一成员在冷却窗口内重复上报 → rate_limited。
	resp3 := aliceWS.request(protocol.TypeVoiceTransportFailed, protocol.VoiceTransportFailedRequest{
		ChannelID: ch.ID, Media: "voice", Reason: "ice_failed",
	})
	var ack3 protocol.VoiceTransportFailedOK
	if err := resp3.DecodePayload(&ack3); err != nil {
		t.Fatalf("decode third ack: %v", err)
	}
	if ack3.Degraded || ack3.Reason != "rate_limited" {
		t.Fatalf("third ack = %+v, want degraded=false reason=rate_limited", ack3)
	}

	// 5) 重建：alice 重新 join，服务端必须按 relay 回 acks 并主动下发 SFU offer。
	aliceWS.request(protocol.TypeVoiceLeave, protocol.VoiceLeaveRequest{ChannelID: ch.ID})
	observeRelayJoin(t, aliceWS, ch.ID)
}

// allTransport 断言全体成员的 per-member transport/reason 一致。
func allTransport(s protocol.VoiceStatePayload, transport, reason string) bool {
	for _, m := range s.Members {
		if m.Transport != transport || m.TransportReason != reason {
			return false
		}
	}
	return true
}

// observeRelayJoin 发 voice.join 并同时等 join.ok 与 SFU 的 voice offer
// （offer 可能在 .ok 之前到达，所以不能用 wsClient.request——它会吞掉推送）。
func observeRelayJoin(t *testing.T, c *wsClient, channelID string) {
	t.Helper()
	env, err := protocol.New(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: channelID})
	if err != nil {
		t.Fatalf("build voice.join: %v", err)
	}
	c.sendRaw(env)

	deadline := time.Now().Add(10 * time.Second)
	sawOK, sawOffer := false, false
	for !(sawOK && sawOffer) {
		msg, err := c.recv(deadline)
		if err != nil {
			t.Fatalf("waiting relay join (ok=%v offer=%v): %v", sawOK, sawOffer, err)
		}
		switch {
		case msg.Type == protocol.TypeOK(protocol.TypeVoiceJoin):
			var ok protocol.VoiceJoinOK
			if err := msg.DecodePayload(&ok); err != nil {
				t.Fatalf("decode voice.join.ok: %v", err)
			}
			if ok.Transport != "relay" || ok.Role != "answerer" {
				t.Fatalf("voice.join.ok = %+v, want transport=relay role=answerer", ok)
			}
			sawOK = true
		case msg.Type == protocol.TypeMediaSignal:
			var push protocol.MediaSignalPush
			if err := msg.DecodePayload(&push); err != nil {
				continue
			}
			if push.Media == "voice" && push.Kind == "offer" && push.Target == "sfu" {
				sawOffer = true
			}
		}
	}
}
