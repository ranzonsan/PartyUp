package server_test

import (
	"testing"

	"partyup/internal/protocol"
)

// TestScreenModeIndependentOfVoiceMode 锁定「语音与屏幕共享是两套独立策略」：
// 语音 relay + 屏幕 direct 必须能同时成立，且 voice.state 用两个字段分别暴露。
func TestScreenModeIndependentOfVoiceMode(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	_, bobToken := env.registerViaInvite(code, "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)
	ch := aliceWS.createChannel("share")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	// 1) 屏幕共享在 screen_mode=direct 下必须可用，且 share.mode 是服务端策略值。
	aliceWS.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Source:    protocol.ScreenSource{ID: "42", Title: "DISPLAY1", Width: 2560, Height: 1440, Kind: "screen"},
	})
	stateEnv := bobWS.waitFor(protocol.TypeScreenState)
	var state protocol.ScreenStatePayload
	if err := stateEnv.DecodePayload(&state); err != nil {
		t.Fatalf("decode screen.state: %v", err)
	}
	if state.Mode != "direct" {
		t.Fatalf("screen.state.mode = %q, want direct", state.Mode)
	}
	if len(state.Shares) != 1 || state.Shares[0].Mode != "direct" {
		t.Fatalf("screen.state shares = %+v, want 1 share with mode=direct", state.Shares)
	}

	// 2) 同一频道里语音仍走 relay：voice.state 必须同时给出两个 mode。
	aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	st := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 2 })
	if st.VoiceMode != "relay" || st.ScreenMode != "direct" {
		t.Fatalf("voice.state = voice_mode=%q screen_mode=%q, want relay/direct", st.VoiceMode, st.ScreenMode)
	}
}

// TestScreenModeRelayShareAccepted 锁定 screen_mode=relay 的新契约（2026-11-03 起）：
// 屏幕共享走服务端 SFU 视频中继，screen.start 必须被接受，且不能影响语音成员状态。
// 具体视频转发行为由 video_relay_test.go 覆盖。
func TestScreenModeRelayShareAccepted(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	_, bobToken := env.registerViaInvite(code, "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)
	ch := aliceWS.createChannel("share")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	// 语音先正常工作。
	aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	bobWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 2 })

	// 1) screen.start 现在必须被接受（relay = 走服务端 SFU 视频中继）。
	aliceWS.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Mode:      "direct", // 客户端说 direct 也不能改变服务端策略
		Source:    protocol.ScreenSource{ID: "42", Title: "DISPLAY1", Width: 1280, Height: 720, Kind: "screen"},
	})

	// 2) screen.state 回显的是服务端策略 relay，不是客户端说的 direct。
	stateEnv := bobWS.waitFor(protocol.TypeScreenState)
	var state protocol.ScreenStatePayload
	if err := stateEnv.DecodePayload(&state); err != nil {
		t.Fatalf("decode screen.state: %v", err)
	}
	if state.Mode != "relay" || len(state.Shares) != 1 || state.Shares[0].Mode != "relay" {
		t.Fatalf("screen.state = mode=%q shares=%+v, want 1 share with mode=relay", state.Mode, state.Shares)
	}

	// 3) 语音成员状态没有被污染：仍两人在语音，且 screen_mode=relay。
	aliceWS.request(protocol.TypeVoiceSetMute, protocol.VoiceSetMuteRequest{ChannelID: ch.ID, Muted: true})
	st := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return len(s.Members) == 2 })
	if st.ScreenMode != "relay" {
		t.Fatalf("voice.state.screen_mode = %q, want relay", st.ScreenMode)
	}
	if st.VoiceMode != "relay" {
		t.Fatalf("voice.state.voice_mode = %q, want relay", st.VoiceMode)
	}
}

// TestMediaSignalRoutesByMediaField 锁定路由只认 media 字段，不靠 target 猜：
// voice + voice_mode=direct 不允许 target=sfu；screen 在 direct 下按成员投递并透传 media。
func TestMediaSignalRoutesByMediaField(t *testing.T) {
	env := newTestEnvWithModes(t, "direct", "direct")
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)
	ch := aliceWS.createChannel("share")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)

	// voice_mode=direct 时 target=sfu 是自相矛盾：必须报错，避免静默路由到错误对象。
	aliceWS.expectError(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID,
		Media:     "voice",
		Kind:      "answer",
		Target:    "sfu",
		Payload:   `{"type":"answer","sdp":"v=0"}`,
	}, protocol.CodeInvalidArgument)

	// screen 在 direct 下按 to 定向投递，且下游信封必须带 media=screen。
	aliceWS.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID,
		Media:     "screen",
		Kind:      "offer",
		To:        bob.ID,
		Payload:   `{"type":"offer","sdp":"v=0"}`,
	})
	sigEnv := bobWS.waitFor(protocol.TypeMediaSignal)
	var sig protocol.MediaSignalPush
	if err := sigEnv.DecodePayload(&sig); err != nil {
		t.Fatalf("decode media.signal: %v", err)
	}
	if sig.Media != "screen" {
		t.Fatalf("media.signal.media = %q, want screen", sig.Media)
	}
	if sig.Target != "" {
		t.Fatalf("media.signal.target = %q, want empty (direct)", sig.Target)
	}

	// voice 在 direct 下同样按成员投递，media 必须是 voice。
	aliceWS.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: ch.ID,
		Media:     "voice",
		Kind:      "offer",
		Payload:   `{"type":"offer","sdp":"v=0 fake voice"}`,
	})
	voiceEnv := bobWS.waitFor(protocol.TypeMediaSignal)
	var voiceSig protocol.MediaSignalPush
	if err := voiceEnv.DecodePayload(&voiceSig); err != nil {
		t.Fatalf("decode voice media.signal: %v", err)
	}
	if voiceSig.Media != "voice" {
		t.Fatalf("media.signal.media = %q, want voice", voiceSig.Media)
	}
}

// waitVoiceState 阻塞读取 voice.state 直到满足谓词（每次 voice.join/mute 都会推一条）。
func waitVoiceState(t *testing.T, c *wsClient, pred func(protocol.VoiceStatePayload) bool) protocol.VoiceStatePayload {
	t.Helper()
	for i := 0; i < 20; i++ {
		env := c.waitFor(protocol.TypeVoiceState)
		var st protocol.VoiceStatePayload
		if err := env.DecodePayload(&st); err != nil {
			t.Fatalf("decode voice.state: %v", err)
		}
		if pred(st) {
			return st
		}
	}
	t.Fatalf("voice.state never satisfied predicate")
	return protocol.VoiceStatePayload{}
}

// TestDefaultScreenModeIsRelay 锁定默认值（2026-11-04 决策）：
// 不带任何 --screen-mode 启动时，屏幕共享默认走服务端 SFU 中继。
//
// 理由：direct 只能 1v1 观看，第三人看不到；默认配置必须让"多数人能看到"。
// 想省带宽的部署者显式传 --screen-mode=direct（行为仍由 TestScreenModeIndependentOfVoiceMode 等锁定）。
func TestDefaultScreenModeIsRelay(t *testing.T) {
	env := newTestEnv(t) // 不传 screen 策略 → 走服务端默认值
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ch := aliceWS.createChannel("default-screen")
	aliceWS.subscribe(ch.ID)

	st := waitVoiceState(t, aliceWS, func(s protocol.VoiceStatePayload) bool { return true })
	if st.ScreenMode != "relay" {
		t.Fatalf("voice.state.screen_mode = %q, want relay (默认值)", st.ScreenMode)
	}

	aliceWS.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Source:    protocol.ScreenSource{ID: "1", Title: "DISPLAY1", Width: 1920, Height: 1080, Kind: "screen"},
	})
	env2 := aliceWS.waitFor(protocol.TypeScreenState)
	var state protocol.ScreenStatePayload
	if err := env2.DecodePayload(&state); err != nil {
		t.Fatalf("decode screen.state: %v", err)
	}
	if state.Mode != "relay" {
		t.Fatalf("screen.state.mode = %q, want relay (默认值)", state.Mode)
	}
	if len(state.Shares) != 1 || state.Shares[0].Mode != "relay" {
		t.Fatalf("screen.state shares = %+v, want 1 share with mode=relay", state.Shares)
	}
}
