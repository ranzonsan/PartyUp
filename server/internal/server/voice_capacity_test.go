package server_test

import (
	"strings"
	"testing"
	"time"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
)

// joinVoiceExpectConflict 发一条 voice.join 并断言收到 .err，返回解码后的载荷。
// wsClient.request 遇到 .err 会直接 Fatal，所以"预期失败"的请求必须走原始收发。
func joinVoiceExpectConflict(t *testing.T, ws *wsClient, channelID string) protocol.ErrorPayload {
	t.Helper()
	req, err := protocol.New(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: channelID})
	if err != nil {
		t.Fatalf("build voice.join: %v", err)
	}
	ws.sendRaw(req)
	deadline := time.Now().Add(10 * time.Second)
	for {
		msg, err := ws.recv(deadline)
		if err != nil {
			t.Fatalf("waiting voice.join response: %v", err)
		}
		if msg.ID != req.ID {
			continue
		}
		if msg.Type != protocol.TypeError(protocol.TypeVoiceJoin) {
			t.Fatalf("voice.join = %s %s, want voice.join.err", msg.Type, msg.Payload)
		}
		var ep protocol.ErrorPayload
		if err := msg.DecodePayload(&ep); err != nil {
			t.Fatalf("decode error payload: %v", err)
		}
		return ep
	}
}

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
	ep := joinVoiceExpectConflict(t, extra, ch.ID)
	if ep.Code != protocol.CodeConflict {
		t.Fatalf("error code = %q, want %q", ep.Code, protocol.CodeConflict)
	}
	if !strings.Contains(ep.Message, "channel voice is full") {
		t.Fatalf("error message = %q, want it to mention 'channel voice is full'", ep.Message)
	}

	// 语音被拒 ≠ 频道不能进：文字必须照常可用。
	if resp := extra.request(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID: ch.ID, Body: "我还是能打字", ClientMsgID: "overflow-1",
	}); resp.Type != protocol.TypeOK(protocol.TypeTextSend) {
		t.Fatalf("text.send after voice full = %s %s, want ok（文字不受语音容量影响）", resp.Type, resp.Payload)
	}
}

// TestVoiceDirectCapacityGate 是 direct（P2P）语音的同类护栏：
// P2P mesh 只实现 1v1，第 3 个人进来「能进但互相听不到」，与 relay 第 13 人是同一类
// 静默降级。所以 direct 下第 3 个成员必须在 voice.join 就被明确拒绝，并且错误信息
// 要告诉用户**出路**（改 --voice-mode=relay），否则用户只会在客户端里干着急。
func TestVoiceDirectCapacityGate(t *testing.T) {
	env := newTestEnvWithModes(t, "direct", "direct")
	_, adminToken := env.bootstrapAdmin("admin", "secret123")
	admin := dialWS(t, env.http.URL, adminToken)
	ch := admin.createChannel("direct-cap")
	admin.subscribe(ch.ID)

	_, secondToken := env.registerViaInvite(env.newInvite(), "second", "secret123")
	second := dialWS(t, env.http.URL, secondToken)
	second.subscribe(ch.ID)

	for i, ws := range []*wsClient{admin, second} {
		if resp := ws.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID}); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
			t.Fatalf("direct member #%d voice.join = %s %s, want ok（1v1 的两人都必须成功）", i+1, resp.Type, resp.Payload)
		}
	}
	waitVoiceState(t, admin, func(s protocol.VoiceStatePayload) bool {
		return len(s.Members) == 2 // DirectMaxMembers
	})

	// 第 3 个人：必须被拒绝，且错误信息要给出路。
	_, thirdToken := env.registerViaInvite(env.newInvite(), "third", "secret123")
	third := dialWS(t, env.http.URL, thirdToken)
	third.subscribe(ch.ID)
	ep := joinVoiceExpectConflict(t, third, ch.ID)
	if ep.Code != protocol.CodeConflict {
		t.Fatalf("error code = %q, want %q", ep.Code, protocol.CodeConflict)
	}
	if !strings.Contains(ep.Message, "直连模式") || !strings.Contains(ep.Message, "--voice-mode=relay") {
		t.Fatalf("error message = %q, want it to explain the way out (--voice-mode=relay)", ep.Message)
	}

	// 语音被拒 ≠ 频道不能进：文字照常。
	if resp := third.request(protocol.TypeTextSend, protocol.TextSendRequest{
		ChannelID: ch.ID, Body: "直连满了我也能打字", ClientMsgID: "direct-overflow-1",
	}); resp.Type != protocol.TypeOK(protocol.TypeTextSend) {
		t.Fatalf("text.send after direct voice full = %s %s, want ok", resp.Type, resp.Payload)
	}
}
