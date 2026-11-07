package server_test

// B2 严格接收（screen.watch.isolated.v1）的真实 HTTP/WS 回归。
//
// 这些测试只走公开协议面：真实 WebSocket 握手、真实 auth 能力声明、真实
// screen.watch/screen.unwatch/media.signal 信封。它们**不**直接读写 hub/SFU 的
// 内部 map——断言的是客户端能观察到的事实（offer 只到 owner 连接、身份不匹配
// 明确报错、并发只产生一个 owner、断线只清自己的 lease）。
//
// 这里不起真实 RTP：占位 SDP 只能区分「信令到达了接收 lease」（invalid_argument）
// 与「身份/会话不匹配」（conflict）。真实 UDP/DTLS/SRTP 的帧级隔离由 Pion 端到端
// 测试与 native loopback 回归覆盖。

import (
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"partyup/internal/protocol"
)

// dialWSCaps 与 dialWSWithCapabilities 相同，但把服务端 auth.ok 里的能力列表
// 一并返回：B2 的「服务端到底广告了什么」必须由客户端可见事实来断言。
func dialWSCaps(t *testing.T, httpURL, token string, caps ...string) (*wsClient, []string) {
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
	return c, ok.Server.Capabilities
}

func hasCap(caps []string, want string) bool {
	for _, cap := range caps {
		if cap == want {
			return true
		}
	}
	return false
}

// watchIsolated 发一条带 receive_id 的 screen.watch，同时收下服务端随后推送的
// 严格接收 offer（offer 先于 watch.ok 入队，不能先 request 再等，否则会被 request
// 当作无关推送跳过）。
func (c *wsClient) watchIsolated(channelID, sharerID, receiveID string) (protocol.ScreenWatchOK, protocol.MediaSignalPush) {
	c.t.Helper()
	req, err := protocol.New(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: channelID,
		SharerID:  sharerID,
		ReceiveID: receiveID,
	})
	if err != nil {
		c.t.Fatalf("build screen.watch: %v", err)
	}
	c.sendRaw(req)

	var ok protocol.ScreenWatchOK
	var offer protocol.MediaSignalPush
	gotOK, gotOffer := false, false
	deadline := time.Now().Add(10 * time.Second)
	for !gotOK || !gotOffer {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting for watch ack/offer session=%s: %v", receiveID, err)
		}
		if env.ID == req.ID {
			if strings.HasSuffix(env.Type, ".err") {
				var p protocol.ErrorPayload
				_ = env.DecodePayload(&p)
				c.t.Fatalf("screen.watch failed: code=%s message=%s", p.Code, p.Message)
			}
			if strings.HasSuffix(env.Type, ".ok") {
				if err := env.DecodePayload(&ok); err != nil {
					c.t.Fatalf("decode screen.watch.ok: %v", err)
				}
				gotOK = true
			}
			continue
		}
		if env.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := env.DecodePayload(&push); err != nil {
			continue
		}
		if push.Media == "screen" && push.Kind == "offer" && push.SessionID == receiveID {
			offer = push
			gotOffer = true
		}
	}
	return ok, offer
}

// expectNoScreenOffer 在窗口期内断言本连接收不到该 session 的 offer（推送属于
// 另一条连接是跨连接泄漏）。其它类型的推送（screen.state/presence）被跳过。
//
// **调用方必须使用专用的一次性连接**：gorilla/websocket 的读错误是永久的
// （NextReader/ReadMessage 一旦返回非 nil error，后续调用永远返回同一 error），
// 这里的读超时会毒化整条连接，不能在同一 socket 上继续发 RPC（真实失败根因）。
func expectNoScreenOffer(t *testing.T, c *wsClient, sessionID string, window time.Duration) {
	t.Helper()
	deadline := time.Now().Add(window)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			return // 读超时 = 窗口内没有该 offer；该连接到此作废
		}
		if env.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := env.DecodePayload(&push); err != nil {
			continue
		}
		if push.Media == "screen" && push.Kind == "offer" && push.SessionID == sessionID {
			t.Fatalf("receive offer for session %s leaked to a non-owner connection", sessionID)
		}
	}
}

// unwatchSession 按会话精确撤销严格接收 lease（B2）：只带 receive_id，不猜 target。
func (c *wsClient) unwatchSession(channelID, receiveID string) protocol.ScreenUnwatchOK {
	c.t.Helper()
	env := c.request(protocol.TypeScreenUnwatch, protocol.ScreenUnwatchRequest{
		ChannelID: channelID,
		ReceiveID: receiveID,
	})
	var out protocol.ScreenUnwatchOK
	if err := env.DecodePayload(&out); err != nil {
		c.t.Fatalf("decode screen.unwatch.ok: %v", err)
	}
	return out
}

// signalPayload 是一条结构合法、但 SDP 明显不是真实 answer 的信令：服务端只把它
// 交给接收会话（随后 SetRemoteDescription 报错），因此能区分「信令被当前 lease
// 接受」和「身份不匹配被拒绝」，不需要真实协商。
func signalPayload(channelID, kind, sessionID, nonce string) protocol.MediaSignalRequest {
	return protocol.MediaSignalRequest{
		ChannelID:    channelID,
		Target:       "sfu",
		Media:        "screen",
		Kind:         kind,
		SessionID:    sessionID,
		SessionNonce: nonce,
		Payload:      `{"type":"` + kind + `","sdp":"v=0\r\n"}`,
	}
}

// TestIsolatedCapabilityAndReceiveIDContract 锁定能力广告与 opt-in 门槛：
//   - relay 广告 screen.watch.isolated.v1，direct 不广告（B2b 未实现）；
//   - 声明 isolated 的连接不带 receive_id → 明确拒绝，不退回 sink-only；
//   - 没声明 isolated 的连接带 receive_id → NOT_IMPLEMENTED，不偷偷降级。
func TestIsolatedCapabilityAndReceiveIDContract(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	strictWS, caps := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	if !hasCap(caps, protocol.CapabilityScreenWatchIsolated) {
		t.Fatalf("relay server did not advertise %s: %v",
			protocol.CapabilityScreenWatchIsolated, caps)
	}
	legacyWS, _ := dialWSCaps(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-contract")
	for _, c := range []*wsClient{aliceWS, strictWS, legacyWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, strictWS, ch.ID, alice.ID, 0)

	// opt-in 了严格能力但不带会话身份：明确 invalid_argument。
	strictWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID,
	}, protocol.CodeInvalidArgument)

	// 未声明严格能力却带 receive_id：明确 not_implemented，不降级。
	legacyWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: "legacy-receive-id",
	}, protocol.CodeNotImplemented)
}

// TestIsolatedCapabilityAdvertisedInDirectMode 锁定 direct 模式也广告严格接收
// 能力：B2b 起服务端为 direct 做会话经纪（viewer 的 receive 传输在本地，
// 共享者的 per-viewer 发送传输是 B2b 的另一半）。详细会话合同见
// screen_direct_ws_test.go；这里只锁能力广告，避免旧测试继续断言「不支持」。
func TestIsolatedCapabilityAdvertisedInDirectMode(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	_ = dialWS(t, env.http.URL, aliceToken)
	_, caps := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	if !hasCap(caps, protocol.CapabilityScreenWatchIsolated) {
		t.Fatalf("direct screen mode did not advertise %s: %v",
			protocol.CapabilityScreenWatchIsolated, caps)
	}
}

// TestIsolatedOfferOnlyToOwnerConnectionAndIdentityBinding 锁定：
//   - offer 只投递给发起 watch 的连接（同一成员另一 socket 收不到）；
//   - answer/candidate 必须同时匹配 session + nonce + 真实连接归属；
//   - 跨成员、同成员另一 socket、旧会话、错 nonce 一律 CONFLICT；
//   - 正确归属的信令确实到达接收 lease（坏 SDP 只会得到 invalid_argument）。
func TestIsolatedOfferOnlyToOwnerConnectionAndIdentityBinding(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ownerWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	otherWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	// 负向观察必须用专用、一次性连接：400ms 读超时会永久毒化 gorilla 连接，
	// 若在 otherWS 上做，后续 expectError/RPC 会立刻拿到同一个 i/o timeout。
	// 它必须是同频道同成员，否则「offer 误广播到频道」的 bug 不会落在它身上。
	observerWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("isolated-owner")
	for _, c := range []*wsClient{aliceWS, ownerWS, otherWS, observerWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	const firstReceive = "receive-owner-0001"
	ack, offer := ownerWS.watchIsolated(ch.ID, alice.ID, firstReceive)
	if !ack.Isolated || ack.SessionID != firstReceive || ack.Nonce == "" ||
		ack.Slot != -1 || ack.Transport != "relay" {
		t.Fatalf("strict watch ack = %+v, want isolated session=%s slot=-1 relay", ack, firstReceive)
	}
	if offer.SessionNonce != ack.Nonce {
		t.Fatalf("offer nonce = %q, want %q", offer.SessionNonce, ack.Nonce)
	}
	// 同一成员的另一条连接绝不能收到这条 offer。observer 到此一次性用完并关闭：
	// 它已经被读超时毒化，不再承载任何 RPC（otherWS 保持干净）。
	expectNoScreenOffer(t, observerWS, firstReceive, 400*time.Millisecond)
	_ = observerWS.conn.Close()

	// 同一成员另一 socket 不能顶掉/停止当前 lease。
	otherWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: "receive-other-0002",
	}, protocol.CodeConflict)
	uw := otherWS.unwatchShare(ch.ID, alice.ID)
	if uw.SharerID != "" {
		t.Fatalf("non-owner unwatch reported clearing %q, want empty (must not clear the owner lease)", uw.SharerID)
	}
	// 非 owner 的 unwatch 之后，owner 的 lease 仍然有效：正确归属的信令到达会话
	// （坏 SDP → invalid_argument），而不是身份冲突。
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "answer", firstReceive, ack.Nonce), protocol.CodeInvalidArgument)

	// 错误 nonce：CONFLICT（身份不匹配，绝不落到会话）。
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "answer", firstReceive, ack.Nonce+"-wrong"), protocol.CodeConflict)

	// 跨成员使用别人的 session/nonce：CONFLICT；同成员另一 socket 冒充 owner：CONFLICT。
	foreign := signalPayload(ch.ID, "candidate", firstReceive, ack.Nonce)
	carolWS.expectError(protocol.TypeMediaSignal, foreign, protocol.CodeConflict)
	otherWS.expectError(protocol.TypeMediaSignal, foreign, protocol.CodeConflict)

	// 换新会话后，旧 session/nonce 一律 CONFLICT（旧 answer/ICE 不能进新接收器）。
	const secondReceive = "receive-owner-0002"
	second, _ := ownerWS.watchIsolated(ch.ID, alice.ID, secondReceive)
	if second.SessionID != secondReceive || second.Nonce == ack.Nonce {
		t.Fatalf("second watch = %+v, want new session/nonce", second)
	}
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "answer", firstReceive, ack.Nonce), protocol.CodeConflict)

	// 未关闭的同一 receive_id 再次请求（代次已推进 → tuple 不同）必须 CONFLICT，
	// 不能隐式覆盖/换掉正在用的接收传输。
	ownerWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: secondReceive,
	}, protocol.CodeConflict)
}

// TestIsolatedConcurrentWatchFromTwoSocketsSingleOwner 锁定跨连接并发 watch：
// 同一成员两条连接同时 watch，只能有一个成功（另一个 CONFLICT），且失败方的
// receive_id 从未被 arm。这里不读内部 map——用「失败方信令被拒绝、成功方信令
// 能到达接收 lease」证明 hub 与 SFU 对 owner 的认知一致。
func TestIsolatedConcurrentWatchFromTwoSocketsSingleOwner(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	aWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	bWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("isolated-concurrent")
	for _, c := range []*wsClient{aliceWS, aWS, bWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForScreenStateFor(t, aWS, ch.ID, func(p protocol.ScreenStatePayload) bool {
		return len(p.Shares) == 1 && p.Shares[0].SharerID == alice.ID
	})

	type outcome struct {
		client  *wsClient
		receive string
		ok      *protocol.ScreenWatchOK
		errCode string
	}
	run := func(c *wsClient, receiveID string) outcome {
		req, err := protocol.New(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
			ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: receiveID,
		})
		if err != nil {
			return outcome{client: c, receive: receiveID, errCode: "build"}
		}
		c.sendRaw(req)
		deadline := time.Now().Add(10 * time.Second)
		for {
			env, err := c.recv(deadline)
			if err != nil {
				return outcome{client: c, receive: receiveID, errCode: "recv"}
			}
			if env.ID != req.ID {
				continue
			}
			if strings.HasSuffix(env.Type, ".ok") {
				var ok protocol.ScreenWatchOK
				if err := env.DecodePayload(&ok); err != nil {
					return outcome{client: c, receive: receiveID, errCode: "decode"}
				}
				return outcome{client: c, receive: receiveID, ok: &ok}
			}
			if strings.HasSuffix(env.Type, ".err") {
				var p protocol.ErrorPayload
				_ = env.DecodePayload(&p)
				return outcome{client: c, receive: receiveID, errCode: p.Code}
			}
		}
	}

	results := make([]outcome, 2)
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); results[0] = run(aWS, "receive-race-aaaa") }()
	go func() { defer wg.Done(); results[1] = run(bWS, "receive-race-bbbb") }()
	wg.Wait()

	var winner, loser *outcome
	for i := range results {
		if results[i].ok != nil {
			if winner != nil {
				t.Fatalf("both connections armed a watch: %+v and %+v", *winner, results[i])
			}
			winner = &results[i]
		} else {
			loser = &results[i]
		}
	}
	if winner == nil || loser == nil {
		t.Fatalf("concurrent watch results = %+v, want exactly one ok and one conflict", results)
	}
	if loser.errCode != protocol.CodeConflict {
		t.Fatalf("losing watch error = %q, want %q", loser.errCode, protocol.CodeConflict)
	}
	// 失败方的 receive_id 从未 arm：它的信令必须是会话冲突，而不是到达会话。
	loser.client.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "answer", loser.receive, "whatever-nonce"), protocol.CodeConflict)
	// 成功方的 lease 才是 hub 与 SFU 一致持有的：坏 SDP 到达会话 → invalid_argument。
	winner.client.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "answer", winner.receive, winner.ok.Nonce), protocol.CodeInvalidArgument)
}

// TestIsolatedDisconnectKeepsOtherConnectionLease 锁定断线清理的归属：
// 一条连接断开只清它自己的接收 lease，不清同成员另一条连接正在看的接收，
// 也不清别的成员的发布。
func TestIsolatedDisconnectKeepsOtherConnectionLease(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobA, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	bobB, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-disconnect")
	for _, c := range []*wsClient{aliceWS, bobA, bobB, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	const receive = "receive-survive-01"
	ack, _ := bobA.watchIsolated(ch.ID, alice.ID, receive)

	// bobB 断开：只清 bobB 自己的状态，bobA 的接收 lease 必须保留。
	_ = bobB.conn.Close()
	time.Sleep(300 * time.Millisecond)

	alive := signalPayload(ch.ID, "answer", receive, ack.Nonce)
	bobA.expectError(protocol.TypeMediaSignal, alive, protocol.CodeInvalidArgument)

	// alice 的发布没有被 bobB 的断线清掉。
	waitForScreenStateFor(t, carolWS, ch.ID, func(p protocol.ScreenStatePayload) bool {
		for _, share := range p.Shares {
			if share.SharerID == alice.ID {
				return true
			}
		}
		return false
	})
}

// TestIsolatedUnwatchStopsLeaseButKeepsOwnPublication 锁定停看与发布互不牵连：
// 观看者自己也在共享时，unwatch 只关它的接收 lease，screen.state 里它自己的共享
// 仍在；再次 watch 后共享者停发，lease 被关闭（后续信令 CONFLICT），自己的共享
// 仍不受影响。
func TestIsolatedUnwatchStopsLeaseButKeepsOwnPublication(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-unwatch")
	for _, c := range []*wsClient{aliceWS, bobWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	// bob 同时是发布者：它自己的共享在停看前后都必须存在。
	startSyntheticShare(t, bobWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, bob.ID, 0)

	const receive = "receive-unwatch-01"
	ack, _ := bobWS.watchIsolated(ch.ID, alice.ID, receive)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 1)

	uw := bobWS.unwatchShare(ch.ID, alice.ID)
	if uw.SharerID != alice.ID {
		t.Fatalf("unwatch sharer = %q, want alice", uw.SharerID)
	}
	// ACK 返回即已关闭接收 lease：再用旧 session/nonce 发信令必须 CONFLICT。
	bobWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", receive, ack.Nonce), protocol.CodeConflict)
	// 同一条 screen.state 里断言两件事：alice 的观看者清零（接收 lease 关了），
	// bob 自己的共享仍在（停看不清本端发布）。
	waitForScreenStateFor(t, carolWS, ch.ID, func(p protocol.ScreenStatePayload) bool {
		aliceCleared := true
		bobPresent := false
		for _, share := range p.Shares {
			if share.SharerID == alice.ID && len(share.Viewers) != 0 {
				aliceCleared = false
			}
			if share.SharerID == bob.ID {
				bobPresent = true
			}
		}
		return aliceCleared && bobPresent
	})

	// 重新 watch 后由共享者 stop：lease 必须被关闭（不能停在最后一帧），
	// 且 bob 自己的共享不受影响。
	const again = "receive-unwatch-02"
	againAck, _ := bobWS.watchIsolated(ch.ID, alice.ID, again)
	aliceWS.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: ch.ID})
	bobWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", again, againAck.Nonce), protocol.CodeConflict)
	waitForScreenStateFor(t, carolWS, ch.ID, func(p protocol.ScreenStatePayload) bool {
		aliceGone := true
		bobPresent := false
		for _, share := range p.Shares {
			if share.SharerID == alice.ID {
				aliceGone = false
			}
			if share.SharerID == bob.ID {
				bobPresent = true
			}
		}
		return aliceGone && bobPresent
	})
}

// TestIsolatedUnwatchByReceiveIDClosesExactLease 锁定严格撤租的真实协议语义：
//   - receive_id 撤租会真正关闭对应接收 lease（ACK 后旧 session/nonce 信令 CONFLICT）；
//   - 同一会话重复撤租幂等（空 SharerID），不报错、不影响其它会话；
//   - 旧会话的迟到撤租绝不能清掉同 target 的新会话（会话 ≠ 目标）。
func TestIsolatedUnwatchByReceiveIDClosesExactLease(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ownerWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-unwatch-by-id")
	for _, c := range []*wsClient{aliceWS, ownerWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	const first = "receive-unwatch-by-id-01"
	ack, _ := ownerWS.watchIsolated(ch.ID, alice.ID, first)

	uw := ownerWS.unwatchSession(ch.ID, first)
	if uw.SharerID != alice.ID {
		t.Fatalf("session unwatch sharer = %q, want alice", uw.SharerID)
	}
	// ACK 返回即 lease 已关闭：旧 session/nonce 必须 CONFLICT。
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", first, ack.Nonce), protocol.CodeConflict)

	// 幂等：同一会话再撤一次返回空 target，不报错。
	if again := ownerWS.unwatchSession(ch.ID, first); again.SharerID != "" {
		t.Fatalf("repeated session unwatch reported clearing %q, want empty", again.SharerID)
	}

	// 同 target 的新会话：旧 session 的迟到撤租必须无效果。
	const second = "receive-unwatch-by-id-02"
	secondAck, _ := ownerWS.watchIsolated(ch.ID, alice.ID, second)
	if secondAck.SessionID != second || secondAck.Nonce == ack.Nonce {
		t.Fatalf("second watch = %+v, want new session/nonce", secondAck)
	}
	if stale := ownerWS.unwatchSession(ch.ID, first); stale.SharerID != "" {
		t.Fatalf("stale session unwatch cleared the new lease: %q", stale.SharerID)
	}
	// 新 lease 仍然有效：正确归属的信令到达接收会话（坏 SDP → invalid_argument）。
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", second, secondAck.Nonce), protocol.CodeInvalidArgument)

	// legacy 目标语义保持：只带 sharer_id 的 unwatch 仍能停当前目标。
	if legacy := ownerWS.unwatchShare(ch.ID, alice.ID); legacy.SharerID != alice.ID {
		t.Fatalf("legacy unwatch sharer = %q, want alice", legacy.SharerID)
	}
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", second, secondAck.Nonce), protocol.CodeConflict)
}

// TestIsolatedUnwatchSessionIdentityRejectsForeignSockets 锁定严格撤租的身份：
//   - 同成员另一 socket、跨成员连接都不能替 owner 撤租，owner lease 不受影响；
//   - 非法 receive_id / 未声明 isolated 的连接被明确拒绝，不静默成功；
//   - 会话完全匹配时 sharer_id 不参与身份判定（旧 target guard 不能冒充身份）。
func TestIsolatedUnwatchSessionIdentityRejectsForeignSockets(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	ownerWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	otherWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	legacyWS, _ := dialWSCaps(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-unwatch-identity")
	for _, c := range []*wsClient{aliceWS, ownerWS, otherWS, carolWS, legacyWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	const receive = "receive-unwatch-identity-01"
	ack, _ := ownerWS.watchIsolated(ch.ID, alice.ID, receive)

	// 同成员另一 socket 无权撤租：幂等空 ACK，owner lease 保持。
	if uw := otherWS.unwatchSession(ch.ID, receive); uw.SharerID != "" {
		t.Fatalf("other socket cleared the owner lease: %q", uw.SharerID)
	}
	// 跨成员同样无权。
	if uw := carolWS.unwatchSession(ch.ID, receive); uw.SharerID != "" {
		t.Fatalf("foreign member cleared the owner lease: %q", uw.SharerID)
	}
	// 非法 receive_id 与未声明 isolated 的连接：明确拒绝，不静默成功。
	otherWS.expectError(protocol.TypeScreenUnwatch, protocol.ScreenUnwatchRequest{
		ChannelID: ch.ID, ReceiveID: "short",
	}, protocol.CodeInvalidArgument)
	legacyWS.expectError(protocol.TypeScreenUnwatch, protocol.ScreenUnwatchRequest{
		ChannelID: ch.ID, ReceiveID: receive,
	}, protocol.CodeInvalidArgument)
	// 以上都没有动到 owner 的 lease：正确归属的信令仍到达会话。
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", receive, ack.Nonce), protocol.CodeInvalidArgument)

	// 会话完全匹配时，sharer_id 不是身份：给一个错误 target 也必须撤掉匹配会话。
	env2 := ownerWS.request(protocol.TypeScreenUnwatch, protocol.ScreenUnwatchRequest{
		ChannelID: ch.ID, ReceiveID: receive, SharerID: "not-the-current-target",
	})
	var uw protocol.ScreenUnwatchOK
	if err := env2.DecodePayload(&uw); err != nil {
		t.Fatalf("decode screen.unwatch.ok: %v", err)
	}
	if uw.SharerID != alice.ID {
		t.Fatalf("target mismatch blocked session unwatch: sharer = %q, want alice", uw.SharerID)
	}
	ownerWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", receive, ack.Nonce), protocol.CodeConflict)
}

// TestIsolatedUnsubscribeClosesLease 锁定退订会关闭该连接的接收 lease：退订后
// 旧 session/nonce 的信令被订阅检查拒绝，不会落到任何接收器。
func TestIsolatedUnsubscribeClosesLease(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("isolated-unsub")
	for _, c := range []*wsClient{aliceWS, bobWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, carolWS, ch.ID, alice.ID, 0)

	const receive = "receive-unsub-0001"
	ack, _ := bobWS.watchIsolated(ch.ID, alice.ID, receive)

	bobWS.request(protocol.TypeChannelUnsub, protocol.ChannelSubscribeRequest{ChannelID: ch.ID})
	bobWS.expectError(protocol.TypeMediaSignal,
		signalPayload(ch.ID, "candidate", receive, ack.Nonce), protocol.CodeForbidden)
}
