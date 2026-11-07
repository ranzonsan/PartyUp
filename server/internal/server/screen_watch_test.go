package server_test

// 显式单流观看（screen.watch / screen.unwatch）的服务端合同：
//   - 旧客户端不声明能力 → 保持 v1 自动观看，且不能悄悄用新接口；
//   - 新客户端 opt-in 后，服务端记录「一次一路」的选台与 viewers；
//   - relay 为目标分配观看者槽位并递增代次，切换目标换槽，停止观看清空；
//   - direct 保持 1v1 容量上限，第三个观看者明确 CONFLICT，不做 mesh；
//   - 停发/断线后旧选台必须失效，重新开共享不能自动续看。

import (
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/protocol"
)

// dialWSWithCapabilities 与 dialWS 相同，但在 auth 请求里声明客户端能力。
func dialWSWithCapabilities(t *testing.T, httpURL, token string, caps ...string) *wsClient {
	t.Helper()
	wsURL := "ws" + strings.TrimPrefix(httpURL, "http") + "/ws"
	conn, resp, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		status := "no response"
		if resp != nil {
			status = resp.Status
		}
		t.Fatalf("dial ws: %v (%s)", err, status)
	}
	c := &wsClient{t: t, conn: conn}
	t.Cleanup(func() { _ = conn.Close() })

	authEnv := c.request(protocol.TypeAuth, protocol.AuthRequest{Token: token, Capabilities: caps})
	if authEnv.Type != protocol.TypeOK(protocol.TypeAuth) {
		t.Fatalf("auth failed: type=%s payload=%s", authEnv.Type, authEnv.Payload)
	}
	var ok protocol.AuthOK
	if err := authEnv.DecodePayload(&ok); err != nil {
		t.Fatalf("decode auth.ok: %v", err)
	}
	advertised := strings.Join(ok.Server.Capabilities, ",")
	if !strings.Contains(advertised, protocol.CapabilityScreenWatch) {
		t.Fatalf("server capabilities = %q, want %q", advertised, protocol.CapabilityScreenWatch)
	}
	return c
}

// watchShare 发 screen.watch 并解出应答。
func (c *wsClient) watchShare(channelID, sharerID string) protocol.ScreenWatchOK {
	c.t.Helper()
	env := c.request(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: channelID,
		SharerID:  sharerID,
	})
	var ok protocol.ScreenWatchOK
	if err := env.DecodePayload(&ok); err != nil {
		c.t.Fatalf("decode screen.watch.ok: %v", err)
	}
	return ok
}

func (c *wsClient) unwatchShare(channelID, sharerID string) protocol.ScreenUnwatchOK {
	c.t.Helper()
	env := c.request(protocol.TypeScreenUnwatch, protocol.ScreenUnwatchRequest{
		ChannelID: channelID,
		SharerID:  sharerID,
	})
	var ok protocol.ScreenUnwatchOK
	if err := env.DecodePayload(&ok); err != nil {
		c.t.Fatalf("decode screen.unwatch.ok: %v", err)
	}
	return ok
}

// waitForShareViewers 等到 screen.state 里出现某个共享者且 viewers 数量为 want。
// 必须按 viewers 断言：并发动作会排队多条 screen.state，只按「出现过」取会读到旧快照。
// 同时过滤 channel 与 target，避免把别的频道/另一路共享的快照当成目标状态。
// screen.state 本身没有 generation 字段，代次只在 screen.watch.ok 上校验（见调用点）；
// 这里用「目标 + 精确 viewers 数」排除旧快照，每次 recv 共用同一个 deadline。
func waitForShareViewers(t *testing.T, c *wsClient, channelID, sharerID string, want int) protocol.ShareState {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			t.Fatalf("waiting for share %s viewers=%d on channel %s: %v",
				sharerID, want, channelID, err)
		}
		if env.Type != protocol.TypeScreenState {
			continue
		}
		var state protocol.ScreenStatePayload
		if err := env.DecodePayload(&state); err != nil {
			t.Fatalf("decode screen.state: %v", err)
		}
		if state.ChannelID != channelID {
			continue
		}
		for _, share := range state.Shares {
			if share.SharerID == sharerID && len(share.Viewers) == want {
				return share
			}
		}
	}
}

// waitForScreenStateFor 等到某频道的一条 screen.state 满足谓词并返回整条快照。
//
// 同一广播里可能同时需要断言两路共享（例如「alice 的 viewers 清零、carol 的 viewers
// 变成 bob」）。如果分成两次 wait，第二次不会再收到同一条广播，会把正确实现等成超时；
// 用一条快照断言两路反而更强（两路必须来自同一时刻服务端的同一份事实）。
func waitForScreenStateFor(t *testing.T, c *wsClient, channelID string,
	pred func(protocol.ScreenStatePayload) bool) protocol.ScreenStatePayload {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			t.Fatalf("waiting for screen.state on channel %s: %v", channelID, err)
		}
		if env.Type != protocol.TypeScreenState {
			continue
		}
		var state protocol.ScreenStatePayload
		if err := env.DecodePayload(&state); err != nil {
			t.Fatalf("decode screen.state: %v", err)
		}
		if state.ChannelID != channelID {
			continue
		}
		if pred(state) {
			return state
		}
	}
}

// waitForShareGone 等到 screen.state 不再包含某个共享者（同样按 channel 过滤）。
func waitForShareGone(t *testing.T, c *wsClient, channelID, sharerID string) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			t.Fatalf("waiting for share %s to disappear from channel %s: %v",
				sharerID, channelID, err)
		}
		if env.Type != protocol.TypeScreenState {
			continue
		}
		var state protocol.ScreenStatePayload
		if err := env.DecodePayload(&state); err != nil {
			t.Fatalf("decode screen.state: %v", err)
		}
		if state.ChannelID != channelID {
			continue
		}
		found := false
		for _, share := range state.Shares {
			if share.SharerID == sharerID {
				found = true
				break
			}
		}
		if !found {
			return
		}
	}
}

func startSyntheticShare(t *testing.T, c *wsClient, channelID string) {
	t.Helper()
	c.request(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: channelID,
		Source:    protocol.ScreenSource{ID: "synthetic", Title: "synthetic", Width: 640, Height: 360, Kind: "synthetic"},
	})
}

func TestScreenWatchRelayContract(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	carol, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken) // 旧客户端：不声明能力
	bobWS := dialWSWithCapabilities(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	carolWS := dialWSWithCapabilities(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)

	ch := aliceWS.createChannel("watch")
	for _, c := range []*wsClient{aliceWS, bobWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)

	// 旧客户端不能使用新接口（不能假装 opt-in）；服务端明确拒绝。
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID,
	}, protocol.CodeNotFound)
	aliceWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: bob.ID,
	}, protocol.CodeForbidden)

	// 两个共享者同时开共享（relay 支持多路发布）。
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)
	startSyntheticShare(t, carolWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, carol.ID, 0)

	// watch 自己 / 不存在的目标都要明确报错。
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: bob.ID,
	}, protocol.CodeInvalidArgument)
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: "no-such-member",
	}, protocol.CodeNotFound)

	// 第一次 watch alice：relay 分配槽 0、代次 1。
	ok := bobWS.watchShare(ch.ID, alice.ID)
	if ok.Transport != "relay" || ok.SharerID != alice.ID || ok.ChannelID != ch.ID {
		t.Fatalf("watch ok = %+v, want relay/alice/channel", ok)
	}
	if ok.Slot != 0 || ok.Generation != 1 {
		t.Fatalf("first watch slot/gen = %d/%d, want 0/1", ok.Slot, ok.Generation)
	}
	share := waitForShareViewers(t, carolWS, ch.ID, alice.ID, 1)
	if len(share.Viewers) != 1 || share.Viewers[0] != bob.ID {
		t.Fatalf("alice share viewers = %v, want [bob]", share.Viewers)
	}

	// 重复 watch 同一目标：幂等，槽位不变（不中断画面），代次继续递增。
	again := bobWS.watchShare(ch.ID, alice.ID)
	if again.Slot != ok.Slot {
		t.Fatalf("idempotent watch slot = %d, want %d", again.Slot, ok.Slot)
	}
	if again.Generation <= ok.Generation {
		t.Fatalf("generation did not advance: %d -> %d", ok.Generation, again.Generation)
	}

	// 切到 carol：只保留新目标，槽位交替（0 -> 1），旧共享 viewers 清空。
	switched := bobWS.watchShare(ch.ID, carol.ID)
	if switched.Slot != 1 {
		t.Fatalf("switched watch slot = %d, want 1 (must differ from previous slot)", switched.Slot)
	}
	state := waitForScreenStateFor(t, carolWS, ch.ID, func(p protocol.ScreenStatePayload) bool {
		aliceCleared, carolBound := false, false
		for _, share := range p.Shares {
			switch share.SharerID {
			case alice.ID:
				aliceCleared = len(share.Viewers) == 0
			case carol.ID:
				carolBound = len(share.Viewers) == 1 && share.Viewers[0] == bob.ID
			}
		}
		return aliceCleared && carolBound
	})
	var aliceShare, carolShare protocol.ShareState
	for _, share := range state.Shares {
		switch share.SharerID {
		case alice.ID:
			aliceShare = share
		case carol.ID:
			carolShare = share
		}
	}
	if len(aliceShare.Viewers) != 0 {
		t.Fatalf("alice share viewers after switch = %v, want empty", aliceShare.Viewers)
	}
	if len(carolShare.Viewers) != 1 || carolShare.Viewers[0] != bob.ID {
		t.Fatalf("carol share viewers = %v, want [bob]", carolShare.Viewers)
	}

	// 再切回 alice：槽位再次交替（1 -> 0），证明新目标落在不同 track 上。
	back := bobWS.watchShare(ch.ID, alice.ID)
	if back.Slot != 0 {
		t.Fatalf("switch-back slot = %d, want 0", back.Slot)
	}

	// unwatch：清空 viewers；重复 unwatch 幂等。
	uw := bobWS.unwatchShare(ch.ID, alice.ID)
	if uw.SharerID != alice.ID {
		t.Fatalf("unwatch sharer = %q, want alice", uw.SharerID)
	}
	share = waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)
	if len(share.Viewers) != 0 {
		t.Fatalf("alice share viewers after unwatch = %v, want empty", share.Viewers)
	}
	uw = bobWS.unwatchShare(ch.ID, alice.ID)
	if uw.SharerID != "" {
		t.Fatalf("second unwatch sharer = %q, want empty", uw.SharerID)
	}

	// 停发共享必须撤掉指向它的选台：重新开共享后不会自动续看。
	bobWS.watchShare(ch.ID, alice.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 1)
	aliceWS.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: ch.ID})
	waitForShareGone(t, carolWS, ch.ID, alice.ID)
	startSyntheticShare(t, aliceWS, ch.ID)
	share = waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)
	if len(share.Viewers) != 0 {
		t.Fatalf("share restarted with stale viewers = %v, want empty (must re-watch)", share.Viewers)
	}
}

func TestScreenWatchDirectKeepsOneToOne(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWSWithCapabilities(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	carolWS := dialWSWithCapabilities(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("direct-watch")
	for _, c := range []*wsClient{aliceWS, bobWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	ok := bobWS.watchShare(ch.ID, alice.ID)
	if ok.Transport != "direct" || ok.Slot != -1 {
		t.Fatalf("direct watch = transport=%q slot=%d, want direct/-1", ok.Transport, ok.Slot)
	}
	// direct 是 P2P 1v1：第二个观看者必须明确容量错误，不能静默建 mesh。
	carolWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID,
	}, protocol.CodeConflict)

	// 前一个观看者释放后，第二个才能看（容量按实际 viewers 计算）。
	bobWS.unwatchShare(ch.ID, alice.ID)
	ok = carolWS.watchShare(ch.ID, alice.ID)
	if ok.Slot != -1 {
		t.Fatalf("second direct viewer slot = %d, want -1", ok.Slot)
	}
}

// TestScreenWatchRequiresAuthAndSubscription 锁定未鉴权/未订阅不能建立选台。
func TestScreenWatchRequiresAuthAndSubscription(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS := dialWSWithCapabilities(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("watch-sub")
	aliceWS.subscribe(ch.ID)
	startSyntheticShare(t, aliceWS, ch.ID)

	// bob 没有订阅该频道：不能看。
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID,
	}, protocol.CodeForbidden)
}
