package sfu

// 接收会话本端轨 codec 的回归（relay 观看端 0 帧缺陷）。
//
// 现场：发布者真实上传 video/VP8（服务端日志 `screen publisher attached codec=video/VP8`），
// 但严格接收会话的本端轨被写死成 H264。Pion 的 TrackLocalStaticRTP.writeRTP 会用
// **绑定时的 payload type** 覆盖每个 RTP 包的包头 PT，于是 VP8 payload 被以 H264 的
// PT 发出去：ICE/DTLS/SRTP 全通、RTP 计数增长、客户端 video track attached，但
// 解码器按 H264 解 VP8 → 0 帧（RemoteFrames=0，UI 停在「等待首帧」）。
//
// 这里用真实 Pion PeerConnection（服务端生产 VideoRoom + 客户端替身，loopback UDP）
// 断言客户端**实际协商/绑定到的远端轨 codec**，而不是只看服务端内部状态。

import (
	"sync"
	"testing"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
)

// codecCapture 记录客户端 PC 上 OnTrack 拿到的远端轨 codec。
type codecCapture struct {
	notify chan struct{}

	mu    sync.Mutex
	codec string
}

func newCodecCapture() *codecCapture {
	return &codecCapture{notify: make(chan struct{}, 1)}
}

func (c *codecCapture) set(codec string) {
	c.mu.Lock()
	c.codec = codec
	c.mu.Unlock()
	select {
	case c.notify <- struct{}{}:
	default:
	}
}

func (c *codecCapture) get() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.codec
}

func (c *codecCapture) wait(t *testing.T, window time.Duration) string {
	t.Helper()
	deadline := time.Now().Add(window)
	for {
		if got := c.get(); got != "" {
			return got
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatalf("timed out after %s waiting for the client to attach a remote track", window)
		}
		if remaining > 10*time.Millisecond {
			remaining = 10 * time.Millisecond
		}
		select {
		case <-c.notify:
		case <-time.After(remaining):
		}
	}
}

// TestVideoReceiveTrackCodecFollowsPublisherVP8 是核心回归：VP8 发布者的接收会话，
// 服务端本端轨与客户端真正绑定的远端轨都必须是 VP8（修复前两者都是 H264）。
func TestVideoReceiveTrackCodecFollowsPublisherVP8(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publishCodec("alice", vp8TestCodec())

	pubCodec, ok := h.room.PublisherCodec("alice")
	if !ok || pubCodec.MimeType != webrtc.MimeTypeVP8 {
		t.Fatalf("PublisherCodec(alice) = %q ok=%v, want %q", pubCodec.MimeType, ok, webrtc.MimeTypeVP8)
	}

	rx := h.openReceive("viewer", "alice", 1, "rx-vp8")
	if codec, ok := h.room.ReceiveSessionCodec("rx-vp8"); !ok || codec.MimeType != webrtc.MimeTypeVP8 {
		t.Fatalf("ReceiveSessionCodec = %q ok=%v, want %q (local track must use the publisher codec)",
			codec.MimeType, ok, webrtc.MimeTypeVP8)
	}
	if info, ok := h.room.ReceiveSession("rx-vp8"); !ok || info.Codec != webrtc.MimeTypeVP8 {
		t.Fatalf("ReceiveSession.Codec = %q ok=%v, want %q", info.Codec, ok, webrtc.MimeTypeVP8)
	}

	h.pushMedia(alice, rx, "vp8", 2)

	// 客户端侧证据：远端轨 codec 由收到的 PT 经 SDP 映射得到。修复前这里是 H264，
	// 而 payload 是 VP8 —— 传输连通但解码器永远出不了帧。
	if got := rx.codecOf(); got != webrtc.MimeTypeVP8 {
		t.Fatalf("viewer remote track codec = %q, want %q (payload/PT mismatch breaks decoding)",
			got, webrtc.MimeTypeVP8)
	}
	t.Logf("vp8: publisher=%s session_track=%s client_remote_track=%s delivered=%d",
		pubCodec.MimeType, webrtc.MimeTypeVP8, rx.codecOf(), len(rx.delivered()))
}

// TestVideoReceiveWatchBeforePublisherAttach 锁定真实的时序：观看者 screen.watch 可能
// 早于发布轨 attach（实测早约 1.5s）。此时接收会话先用占位轨，发布轨 attach 时必须在
// **写出第一包之前**换成发布者真实 codec，客户端最终仍绑定 VP8。
func TestVideoReceiveWatchBeforePublisherAttach(t *testing.T) {
	h := newReplayHarness(t)

	// 发布轨还不存在：观看者先开接收会话（生产里 watch 可能先到）。
	if _, ok := h.room.PublisherCodec("alice"); ok {
		t.Fatal("PublisherCodec(alice) reported a codec before the publication attached")
	}
	rx := h.openReceive("viewer", "alice", 1, "rx-early")
	early, ok := h.room.ReceiveSessionCodec("rx-early")
	if !ok {
		t.Fatal("ReceiveSessionCodec missing early session")
	}
	if early.MimeType != webrtc.MimeTypeH264 {
		t.Fatalf("early session placeholder codec = %q, want %q", early.MimeType, webrtc.MimeTypeH264)
	}

	// 发布者晚到（VP8）。attach 时必须把已经存在的接收会话换成本端轨 = VP8。
	alice := h.publishCodec("alice", vp8TestCodec())
	h.waitUntil("early receive session to adopt the publisher codec", func() bool {
		codec, ok := h.room.ReceiveSessionCodec("rx-early")
		return ok && codec.MimeType == webrtc.MimeTypeVP8
	})

	h.pushMedia(alice, rx, "late", 2)
	if got := rx.codecOf(); got != webrtc.MimeTypeVP8 {
		t.Fatalf("viewer remote track codec = %q, want %q after a late publication attach",
			got, webrtc.MimeTypeVP8)
	}
	if got := h.room.ReceivePacketsWritten("rx-early"); got == 0 {
		t.Fatal("early receive session never received packets after the publication attached")
	}
	t.Logf("watch-before-publish: early_codec=%s final_codec=%s packets=%d",
		early.MimeType, webrtc.MimeTypeVP8, h.room.ReceivePacketsWritten("rx-early"))
}

// TestVideoReceiveSwitchTargetCodec 锁定换台/换目标：新目标用另一个 codec 时，新一代
// 接收会话必须跟随**新目标**的 codec，不能沿用上一目标的轨 codec。
func TestVideoReceiveSwitchTargetCodec(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publishCodec("alice", vp8TestCodec())
	bob := h.publishCodec("bob", h264TestCodec())

	// 先看 alice（VP8）。
	rx1 := h.openReceive("viewer", "alice", 1, "rx-switch-a")
	h.pushMedia(alice, rx1, "a", 1)
	if got := rx1.codecOf(); got != webrtc.MimeTypeVP8 {
		t.Fatalf("first target remote track codec = %q, want %q", got, webrtc.MimeTypeVP8)
	}

	// 换台：生产上是 CloseReceive + 新 generation 的 OpenReceive（新 session_id）。
	if !h.room.CloseReceive("viewer", "rx-switch-a") {
		t.Fatal("CloseReceive(rx-switch-a) reported no session")
	}
	_ = rx1.pc.Close()

	rx2 := h.openReceive("viewer", "bob", 2, "rx-switch-b")
	if codec, ok := h.room.ReceiveSessionCodec("rx-switch-b"); !ok || codec.MimeType != webrtc.MimeTypeH264 {
		t.Fatalf("switched session codec = %q ok=%v, want %q (must follow the new target)",
			codec.MimeType, ok, webrtc.MimeTypeH264)
	}
	h.pushMedia(bob, rx2, "b", 1)
	if got := rx2.codecOf(); got != webrtc.MimeTypeH264 {
		t.Fatalf("second target remote track codec = %q, want %q", got, webrtc.MimeTypeH264)
	}
	// 新会话只交付新目标的 payload。
	for _, d := range rx2.delivered() {
		if string(d.payload) == string(mediaPayload("a", 0)) {
			continue
		}
		if len(d.payload) >= len("partyup-replay/a/") && string(d.payload[:len("partyup-replay/a/")]) == "partyup-replay/a/" {
			t.Fatalf("switched session delivered the old target's payload %q", d.payload)
		}
	}
	if _, ok := h.room.ReceiveSession("rx-switch-a"); ok {
		t.Fatal("old receive session still registered after the switch")
	}
	t.Logf("switch: a=%s b=%s", rx1.codecOf(), rx2.codecOf())
}

// TestVideoRoomLegacySlotPathCodecFollowsPublication 评估并锁定 legacy 混合槽位路径
// （video_room.go 的 H264 占位 + bindSlotLocked 的 ReplaceTrack）：这条路径用
// ReplaceTrack 把槽位重绑到发布者真实轨，Pion 会用新轨 codec 重新绑定 payload type，
// 所以 VP8 发布者时客户端同样绑定 VP8。它不是本次缺陷的来源，也不能被改回占位 codec。
func TestVideoRoomLegacySlotPathCodecFollowsPublication(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publishCodec("alice", vp8TestCodec())

	pc, err := h.clientAPI.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatalf("legacy viewer pc: %v", err)
	}
	t.Cleanup(func() { _ = pc.Close() })
	codec := newCodecCapture()
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		codec.set(remote.Codec().MimeType)
		go func() {
			for {
				if _, _, err := remote.ReadRTP(); err != nil {
					return
				}
			}
		}()
	})

	const viewer = "legacy-viewer"
	h.mu.Lock()
	h.answers[viewer] = h.answerAs(viewer, pc, nil)
	h.mu.Unlock()

	// manual=false 是 v1 旧客户端：join 时自动绑定已有共享（服务端会 ReplaceTrack 到
	// 发布者真实轨）。join 内同步完成 offer/answer。
	if err := h.room.JoinWithMode(viewer, false); err != nil {
		t.Fatalf("legacy join: %v", err)
	}
	if bound := h.room.Bound(viewer); len(bound) != 1 || bound[0] != "alice" {
		t.Fatalf("legacy viewer bound = %v, want [alice]", bound)
	}
	h.waitUntil("legacy viewer to connect", func() bool {
		return pc.ConnectionState() == webrtc.PeerConnectionStateConnected
	})

	// 连接建立前写进槽位的包会被丢弃：发到 OnTrack 出现为止（有界）。
	deadline := time.Now().Add(replayWait)
	for codec.get() == "" {
		if time.Now().After(deadline) {
			t.Fatal("legacy viewer never attached the forwarded video track")
		}
		seq := h.nextSeq()
		alice.send(seq, mediaPayload("legacy", seq))
		time.Sleep(10 * time.Millisecond)
	}
	if got := codec.get(); got != webrtc.MimeTypeVP8 {
		t.Fatalf("legacy viewer remote track codec = %q, want %q; ReplaceTrack must rebind the slot payload type",
			got, webrtc.MimeTypeVP8)
	}
	t.Logf("legacy slot path: publisher=%s client_remote_track=%s", webrtc.MimeTypeVP8, codec.get())
}

// TestVideoReceiveAsksPublisherForKeyframe 证明：观看者中途加入时，SFU 必须向发布者
// 请求关键帧（PLI）。
//
// 这是「观看端 0 帧」的第二半病因：屏幕共享编码器不会定时编关键帧（真机实测一次
// 55s 分享 1634 帧里只有 2 个 keyframe_encoded）。中途加入的观看者从半截 GOP 开始收，
// 没有关键帧就永远解不出帧——表现正是 receive PC connected、服务端 RTP 在涨、
// 客户端 RemoteFrames=0。Pion 不会跨 PeerConnection 转发 RTCP，所以必须由 SFU 自己
// 向发布者发 PLI。
func TestVideoReceiveAsksPublisherForKeyframe(t *testing.T) {
	h := newReplayHarness(t)
	pub := h.publishCodec("alice", vp8TestCodec())

	senders := pub.pc.GetSenders()
	if len(senders) == 0 {
		t.Fatal("publisher PC has no RTP sender")
	}
	sender := senders[0]

	// 发布轨已存在时建立接收会话：OpenReceive 必须在此刻向发布者请求关键帧。
	_ = h.openReceive("viewer", "alice", 1, "rx-keyframe")

	pli := make(chan struct{}, 1)
	go func() {
		for {
			pkts, _, err := sender.ReadRTCP()
			if err != nil {
				return
			}
			for _, pkt := range pkts {
				if _, ok := pkt.(*rtcp.PictureLossIndication); ok {
					select {
					case pli <- struct{}{}:
					default:
					}
					return
				}
			}
		}
	}()

	select {
	case <-pli:
		t.Log("publisher received a PLI: mid-stream join requested a keyframe")
	case <-time.After(5 * time.Second):
		t.Fatal("SFU never sent a PLI to the publisher: a mid-stream viewer can never decode")
	}
}
