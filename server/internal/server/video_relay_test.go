package server_test

import (
	"encoding/json"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"

	"partyup/internal/protocol"
)

// videoTestPeer 是「一个成员 + 一条到服务端的视频 PC」的假客户端（pion 实现），
// 用来在没有媒体核心/DLL 的情况下验证 SFU 视频转发路径。
type videoTestPeer struct {
	t        *testing.T
	name     string
	ws       *wsClient
	memberID string
	pc       *webrtc.PeerConnection
	sharer   *webrtc.TrackLocalStaticRTP

	frames   atomic.Int64
	packets  atomic.Int64
	rxCancel chan struct{}
}

// subscribeWithVideoOffer 订阅频道并等第一条**视频** offer（media=screen, target=sfu, kind=offer）。
// 不能直接用 wsClient.subscribe：它会在等 .ok 时把先到的 offer 当无关消息吞掉。
func (p *videoTestPeer) subscribeWithVideoOffer(channelID string) protocol.MediaSignalPush {
	p.t.Helper()
	env := protocol.MustNew(protocol.TypeChannelSub, protocol.ChannelSubscribeRequest{ChannelID: channelID})
	p.ws.sendRaw(env)
	deadline := time.Now().Add(10 * time.Second)
	for {
		msg, err := p.ws.recv(deadline)
		if err != nil {
			p.t.Fatalf("%s: waiting video offer: %v", p.name, err)
		}
		if msg.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := msg.DecodePayload(&push); err != nil {
			continue
		}
		if push.Media == "screen" && push.Kind == "offer" && push.Target == "sfu" {
			return push
		}
	}
}

// waitVideoOffer 等通道上下一条视频 offer（用于 screen.start 触发服务端 Refresh 后重新协商）。
func (p *videoTestPeer) waitVideoOffer() protocol.MediaSignalPush {
	p.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for {
		msg, err := p.ws.recv(deadline)
		if err != nil {
			p.t.Fatalf("%s: waiting refreshed video offer: %v", p.name, err)
		}
		if msg.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := msg.DecodePayload(&push); err != nil {
			continue
		}
		if push.Media == "screen" && push.Kind == "offer" && push.Target == "sfu" {
			return push
		}
	}
}

// setupVideoSession 吃服务端视频 offer，建 PC、回 answer；sharer=true 时挂上假视频轨。
func (p *videoTestPeer) setupVideoSession(offer protocol.MediaSignalPush, sharer bool) {
	p.t.Helper()
	if p.pc != nil {
		_ = p.pc.Close()
		p.pc = nil
	}
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		p.t.Fatalf("%s: new pc: %v", p.name, err)
	}
	p.pc = pc
	if !sharer {
		p.rxCancel = make(chan struct{})
		pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
			if track.Kind() != webrtc.RTPCodecTypeVideo {
				return
			}
			go func() {
				for {
					select {
					case <-p.rxCancel:
						return
					default:
					}
					pkt, _, err := track.ReadRTP()
					if err != nil {
						return
					}
					p.packets.Add(1)
					if pkt.Marker {
						p.frames.Add(1)
					}
				}
			}()
		})
	}

	var sdp struct {
		SDP string `json:"sdp"`
	}
	if err := json.Unmarshal([]byte(offer.Payload), &sdp); err != nil || sdp.SDP == "" {
		p.t.Fatalf("%s: bad video offer: %v", p.name, err)
	}
	if sharer {
		// answerer 侧 pion 不会为「服务端 recvonly m-line」自动建 sender，
		// 所以先 AddTransceiverFromTrack(sendonly)：SetRemoteDescription 时它会复用这条 m-line（mid=0）。
		p.sharer, err = webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeH264, ClockRate: 90000,
		}, "screen-"+p.name, "screen")
		if err != nil {
			p.t.Fatalf("%s: new screen track: %v", p.name, err)
		}
		if _, err := pc.AddTransceiverFromTrack(p.sharer, webrtc.RTPTransceiverInit{
			Direction: webrtc.RTPTransceiverDirectionSendonly,
		}); err != nil {
			p.t.Fatalf("%s: add screen transceiver: %v", p.name, err)
		}
	}
	if err := pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: sdp.SDP,
	}); err != nil {
		p.t.Fatalf("%s: set remote video offer: %v", p.name, err)
	}

	trs := pc.GetTransceivers()
	videoTrs := make([]*webrtc.RTPTransceiver, 0, 3)
	for _, tr := range trs {
		if tr.Kind() == webrtc.RTPCodecTypeVideo {
			videoTrs = append(videoTrs, tr)
		}
	}
	// 约定：第 0 条是服务端 recvonly（本端上传屏幕），其后是 sendonly 视频槽（本端观看）。
	if len(videoTrs) != 1+2 {
		p.t.Fatalf("%s: video transceivers = %d, want 3 (1 upload + 2 slots)", p.name, len(videoTrs))
	}
	if sharer {
		// 校验：服务端 recvonly 的 m-line（mid=0）上确实挂着我们的发送轨。
		attached := false
		for _, tr := range videoTrs {
			if tr.Sender() != nil && tr.Sender().Track() != nil {
				attached = true
			}
		}
		if !attached {
			p.t.Fatalf("%s: screen track not attached to any video sender", p.name)
		}
	}

	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		p.t.Fatalf("%s: create video answer: %v", p.name, err)
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		p.t.Fatalf("%s: set local video answer: %v", p.name, err)
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		p.t.Fatalf("%s: local video description nil", p.name)
	}
	payload, _ := json.Marshal(map[string]string{"type": "answer", "sdp": local.SDP})
	p.ws.request(protocol.TypeMediaSignal, protocol.MediaSignalRequest{
		ChannelID: offer.ChannelID,
		Target:    "sfu",
		Media:     "screen",
		Kind:      "answer",
		Payload:   string(payload),
	})

}

// waitConnected 等 PC 连上（否则 WriteRTP 会被丢）。
func (p *videoTestPeer) waitConnected() {
	p.t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if p.pc.ConnectionState() == webrtc.PeerConnectionStateConnected {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	p.t.Fatalf("%s: video pc never connected (state=%s)", p.name, p.pc.ConnectionState())
}

// sendScreenFrames 以 30fps 写 n 帧假 H264 RTP（每帧 2 个包，第二包带 marker）。
func (p *videoTestPeer) sendScreenFrames(n int) {
	p.t.Helper()
	seq := uint16(0)
	for f := 0; f < n; f++ {
		for part := 0; part < 2; part++ {
			seq++
			pkt := &rtp.Packet{
				Header: rtp.Header{
					Version:        2,
					PayloadType:    96,
					SequenceNumber: seq,
					Timestamp:      uint32(f) * 3000,
					SSRC:           0x51525354,
					Marker:         part == 1,
				},
				Payload: []byte{0x00, 0x00, 0x00, 0x01, 0x65, byte(f)},
			}
			if err := p.sharer.WriteRTP(pkt); err != nil {
				p.t.Fatalf("%s: write screen rtp: %v", p.name, err)
			}
		}
		time.Sleep(33 * time.Millisecond)
	}
}

func (p *videoTestPeer) close() {
	if p.rxCancel != nil {
		close(p.rxCancel)
	}
	if p.pc != nil {
		_ = p.pc.Close()
	}
}

// TestScreenRelayForwardsVideoToViewers 覆盖 SFU 视频的三条关键行为：
//  1. 1 个共享者 + 2 个观看者：两人都收到帧；
//  2. 中途加入的观看者能看到**已经开始**的共享（offer 后再回填槽位）；
//  3. screen.stop 后槽位清空，观看者不再收到帧（不会停在最后一帧）。
func TestScreenRelayForwardsVideoToViewers(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "relay")
	alice, aliceToken := env.bootstrapAdmin("alice", "secret123")
	code := env.newInvite()
	bob, bobToken := env.registerViaInvite(code, "bob", "secret123")
	carol, carolToken := env.registerViaInvite(env.newInvite(), "carol", "secret123")
	dave, daveToken := env.registerViaInvite(env.newInvite(), "dave", "secret123")

	newPeer := func(name, token, memberID string) *videoTestPeer {
		p := &videoTestPeer{t: t, name: name, memberID: memberID, ws: dialWS(t, env.http.URL, token)}
		t.Cleanup(p.close)
		return p
	}
	aliceP := newPeer("alice", aliceToken, alice.ID)
	bobP := newPeer("bob", bobToken, bob.ID)
	carolP := newPeer("carol", carolToken, carol.ID)

	// alice 建频道；三人订阅 → 各自拿到视频 offer 并建立视频 PC（观看者先就绪）。
	ch := aliceP.ws.createChannel("video")
	for _, p := range []*videoTestPeer{aliceP, bobP, carolP} {
		offer := p.subscribeWithVideoOffer(ch.ID)
		p.setupVideoSession(offer, p == aliceP)
	}
	for _, p := range []*videoTestPeer{aliceP, bobP, carolP} {
		p.waitConnected()
	}

	// alice 开始共享（服务端需要在 screen.state 里记录，媒体由假 RTP 驱动）。
	// 注意不能用 wsClient.request：它会在等 .ok 时把服务端同时推来的**重建 offer** 吞掉。
	aliceP.ws.sendRaw(protocol.MustNew(protocol.TypeScreenStart, protocol.ScreenStartRequest{
		ChannelID: ch.ID,
		Source: protocol.ScreenSource{
			ID: "synthetic", Title: "1280x720 synthetic", Width: 1280, Height: 720, Kind: "synthetic",
		},
	}))
	// screen.start 会服务端重建共享者的视频 PC（客户端只有在 answer 前才能挂轨，
	// ABI v2.11 无重协商）——所以共享者要重新吃一条 offer 并带轨应答。
	aliceP.setupVideoSession(aliceP.waitVideoOffer(), true)
	aliceP.waitConnected()

	// 等房间把 alice 的 recvonly 视频轨识别为发布者（OnTrack 在 RTP 到达后才触发）。
	firstStream := make(chan struct{})
	go func() { aliceP.sendScreenFrames(40); close(firstStream) }() // ~1.3s @30fps
	waitUntilFrames(t, map[string]*videoTestPeer{"bob": bobP, "carol": carolP}, 10, 8*time.Second)

	// 约束 2：中途加入的 dave 也能看到正在进行的共享。
	daveP := newPeer("dave", daveToken, dave.ID)
	offer := daveP.subscribeWithVideoOffer(ch.ID)
	daveP.setupVideoSession(offer, false)
	daveP.waitConnected()
	secondStream := make(chan struct{})
	go func() { aliceP.sendScreenFrames(30); close(secondStream) }()
	waitUntilFrames(t, map[string]*videoTestPeer{"dave": daveP}, 10, 8*time.Second)

	// 约束 3：停止共享后槽位清空 → 观看者不再收到新帧。
	// 先等假流发完（否则「停止后还在涨」只是测试自己在继续灌数据），再停共享。
	<-firstStream
	<-secondStream
	time.Sleep(300 * time.Millisecond) // 让在途包落地
	if resp := aliceP.ws.request(protocol.TypeScreenStop, protocol.ScreenStopRequest{ChannelID: ch.ID}); resp.Type != protocol.TypeOK(protocol.TypeScreenStop) {
		t.Fatalf("screen.stop failed: %s %s", resp.Type, resp.Payload)
	}
	time.Sleep(500 * time.Millisecond)
	base := daveP.frames.Load()
	time.Sleep(800 * time.Millisecond)
	if got := daveP.frames.Load(); got-base > 2 {
		t.Fatalf("frames kept flowing after screen.stop: +%d", got-base)
	}

	t.Logf("video relay: bob frames=%d packets=%d, carol frames=%d packets=%d, dave(late) frames=%d",
		bobP.frames.Load(), bobP.packets.Load(), carolP.frames.Load(), carolP.packets.Load(), daveP.frames.Load())
}

func waitUntilFrames(t *testing.T, peers map[string]*videoTestPeer, minFrames int64, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		ok := true
		for _, p := range peers {
			if p.frames.Load() < minFrames {
				ok = false
				break
			}
		}
		if ok {
			return
		}
		time.Sleep(30 * time.Millisecond)
	}
	for name, p := range peers {
		t.Logf("%s: frames=%d packets=%d", name, p.frames.Load(), p.packets.Load())
	}
	t.Fatalf("did not reach %d frames on all viewers within %s", minFrames, timeout)
}

// TestScreenDirectCreatesNoVideoPeerConnection 锁定「direct 下不能凭空多建视频连接」：
// 用户以为屏幕是 P2P，实际却连服务器既是资源浪费也是隐私问题。
// 判定：订阅频道后 500ms 内收不到任何 media=screen 的媒体信令（relay 下会立刻收到 offer）。
func TestScreenDirectCreatesNoVideoPeerConnection(t *testing.T) {
	env := newTestEnvWithModes(t, "relay", "direct") // 语音 relay、屏幕 direct
	_, aliceToken := env.bootstrapAdmin("alice", "secret123")
	ws := dialWS(t, env.http.URL, aliceToken)
	ch := ws.createChannel("no-video")
	ws.subscribe(ch.ID)

	deadline := time.Now().Add(600 * time.Millisecond)
	for {
		env2, err := ws.recv(deadline)
		if err != nil {
			// 超时即通过：direct 下不该有任何视频媒体信令。
			return
		}
		if env2.Type != protocol.TypeMediaSignal {
			continue
		}
		var push protocol.MediaSignalPush
		if err := env2.DecodePayload(&push); err != nil {
			continue
		}
		if push.Media == "screen" {
			t.Fatalf("direct screen mode must not create a server video PC, got %s/%s", push.Target, push.Kind)
		}
	}
}
