package server_test

// B2b direct 的严格接收会话回归（真实 HTTP/WS）。
//
// direct 模式下屏幕共享是端到端 P2P：服务端不看媒体、不生成 SDP，只做**会话
// 经纪**——把认证过的 (viewer, sharer, session, nonce, generation) 绑定起来，
// 让共享者为这个 viewer 建独立的发送传输，并只在会话双方之间路由信令。
//
// 这些测试只走公开协议面。它们锁定的是客户端能观察到的事实：
//   - 严格 watch 不再返回 NOT_IMPLEMENTED，而是回 isolated=true/transport=direct；
//   - 共享者**自己的发布连接**收到 viewer_arm，带服务端生成的 session/nonce/gen；
//   - 共享者的 offer 只到该 viewer 的连接，viewer 的 answer 只回共享者；
//   - 错 nonce、跨成员、已撤销会话、错方向一律 CONFLICT（不降级为广播）；
//   - unwatch/stop 让另一侧收到按会话的关闭控制，之后旧会话信令继续被拒；
//   - 没有 screen.publish.sessions.v1 的共享者 → NOT_IMPLEMENTED。
//
// 真实 UDP/DTLS/SRTP 的帧级隔离见 screen_direct_rtp_test.go（Pion 端到端）。

import (
	"strings"
	"testing"
	"time"

	"partyup/internal/protocol"
)

// waitMediaPush 在下一条 media.signal 推送里找匹配 (kind, sessionID) 的那条。
// 其它推送（screen.state/presence/ping）被跳过；超时即失败。
func (c *wsClient) waitMediaPush(kind, sessionID string, timeout time.Duration) protocol.MediaSignalPush {
	c.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting for media push kind=%s session=%s: %v", kind, sessionID, err)
		}
		if env.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := env.DecodePayload(&push); err != nil {
			c.t.Fatalf("decode media.signal push: %v", err)
		}
		if push.Kind == kind && push.SessionID == sessionID {
			return push
		}
	}
}

// watchDirectSession 发严格 watch 并只等 watch.ok/err（direct 的 offer 由共享者
// 发出，可能在 ack 之后，也可能先到；这里不假设顺序）。
func (c *wsClient) watchDirectSession(channelID, sharerID, receiveID string) protocol.ScreenWatchOK {
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
	deadline := time.Now().Add(10 * time.Second)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			c.t.Fatalf("waiting screen.watch ack session=%s: %v", receiveID, err)
		}
		if env.ID != req.ID {
			continue
		}
		if strings.HasSuffix(env.Type, ".err") {
			var p protocol.ErrorPayload
			_ = env.DecodePayload(&p)
			c.t.Fatalf("screen.watch (direct strict) failed: code=%s message=%s", p.Code, p.Message)
		}
		var ok protocol.ScreenWatchOK
		if err := env.DecodePayload(&ok); err != nil {
			c.t.Fatalf("decode screen.watch.ok: %v", err)
		}
		return ok
	}
}

// directSignal 构造一条 direct 严格会话的信令（target 留空 = 对端成员）。
func directSignal(channelID, to, kind, sessionID, nonce string, generation uint64) protocol.MediaSignalRequest {
	return protocol.MediaSignalRequest{
		ChannelID:    channelID,
		To:           to,
		Media:        "screen",
		Kind:         kind,
		SessionID:    sessionID,
		SessionNonce: nonce,
		Generation:   generation,
		Payload:      `{"type":"` + kind + `","sdp":"v=0\r\n"}`,
	}
}

// TestDirectIsolatedWatchArmsPerViewerPublishSession 是 B2b direct 的主合同：
// 严格 watch 建立真正的 per-viewer 会话，双向信令按会话路由，停止只影响该会话。
func TestDirectIsolatedWatchArmsPerViewerPublishSession(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	carol, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")

	// 共享者的发布连接必须显式声明 per-viewer 发布会话能力。
	aliceWS, caps := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	if !hasCap(caps, protocol.CapabilityScreenWatchIsolated) {
		t.Fatalf("direct server did not advertise %s: %v",
			protocol.CapabilityScreenWatchIsolated, caps)
	}
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("direct-isolated")
	for _, c := range []*wsClient{aliceWS, bobWS, carolWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 3)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	const first = "direct-receive-0001"
	ack := bobWS.watchDirectSession(ch.ID, alice.ID, first)
	if !ack.Isolated || ack.Transport != "direct" || ack.Slot != -1 ||
		ack.SessionID != first || ack.Nonce == "" || ack.Generation == 0 {
		t.Fatalf("direct strict watch ack = %+v, want isolated transport=direct slot=-1 session=%s nonce/gen set", ack, first)
	}

	// 共享者（发布连接）收到 arm：会话身份由服务端生成，viewer 身份来自认证。
	arm := aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, first, 10*time.Second)
	if arm.From != bob.ID || arm.SessionNonce != ack.Nonce || arm.Generation != ack.Generation ||
		arm.Media != "screen" {
		t.Fatalf("viewer_arm = %+v, want from=%s nonce=%s gen=%d", arm, bob.ID, ack.Nonce, ack.Generation)
	}

	// 共享者为该会话发 offer → 只到 owner viewer 连接。
	offer := directSignal(ch.ID, bob.ID, "offer", first, ack.Nonce, ack.Generation)
	aliceWS.request(protocol.TypeMediaSignal, offer)
	push := bobWS.waitMediaPush("offer", first, 10*time.Second)
	if push.From != alice.ID || push.Target != "" || push.SessionNonce != ack.Nonce {
		t.Fatalf("direct offer push = %+v, want from=%s target empty nonce=%s", push, alice.ID, ack.Nonce)
	}

	// viewer 的 answer 只回共享者（同频道的 carol 收不到）。
	bobWS.request(protocol.TypeMediaSignal, directSignal(ch.ID, alice.ID, "answer", first, ack.Nonce, ack.Generation))
	answer := aliceWS.waitMediaPush("answer", first, 10*time.Second)
	if answer.From != bob.ID || answer.SessionNonce != ack.Nonce {
		t.Fatalf("direct answer push = %+v, want from=%s nonce=%s", answer, bob.ID, ack.Nonce)
	}

	// 错 nonce / 错方向 / 跨成员都不能进会话。
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", first, ack.Nonce+"-wrong", ack.Generation), protocol.CodeConflict)
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "offer", first, ack.Nonce, ack.Generation), protocol.CodeConflict)
	carolWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, carol.ID, "answer", first, ack.Nonce, ack.Generation), protocol.CodeConflict)

	// 按会话 unwatch：共享者收到该会话的关闭控制；之后旧会话信令必须被拒。
	uw := bobWS.unwatchSession(ch.ID, first)
	if uw.SharerID != alice.ID {
		t.Fatalf("unwatch.ok.share_id = %q, want %s", uw.SharerID, alice.ID)
	}
	closed := aliceWS.waitMediaPush(protocol.MediaSignalKindViewerClose, first, 10*time.Second)
	if closed.From != bob.ID || closed.SessionNonce != ack.Nonce {
		t.Fatalf("viewer_close = %+v, want from=%s nonce=%s", closed, bob.ID, ack.Nonce)
	}
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", first, ack.Nonce, ack.Generation), protocol.CodeConflict)

	// 重看必须建新会话（新 nonce/generation），旧会话 id 不会被复用。
	const second = "direct-receive-0002"
	next := bobWS.watchDirectSession(ch.ID, alice.ID, second)
	if next.SessionID != second || next.Nonce == ack.Nonce || next.Generation <= ack.Generation {
		t.Fatalf("second watch = %+v, want new session/nonce/generation", next)
	}
	_ = aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, second, 10*time.Second)
}

// TestDirectIsolatedRequiresSharerPublishCapability 锁定「对端不具备直接发送会话
// 能力时明确 NOT_IMPLEMENTED」：绝不退回旧共享 PC 的暂停/恢复冒充隔离。
func TestDirectIsolatedRequiresSharerPublishCapability(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	// 共享者只声明旧的 watch 能力，不声明 screen.publish.sessions.v1。
	aliceWS := dialWS(t, env.http.URL, aliceToken)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("direct-no-publish")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: "direct-receive-cap",
	}, protocol.CodeNotImplemented)
}

// TestDirectSharerStopClosesViewerSession 锁定：共享者 screen.stop 时观看者收到
// 按会话的关闭控制（只关接收传输，不清本地发布），旧会话信令继续被拒。
func TestDirectSharerStopClosesViewerSession(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("direct-stop")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	const session = "direct-stop-recv-1"
	ack := bobWS.watchDirectSession(ch.ID, alice.ID, session)
	_ = aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, session, 10*time.Second)

	aliceWS.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: ch.ID})
	closed := bobWS.waitMediaPush(protocol.MediaSignalKindReceiveClose, session, 10*time.Second)
	if closed.From != alice.ID || closed.SessionNonce != ack.Nonce {
		t.Fatalf("receive_close = %+v, want from=%s nonce=%s", closed, alice.ID, ack.Nonce)
	}
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation), protocol.CodeConflict)
}

// TestDirectLegacyViewerKeepsPauseControl 锁定旧 direct 观看者路径不变：没有
// receive_id 时仍走 pause/resume 控制，不带会话身份。
func TestDirectLegacyViewerKeepsPauseControl(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken, protocol.CapabilityScreenWatch)
	ch := aliceWS.createChannel("direct-legacy")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	ack := bobWS.watchShare(ch.ID, alice.ID)
	if ack.Isolated || ack.Transport != "direct" || ack.SessionID != "" {
		t.Fatalf("legacy direct watch ack = %+v, want isolated=false transport=direct no session", ack)
	}
	arm := aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, "", 10*time.Second)
	if arm.From != bob.ID {
		t.Fatalf("legacy viewer_arm = %+v, want from=%s with empty session", arm, bob.ID)
	}
	uw := bobWS.unwatchShare(ch.ID, alice.ID)
	if uw.SharerID != alice.ID {
		t.Fatalf("legacy unwatch.ok.share_id = %q, want %s", uw.SharerID, alice.ID)
	}
	closed := aliceWS.waitMediaPush(protocol.MediaSignalKindViewerClose, "", 10*time.Second)
	if closed.From != bob.ID {
		t.Fatalf("legacy viewer_close = %+v, want from=%s", closed, bob.ID)
	}
}

// expectNoDirectArm 在窗口期内断言连接收不到该会话的 viewer_arm。调用方必须使用
// **一次性连接**：gorilla/websocket 的读超时会永久毒化该连接，之后不能再发 RPC。
func expectNoDirectArm(t *testing.T, c *wsClient, sessionID string, window time.Duration) {
	t.Helper()
	deadline := time.Now().Add(window)
	for {
		env, err := c.recv(deadline)
		if err != nil {
			return // 读超时 = 窗口内没有 arm
		}
		if env.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := env.DecodePayload(&push); err != nil {
			continue
		}
		if push.Kind == protocol.MediaSignalKindViewerArm && push.SessionID == sessionID {
			t.Fatalf("viewer_arm for session %s was delivered to a connection that does not own the share", sessionID)
		}
	}
}

// TestDirectStrictSignalRequiresExactGeneration 锁定 direct 严格信令的 generation
// 归属：错代次和缺省 0 都必须 CONFLICT；失败不能关闭会话，带对代次仍可投递。
func TestDirectStrictSignalRequiresExactGeneration(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	aliceWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := aliceWS.createChannel("direct-generation")
	aliceWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	aliceWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, aliceWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	const session = "direct-generation-1"
	ack := bobWS.watchDirectSession(ch.ID, alice.ID, session)
	_ = aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, session, 10*time.Second)

	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation+1), protocol.CodeConflict)
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, 0), protocol.CodeConflict)

	// 两次拒绝都不能摘除会话；正确代次仍应到达共享者。
	bobWS.request(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation))
	push := aliceWS.waitMediaPush("answer", session, 10*time.Second)
	if push.From != bob.ID || push.Generation != ack.Generation || push.SessionNonce != ack.Nonce {
		t.Fatalf("answer after rejected generations = %+v, want from=%s gen=%d nonce=%s",
			push, bob.ID, ack.Generation, ack.Nonce)
	}
}

// TestDirectStrictWatchBindsRealShareOwnerNotSiblingSocket 锁定发布归属是**连接级**
// 的：共享由哪条连接 screen.start，就必须由哪条连接接收 viewer_arm。同一成员的
// 另一条 socket 即使声明了 screen.publish.sessions.v1，也没有这路共享的 source，
// 不能补位；严格 watch 必须明确 NOT_IMPLEMENTED，且 arm 不能发错窗口。
func TestDirectStrictWatchBindsRealShareOwnerNotSiblingSocket(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	// 发起共享的连接是旧客户端：没有 per-viewer 发布能力。
	ownerWS, _ := dialWSCaps(t, env.http.URL, aliceToken, protocol.CapabilityScreenWatch)
	// 同成员的另一条连接有能力，但没有这路共享的采集源/轨。
	siblingWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)

	ch := ownerWS.createChannel("direct-owner-binding")
	ownerWS.subscribe(ch.ID)
	siblingWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	ownerWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, ownerWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	const session = "direct-owner-binding-1"
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: session,
	}, protocol.CodeNotImplemented)

	// 不能按同 member 的能力 socket 补位：arm 一条都不能发。
	expectNoDirectArm(t, siblingWS, session, 500*time.Millisecond)
	expectNoDirectArm(t, ownerWS, session, 150*time.Millisecond)
}

// TestDirectSessionNotMigratedToSiblingAfterOwnerDisconnects 锁定 owner 断线后原
// 会话必须被撤销，绝不能把旧 session 迁移到同 member 的另一条 socket：
//   - 观看者立即收到 receive_close；
//   - 有能力但未发起共享的 sibling 收不到旧会话 arm；
//   - 旧 session 的 offer/answer 之后一律 CONFLICT。
func TestDirectSessionNotMigratedToSiblingAfterOwnerDisconnects(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")

	ownerWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	ch := ownerWS.createChannel("direct-owner-drop")
	ownerWS.subscribe(ch.ID)
	bobWS.subscribe(ch.ID)
	ownerWS.waitForPresenceWith(ch.ID, 2)
	startSyntheticShare(t, ownerWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)

	const session = "direct-owner-drop-1"
	ack := bobWS.watchDirectSession(ch.ID, alice.ID, session)
	_ = ownerWS.waitMediaPush(protocol.MediaSignalKindViewerArm, session, 10*time.Second)

	siblingWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	siblingWS.subscribe(ch.ID)
	ownerWS.waitForPresenceWith(ch.ID, 2)

	// 发布连接断开 = 共享消失；会话必须撤销，而不是换个 socket 续命。
	_ = ownerWS.conn.Close()
	closed := bobWS.waitMediaPush(protocol.MediaSignalKindReceiveClose, session, 10*time.Second)
	if closed.From != alice.ID || closed.SessionNonce != ack.Nonce {
		t.Fatalf("receive_close after owner drop = %+v, want from=%s nonce=%s", closed, alice.ID, ack.Nonce)
	}
	expectNoDirectArm(t, siblingWS, session, 500*time.Millisecond)

	// 新建一条同成员连接发旧会话 offer：没有会话归属，必须拒绝。
	sibling2WS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	sibling2WS.subscribe(ch.ID)
	bobWS.waitForPresenceWith(ch.ID, 2)
	sibling2WS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, bob.ID, "offer", session, ack.Nonce, ack.Generation), protocol.CodeConflict)
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation), protocol.CodeConflict)
}

// TestDirectSessionIDCollisionDoesNotOverwrite 锁定 directSessions 的键唯一性：
// receive_id 由客户端生成，跨 viewer / 跨 socket 复用同一字符串时不能静默覆盖
// 仍活着的 lease。碰撞必须明确 CONFLICT，旧会话的信令与撤租继续作用于旧会话；
// 只有旧会话真正关闭后，同一个 id 才可重新使用。
func TestDirectSessionIDCollisionDoesNotOverwrite(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	carol, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")
	bob, bobToken := env.registerViaInvite(env.newInvite(), "bob", "secret123")
	_, daveToken := env.registerViaInvite(env.newInvite(), "dave", "secret123")

	aliceWS, _ := dialWSCaps(t, env.http.URL, aliceToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	carolWS, _ := dialWSCaps(t, env.http.URL, carolToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenPublishSessions)
	bobWS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	bob2WS, _ := dialWSCaps(t, env.http.URL, bobToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)
	daveWS, _ := dialWSCaps(t, env.http.URL, daveToken,
		protocol.CapabilityScreenWatch, protocol.CapabilityScreenWatchIsolated)

	ch := aliceWS.createChannel("direct-session-collision")
	for _, c := range []*wsClient{aliceWS, carolWS, bobWS, bob2WS, daveWS} {
		c.subscribe(ch.ID)
	}
	aliceWS.waitForPresenceWith(ch.ID, 4)
	startSyntheticShare(t, aliceWS, ch.ID)
	startSyntheticShare(t, carolWS, ch.ID)
	waitForShareViewers(t, bobWS, ch.ID, alice.ID, 0)
	waitForShareViewers(t, daveWS, ch.ID, carol.ID, 0)

	const session = "direct-collision-0001"
	ack := bobWS.watchDirectSession(ch.ID, alice.ID, session)
	_ = aliceWS.waitMediaPush(protocol.MediaSignalKindViewerArm, session, 10*time.Second)

	// 跨 viewer + 跨共享者复用同一个 receive_id：必须拒绝，不能顶掉 Bob 的会话。
	daveWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: carol.ID, ReceiveID: session,
	}, protocol.CodeConflict)
	daveWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, carol.ID, "answer", session, "not-bobs-nonce", 1), protocol.CodeConflict)

	// Bob 的旧会话仍是唯一持有者：他的 answer 带着旧 nonce/代次仍能到 Alice。
	bobWS.request(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation))
	first := aliceWS.waitMediaPush("answer", session, 10*time.Second)
	if first.From != bob.ID || first.SessionNonce != ack.Nonce {
		t.Fatalf("answer after cross-viewer collision = %+v, want from=%s nonce=%s", first, bob.ID, ack.Nonce)
	}

	// 同成员另一条 socket 不能替 owner 撤租，也不能用同 id 抢走选台。
	if uw := bob2WS.unwatchSession(ch.ID, session); uw.SharerID != "" {
		t.Fatalf("sibling unwatch closed the owner's session: %+v", uw)
	}
	bob2WS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: session,
	}, protocol.CodeConflict)

	// 同 viewer 重复 watch 同一个 target 且复用同一 id：显式冲突（不幂等覆盖、
	// 不换 generation），旧会话必须原样保留。
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: alice.ID, ReceiveID: session,
	}, protocol.CodeConflict)

	// 同 viewer 换 target 但复用同一 id：仍必须拒绝，旧会话不能被替换。
	bobWS.expectError(protocol.TypeScreenWatch, protocol.ScreenWatchRequest{
		ChannelID: ch.ID, SharerID: carol.ID, ReceiveID: session,
	}, protocol.CodeConflict)

	// 两次拒绝后旧会话仍完整：信令照常路由。
	bobWS.request(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation))
	second := aliceWS.waitMediaPush("answer", session, 10*time.Second)
	if second.From != bob.ID || second.Generation != ack.Generation {
		t.Fatalf("answer after same-viewer collision = %+v, want from=%s gen=%d", second, bob.ID, ack.Generation)
	}

	// 旧会话撤租必须只关它自己：viewer_close 带旧身份，之后同一 id 才可复用。
	uw := bobWS.unwatchSession(ch.ID, session)
	if uw.SharerID != alice.ID {
		t.Fatalf("unwatch.ok.sharer_id = %q, want %s", uw.SharerID, alice.ID)
	}
	closed := aliceWS.waitMediaPush(protocol.MediaSignalKindViewerClose, session, 10*time.Second)
	if closed.From != bob.ID || closed.SessionNonce != ack.Nonce {
		t.Fatalf("viewer_close on collision cleanup = %+v, want from=%s nonce=%s", closed, bob.ID, ack.Nonce)
	}
	bobWS.expectError(protocol.TypeMediaSignal,
		directSignal(ch.ID, alice.ID, "answer", session, ack.Nonce, ack.Generation), protocol.CodeConflict)

	next := daveWS.watchDirectSession(ch.ID, carol.ID, session)
	if next.SessionID != session || next.Generation == 0 {
		t.Fatalf("reuse after close = %+v, want session=%s with a new generation", next, session)
	}
	_ = carolWS.waitMediaPush(protocol.MediaSignalKindViewerArm, session, 10*time.Second)
}
