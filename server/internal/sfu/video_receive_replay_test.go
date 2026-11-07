package sfu

// B2 严格接收的**真实 UDP/SRTP 层**回归：在 socket 层暂扣真实加密 RTP datagram，
// 再把原封不动的旧密文投给新接收会话。
//
// 为什么需要它（docs/design/v1.1-receive-isolation-contract.md 第 30/31/39 行）：
// 之前的 direct 回归只做到「RTP 不再增长」（quiescence），那是「新帧停了」，不是
// 「旧 datagram 被重放」；只调旧 FrameAccepted/槽位回调也证明不了传输层隔离。
// 这里让**生产** VideoRoom 的接收 PC 真的把 SRTP 加密包写出去，在 net.PacketConn
// 装饰器里交付前暂扣，再用**原发送 socket** 把旧字节投给当前 endpoint。
//
// 拓扑（全部 loopback，不采桌面、不开麦、不连真实服务器、无真实音频）：
//
//	发布者客户端 PC（alice/bob，合成 H264 payload）
//	      │ 真实 SRTP
//	      ▼
//	生产 VideoRoom.onTrack 读循环 ──► writeToReceivesLocked（生产 fanout）
//	      │
//	      ▼
//	接收 PC（每个严格 watch 一条）── 真实 SRTP ──► 受控 socket（captureConn）
//	      │                                             │ hold 暂扣 / resend 原字节
//	      ▼                                             ▼
//	观看者客户端 PC（每条会话一条，独立 ICE/DTLS/SRTP）
//
// 受控 socket 由 ice.UDPMuxDefault 承载：房间内**所有** PeerConnection（发布者 PC 与
// 每条接收 PC）共用同一个服务端 UDP 端口，所以重放包与合法媒体**同源**（同 socket、
// 同源端口、同源 IP），不可能靠「旧端口已经没人监听」蒙过去；接收侧只能靠新的
// DTLS/SRTP 身份拒绝它。目的地是接收会话自己的 endpoint —— 就在重放前后，同一
// endpoint 上有真实媒体在正常交付（positive control 见测试内注释）。
//
// 未覆盖（如实标记，不得当成已验证）：native libwebrtc（Windows）侧的旧 UDP 重放、
// direct（无 SFU）模式的重放、真实硬件/双机 E2E、真实语音帧。

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

const (
	// replayHeldLimit 是暂扣记录上限（有界；超出即放行并记账，不堆包历史）。
	replayHeldLimit = 8
	// replayAttemptWait 是「发一包 → 有界等它到」的单次窗口。
	replayAttemptWait = 150 * time.Millisecond
	// replayWait 是整体条件等待上限（连接建立、首帧、重放后的屏障）。
	replayWait = 8 * time.Second
	// replayStopWindow 是「停看后不应再交付」的有界观察窗。
	replayStopWindow = 400 * time.Millisecond
)

// ------------------------------------------------------------------
// 受控 socket：交付前暂扣真实加密 datagram
// ------------------------------------------------------------------

// heldDatagram 是一条被暂扣的「服务端 → 观看者」datagram 快照。
type heldDatagram struct {
	data []byte   // SRTP 密文原字节（不解密、不复制出第二份语义）
	dst  net.Addr // 原目的 endpoint（旧会话的观看者地址）
	seq  uint16   // 明文 RTP 头里的 seq（SRTP 只加密 payload，头是明文）
	ssrc uint32
}

// mediaDatagramHeader 用成熟 RTP 库判断一条**已加密** datagram 是不是 RTP 媒体。
//
// SRTP 只加密 payload，RTP 头保持明文，所以直接用 pion/rtp 读头即可，不手写字节
// parser。动态 payload type 把 RTCP（192–223）排除；STUN/DTLS 的头不满足 RTP 版本
// 与最小长度，Unmarshal 直接失败。
func mediaDatagramHeader(buf []byte) (rtp.Header, bool) {
	var pkt rtp.Packet
	var header rtp.Header
	if err := pkt.Unmarshal(buf); err != nil {
		// 整包解析可能因为扩展块/填充边界失败；此时只回头读 RTP 头（同一个库）。
		var only rtp.Header
		if _, headerErr := only.Unmarshal(buf); headerErr != nil {
			return rtp.Header{}, false
		}
		header = only
	} else {
		header = pkt.Header
	}
	if header.Version != 2 || header.PayloadType < 96 || header.PayloadType > 127 {
		return rtp.Header{}, false
	}

	return header, true
}

// captureConn 是真实 UDP socket 的 net.PacketConn 装饰器（仅测试用）。
//
// 它是 Pion 在本机的唯一出口（SRTP 加密之后、ICE 之后），所以这里拿到的就是线上
// 真正会被观看者收到的那些字节。hold 期间只暂扣媒体包；STUN/DTLS/RTCP 一律直通，
// 避免把保活与握手一起停掉（那样就只是「断网」，不是「旧包重放」）。
//
// WriteTo 会被发布者读循环在持有 VideoRoom.mu 时调用，所以这里绝不阻塞、不回调
// 房间：只有非阻塞 channel 投递与有界记录。
type captureConn struct {
	net.PacketConn

	holdCh chan heldDatagram

	mu            sync.Mutex
	hold          bool
	mediaWrites   uint64
	otherWrites   uint64
	heldCount     uint64
	overflowCount uint64
	held          []heldDatagram
	lastMediaAddr net.Addr
	lastMediaSeq  uint16
}

// captureStats 是给测试日志/证据用的一次性快照。
type captureStats struct {
	MediaWrites  uint64
	OtherWrites  uint64
	Held         uint64
	Overflow     uint64
	HeldRecords  int
	LastMediaDst string
	LastMediaSeq uint16
}

func newCaptureConn(t *testing.T) *captureConn {
	t.Helper()
	conn, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen controlled udp socket: %v", err)
	}
	c := &captureConn{PacketConn: conn, holdCh: make(chan heldDatagram, replayHeldLimit)}
	t.Cleanup(func() { _ = conn.Close() })
	return c
}

func (c *captureConn) WriteTo(buf []byte, dst net.Addr) (int, error) {
	header, isMedia := mediaDatagramHeader(buf)

	c.mu.Lock()
	if !isMedia {
		c.otherWrites++
		c.mu.Unlock()

		return c.PacketConn.WriteTo(buf, dst)
	}
	c.mediaWrites++
	c.lastMediaAddr = dst
	c.lastMediaSeq = header.SequenceNumber
	holdIt := c.hold
	held := heldDatagram{}
	if holdIt {
		if len(c.held) >= replayHeldLimit {
			// 有界：超出上限就放行并记账，绝不无限堆积包历史。
			c.overflowCount++
			holdIt = false
		} else {
			held = heldDatagram{
				data: append([]byte(nil), buf...),
				dst:  dst,
				seq:  header.SequenceNumber,
				ssrc: header.SSRC,
			}
			c.held = append(c.held, held)
			c.heldCount++
		}
	}
	c.mu.Unlock()

	if holdIt {
		select {
		case c.holdCh <- held:
		default:
			// 测试只等一两条；队列满说明调用方没在等，同样不阻塞写路径。
		}

		// 已暂扣：假装写成功，但字节没有离开本进程 —— 对端此刻收不到。
		return len(buf), nil
	}

	return c.PacketConn.WriteTo(buf, dst)
}

func (c *captureConn) setHold(on bool) {
	c.mu.Lock()
	c.hold = on
	c.mu.Unlock()
}

// resend 把一段**原封不动**的 datagram 从这条 socket（同一源 IP/端口）重发到 dst。
// 它绕过 hold，直接走底层 net.PacketConn：这就是「旧密文重放」本身。
func (c *captureConn) resend(data []byte, dst net.Addr) error {
	if dst == nil {
		return fmt.Errorf("resend: nil destination")
	}
	_, err := c.PacketConn.WriteTo(data, dst)

	return err
}

func (c *captureConn) snapshot() captureStats {
	c.mu.Lock()
	defer c.mu.Unlock()

	dst := ""
	if c.lastMediaAddr != nil {
		dst = c.lastMediaAddr.String()
	}

	return captureStats{
		MediaWrites:  c.mediaWrites,
		OtherWrites:  c.otherWrites,
		Held:         c.heldCount,
		Overflow:     c.overflowCount,
		HeldRecords:  len(c.held),
		LastMediaDst: dst,
		LastMediaSeq: c.lastMediaSeq,
	}
}

func (c *captureConn) currentMediaDst() net.Addr {
	c.mu.Lock()
	defer c.mu.Unlock()

	return c.lastMediaAddr
}

// ------------------------------------------------------------------
// 客户端替身：真实 Pion PeerConnection（独立 ICE/DTLS/SRTP）
// ------------------------------------------------------------------

// deliveredRTP 是观看者客户端**真正解开 SRTP 之后**读到的一条 RTP。
type deliveredRTP struct {
	seq     uint16
	ssrc    uint32
	payload []byte
}

// receivePeer 是一条严格接收会话的客户端侧。它记录实际交付的 RTP，从而可以断言
// 「旧密文没有变成新会话的应用交付」——不是给 sink 贴 epoch 标签。
type receivePeer struct {
	session     string
	nonce       string
	gen         uint64
	pc          *webrtc.PeerConnection
	ufrag       string
	fingerprint string
	notify      chan struct{}

	mu  sync.Mutex
	log []deliveredRTP
	// codec 是客户端**真实协商/绑定到的远端轨 codec**（由收到的第一个 RTP 的
	// payload type 经 SDP 映射得到）。它必须等于发布者实际发送的 codec，
	// 否则客户端会按错误的解码器解包（本缺陷的直接现场）。
	codec string
}

func (rx *receivePeer) record(d deliveredRTP) {
	rx.mu.Lock()
	rx.log = append(rx.log, d)
	rx.mu.Unlock()

	select {
	case rx.notify <- struct{}{}:
	default:
	}
}

func (rx *receivePeer) setCodec(mimeType string) {
	rx.mu.Lock()
	rx.codec = mimeType
	rx.mu.Unlock()
}

func (rx *receivePeer) codecOf() string {
	rx.mu.Lock()
	defer rx.mu.Unlock()
	return rx.codec
}

// delivered 返回目前所有已解密交付的 RTP（含重复/乱序，按到达顺序）。
func (rx *receivePeer) delivered() []deliveredRTP {
	rx.mu.Lock()
	defer rx.mu.Unlock()

	out := make([]deliveredRTP, len(rx.log))
	copy(out, rx.log)

	return out
}

func (rx *receivePeer) hasSeq(seq uint16) bool {
	for _, d := range rx.delivered() {
		if d.seq == seq {
			return true
		}
	}

	return false
}

func (rx *receivePeer) hasPayload(payload []byte) bool {
	for _, d := range rx.delivered() {
		if string(d.payload) == string(payload) {
			return true
		}
	}

	return false
}

// waitFor 有界等待一条满足条件的交付（不是固定 sleep 碰概率：靠 notify 唤醒 +
// 截止时间）。返回 false 表示窗口内没有出现。
func (rx *receivePeer) waitFor(pred func(deliveredRTP) bool, window time.Duration) (deliveredRTP, bool) {
	deadline := time.Now().Add(window)
	for {
		rx.mu.Lock()
		for _, d := range rx.log {
			if pred(d) {
				rx.mu.Unlock()

				return d, true
			}
		}
		rx.mu.Unlock()

		remaining := time.Until(deadline)
		if remaining <= 0 {
			return deliveredRTP{}, false
		}
		if remaining > 10*time.Millisecond {
			remaining = 10 * time.Millisecond
		}
		select {
		case <-rx.notify:
		case <-time.After(remaining):
		}
	}
}

// publishPeer 是发布者的客户端替身：真实 PC + 一条 H264 本地轨。
type publishPeer struct {
	id    string
	pc    *webrtc.PeerConnection
	track *webrtc.TrackLocalStaticRTP
}

// send 写一条合成 RTP（seq/payload 由调用方给，payload 是测试用的可识别字符串）。
// 传输还没绑定时 Pion 直接丢弃，调用方的 push 循环会重试。
func (p *publishPeer) send(seq uint16, payload []byte) {
	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    96,
			SequenceNumber: seq,
			Timestamp:      uint32(seq) * 3000,
			SSRC:           0x5150,
			Marker:         true,
		},
		Payload: payload,
	}
	_ = p.track.WriteRTP(pkt)
}

// mediaPayload 生成一条可识别的合成 payload（真实 H264 内容不参与本测试）。
func mediaPayload(tag string, seq uint16) []byte {
	return []byte(fmt.Sprintf("partyup-replay/%s/%d", tag, seq))
}

// ------------------------------------------------------------------
// 现场：受控 socket + 生产 VideoRoom + 真实客户端 PC
// ------------------------------------------------------------------

type replayHarness struct {
	t         *testing.T
	room      *VideoRoom
	conn      *captureConn
	mux       *ice.UDPMuxDefault
	clientAPI *webrtc.API

	mu      sync.Mutex
	answers map[string]func(kind, payload string)
	pubs    map[string]*publishPeer
	seq     uint16
}

func newTestClientAPI(t *testing.T) *webrtc.API {
	t.Helper()
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		t.Fatalf("client media engine: %v", err)
	}
	se := webrtc.SettingEngine{}
	// 只跑 loopback UDP：两端口都在本机，不需要 STUN/TURN/真实网卡。
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	se.SetIncludeLoopbackCandidate(true)
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)

	return webrtc.NewAPI(webrtc.WithMediaEngine(media), webrtc.WithSettingEngine(se))
}

func newReplayHarness(t *testing.T) *replayHarness {
	t.Helper()

	conn := newCaptureConn(t)
	// 房间内所有 PeerConnection 共用这一条服务端 socket/端口：旧会话密文与当前
	// 合法媒体因此同源，重放不可能被当成「旧端口已经关闭」。
	mux := ice.NewUDPMuxDefault(ice.UDPMuxParams{UDPConn: conn})
	t.Cleanup(func() { _ = mux.Close() })

	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		t.Fatalf("server media engine: %v", err)
	}
	se := webrtc.SettingEngine{}
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	se.SetIncludeLoopbackCandidate(true)
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	se.SetICEUDPMux(mux)
	api := webrtc.NewAPI(webrtc.WithMediaEngine(media), webrtc.WithSettingEngine(se))

	h := &replayHarness{
		t:         t,
		conn:      conn,
		mux:       mux,
		clientAPI: newTestClientAPI(t),
		answers:   make(map[string]func(kind, payload string)),
		pubs:      make(map[string]*publishPeer),
		seq:       1000,
	}

	room, err := newVideoWithAPI("replay-channel",
		slog.New(slog.NewTextHandler(io.Discard, nil)),
		h.dispatch, nil, api)
	if err != nil {
		t.Fatalf("newVideoWithAPI: %v", err)
	}
	h.room = room
	t.Cleanup(func() {
		room.Close()
		if n := room.activeReceiveCount(); n != 0 {
			t.Errorf("room.Close left %d strict receive session(s) registered", n)
		}
	})

	return h
}

func (h *replayHarness) dispatch(memberID, kind, payload string) {
	h.mu.Lock()
	answer := h.answers[memberID]
	h.mu.Unlock()
	if answer == nil {
		h.t.Errorf("no client handler registered for %q signal to %q", kind, memberID)

		return
	}
	if kind != "offer" {
		h.t.Errorf("server sent unexpected %q signal to %q", kind, memberID)

		return
	}
	answer(kind, payload)
}

func (h *replayHarness) nextSeq() uint16 {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.seq++

	return h.seq
}

// waitUntil 是有界条件等待（截止时间 + 5ms 轮询），不是靠固定 sleep 猜时序。
func (h *replayHarness) waitUntil(what string, cond func() bool) {
	h.t.Helper()
	deadline := time.Now().Add(replayWait)
	for {
		if cond() {
			return
		}
		if time.Now().After(deadline) {
			h.t.Fatalf("timed out after %s waiting for %s", replayWait, what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// answerAs 实现客户端收到服务端 offer 后的真实握手：SetRemoteDescription(offer) →
// （发布者）AddTrack 复用 offer 里的 recvonly m-line → CreateAnswer 并等候选收齐
// （非 trickle，与生产同款）→ 把 answer 交回房间。
func (h *replayHarness) answerAs(memberID string, pc *webrtc.PeerConnection, track webrtc.TrackLocal) func(kind, payload string) {
	return func(_ string, payload string) {
		var msg struct {
			Type string `json:"type"`
			SDP  string `json:"sdp"`
		}
		if err := json.Unmarshal([]byte(payload), &msg); err != nil {
			h.t.Errorf("decode offer for %s: %v", memberID, err)

			return
		}
		if err := pc.SetRemoteDescription(webrtc.SessionDescription{
			Type: webrtc.SDPTypeOffer,
			SDP:  msg.SDP,
		}); err != nil {
			h.t.Errorf("set remote offer for %s: %v", memberID, err)

			return
		}
		if track != nil {
			if _, err := pc.AddTrack(track); err != nil {
				h.t.Errorf("add publish track for %s: %v", memberID, err)

				return
			}
		}
		answer, err := pc.CreateAnswer(nil)
		if err != nil {
			h.t.Errorf("create answer for %s: %v", memberID, err)

			return
		}
		gathered := webrtc.GatheringCompletePromise(pc)
		if err := pc.SetLocalDescription(answer); err != nil {
			h.t.Errorf("set local answer for %s: %v", memberID, err)

			return
		}
		select {
		case <-gathered:
		case <-time.After(replayWait):
			h.t.Errorf("client candidate gathering did not finish for %s", memberID)

			return
		}
		local := pc.LocalDescription()
		if local == nil {
			h.t.Errorf("client local description is nil for %s", memberID)

			return
		}
		if err := h.room.HandleSignal(memberID, "answer", []byte(mustJSON(map[string]any{
			"type": "answer",
			"sdp":  local.SDP,
		}))); err != nil {
			h.t.Errorf("apply answer for %s: %v", memberID, err)
		}
	}
}

// h264TestCodec / vp8TestCodec 是测试发布轨的 codec 能力。
func h264TestCodec() webrtc.RTPCodecCapability {
	return webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000}
}

func vp8TestCodec() webrtc.RTPCodecCapability {
	return webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000}
}

// publish 让一个成员用真实 PC 向房间发布 H264 合成屏幕轨（历史用例默认 codec），
// 并等生产 onTrack 挂上发布。VP8 场景用 publishCodec。
func (h *replayHarness) publish(id string) *publishPeer {
	return h.publishCodec(id, h264TestCodec())
}

// publishCodec 与 publish 相同，但用调用方指定的 codec 建发布轨：服务端的
// remote.Codec() 会拿到这个 codec，从而驱动接收会话本端轨的 codec 选择。
func (h *replayHarness) publishCodec(id string, codec webrtc.RTPCodecCapability) *publishPeer {
	h.t.Helper()

	pc, err := h.clientAPI.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		h.t.Fatalf("publisher pc %s: %v", id, err)
	}
	track, err := webrtc.NewTrackLocalStaticRTP(codec, "screen-"+id, "partyup-replay")
	if err != nil {
		h.t.Fatalf("publisher track %s: %v", id, err)
	}
	// 服务端也会向该成员的槽位发东西（本测试里 manual 模式不绑定，通常无包）；
	// 不读远端轨会让 Pion 报 "no handler"，所以照样读掉。
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		go func() {
			for {
				if _, _, err := remote.ReadRTP(); err != nil {
					return
				}
			}
		}()
	})
	h.t.Cleanup(func() { _ = pc.Close() })

	h.mu.Lock()
	h.answers[id] = h.answerAs(id, pc, track)
	h.pubs[id] = &publishPeer{id: id, pc: pc, track: track}
	pub := h.pubs[id]
	h.mu.Unlock()

	// 严格接收是手动的（manual=true）：不 watch 就不绑定。生产同样先 Join 后 watch。
	if err := h.room.JoinWithMode(id, true); err != nil {
		h.t.Fatalf("join publisher %s: %v", id, err)
	}
	h.waitUntil("room to attach publication "+id, func() bool {
		// Pion fires OnTrack only after the first RTP packet, not after SDP alone.
		seq := h.nextSeq()
		pub.send(seq, mediaPayload("bootstrap-"+id, seq))
		for _, owner := range h.room.Publishers() {
			if owner == id {
				return true
			}
		}

		return false
	})

	return pub
}

// legacyPeer 建一个「旧混合 PC」的客户端替身（发布与观看同一条 PC 的 v1 路径），
// 只用来标定 VideoRoom.Refresh 的边界：它是 legacy 重建，不是严格接收重建。
func (h *replayHarness) legacyPeer(id string) *webrtc.PeerConnection {
	h.t.Helper()

	pc, err := h.clientAPI.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		h.t.Fatalf("legacy pc %s: %v", id, err)
	}
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		go func() {
			for {
				if _, _, err := remote.ReadRTP(); err != nil {
					return
				}
			}
		}()
	})
	h.t.Cleanup(func() { _ = pc.Close() })

	h.mu.Lock()
	h.answers[id] = h.answerAs(id, pc, nil)
	h.mu.Unlock()

	return pc
}

// openReceive 走生产的严格接收路径：OpenReceive（服务端主动 offer）→ 客户端真实
// PC 建 ICE/DTLS/SRTP 并回 answer → HandleReceiveSignal 校验 session/nonce 后应用。
func (h *replayHarness) openReceive(viewer, target string, gen uint64, sessionID string) *receivePeer {
	h.t.Helper()

	offer, err := h.room.OpenReceive(viewer, target, gen, sessionID)
	if err != nil {
		h.t.Fatalf("OpenReceive(%s): %v", sessionID, err)
	}
	ufrag, fingerprint := sdpTransportIdentity(h.t, offer.SDP)

	pc, err := h.clientAPI.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		h.t.Fatalf("viewer pc %s: %v", sessionID, err)
	}
	rx := &receivePeer{
		session:     sessionID,
		nonce:       offer.Nonce,
		gen:         gen,
		pc:          pc,
		ufrag:       ufrag,
		fingerprint: fingerprint,
		notify:      make(chan struct{}, 1),
	}
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		// 客户端侧的远端轨 codec 由**收到的 RTP payload type 经协商 SDP 映射**决定。
		// 服务端把 VP8 payload 用 H264 的 PT 发出来时，这里会是 H264——正是收帧
		// 失败的传输层证据（解码器与 payload 不匹配）。
		rx.setCodec(remote.Codec().MimeType)
		go func() {
			for {
				pkt, _, err := remote.ReadRTP()
				if err != nil {
					return
				}
				rx.record(deliveredRTP{
					seq:     pkt.SequenceNumber,
					ssrc:    pkt.SSRC,
					payload: append([]byte(nil), pkt.Payload...),
				})
			}
		}()
	})
	h.t.Cleanup(func() { _ = pc.Close() })

	if err := pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  offer.SDP,
	}); err != nil {
		h.t.Fatalf("viewer set remote offer %s: %v", sessionID, err)
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		h.t.Fatalf("viewer create answer %s: %v", sessionID, err)
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		h.t.Fatalf("viewer set local answer %s: %v", sessionID, err)
	}
	select {
	case <-gathered:
	case <-time.After(replayWait):
		h.t.Fatalf("viewer candidate gathering did not finish for %s", sessionID)
	}
	local := pc.LocalDescription()
	if local == nil {
		h.t.Fatalf("viewer local description is nil for %s", sessionID)
	}
	if err := h.room.HandleReceiveSignal(viewer, sessionID, offer.Nonce, "answer",
		[]byte(mustJSON(map[string]any{"type": "answer", "sdp": local.SDP}))); err != nil {
		h.t.Fatalf("HandleReceiveSignal(answer) %s: %v", sessionID, err)
	}
	h.waitUntil("receive pc "+sessionID+" to connect", func() bool {
		return pc.ConnectionState() == webrtc.PeerConnectionStateConnected
	})

	return rx
}

// pushMedia 用真实发布链路把合成 RTP 推到观看者，直到它解密交付 want 条。
// 每次发送用唯一 seq/payload，「发一包 → 有界等这一包」循环重试，不用固定 sleep。
func (h *replayHarness) pushMedia(pub *publishPeer, rx *receivePeer, tag string, want int) []deliveredRTP {
	h.t.Helper()

	deadline := time.Now().Add(replayWait)
	got := make([]deliveredRTP, 0, want)
	for len(got) < want {
		if time.Now().After(deadline) {
			h.t.Fatalf("publisher %s: only %d/%d media packet(s) reached session %s",
				pub.id, len(got), want, rx.session)
		}
		seq := h.nextSeq()
		payload := mediaPayload(tag, seq)
		pub.send(seq, payload)
		if d, ok := rx.waitFor(func(d deliveredRTP) bool {
			return string(d.payload) == string(payload)
		}, replayAttemptWait); ok {
			got = append(got, d)
		}
	}

	return got
}

// withholdNextMedia 打开 hold 并等**我们刚写的那一条**真实加密媒体 datagram 被暂扣。
// 队列里可能有前面 push 循环的迟到包（它们同样被暂扣、不会交付），所以按 seq 精确
// 匹配，而不是「队列里第一条就是它」。
func (h *replayHarness) withholdNextMedia(pub *publishPeer, tag string) (heldDatagram, []byte, uint16) {
	h.t.Helper()

	seq := h.nextSeq()
	payload := mediaPayload(tag, seq)

	h.conn.setHold(true)
	pub.send(seq, payload)

	deadline := time.Now().Add(replayWait)
	for {
		select {
		case held := <-h.conn.holdCh:
			if held.seq == seq {
				h.conn.setHold(false)

				return held, payload, seq
			}
			// 迟到包：继续暂扣着（不交付），继续等我们的那一条。
		case <-time.After(time.Until(deadline)):
			h.conn.setHold(false)
			h.t.Fatalf("no media datagram with seq %d was withheld within %s", seq, replayWait)
		}
		if time.Now().After(deadline) {
			h.conn.setHold(false)
			h.t.Fatalf("no media datagram with seq %d was withheld within %s", seq, replayWait)
		}
	}
}

// assertSRTPCiphertext 确认暂扣到的是 SRTP 密文而不是明文 RTP 缓冲区：
// 明文字节不应出现在 datagram 里，且线上字节比明文 RTP 长（带认证标签）。
func assertSRTPCiphertext(t *testing.T, data, payload []byte) {
	t.Helper()

	if len(data) == 0 {
		t.Fatal("withheld datagram is empty")
	}
	if bytes.Contains(data, payload) {
		t.Fatalf("withheld datagram still contains the plaintext payload %q: it was captured before SRTP encryption", payload)
	}
	bare := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    96,
			SequenceNumber: 1,
			SSRC:           0x5150,
		},
		Payload: payload,
	}
	plain, err := bare.Marshal()
	if err != nil {
		t.Fatalf("marshal plaintext RTP for comparison: %v", err)
	}
	if len(data) <= len(plain) {
		t.Fatalf("withheld datagram (%d bytes) is not larger than the plaintext RTP packet (%d bytes); SRTP auth tag missing",
			len(data), len(plain))
	}
}

func (h *replayHarness) strictPC(sessionID string) *webrtc.PeerConnection {
	h.room.mu.Lock()
	defer h.room.mu.Unlock()
	if session := h.room.receives[sessionID]; session != nil {
		return session.pc
	}

	return nil
}

// containsMember 是 Members() 的线性查找（测试用）。
func containsMember(list []string, want string) bool {
	for _, item := range list {
		if item == want {
			return true
		}
	}

	return false
}

// ------------------------------------------------------------------
// 1) positive control：暂扣的旧媒体 datagram 在会话仍有效时释放 → 收到已知 payload
// ------------------------------------------------------------------

func TestVideoReceiveWithheldDatagramDeliversOnRelease(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publish("alice")
	rx := h.openReceive("viewer", "alice", 1, "rx-hold")
	h.pushMedia(alice, rx, "live", 1)

	held, payload, seq := h.withholdNextMedia(alice, "held")
	if held.seq != seq {
		t.Fatalf("held datagram RTP header seq = %d, want %d (capture must be the packet we wrote)", held.seq, seq)
	}
	assertSRTPCiphertext(t, held.data, payload)

	// 暂扣发生在**交付之前**：对端此刻一条都没有这条 seq。
	if rx.hasSeq(seq) {
		t.Fatalf("withheld datagram (seq %d) was already delivered before release", seq)
	}
	if got := h.room.ReceivePacketsWritten("rx-hold"); got == 0 {
		t.Fatal("production receive fanout counter did not grow; the packet never entered the receive track")
	}

	// 释放：同 socket、同目的地址、原字节。
	if got := h.conn.currentMediaDst(); got == nil || got.String() != held.dst.String() {
		t.Fatalf("held datagram dst = %v, current media dst = %v", held.dst, got)
	}
	if err := h.conn.resend(held.data, held.dst); err != nil {
		t.Fatalf("resend withheld datagram: %v", err)
	}
	delivered, ok := rx.waitFor(func(d deliveredRTP) bool { return d.seq == seq }, replayWait)
	if !ok {
		t.Fatalf("released datagram (seq %d, %d bytes) was never delivered to the client", seq, len(held.data))
	}
	if string(delivered.payload) != string(payload) {
		t.Fatalf("released datagram payload = %q, want %q", delivered.payload, payload)
	}

	stats := h.conn.snapshot()
	t.Logf("positive control: withheld=%d bytes=%d seq=%d dst=%s media_writes=%d other_writes=%d held_records=%d",
		stats.Held, len(held.data), seq, held.dst, stats.MediaWrites, stats.OtherWrites, stats.HeldRecords)
}

// ------------------------------------------------------------------
// 2) A(g1) → B(g2) → A(g3)：旧 g1 密文投给 g3 不产生交付，当期新媒体继续
// ------------------------------------------------------------------

func TestVideoReceiveReplayOldCiphertextToNewSessions(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publish("alice")
	bob := h.publish("bob")

	// --- g1：看 alice，暂扣一条真实密文（旧会话、旧密钥）。
	rx1 := h.openReceive("viewer", "alice", 1, "rx-g1")
	h.pushMedia(alice, rx1, "g1-live", 1)
	old, oldPayload, oldSeq := h.withholdNextMedia(alice, "g1-old")
	if old.seq != oldSeq {
		t.Fatalf("g1 held seq = %d, want %d", old.seq, oldSeq)
	}
	assertSRTPCiphertext(t, old.data, oldPayload)
	if rx1.hasSeq(oldSeq) {
		t.Fatal("g1 held datagram was delivered before release")
	}
	oldUfrag, oldFP, oldNonce := rx1.ufrag, rx1.fingerprint, rx1.nonce
	oldDst := old.dst.String()

	// --- 关闭 g1（严格撤租 + 客户端关接收传输）。
	if !h.room.CloseReceive("viewer", "rx-g1") {
		t.Fatal("CloseReceive(rx-g1) reported no session")
	}
	if _, ok := h.room.ReceiveSession("rx-g1"); ok {
		t.Fatal("rx-g1 is still registered after CloseReceive")
	}
	if rx1.pc.ConnectionState() != webrtc.PeerConnectionStateClosed {
		t.Logf("note: server-side close of rx-g1 left the client PC in state %s", rx1.pc.ConnectionState())
	}
	_ = rx1.pc.Close()

	// --- g2：换看 bob（新目标 + 新传输 + 新密钥）。
	rx2 := h.openReceive("viewer", "bob", 2, "rx-g2")
	if rx2.nonce == oldNonce {
		t.Fatal("g2 reused the g1 session nonce")
	}
	if rx2.ufrag == oldUfrag || rx2.fingerprint == oldFP {
		t.Fatalf("g2 reused g1 transport identity (ufrag %q, fingerprint %q)", oldUfrag, oldFP)
	}
	h.pushMedia(bob, rx2, "g2-live", 2)
	if rx2.hasPayload(oldPayload) {
		t.Fatal("g2 delivered the old target's payload")
	}
	if !h.room.CloseReceive("viewer", "rx-g2") {
		t.Fatal("CloseReceive(rx-g2) reported no session")
	}
	_ = rx2.pc.Close()

	// --- g3：A→B→A 回看 alice（第三次新传输）。
	rx3 := h.openReceive("viewer", "alice", 3, "rx-g3")
	if rx3.nonce == oldNonce || rx3.nonce == rx2.nonce {
		t.Fatal("g3 reused an earlier session nonce")
	}
	if rx3.ufrag == oldUfrag || rx3.fingerprint == oldFP {
		t.Fatalf("g3 reused g1 transport identity (ufrag %q, fingerprint %q)", oldUfrag, oldFP)
	}
	h.pushMedia(alice, rx3, "g3-live", 1)

	// 同一 endpoint 上的对照：先暂扣并释放**当前会话**的一条密文，证明
	// 「原 socket → 该 endpoint」这条 resend 路径确实交付媒体；随后投 g1 旧密文。
	cur, curPayload, curSeq := h.withholdNextMedia(alice, "g3-held")
	curDst := h.conn.currentMediaDst()
	if curDst == nil || curDst.String() != cur.dst.String() {
		t.Fatalf("control datagram dst %s is not the current media dst %v", cur.dst, curDst)
	}
	if err := h.conn.resend(cur.data, cur.dst); err != nil {
		t.Fatalf("resend current-session datagram: %v", err)
	}
	if _, ok := rx3.waitFor(func(d deliveredRTP) bool { return d.seq == curSeq }, replayWait); !ok {
		t.Fatalf("control: current-session datagram (seq %d) was not delivered on its own endpoint", curSeq)
	}
	if !rx3.hasPayload(curPayload) {
		t.Fatal("control: current-session datagram delivered without its payload")
	}

	// 重放 g1 原封不动旧密文到 g3 的 endpoint（同源 socket，同一可达 tuple）。
	if err := h.conn.resend(old.data, cur.dst); err != nil {
		t.Fatalf("replay g1 ciphertext to g3 endpoint: %v", err)
	}

	// 屏障：让当期新媒体继续通过，并等到最后一包真的到达，再断言没有旧包。
	h.pushMedia(alice, rx3, "g3-after", 3)

	if rx3.hasSeq(oldSeq) {
		t.Fatalf("new session delivered the replayed g1 ciphertext (seq %d)", oldSeq)
	}
	if rx3.hasPayload(oldPayload) {
		t.Fatalf("new session delivered the replayed g1 payload %q", oldPayload)
	}
	if _, ok := h.room.ReceiveSession("rx-g1"); ok {
		t.Fatal("stale g1 session re-appeared in the room")
	}
	if err := h.room.HandleReceiveSignal("viewer", "rx-g1", oldNonce, "answer",
		[]byte(`{"type":"answer","sdp":"v=0\r\n"}`)); err == nil {
		t.Fatal("answer for the closed g1 session was accepted")
	}

	stats := h.conn.snapshot()
	if stats.MediaWrites == 0 || stats.Held == 0 {
		t.Fatalf("capture device never saw media (media_writes=%d held=%d)", stats.MediaWrites, stats.Held)
	}
	if stats.HeldRecords > replayHeldLimit {
		t.Fatalf("held records = %d, exceeds the bound %d", stats.HeldRecords, replayHeldLimit)
	}
	t.Logf("A->B->A replay: old_seq=%d old_bytes=%d old_dst=%s new_dst=%s "+
		"old_ufrag=%s new_ufrag=%s media_writes=%d non_media_writes=%d held=%d overflow=%d",
		oldSeq, len(old.data), oldDst, cur.dst, oldUfrag, rx3.ufrag,
		stats.MediaWrites, stats.OtherWrites, stats.Held, stats.Overflow)
}

// ------------------------------------------------------------------
// 3) 严格重建接收会话 + legacy Refresh 边界 + 停看后无交付
// ------------------------------------------------------------------

func TestVideoReceiveStrictRebuildAndLegacyRefreshBoundary(t *testing.T) {
	h := newReplayHarness(t)
	alice := h.publish("alice")

	// 第一次观看：保留一条旧会话密文。
	rx1 := h.openReceive("viewer", "alice", 1, "rx-rebuild-1")
	h.pushMedia(alice, rx1, "first", 1)
	old, oldPayload, oldSeq := h.withholdNextMedia(alice, "old-established")
	assertSRTPCiphertext(t, old.data, oldPayload)
	if rx1.hasSeq(oldSeq) {
		t.Fatal("held datagram was delivered before release")
	}
	oldUfrag, oldFP, oldNonce := rx1.ufrag, rx1.fingerprint, rx1.nonce
	oldDst := old.dst.String()
	if !h.room.CloseReceive("viewer", "rx-rebuild-1") {
		t.Fatal("CloseReceive(rx-rebuild-1) reported no session")
	}
	_ = rx1.pc.Close()

	// 严格重建：CloseReceive + OpenReceive（新 session_id / nonce / ICE / DTLS），
	// 这才是接收会话重建。legacy Refresh 不参与严格接收（见下）。
	rx2 := h.openReceive("viewer", "alice", 2, "rx-rebuild-2")
	if rx2.nonce == oldNonce {
		t.Fatal("strict rebuild reused the old session nonce")
	}
	if rx2.ufrag == oldUfrag || rx2.fingerprint == oldFP {
		t.Fatalf("strict rebuild reused the old transport identity (ufrag %q, fingerprint %q)", oldUfrag, oldFP)
	}
	h.pushMedia(alice, rx2, "rebuilt", 2)

	// 旧密文投给重建后的 endpoint：同一 endpoint 正有真实媒体在交付（上面刚推过）。
	rebuildDst := h.conn.currentMediaDst()
	if rebuildDst == nil {
		t.Fatal("no media destination recorded for the rebuilt session")
	}
	if err := h.conn.resend(old.data, rebuildDst); err != nil {
		t.Fatalf("replay old ciphertext to rebuilt session: %v", err)
	}
	h.pushMedia(alice, rx2, "rebuilt-after", 3)
	if rx2.hasSeq(oldSeq) || rx2.hasPayload(oldPayload) {
		t.Fatalf("rebuilt session delivered the old-session ciphertext (seq %d payload %q)", oldSeq, oldPayload)
	}

	// legacy Refresh 的边界：它重建的是发布/观看**混合 PC**（v1 路径），不得替换或
	// 关闭严格接收传输，也不得改它的 nonce。场景真实：观看者自己开始共享时服务端
	// 会 Refresh 他的混合 PC，而他的严格接收会话必须继续有效。
	strictPC := h.strictPC("rx-rebuild-2")
	if strictPC == nil {
		t.Fatal("strict receive PC missing before legacy Refresh")
	}
	legacy := h.legacyPeer("viewer")
	if containsMember(h.room.Members(), "viewer") {
		t.Fatal("viewer already had a legacy video peer before Refresh")
	}
	if err := h.room.Refresh("viewer"); err != nil {
		t.Fatalf("legacy Refresh(viewer): %v", err)
	}
	if !containsMember(h.room.Members(), "viewer") {
		t.Fatal("legacy Refresh did not create the legacy video peer")
	}
	if got := h.strictPC("rx-rebuild-2"); got != strictPC {
		t.Fatal("legacy Refresh replaced the strict receive PeerConnection")
	}
	info, ok := h.room.ReceiveSession("rx-rebuild-2")
	if !ok || info.Nonce != rx2.nonce || info.Closed {
		t.Fatalf("legacy Refresh changed the strict receive session: %+v ok=%v", info, ok)
	}
	if n := h.room.activeReceiveCount(); n != 1 {
		t.Fatalf("active strict receive sessions after legacy Refresh = %d, want 1", n)
	}
	if legacy.ConnectionState() == webrtc.PeerConnectionStateClosed {
		t.Fatal("legacy Refresh did not rebuild the legacy PC")
	}
	// legacy 混合 PC 与严格接收是两条独立传输：严格接收仍在交付当期新媒体。
	h.pushMedia(alice, rx2, "after-legacy-refresh", 1)

	// 停看：只销毁接收传输/会话，发布与采集不受影响。
	if !h.room.CloseReceive("viewer", "rx-rebuild-2") {
		t.Fatal("CloseReceive(rx-rebuild-2) reported no session")
	}
	if n := h.room.activeReceiveCount(); n != 0 {
		t.Fatalf("active strict receive sessions after stop = %d, want 0", n)
	}
	if pubs := h.room.Publishers(); len(pubs) != 1 || pubs[0] != "alice" {
		t.Fatalf("publishers after stop = %v, want [alice]", pubs)
	}
	if _, ok := h.room.ReceiveSession("rx-rebuild-2"); ok {
		t.Fatal("stopped receive session is still registered")
	}

	// 停看之后无媒体交付：发布者继续走真实链路发包，客户端侧在有界观察窗内
	// 不能拿到任何**停看之后才发出**的媒体（只断言停看后的新 payload，避免把
	// 停看瞬间还在途的旧包误判成泄漏）。
	afterStop := make([][]byte, 0, 3)
	for i := 0; i < 3; i++ {
		seq := h.nextSeq()
		payload := mediaPayload("after-stop", seq)
		alice.send(seq, payload)
		afterStop = append(afterStop, payload)
	}
	time.Sleep(replayStopWindow)
	for _, payload := range afterStop {
		if rx2.hasPayload(payload) {
			t.Fatalf("stopped strict receive session delivered post-stop media %q", payload)
		}
	}

	stats := h.conn.snapshot()
	if stats.HeldRecords > replayHeldLimit {
		t.Fatalf("held records = %d, exceeds the bound %d", stats.HeldRecords, replayHeldLimit)
	}
	t.Logf("strict rebuild + legacy Refresh boundary: old_dst=%s rebuilt_dst=%s old_seq=%d "+
		"media_writes=%d non_media_writes=%d held=%d",
		oldDst, rebuildDst, oldSeq, stats.MediaWrites, stats.OtherWrites, stats.Held)
}
