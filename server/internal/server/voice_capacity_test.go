package server_test

import (
	"strings"
	"testing"
	"time"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
)

// TestVoiceChannelCapacityGate 是 2026-11-04 容量测试的永久护栏：
//
// 第 13 个成员 join 中继语音必须收到 CONFLICT，**绝不能悄悄成功**。
// 背景：槽位是「发布者→槽位」的全局唯一映射，只有 sfu.MaxRemoteSlots 个；
// 槽位用完时旧代码只在服务端打一条 WARN，然后第 13 个人能加入、能听见别人，
// 但没人能听见他——用户只会觉得「怎么没人理我」。这类静默降级比崩溃更糟。
func TestVoiceChannelCapacityGate(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	_, adminToken := env.bootstrapAdmin("admin", "secret123")
	admin := dialWS(t, env.http.URL, adminToken)
	ch := admin.createChannel("capacity")
	admin.subscribe(ch.ID)

	// admin + 11 个邀请成员 = 满员 12（= sfu.MaxRemoteSlots）。
	members := []*wsClient{admin}
	for i := 1; i < sfu.MaxRemoteSlots; i++ {
		_, token := env.registerViaInvite(env.newInvite(), "cap"+string(rune('a'+i)), "secret123")
		ws := dialWS(t, env.http.URL, token)
		ws.subscribe(ch.ID)
		members = append(members, ws)
	}
	for i, ws := range members {
		if resp := ws.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID}); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
			t.Fatalf("member #%d voice.join = %s %s, want ok（满员之前都必须成功）", i+1, resp.Type, resp.Payload)
		}
	}
	// 等服务端真的把 12 个成员都记上（join 是同步的，但保险起见看一下 voice.state）。
	waitVoiceState(t, admin, func(s protocol.VoiceStatePayload) bool {
		return len(s.Members) == sfu.MaxRemoteSlots
	})

	// 第 13 个人：必须被明确拒绝。
	_, extraToken := env.registerViaInvite(env.newInvite(), "overflow", "secret123")
	extra := dialWS(t, env.http.URL, extraToken)
	extra.subscribe(ch.ID)
	req, err := protocol.New(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	if err != nil {
		t.Fatalf("build voice.join: %v", err)
	}
	extra.sendRaw(req)
	deadline := time.Now().Add(10 * time.Second)
	for {
		msg, err := extra.recv(deadline)
		if err != nil {
			t.Fatalf("waiting voice.join response: %v", err)
		}
		if msg.ID != req.ID {
			continue
		}
		if msg.Type != protocol.TypeError(protocol.TypeVoiceJoin) {
			t.Fatalf("13th voice.join = %s %s, want voice.join.err CONFLICT", msg.Type, msg.Payload)
		}
		var ep protocol.ErrorPayload
		if err := msg.DecodePayload(&ep); err != nil {
			t.Fatalf("decode error payload: %v", err)
		}
		if ep.Code != protocol.CodeConflict {
			t.Fatalf("error code = %q, want %q", ep.Code, protocol.CodeConflict)
		}
		if !strings.Contains(ep.Message, "channel voice is full") {
			t.Fatalf("error message = %q, want it to mention 'channel voice is full'", ep.Message)
		}
		break
	}

	// 语音被拒 ≠ 频道不能进：文字必须照常可用。
	if resp := extra.request(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID: ch.ID, Body: "我还是能打字", ClientMsgID: "overflow-1",
	}); resp.Type != protocol.TypeOK(protocol.TypeTextSend) {
		t.Fatalf("text.send after voice full = %s %s, want ok（文字不受语音容量影响）", resp.Type, resp.Payload)
	}
}
