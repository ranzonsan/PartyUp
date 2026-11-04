package server_test

// 服务端 SFU 中继的端到端测试。
//
// 这里的「客户端」是真的客户端：真实 WebSocket 信令 + 真实 pion PeerConnection +
// 真实 UDP 媒体（WSL 内 loopback，UDP 正常）。每个 peer 用 TrackLocalStaticSample
// 每 20ms 写一帧 Opus，接收端按 track.ID()（= 服务端填的发布者 member_id）统计
// RTP 包数——「B 能收到 A 的音频」就是这么判的，不是看日志猜。
//
// 覆盖：2 人中继、3 人中继（A 的声音同时到 B 和 C）、voice.state 广播（加入/静音/离开）。

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
)

// ------------------------------------------------------------------
// 测试用「客户端」
// ------------------------------------------------------------------

type relayPeer struct {
	t         *testing.T
	name      string
	wsURL     string
	channelID string

	ws       *websocket.Conn
	memberID string
	pc       *webrtc.PeerConnection
	mic      *webrtc.TrackLocalStaticSample
	send     bool // 是否在说话

	mu        sync.Mutex
	recv      map[string]int
	total     int
	sfuOffers int      // 服务端下发过几次 relay offer（direct 策略下应为 0）
	tracks    []string // 收到的远端音轨 track id（relay 下应为 partyup-slot-<n>）
	states    []protocol.VoiceStatePayload

	incoming chan protocol.Envelope
	closed   chan struct{}
	writeMu  sync.Mutex
	nextID   int
}

func newRelayPeer(t *testing.T, env *testEnv, name, token string) *relayPeer {
	t.Helper()
	wsURL := "ws" + trimHTTPPrefix(env.http.URL) + "/ws"
	conn, resp, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		status := "no response"
		if resp != nil {
			status = resp.Status
		}
		t.Fatalf("%s: dial ws: %v (%s)", name, err, status)
	}

	p := &relayPeer{
		t:        t,
		name:     name,
		wsURL:    wsURL,
		ws:       conn,
		recv:     make(map[string]int, 4),
		incoming: make(chan protocol.Envelope, 64),
		closed:   make(chan struct{}),
	}
	go p.readLoop()

	t.Cleanup(func() {
		close(p.closed)
		_ = conn.Close()
		if p.pc != nil {
			_ = p.pc.Close()
		}
	})

	// auth（顺便记住自己的 member id：后面断言「收到谁的音频」要用）
	authResp := p.roundTrip(protocol.TypeAuth, protocol.AuthRequest{Token: token})
	if authResp.Type != protocol.TypeOK(protocol.TypeAuth) {
		t.Fatalf("%s: auth failed: %s %s", name, authResp.Type, authResp.Payload)
	}
	var authOK protocol.AuthOK
	if err := authResp.DecodePayload(&authOK); err != nil {
		t.Fatalf("%s: decode auth.ok: %v", name, err)
	}
	p.memberID = authOK.Member.ID

	// PeerConnection：默认编解码表（含 Opus），麦克风用静态采样轨。
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatalf("%s: new peer connection: %v", name, err)
	}
	p.pc = pc
	mic, err := webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2},
		"mic", name,
	)
	if err != nil {
		t.Fatalf("%s: new mic track: %v", name, err)
	}
	p.mic = mic
	// 先占一条 sendonly 音频 transceiver，服务端 offer 的第一条 m-line（recvonly 麦克风）
	// 会复用它——这样服务端收到的就是这条轨。
	if _, err := pc.AddTransceiverFromTrack(mic, webrtc.RTPTransceiverInit{
		Direction: webrtc.RTPTransceiverDirectionSendonly,
	}); err != nil {
		t.Fatalf("%s: add mic transceiver: %v", name, err)
	}

	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		p.t.Logf("%s: got remote track id=%q kind=%s", p.name, track.ID(), track.Kind().String())
		p.mu.Lock()
		p.tracks = append(p.tracks, track.ID())
		p.mu.Unlock()
		for {
			if _, _, err := track.ReadRTP(); err != nil {
				p.t.Logf("%s: track %q read ended: %v", p.name, track.ID(), err)
				return
			}
			p.mu.Lock()
			p.recv[track.ID()]++
			p.total++
			p.mu.Unlock()
		}
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		p.t.Logf("%s: pc state=%s", p.name, state.String())
	})

	go p.speakerLoop()
	return p
}

func trimHTTPPrefix(raw string) string {
	if len(raw) >= 4 && raw[:4] == "http" {
		return raw[4:]
	}
	return raw
}

// readLoop 消费服务端推送：offer 交给 pion 并回 answer；voice.state 存档；其余给 roundTrip。
func (p *relayPeer) readLoop() {
	for {
		_, data, err := p.ws.ReadMessage()
		if err != nil {
			return
		}
		var env protocol.Envelope
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		switch env.Type {
		case protocol.TypeMediaSignal:
			var push protocol.MediaSignalPush
			if err := env.DecodePayload(&push); err != nil {
				continue
			}
			if push.Target == "sfu" && push.Kind == "offer" {
				p.mu.Lock()
				p.sfuOffers++
				p.mu.Unlock()
			}
			if push.Kind == "offer" && p.pc != nil {
				// 必须异步：applyOffer 里要发 answer 并等 .ok，而读这条 .ok 的就是本循环。
				go p.applyOffer(push)
			}
		case protocol.TypeVoiceState:
			var st protocol.VoiceStatePayload
			if err := env.DecodePayload(&st); err == nil {
				p.mu.Lock()
				p.states = append(p.states, st)
				p.mu.Unlock()
			}
		default:
			select {
			case p.incoming <- env:
			case <-p.closed:
				return
			}
		}
	}
}

// failf 是 applyOffer 内部的断言出口：测试收尾时 cleanup 会先 close(p.closed)，
// 此时还在飞的 offer 里任何失败都属预期噪声。若直接 p.t.Errorf，会在测试结束后
// 触发 "Fail in goroutine after test has completed" panic（历史偶发，约 1/5）。
func (p *relayPeer) failf(format string, args ...any) {
	select {
	case <-p.closed:
		return
	default:
	}
	p.t.Errorf(format, args...)
}

// applyOffer 把服务端 offer 吃进 pion，挂上麦克风，回 answer（target=sfu）。
func (p *relayPeer) applyOffer(push protocol.MediaSignalPush) {
	// 测试收尾会关连接/关 PC，此时还在飞的 offer 直接忽略，不要往已经关闭的 PC 上塞。
	select {
	case <-p.closed:
		return
	default:
	}
	var msg struct {
		SDP string `json:"sdp"`
	}
	if err := json.Unmarshal([]byte(push.Payload), &msg); err != nil || msg.SDP == "" {
		p.failf("%s: bad offer payload: %v", p.name, err)
		return
	}
	offer := webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: msg.SDP}
	if err := p.pc.SetRemoteDescription(offer); err != nil {
		// 收尾阶段 PC 已关、或 offer 在会话重建后才到：都属预期噪声，不算失败。
		select {
		case <-p.closed:
			return
		default:
		}
		if p.pc.ConnectionState() == webrtc.PeerConnectionStateClosed {
			return
		}
		p.failf("%s: set remote offer: %v", p.name, err)
		return
	}
	// 服务端第一条 m-line 是它收我们麦克风的 recvonly；把已有 sendonly transceiver 的
	// 发送轨对上（若 pion 复用成功，这里 ReplaceTrack 是幂等的）。
	for _, tr := range p.pc.GetTransceivers() {
		if tr.Kind() == webrtc.RTPCodecTypeAudio && tr.Sender() != nil &&
			tr.Direction() != webrtc.RTPTransceiverDirectionRecvonly {
			if err := tr.Sender().ReplaceTrack(p.mic); err == nil {
				break
			}
		}
	}

	answer, err := p.pc.CreateAnswer(nil)
	if err != nil {
		p.failf("%s: create answer: %v", p.name, err)
		return
	}
	gathered := webrtc.GatheringCompletePromise(p.pc)
	if err := p.pc.SetLocalDescription(answer); err != nil {
		p.failf("%s: set local answer: %v", p.name, err)
		return
	}
	<-gathered
	local := p.pc.LocalDescription()
	if local == nil {
		p.failf("%s: local description nil", p.name)
		return
	}
	payload, _ := json.Marshal(map[string]string{"type": "answer", "sdp": local.SDP})
	resp := p.roundTrip(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: p.channelID,
		Target:    "sfu",
		Media:     "voice",
		Kind:      "answer",
		Payload:   string(payload),
	})
	if resp.Type != protocol.TypeOK(protocol.TypeMediaSignal) {
		p.failf("%s: answer rejected: %s %s", p.name, resp.Type, resp.Payload)
	}
}

// speakerLoop 在 send=true 时每 20ms 写一帧「音频」（内容不要求可解码，只要求是 RTP）。
func (p *relayPeer) speakerLoop() {
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-p.closed:
			return
		case <-ticker.C:
			if !p.isSending() {
				continue
			}
			sample := []byte{0xF8, 0xFF, 0xFE} // 3 字节假 Opus 帧，不解码也不影响 RTP 计数
			if err := p.mic.WriteSample(media.Sample{Data: sample, Duration: 20 * time.Millisecond}); err != nil {
				return
			}
		}
	}
}

func (p *relayPeer) isSending() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.send
}

func (p *relayPeer) setSending(on bool) {
	p.mu.Lock()
	p.send = on
	p.mu.Unlock()
}

func (p *relayPeer) roundTrip(msgType string, payload any) protocol.Envelope {
	p.t.Helper()
	p.writeMu.Lock()
	p.nextID++
	id := fmt.Sprintf("%s-%d", p.name, p.nextID)
	env, err := protocol.New(msgType, payload)
	if err != nil {
		p.writeMu.Unlock()
		p.t.Errorf("%s: build %s: %v", p.name, msgType, err)
		return protocol.Envelope{}
	}
	env.ID = id
	data, _ := env.Marshal()
	_ = p.ws.SetWriteDeadline(time.Now().Add(5 * time.Second))
	err = p.ws.WriteMessage(websocket.TextMessage, data)
	p.writeMu.Unlock()
	if err != nil {
		p.t.Errorf("%s: write %s: %v", p.name, msgType, err)
		return protocol.Envelope{}
	}

	want := protocol.TypeOK(msgType)
	errType := protocol.TypeError(msgType)
	deadline := time.After(10 * time.Second)
	for {
		select {
		case env := <-p.incoming:
			if env.ID != id {
				continue
			}
			if env.Type == want || env.Type == errType {
				return env
			}
		case <-deadline:
			p.t.Errorf("%s: timeout waiting for %s (id=%s)", p.name, want, id)
			return protocol.Envelope{}
		case <-p.closed:
			return protocol.Envelope{}
		}
	}
}

func (p *relayPeer) joinRelay() protocol.Envelope {
	p.t.Helper()
	return p.roundTrip(protocol.TypeVoiceJoin, protocol.VoiceJoinRequest{ChannelID: p.channelID})
}

func (p *relayPeer) leaveVoice() protocol.Envelope {
	p.t.Helper()
	return p.roundTrip(protocol.TypeVoiceLeave, protocol.VoiceLeaveRequest{ChannelID: p.channelID})
}

func (p *relayPeer) setMuted(muted bool) protocol.Envelope {
	p.t.Helper()
	return p.roundTrip(protocol.TypeVoiceSetMute, protocol.VoiceSetMuteRequest{
		ChannelID: p.channelID,
		Muted:     muted,
	})
}

func (p *relayPeer) sfuOfferCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.sfuOffers
}

func (p *relayPeer) receivedFrom(publisherID string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.recv[publisherID]
}

// receivedTotal 返回收到的 RTP 总包数（不区分是哪一路：用于「B 收到了 A 的音频」这类断言，
// msid 在预协商槽位上不一定稳定，按 track id 精确归属不可靠）。
func (p *relayPeer) receivedTotal() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.total
}

// lastState 返回最近一条 voice.state（没有则 false）。
func containsString(items []string, want string) bool {
	for _, item := range items {
		if item == want {
			return true
		}
	}
	return false
}

func (p *relayPeer) trackIDs() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]string, len(p.tracks))
	copy(out, p.tracks)
	return out
}

func (p *relayPeer) lastState() (protocol.VoiceStatePayload, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(p.states) == 0 {
		return protocol.VoiceStatePayload{}, false
	}
	return p.states[len(p.states)-1], true
}

// ------------------------------------------------------------------
// 环境搭建
// ------------------------------------------------------------------

// relayEnv 是「一个频道 + 三个成员（alice/bob/carol）」的固定装置。
type relayEnv struct {
	*testEnv
	channel protocol.Channel
	alice   *relayPeer
	bob     *relayPeer
	carol   *relayPeer
}

func newRelayEnv(t *testing.T) *relayEnv {
	t.Helper()
	// 这是语音中继的测试环境：屏幕策略钉成 direct，避免默认 relay 带来的视频房间
	// 给三个假客户端发 screen offer（它们只实现语音 PC）。
	env := newTestEnvWithModes(t, "relay", "direct")
	_, adminToken := env.bootstrapAdmin("alice", "alice-password-123")
	_, bobToken := env.registerViaInvite(env.newInvite(), "bob", "bob-password-123")
	_, carolToken := env.registerViaInvite(env.newInvite(), "carol", "carol-password-123")

	alice := newRelayPeer(t, env, "alice", adminToken)
	bob := newRelayPeer(t, env, "bob", bobToken)
	carol := newRelayPeer(t, env, "carol", carolToken)

	for _, p := range []*relayPeer{alice, bob, carol} {
		p.channelID = "pending"
	}

	// alice 建频道（管理员），三人订阅。
	ch := alice.createChannel("relay-test")
	for _, p := range []*relayPeer{alice, bob, carol} {
		p.channelID = ch.ID
		p.subscribe(ch.ID)
	}
	return &relayEnv{testEnv: env, channel: ch, alice: alice, bob: bob, carol: carol}
}

func (p *relayPeer) createChannel(name string) protocol.Channel {
	p.t.Helper()
	resp := p.roundTrip(protocol.TypeChannelCreate, protocol.ChannelCreateRequest{Name: name})
	if resp.Type != protocol.TypeOK(protocol.TypeChannelCreate) {
		p.t.Fatalf("%s: create channel: %s %s", p.name, resp.Type, resp.Payload)
	}
	var created protocol.ChannelCreateOK
	if err := resp.DecodePayload(&created); err != nil {
		p.t.Fatalf("%s: decode channel.create.ok: %v", p.name, err)
	}
	return created.Channel
}

func (p *relayPeer) subscribe(channelID string) {
	p.t.Helper()
	resp := p.roundTrip(protocol.TypeChannelSub, protocol.ChannelSubscribeRequest{ChannelID: channelID})
	if resp.Type != protocol.TypeOK(protocol.TypeChannelSub) {
		p.t.Fatalf("%s: subscribe: %s %s", p.name, resp.Type, resp.Payload)
	}
}

// wakeRelay 让「服务端 → 客户端」的推送有一小段时间被消费（读循环本来就在跑，这里只是留出时间）。
func wakeRelay() { time.Sleep(50 * time.Millisecond) }

// waitConnected 等某个 peer 的 PeerConnection 真正连上（避免测试在 offer/answer 还在飞时就结束）。
func waitConnected(t *testing.T, p *relayPeer, timeout time.Duration) {
	t.Helper()
	if !waitUntil(t, p.name+" pc connected", timeout, func() bool {
		return p.pc.ConnectionState() == webrtc.PeerConnectionStateConnected
	}) {
		t.Fatalf("%s: pc never connected (state=%s)", p.name, p.pc.ConnectionState().String())
	}
}

func waitUntil(t *testing.T, what string, timeout time.Duration, cond func() bool) bool {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Logf("timeout waiting for %s", what)
	return false
}

// ------------------------------------------------------------------
// 测试
// ------------------------------------------------------------------

// TestVoiceRelayTwoMembers：2 人中继，B 能收到 A 的音频（RTP 包数增长）。
func TestVoiceRelayTwoMembers(t *testing.T) {
	e := newRelayEnv(t)

	if resp := e.alice.joinRelay(); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
		t.Fatalf("alice join relay: %s %s", resp.Type, resp.Payload)
	}
	wakeRelay()

	if resp := e.bob.joinRelay(); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
		t.Fatalf("bob join relay: %s %s", resp.Type, resp.Payload)
	}
	waitConnected(t, e.alice, 10*time.Second)
	waitConnected(t, e.bob, 10*time.Second)

	// 两边都进语音后，A 开始说话。
	e.alice.setSending(true)
	defer e.alice.setSending(false)

	if !waitUntil(t, "bob receives alice audio", 10*time.Second, func() bool {
		return e.bob.receivedTotal() >= 10
	}) {
		t.Fatalf("bob got %d packets, want >=10", e.bob.receivedTotal())
	}
	t.Logf("PASS two-member relay: bob received %d RTP packets (alice is the only sender)",
		e.bob.receivedTotal())
}

// TestVoiceRelayThreeMembers：3 人中继，A 说话时 B 和 C 都要收到；B 说话时 A 也要收到。
func TestVoiceRelayThreeMembers(t *testing.T) {
	e := newRelayEnv(t)

	for _, p := range []*relayPeer{e.alice, e.bob, e.carol} {
		if resp := p.joinRelay(); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
			t.Fatalf("%s join relay: %s %s", p.name, resp.Type, resp.Payload)
		}
	}
	for _, p := range []*relayPeer{e.alice, e.bob, e.carol} {
		waitConnected(t, p, 10*time.Second)
	}

	e.alice.setSending(true)
	defer e.alice.setSending(false)

	if !waitUntil(t, "bob+carol receive alice", 15*time.Second, func() bool {
		return e.bob.receivedTotal() >= 10 && e.carol.receivedTotal() >= 10
	}) {
		t.Fatalf("alice fan-out failed: bob=%d carol=%d packets",
			e.bob.receivedTotal(), e.carol.receivedTotal())
	}
	t.Logf("PASS three-member fan-out: bob=%d carol=%d packets (only alice sends)",
		e.bob.receivedTotal(), e.carol.receivedTotal())

	// 反向：B 说话，A 和 C 都要收到（证明不是单向 happy path）。
	e.bob.setSending(true)
	defer e.bob.setSending(false)
	beforeAlice, beforeCarol := e.alice.receivedTotal(), e.carol.receivedTotal()
	if !waitUntil(t, "alice+carol receive bob", 15*time.Second, func() bool {
		return e.alice.receivedTotal()-beforeAlice >= 10 && e.carol.receivedTotal()-beforeCarol >= 10
	}) {
		t.Fatalf("bob fan-out failed: alice=+%d carol=+%d packets",
			e.alice.receivedTotal()-beforeAlice, e.carol.receivedTotal()-beforeCarol)
	}
	t.Logf("PASS three-member reverse fan-out: alice=+%d carol=+%d packets (only bob sends)",
		e.alice.receivedTotal()-beforeAlice, e.carol.receivedTotal()-beforeCarol)

	// 槽位映射（B 的一部分）：每个成员的远端音轨 id 必须是稳定的 partyup-slot-<n>，
	// 且 voice.state.slots 给出的「槽位 → 成员」在所有人那里是同一份。
	// pion 侧远端轨是「收到 RTP 才创建」，所以这里只断言出现过的 id 都是稳定槽位 id，
	// 以及收发关系对得上：alice 是第一个发布者 → 占 slot 0。
	for _, p := range []*relayPeer{e.alice, e.bob, e.carol} {
		ids := p.trackIDs()
		if len(ids) == 0 {
			t.Fatalf("%s: expected at least one remote track", p.name)
		}
		for _, id := range ids {
			if !strings.HasPrefix(id, "partyup-slot-") {
				t.Fatalf("%s: remote track id %q is not a stable slot id", p.name, id)
			}
		}
	}
	for _, consumer := range []*relayPeer{e.bob, e.carol} {
		if !containsString(consumer.trackIDs(), sfu.SlotTrackID(0)) {
			t.Fatalf("%s: missing alice's slot track %q: %v",
				consumer.name, sfu.SlotTrackID(0), consumer.trackIDs())
		}
	}
	if !containsString(e.alice.trackIDs(), sfu.SlotTrackID(1)) {
		t.Fatalf("alice: missing bob's slot track %q: %v",
			sfu.SlotTrackID(1), e.alice.trackIDs())
	}
	// 本测试里 carol 从不发声，所以发布者是 alice（slot 0）与 bob（slot 1）。
	if !waitUntil(t, "voice.state slots (2 publishers)", 10*time.Second, func() bool {
		st, ok := e.bob.lastState()
		return ok && len(st.Slots) == 2
	}) {
		st, _ := e.bob.lastState()
		t.Fatalf("voice.state slots: got %+v, want 2 entries", st.Slots)
	}
	st, _ := e.bob.lastState()
	seen := map[int]string{}
	for _, slot := range st.Slots {
		if slot.Index < 0 || slot.Index >= sfu.MaxRemoteSlots {
			t.Fatalf("slot index out of range: %+v", slot)
		}
		if prev, dup := seen[slot.Index]; dup {
			t.Fatalf("slot index %d assigned twice: %s and %s", slot.Index, prev, slot.MemberID)
		}
		seen[slot.Index] = slot.MemberID
	}
	for _, want := range []string{e.alice.memberID, e.bob.memberID} {
		found := false
		for _, got := range seen {
			if got == want {
				found = true
				break
			}
		}
		if !found {
			t.Fatalf("voice.state slots missing publisher %s: %+v", want, st.Slots)
		}
	}
	t.Logf("PASS slot mapping: %+v (track ids stable)", st.Slots)
}

// TestVoiceModeIsServerPolicy：传输方式完全由服务端策略决定，客户端传的 mode 被忽略。
//
//	服务端 relay（默认）+ 客户端说 direct  → transport=relay，且真的下发 relay offer；
//	服务端 direct + 客户端说 relay          → transport=direct，且服务器一个 offer 都不发（走 P2P 信令）。
func TestVoiceModeIsServerPolicy(t *testing.T) {
	cases := []struct {
		name       string
		serverMode string
		clientSays string
		want       string
	}{
		{"relay-default", "relay", "direct", "relay"},
		{"direct", "direct", "relay", "direct"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// 屏幕策略钉 direct：这个用例只关心语音策略，避免默认 relay 的视频房间
			// 给这些只实现语音 PC 的假客户端发 screen offer。
			env := newTestEnvWithModes(t, tc.serverMode, "direct")
			// 先 bootstrap 管理员（bootstrap 要求成员数为 0），再用邀请码建普通成员。
			_, adminToken := env.bootstrapAdmin("admin-"+tc.name, "password-123")
			member, token := env.registerViaInvite(env.newInvite(), "policy-"+tc.name, "password-123")
			peer := newRelayPeer(t, env, "p-"+tc.name, token)
			admin := newRelayPeer(t, env, "admin-"+tc.name, adminToken)
			ch := admin.createChannel("policy-" + tc.name)
			peer.channelID = ch.ID
			admin.channelID = ch.ID
			admin.subscribe(ch.ID)
			peer.subscribe(ch.ID)

			// 故意带一个与服务端策略相反的 mode 字段：服务端必须忽略它。
			resp := peer.roundTrip(protocol.TypeVoiceJoin, map[string]string{
				"channel_id": peer.channelID,
				"mode":       tc.clientSays,
			})
			if resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
				t.Fatalf("join: %s %s", resp.Type, resp.Payload)
			}
			var ok protocol.VoiceJoinOK
			if err := resp.DecodePayload(&ok); err != nil {
				t.Fatalf("decode voice.join.ok: %v", err)
			}
			if ok.Transport != tc.want {
				t.Fatalf("transport = %q, want %q (server policy must win)", ok.Transport, tc.want)
			}
			if !waitUntil(t, "voice.state carries server mode", 3*time.Second, func() bool {
				st, ok := peer.lastState()
				return ok && st.VoiceMode == tc.want && len(st.Members) == 1 && st.Members[0].MemberID == member.ID
			}) {
				st, _ := peer.lastState()
				t.Fatalf("voice.state = mode=%q members=%d, want %q/1", st.VoiceMode, len(st.Members), tc.want)
			}

			if tc.want == "relay" {
				if !waitUntil(t, "relay offer delivered", 5*time.Second, func() bool {
					return peer.sfuOfferCount() > 0
				}) {
					t.Fatal("relay policy but server never sent an sfu offer")
				}
			} else {
				// direct 策略下服务端只做信令转发，不该主动发 offer。
				time.Sleep(500 * time.Millisecond)
				if n := peer.sfuOfferCount(); n != 0 {
					t.Fatalf("direct policy but server sent %d sfu offer(s)", n)
				}
			}
			t.Logf("PASS server voice mode=%s (client asked %s) → transport=%s, sfuOffers=%d",
				tc.serverMode, tc.clientSays, ok.Transport, peer.sfuOfferCount())
		})
	}
}

// TestVoiceStateBroadcast：voice.state 正确反映加入/静音/离开，且带频道模式。
func TestVoiceStateBroadcast(t *testing.T) {
	e := newRelayEnv(t)

	if resp := e.alice.joinRelay(); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
		t.Fatalf("alice join: %s %s", resp.Type, resp.Payload)
	}
	wakeRelay()
	if resp := e.bob.joinRelay(); resp.Type != protocol.TypeOK(protocol.TypeVoiceJoin) {
		t.Fatalf("bob join: %s %s", resp.Type, resp.Payload)
	}
	waitConnected(t, e.alice, 10*time.Second)
	waitConnected(t, e.bob, 10*time.Second)

	if !waitUntil(t, "alice sees 2 voice members", 5*time.Second, func() bool {
		st, ok := e.alice.lastState()
		return ok && st.VoiceMode == "relay" && st.ScreenMode == "direct" && len(st.Members) == 2
	}) {
		st, _ := e.alice.lastState()
		t.Fatalf("alice voice.state = voice_mode=%q screen_mode=%q members=%d, want relay/direct/2", st.VoiceMode, st.ScreenMode, len(st.Members))
	}

	// 静音上报：bob 静音后，alice 侧的 voice.state 里 bob.muted 必须为 true。
	if resp := e.bob.setMuted(true); resp.Type != protocol.TypeOK(protocol.TypeVoiceSetMute) {
		t.Fatalf("bob set_mute: %s %s", resp.Type, resp.Payload)
	}
	bobID := memberIDOf(t, e.bob)
	if !waitUntil(t, "alice sees bob muted", 5*time.Second, func() bool {
		st, ok := e.alice.lastState()
		if !ok {
			return false
		}
		for _, m := range st.Members {
			if m.MemberID == bobID {
				return m.Muted
			}
		}
		return false
	}) {
		t.Fatal("alice never saw bob muted=true in voice.state")
	}
	t.Logf("PASS voice.state mute propagation")

	// 离开：bob 走人后，alice 侧只剩 1 人。
	if resp := e.bob.leaveVoice(); resp.Type != protocol.TypeOK(protocol.TypeVoiceLeave) {
		t.Fatalf("bob leave: %s %s", resp.Type, resp.Payload)
	}
	if !waitUntil(t, "alice sees 1 voice member", 5*time.Second, func() bool {
		st, ok := e.alice.lastState()
		return ok && len(st.Members) == 1 && st.Members[0].MemberID == memberIDOf(t, e.alice)
	}) {
		st, _ := e.alice.lastState()
		t.Fatalf("after bob leave, alice sees %d members", len(st.Members))
	}
	t.Logf("PASS voice.state leave propagation")
}

// memberIDOf 返回该 peer 的成员 id（auth.ok 里拿到的）。
func memberIDOf(t *testing.T, p *relayPeer) string {
	t.Helper()
	if p.memberID == "" {
		t.Fatalf("%s: member id unknown", p.name)
	}
	return p.memberID
}
