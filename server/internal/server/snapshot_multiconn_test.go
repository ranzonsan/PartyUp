package server_test

import (
	"encoding/json"
	"testing"
	"time"

	"partyup/internal/protocol"
)

// waitForRoster 轮询 REST 名册直到谓词满足（断线清理是异步的，不能立即读一次就断言）。
func (e *testEnv) waitForRoster(token string, pred func([]protocol.MemberRosterEntry) bool) []protocol.MemberRosterEntry {
	e.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		status, body := e.getRoster(token)
		if status == 200 {
			var roster protocol.MemberListOK
			if err := json.Unmarshal(body, &roster); err != nil {
				e.t.Fatalf("decode roster while polling: %v", err)
			}
			if pred(roster.Members) {
				return roster.Members
			}
		}
		if time.Now().After(deadline) {
			e.t.Fatalf("roster never satisfied predicate (last status=%d body=%s)", status, body)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func (c *wsClient) waitForScreenState(pred func(protocol.ScreenStatePayload) bool) protocol.ScreenStatePayload {
	c.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting for screen.state: %v", err)
		}
		if env.Type != protocol.TypeScreenState {
			continue
		}
		var p protocol.ScreenStatePayload
		if err := env.DecodePayload(&p); err != nil {
			c.t.Fatalf("decode screen.state: %v", err)
		}
		if pred(p) {
			return p
		}
	}
}

// TestLateJoinSeesExistingVoiceAndScreen 锁定晚加入一致性：
// 甲已在语音并共享 → 乙之后才订阅；乙不需要甲再做任何操作，就能从
// subscribe 后的初始 voice.state / screen.state 快照看到真实状态。
func TestLateJoinSeesExistingVoiceAndScreen(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ch := aliceWS.createChannel("late-join")
	aliceWS.subscribe(ch.ID)
	aliceWS.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})
	aliceWS.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Source:    protocol.ScreenSource{ID: "7", Title: "DISPLAY1", Width: 1920, Height: 1080, Kind: "screen"},
	})

	// 乙此时才连接并订阅；甲之后不再做任何操作。
	bobWS := dialWS(t, env.http.URL, bobToken)
	bobWS.subscribe(ch.ID)

	voiceEnv := bobWS.waitFor(protocol.TypeVoiceState)
	var voice protocol.VoiceStatePayload
	if err := voiceEnv.DecodePayload(&voice); err != nil {
		t.Fatalf("decode late-join voice.state: %v", err)
	}
	foundAlice := false
	for _, m := range voice.Members {
		if m.MemberID == alice.ID {
			foundAlice = true
		}
	}
	if !foundAlice {
		t.Fatalf("late-join voice.state members = %+v, want alice", voice.Members)
	}

	state := bobWS.waitForScreenState(func(p protocol.ScreenStatePayload) bool {
		return p.ChannelID == ch.ID && len(p.Shares) == 1
	})
	if state.Shares[0].SharerID != alice.ID || state.Shares[0].MemberName != "alice" {
		t.Fatalf("late-join share = %+v, want alice", state.Shares[0])
	}
	if state.Mode != "relay" || state.Shares[0].Mode != "relay" {
		t.Fatalf("late-join screen modes = %q/%q, want relay/relay", state.Mode, state.Shares[0].Mode)
	}
}

// TestMultiConnDisconnectKeepsOtherVoiceAndShare 锁定多 WebSocket 清理范围：
// 同一成员两条连接分别进入 chA / chB 语音并各自共享；断开 chA 那条连接后，
// 只能清 chA 的语音/共享，不能误清 chB（另一条连接）的状态；成员仍在线。
func TestMultiConnDisconnectKeepsOtherVoiceAndShare(t *testing.T) {
	// 屏幕 direct 避免视频 PC 信令噪声；语音 relay 允许同一成员在两个频道。
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	alice1 := dialWS(t, env.http.URL, aliceToken)
	alice2 := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWS(t, env.http.URL, bobToken)

	chA := alice1.createChannel("multi-a")
	chB := alice1.createChannel("multi-b")
	alice1.subscribe(chA.ID)
	alice2.subscribe(chB.ID)
	bobWS.subscribe(chA.ID)
	bobWS.subscribe(chB.ID)

	// 两条连接分别进不同频道语音。
	alice1.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: chA.ID})
	alice2.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: chB.ID})

	// 两条连接分别共享不同频道。
	alice1.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: chA.ID,
		Source:    protocol.ScreenSource{ID: "1", Title: "A", Width: 1280, Height: 720, Kind: "screen"},
	})
	alice2.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: chB.ID,
		Source:    protocol.ScreenSource{ID: "2", Title: "B", Width: 1280, Height: 720, Kind: "screen"},
	})
	bobWS.waitForScreenState(func(p protocol.ScreenStatePayload) bool {
		return p.ChannelID == chB.ID && len(p.Shares) == 1
	})

	// 断开 alice 的连接 1（持有 chA）。bob 仍订阅两频道：
	// chA 必须收到空 screen.state（该频道的共享随 owner 清理）。
	_ = alice1.conn.Close()
	bobWS.waitForScreenState(func(p protocol.ScreenStatePayload) bool {
		return p.ChannelID == chA.ID && len(p.Shares) == 0
	})

	// chB 的共享仍属于连接 2：新成员 carol 晚加入 chB，必须从初始快照看到它。
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")
	carolWS := dialWS(t, env.http.URL, carolToken)
	carolWS.subscribe(chB.ID)
	state := carolWS.waitForScreenState(func(p protocol.ScreenStatePayload) bool {
		return p.ChannelID == chB.ID && len(p.Shares) == 1
	})
	if state.Shares[0].SharerID != alice.ID {
		t.Fatalf("chB share after conn1 close = %+v, want alice", state.Shares[0])
	}

	// 名册必须仍显示 alice 在线，且只在 chB 语音。
	status, body := env.getRoster(aliceToken)
	if status != 200 {
		t.Fatalf("GET /api/members after conn1 close: status=%d body=%s", status, body)
	}
	var roster protocol.MemberListOK
	if err := json.Unmarshal(body, &roster); err != nil {
		t.Fatalf("decode roster: %v", err)
	}
	entry, ok := rosterByID(roster.Members, alice.ID)
	if !ok || !entry.Online {
		t.Fatalf("alice roster after conn1 close = %+v, want online", entry)
	}
	if len(entry.VoiceChannelIDs) != 1 || entry.VoiceChannelIDs[0] != chB.ID {
		t.Fatalf("alice voice_channel_ids = %v, want [%s]", entry.VoiceChannelIDs, chB.ID)
	}

	// 连接 2 仍可正常停止自己的共享：说明 chB 的共享所有权仍在它手里。
	alice2.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: chB.ID})
	bobWS.waitForScreenState(func(p protocol.ScreenStatePayload) bool {
		return p.ChannelID == chB.ID && len(p.Shares) == 0
	})
}

// TestMultiConnUnsubscribeKeepsOtherVoice 锁定退订也按连接释放：
// 同一成员两条连接订阅同一频道，连接 2 在语音；连接 1 退订不能把连接 2 的语音清掉。
func TestMultiConnUnsubscribeKeepsOtherVoice(t *testing.T) {
	env := newTestEnvWith(t, "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	alice1 := dialWS(t, env.http.URL, aliceToken)
	alice2 := dialWS(t, env.http.URL, aliceToken)

	ch := alice1.createChannel("multi-unsub")
	alice1.subscribe(ch.ID)
	alice2.subscribe(ch.ID)
	alice2.request(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: ch.ID})

	// 连接 1 退订；连接 2 仍订阅且在语音。
	alice1.request(protocol.TypeChannelUnsub, protocol.ChannelSubscribeRequest{ChannelID: ch.ID})

	entry, _ := rosterByID(env.waitForRoster(aliceToken, func(members []protocol.MemberRosterEntry) bool {
		e, ok := rosterByID(members, alice.ID)
		return ok && e.Online && len(e.VoiceChannelIDs) == 1 && e.VoiceChannelIDs[0] == ch.ID
	}), alice.ID)
	if len(entry.VoiceChannelIDs) != 1 || entry.VoiceChannelIDs[0] != ch.ID {
		t.Fatalf("alice after conn1 unsubscribe = %+v, want online in voice %s", entry, ch.ID)
	}

	// 连接 1 断开也不影响；连接 2 断开后才离线。
	_ = alice1.conn.Close()
	entry, _ = rosterByID(env.waitForRoster(aliceToken, func(members []protocol.MemberRosterEntry) bool {
		e, ok := rosterByID(members, alice.ID)
		return ok && e.Online && len(e.VoiceChannelIDs) == 1
	}), alice.ID)
	if len(entry.VoiceChannelIDs) != 1 {
		t.Fatalf("alice after conn1 close = %+v, want still online in voice", entry)
	}

	_ = alice2.conn.Close()
	entry, _ = rosterByID(env.waitForRoster(aliceToken, func(members []protocol.MemberRosterEntry) bool {
		e, ok := rosterByID(members, alice.ID)
		return ok && !e.Online && len(e.VoiceChannelIDs) == 0
	}), alice.ID)
	if entry.Online {
		t.Fatalf("alice after all conns closed = %+v, want offline", entry)
	}
}
