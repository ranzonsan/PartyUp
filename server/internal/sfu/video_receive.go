package sfu

// B2：严格接收传输（独立 receive-only PeerConnection）。
//
// 背景（docs/design/v1.1-receive-isolation-contract.md）：旧实现把「上传自己的屏幕」
// 和「观看别人的屏幕」放在同一条成员 PC 上——recvonly 上传 m-line + 两条 sendonly
// 观看槽。于是停看只能解绑槽（旧 RTP 仍在同一条 DTLS/ICE 上），或者粗暴关掉整条
// PC（会清掉本端发布）。仅给新 sink 贴 target/epoch 也无法鉴别同 SSRC、同传输里
// 迟到的旧包。
//
// 这里为**每次严格 watch** 新建一条只含一路 sendonly 视频的 PeerConnection：
//   * 新 PC ⇒ 新 ICE ufrag/pwd、新 DTLS 指纹、新 SSRC；旧会话的包在新会话上无法解密；
//   * session_id 由客户端在发 watch 前生成（原生先建接收 PC），nonce 由服务端生成并
//     绑定 (viewer, target, generation)；answer/ICE 必须带对两者，过期/错身份拒绝；
//   * CloseReceive 只关这条接收 PC，不碰发布 PC、发布轨、采集源或语音；
//   * 新会话只绑定「绑定之后」写入的 RTP：TrackLocalStaticRTP 不重放历史包。
//
// 发布 PC 保持原样（recvonly 上传 + 旧客户端仍用的两个观看槽），所以 legacy/v1
// 客户端与旧服务端组合行为不变。

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

// ErrReceiveSession 表示严格接收会话不存在、已关闭，或不属于该观看者/该 nonce。
// 调用方应明确报「会话过期/不匹配」，不能静默接受旧信令。
var ErrReceiveSession = errors.New("screen receive session is missing, closed or not owned by this viewer")

// ReceiveOffer 是新建接收会话后要下发给观看者的 offer。
type ReceiveOffer struct {
	SessionID  string
	Nonce      string
	Viewer     string
	Target     string
	Generation uint64
	SDP        string
}

// ReceiveSessionInfo 是接收会话的只读快照（测试/诊断；不含 SDP）。
type ReceiveSessionInfo struct {
	SessionID  string
	Nonce      string
	Viewer     string
	Target     string
	Generation uint64
	HasTrack   bool
	Closed     bool
	// Codec 是本会话当前本端轨的 codec（MimeType）；空表示轨缺失。
	// 它必须等于发布者真实 codec，否则观看端会按错误解码器解包。
	Codec string
}

type videoReceive struct {
	id       string
	nonce    string
	viewer   string
	target   string
	gen      uint64
	pc       *webrtc.PeerConnection
	sender   *webrtc.RTPSender
	tr       *webrtc.RTPTransceiver      // 收 RTCP（PLI/FIR）用：把观看端的关键帧请求转给发布者
	track    *webrtc.TrackLocalStaticRTP // 本会话自己的发送轨（每会话独立 SSRC）
	hasTrack bool
	offerSDP string // 本会话的 offer；同一 tuple 重复请求时幂等回放同一份
	closed   bool
	// closeOnce 保证每条接收 PC 恰好关闭一次：换台、断线、StopShare、房间关闭
	// 可能同时命中同一会话，重复 Close 会打日志/触发回调竞争。
	closeOnce sync.Once
	// packetsWritten 只用于回归/诊断：本会话创建后由发布轨写进来的 RTP 包数。
	packetsWritten atomic.Uint64
	// writeErrors 记录 WriteRTP 失败次数（首次失败会记日志）：轨未绑定/PT 未被
	// 观看端接受时 Pion 会返回错误，这是「服务端在写、观看端 0 帧」的关键证据。
	writeErrors atomic.Uint64
}

// makeReceiveNonce 生成不可猜测的会话 nonce。
func makeReceiveNonce() (string, error) {
	var raw [18]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate receive nonce: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(raw[:]), nil
}

// OpenReceive 为一次严格 watch 新建独立的接收传输。
//
// sessionID 由客户端提供并在发 watch 前已用于建立 native 接收会话。生命周期规则：
//
//   - 同一个未关闭的 sessionID 重复请求：tuple（viewer/target/generation）相同则
//     幂等返回同一份 offer/nonce；tuple 不同（换目标/换代次/别的观看者）明确拒绝。
//     绝不隐式覆盖 r.receives，否则旧 PC 会失去句柄、泄漏且无法关闭；
//   - 同一观看者的旧会话**先建好新 PC 再原子换入**：创建失败时旧 lease 原样保留，
//     不会出现 receiveByViewer 指向已摘除旧 ID 的悬空映射；
//   - 每条接收 PC 通过 closeOnce 恰好关闭一次（换台/断线/StopShare/房间关闭可能并发）。
//
// 发布者此刻可能还没开始上传（screen.start 之后才 OnTrack）：会话先建好，等
// onTrack 到达时再绑定发布轨；绑定之前该会话发不出任何 RTP。
func (r *VideoRoom) OpenReceive(viewer, target string, generation uint64, sessionID string) (*ReceiveOffer, error) {
	if viewer == "" || target == "" || sessionID == "" {
		return nil, errors.New("OpenReceive: viewer, target and session_id are required")
	}
	r.recvMu.Lock()
	defer r.recvMu.Unlock()

	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return nil, errors.New("video room is closed")
	}
	if existing := r.receives[sessionID]; existing != nil && !existing.closed {
		sameTuple := existing.viewer == viewer && existing.target == target &&
			existing.gen == generation
		if !sameTuple {
			r.mu.Unlock()
			return nil, fmt.Errorf("%w: session_id is already active for a different receive tuple",
				ErrReceiveSession)
		}
		// 幂等重试：同一 tuple 返回同一份 offer/nonce，不新建 PC、不覆盖。
		offer := &ReceiveOffer{
			SessionID:  existing.id,
			Nonce:      existing.nonce,
			Viewer:     existing.viewer,
			Target:     existing.target,
			Generation: existing.gen,
			SDP:        existing.offerSDP,
		}
		r.mu.Unlock()
		return offer, nil
	}
	// 注意：这里不摘除/关闭同一观看者的旧会话。旧 lease 在新接收传输建成并原子
	// 换入之前必须保持可用；否则新 PC 创建失败会把观看者置于「两边都没有」。
	//
	// 发布轨此刻可能已经 attach（正常顺序：screen.start → 发布轨 attach → 观看者 watch），
	// 也可能还没到（watch 早于发布轨 attach，实测早约 1.5s）。codec 已知就直接用它建
	// 本端轨；未知时先建占位轨，等发布轨 attach 时在**写出第一包之前**换轨。
	pubCodec, codecKnown := r.publisherCodecLocked(target)
	r.mu.Unlock()

	nonce, err := makeReceiveNonce()
	if err != nil {
		return nil, err
	}

	pc, err := r.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return nil, fmt.Errorf("new receive peer connection: %w", err)
	}
	closeOnError := func(err error) (*ReceiveOffer, error) {
		_ = pc.Close()
		return nil, err
	}

	// 每会话独立的本端轨：codec 必须是**发布者真实 codec**。Pion 写出时用本轨绑定的
	// payload type 覆盖每个包头的 PT（track_local_static.go 的 TrackLocalStaticRTP.writeRTP），
	// 轨 codec 一旦与发布者 payload 的真实 codec 不一致，观看端就会按错误的解码器解包：
	// 传输连通、RTP 也在增长，但解不出帧。发布轨尚未 attach 时先用占位轨（见下方
	// prepareReceivesForPublisherLocked，attach 时在写第一包之前换掉）。
	trackCodec := receivePlaceholderCodec()
	if codecKnown {
		trackCodec = pubCodec
	}
	track, err := webrtc.NewTrackLocalStaticRTP(trackCodec, receiveTrackID(sessionID), "partyup-relay-video")
	if err != nil {
		return closeOnError(fmt.Errorf("new receive track: %w", err))
	}
	tr, err := pc.AddTransceiverFromTrack(
		track,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly},
	)
	if err != nil {
		return closeOnError(fmt.Errorf("add receive transceiver: %w", err))
	}

	offer, err := pc.CreateOffer(nil)
	if err != nil {
		return closeOnError(fmt.Errorf("create receive offer: %w", err))
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(offer); err != nil {
		return closeOnError(fmt.Errorf("set receive local description: %w", err))
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		return closeOnError(errors.New("receive local description is nil after gathering"))
	}

	session := &videoReceive{
		id:       sessionID,
		nonce:    nonce,
		viewer:   viewer,
		target:   target,
		gen:      generation,
		pc:       pc,
		sender:   tr.Sender(),
		tr:       tr,
		track:    track,
		offerSDP: local.SDP,
	}
	// 原子换入：房间仍开着、同 ID 仍未被别人占用，才把新会话登记为观看者的当前
	// 接收传输；旧会话在同一把锁内摘除，锁外关闭（Close 不持 r.mu）。
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return closeOnError(errors.New("video room is closed"))
	}
	if existing := r.receives[sessionID]; existing != nil && !existing.closed {
		// recvMu 已串行化 OpenReceive，正常到不了这里；防御性分支保持无覆盖语义。
		r.mu.Unlock()
		return closeOnError(fmt.Errorf("%w: session_id was registered concurrently", ErrReceiveSession))
	}
	var previous *videoReceive
	if oldID := r.receiveByViewer[viewer]; oldID != "" && oldID != sessionID {
		previous = r.receives[oldID]
		if previous != nil {
			delete(r.receives, oldID)
			previous.closed = true
		}
	}
	r.receives[sessionID] = session
	r.receiveByViewer[viewer] = sessionID
	needKeyframe := false
	if pub := r.pubs[target]; pub != nil {
		// 发布轨已存在：把本会话的本端轨换成发布者真实 codec（OpenReceive 的第一次
		// 加锁与这里之间发布轨可能刚好 attach），再标记 ready；真正转发由发布者的读
		// 循环按会话写入（每会话独立 TrackLocalStaticRTP，便于按会话计数与隔离）。
		r.bindReceiveTrackLocked(session, publicationCodec(pub))
		session.hasTrack = pub.track != nil
		// 发布流已经在跑（观看者从半截 GOP 开始收），必须请发布者立刻编一个关键帧。
		needKeyframe = true
	}
	r.mu.Unlock()
	// 锁外发 RTCP：WriteRTCP 会走发送路径，不在 r.mu 内做。
	if needKeyframe {
		r.requestPublisherKeyframe(target)
	}
	// 新会话已经落库，旧 lease 至此才关闭：切换期间不会出现「两个接收器同时活」，
	// 也不会出现「旧的先没了、新的没建成」。
	if previous != nil {
		r.closeReceivePC(previous)
	}

	r.log.Info("receive session opened", "viewer", viewer, "target", target,
		"session", sessionID, "generation", generation, "sdp_bytes", len(local.SDP),
		"track_codec", session.track.Codec().MimeType,
		"publisher_codec", pubCodec.MimeType, "publisher_codec_known", codecKnown)
	return &ReceiveOffer{
		SessionID:  sessionID,
		Nonce:      nonce,
		Viewer:     viewer,
		Target:     target,
		Generation: generation,
		SDP:        local.SDP,
	}, nil
}

// HandleReceiveSignal 把 answer/candidate 应用到该观看者当前的接收会话。
//
// 校验顺序：会话存在且未关闭 → 属于该观看者 → nonce 匹配 → kind 允许。
// 任何一项不满足都返回 ErrReceiveSession/错误，调用方必须明确拒绝；绝不把
// 旧会话的信令交给新会话。
func (r *VideoRoom) HandleReceiveSignal(viewer, sessionID, nonce, kind string, payload []byte) error {
	if sessionID == "" {
		return fmt.Errorf("%w: session_id is required", ErrReceiveSession)
	}
	r.recvMu.Lock()
	defer r.recvMu.Unlock()

	r.mu.Lock()
	session := r.receives[sessionID]
	if session == nil || session.closed || session.viewer != viewer {
		r.mu.Unlock()
		return ErrReceiveSession
	}
	if nonce == "" || subtle.ConstantTimeCompare([]byte(nonce), []byte(session.nonce)) != 1 {
		r.mu.Unlock()
		return fmt.Errorf("%w: session nonce mismatch", ErrReceiveSession)
	}
	pc := session.pc
	r.mu.Unlock()

	switch kind {
	case "answer":
		var msg struct {
			Type string `json:"type"`
			SDP  string `json:"sdp"`
		}
		if err := decodeSignalPayload(payload, &msg); err != nil {
			return err
		}
		if msg.SDP == "" {
			return errors.New("receive answer.sdp is empty")
		}
		if err := pc.SetRemoteDescription(webrtc.SessionDescription{
			Type: webrtc.SDPTypeAnswer,
			SDP:  msg.SDP,
		}); err != nil {
			return fmt.Errorf("set receive remote answer: %w", err)
		}
		// 诊断：记录观看端 answer 里 video m-line 真正接受了哪些 codec/PT。
		// 这是「服务端轨写出的 PT 是否被观看端接受」的直接证据。
		r.logReceiveAnswerCodecs(sessionID, msg.SDP)
		r.log.Info("receive answer applied", "viewer", viewer, "session", sessionID)
		// 观看端此刻开始解码，但屏幕共享编码器不会定时编关键帧：必须由 SFU 向发布者
		// 请求关键帧，否则中途加入者永远解不出第一帧。
		go r.nudgeReceiveKeyframes(sessionID)
		// 观看端后续的 PLI/FIR 也要转给发布者（跨 PC 的 RTCP 不会自动转发）。
		r.mu.Lock()
		receiveTransceiver := session.tr
		r.mu.Unlock()
		go r.forwardReceiveRTCP(sessionID, receiveTransceiver)
		// 决定性诊断：10s 后打印该接收会话 PC 的真实发送侧统计（outbound-rtp /
		// transport）。WriteRTP 成功只代表「写调用发生」，不代表 sender 有 binding、
		// 包真的离开了服务端。
		go r.logReceiveSendStats(sessionID, 10*time.Second)
		return nil
	case "candidate":
		var msg struct {
			Candidate     string  `json:"candidate"`
			SDPMid        *string `json:"sdpMid"`
			SDPMLineIndex *uint16 `json:"sdpMLineIndex"`
		}
		if err := decodeSignalPayload(payload, &msg); err != nil {
			return err
		}
		if msg.Candidate == "" {
			return errors.New("receive candidate is empty")
		}
		return pc.AddICECandidate(webrtc.ICECandidateInit{
			Candidate:     msg.Candidate,
			SDPMid:        msg.SDPMid,
			SDPMLineIndex: msg.SDPMLineIndex,
		})
	case "offer":
		return errors.New("screen receive: server is the offerer, client must answer, not offer")
	default:
		return fmt.Errorf("unsupported signal kind %q for receive session", kind)
	}
}

// CloseReceive 关闭该观看者的接收会话（sessionID 为空时关闭它的当前会话）。
// 只销毁接收 PC，不碰发布/语音。返回是否真的关闭了一个会话。
func (r *VideoRoom) CloseReceive(viewer, sessionID string) bool {
	r.recvMu.Lock()
	defer r.recvMu.Unlock()

	r.mu.Lock()
	if sessionID == "" {
		sessionID = r.receiveByViewer[viewer]
	}
	session := r.receives[sessionID]
	if session == nil || session.closed || session.viewer != viewer {
		r.mu.Unlock()
		return false
	}
	delete(r.receives, sessionID)
	if r.receiveByViewer[viewer] == sessionID {
		delete(r.receiveByViewer, viewer)
	}
	session.closed = true
	r.mu.Unlock()

	r.closeReceivePC(session)
	r.log.Info("receive session closed", "viewer", viewer, "session", sessionID)
	return true
}

// CloseReceivesForViewer 清理该观看者的所有接收会话（断线/取消）。
func (r *VideoRoom) CloseReceivesForViewer(viewer string) int {
	r.recvMu.Lock()
	defer r.recvMu.Unlock()
	return r.closeReceivesLocked(func(s *videoReceive) bool { return s.viewer == viewer })
}

// CloseReceivesForTarget 在发布停止时关闭所有指向它的接收会话（否则观看者会
// 一直停在最后一帧）。返回关闭数量。
func (r *VideoRoom) CloseReceivesForTarget(target string) int {
	r.recvMu.Lock()
	defer r.recvMu.Unlock()
	return r.closeReceivesLocked(func(s *videoReceive) bool { return s.target == target })
}

// CloseAllReceives 关闭房间内所有接收会话（房间 Close 用）。
func (r *VideoRoom) CloseAllReceives() int {
	r.recvMu.Lock()
	defer r.recvMu.Unlock()
	return r.closeReceivesLocked(func(*videoReceive) bool { return true })
}

// closeReceivesLocked 在 recvMu 内按谓词摘除并关闭；调用方必须持有 recvMu。
func (r *VideoRoom) closeReceivesLocked(match func(*videoReceive) bool) int {
	r.mu.Lock()
	var closing []*videoReceive
	for id, session := range r.receives {
		if session.closed || !match(session) {
			continue
		}
		delete(r.receives, id)
		if r.receiveByViewer[session.viewer] == id {
			delete(r.receiveByViewer, session.viewer)
		}
		session.closed = true
		closing = append(closing, session)
	}
	r.mu.Unlock()
	for _, session := range closing {
		r.closeReceivePC(session)
	}
	return len(closing)
}

// nudgeReceiveKeyframes 在接收会话刚建立时，按退避向发布者请求关键帧（有限次数）。
//
// 单发一次 PLI 可能在发布者 PC 尚未连上/编解码器尚未就绪时丢掉；观看端在拿到关键帧
// 之前会一直「等待首帧」，所以这里按 ~0.2s/0.8s/2s/4s 退避重试（总量有界，会话关闭
// 立即停止）。请求的是**发布者**的关键帧：屏幕共享编码器不会定时编关键帧。
func (r *VideoRoom) nudgeReceiveKeyframes(sessionID string) {
	for _, delay := range []time.Duration{
		200 * time.Millisecond,
		800 * time.Millisecond,
		2000 * time.Millisecond,
		4000 * time.Millisecond,
	} {
		time.Sleep(delay)
		r.mu.Lock()
		session := r.receives[sessionID]
		if session == nil || session.closed {
			r.mu.Unlock()
			return
		}
		target := session.target
		r.mu.Unlock()
		r.requestPublisherKeyframe(target)
	}
}

// forwardReceiveRTCP 读取观看端发来的 RTCP（PLI/FIR），把关键帧请求转给发布者。
//
// 解码端丢包/缺关键帧时会发 PLI；Pion 不会跨 PeerConnection 转发 RTCP，因此必须由
// SFU 转。读循环在 PC 关闭时因读错误自然结束。
func (r *VideoRoom) forwardReceiveRTCP(sessionID string, tr *webrtc.RTPTransceiver) {
	if tr == nil || tr.Receiver() == nil {
		return
	}
	for {
		pkts, _, err := tr.Receiver().ReadRTCP()
		if err != nil {
			return
		}
		r.mu.Lock()
		session := r.receives[sessionID]
		target := ""
		if session != nil && !session.closed {
			target = session.target
		}
		r.mu.Unlock()
		if target == "" {
			return
		}
		for _, pkt := range pkts {
			switch pkt.(type) {
			case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
				r.requestPublisherKeyframe(target)
			}
		}
	}
}

// closeReceivePC 只在 recvMu 内调用；PC.Close 不持 r.mu。closeOnce 保证同一会话的
// PC 恰好关闭一次（换台与断线/StopShare/房间关闭可能先后命中同一条会话）。
func (r *VideoRoom) closeReceivePC(session *videoReceive) {
	if session == nil || session.pc == nil {
		return
	}
	session.closeOnce.Do(func() {
		if err := session.pc.Close(); err != nil {
			r.log.Warn("close receive pc", "session", session.id, "err", err)
		}
	})
}

// ReceiveSession 返回某会话的快照（测试/诊断）。
func (r *VideoRoom) ReceiveSession(sessionID string) (ReceiveSessionInfo, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	session := r.receives[sessionID]
	if session == nil {
		return ReceiveSessionInfo{}, false
	}
	codec := ""
	if session.track != nil {
		codec = session.track.Codec().MimeType
	}
	return ReceiveSessionInfo{
		SessionID:  session.id,
		Nonce:      session.nonce,
		Viewer:     session.viewer,
		Target:     session.target,
		Generation: session.gen,
		HasTrack:   session.hasTrack,
		Closed:     session.closed,
		Codec:      codec,
	}, true
}

// ReceivePacketsWritten 返回该会话创建后被写入的 RTP 包数（回归/诊断）。
// 只增不减；旧会话关闭后不再增长，新会话从 0 开始。
func (r *VideoRoom) ReceivePacketsWritten(sessionID string) uint64 {
	r.mu.Lock()
	session := r.receives[sessionID]
	r.mu.Unlock()
	if session == nil {
		return 0
	}
	return session.packetsWritten.Load()
}

// activeReceiveCount 返回当前活跃接收会话数（测试/容量日志）。
func (r *VideoRoom) activeReceiveCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	count := 0
	for _, session := range r.receives {
		if !session.closed {
			count++
		}
	}
	return count
}

// writeToReceivesLocked 把发布者的一个 RTP 包写到所有指向它的活跃接收会话。
// 调用方必须持有 r.mu（与旧槽位转发一样，锁内只做非阻塞 WriteRTP）。
// 每个会话有自己的 TrackLocalStaticRTP，因此会话之间不会共享 SSRC/发送状态；
// 关闭后的会话已从 receives 摘除，不会再被写入。
func (r *VideoRoom) writeToReceivesLocked(target string, pkt *rtp.Packet) {
	for _, session := range r.receives {
		if session.closed || session.target != target {
			continue
		}
		session.hasTrack = true
		if err := session.track.WriteRTP(pkt); err != nil {
			// 会话刚关闭/发送缓冲满/轨未绑定：丢这一个包，不影响其它会话。
			// 第一次失败一定记日志：轨 codec 与 answer 协商表不匹配时 Pion 的
			// WriteRTP 会返回 ErrUnsupportedCodec（无 binding 就静默丢弃），
			// 表现正是「PC connected、0 帧」，必须能在服务端日志里看见。
			if session.writeErrors.Add(1) == 1 {
				r.log.Warn("screen receive write failed", "target", target, "session", session.id,
					"codec", session.track.Codec().MimeType, "seq", pkt.SequenceNumber, "err", err)
			}
			continue
		}
		if n := session.packetsWritten.Add(1); n == 1 || n%500 == 0 {
			r.log.Info("screen receive relay packets", "target", target, "session", session.id,
				"count", n, "seq", pkt.SequenceNumber, "codec", session.track.Codec().MimeType)
		}
	}
}

// videoCodecMap 抽出 SDP 里 video m-line 的端口与 rtpmap 列表（如
// ["96 VP8/90000", "106 H264/90000"]）。用 pion/sdp 解析，不手写字符串切分。
func videoCodecMap(rawSDP string) (int, []string, error) {
	parsed := &sdp.SessionDescription{}
	if err := parsed.Unmarshal([]byte(rawSDP)); err != nil {
		return 0, nil, err
	}
	for _, media := range parsed.MediaDescriptions {
		if media.MediaName.Media != "video" {
			continue
		}
		codecs := make([]string, 0, len(media.Attributes))
		for _, attr := range media.Attributes {
			if attr.Key == "rtpmap" {
				codecs = append(codecs, attr.Value)
			}
		}
		return media.MediaName.Port.Value, codecs, nil
	}
	return 0, nil, errors.New("sdp has no video m-line")
}

// logReceiveAnswerCodecs 记录观看端 answer 里 video m-line 真正接受了哪些 codec/PT。
//
// 为什么这条日志是「写出的 PT 是否被接受」的直接证据：Pion 的
// TrackLocalStaticRTP 用本轨 codec 在 **media engine** 里的 PT 写包头；观看端只接受
// 它 answer 里列出的 PT。answer 若不含该 PT（例如只列 H264 而服务端绑了 VP8），
// 包会被观看端丢弃：PC connected、服务端 RTP 在涨、观看端 0 帧。
func (r *VideoRoom) logReceiveAnswerCodecs(sessionID, rawSDP string) {
	port, codecs, err := videoCodecMap(rawSDP)
	if err != nil {
		r.log.Warn("receive answer sdp parse failed", "session", sessionID, "err", err)
		return
	}
	r.log.Info("receive answer video codecs", "session", sessionID,
		"port", port, "count", len(codecs), "codecs", strings.Join(codecs, " | "))
}

// markReceivesHasTrackLocked 在发布轨首次出现时更新会话快照（诊断用）。
func (r *VideoRoom) markReceivesHasTrackLocked(target string) {
	for _, session := range r.receives {
		if !session.closed && session.target == target {
			session.hasTrack = true
		}
	}
}

// ------------------------------------------------------------------
// 发布轨 codec → 接收会话本端轨
// ------------------------------------------------------------------

// receivePlaceholderCodec 是发布轨尚未 attach 时接收会话本端轨的临时 codec。
//
// 它只参与 SDP 协商与 Pion 的轨绑定；发布轨 attach 时由 bindReceiveTrackLocked
// 换成发布者真实 codec，且换轨发生在发布者读循环启动（写出第一包）之前，所以
// 占位 codec 的包不可能被发出去。
func receivePlaceholderCodec() webrtc.RTPCodecCapability {
	return webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000}
}

// receiveTrackID 是接收会话本端轨的稳定 id（换轨时保持不变）。
func receiveTrackID(sessionID string) string { return "partyup-rx-" + sessionID }

// sameRTPCodec 判断两个 codec 能力是否同一路媒体格式：mime + 时钟频率 + SDP fmtp。
// fmtp 必须参与比较：H264 的不同 profile / packetization-mode 对解码器是不同格式，
// 只比 mime 会把 VP8 之外的错误掩盖成「看起来一致」。
func sameRTPCodec(a, b webrtc.RTPCodecCapability) bool {
	return strings.EqualFold(a.MimeType, b.MimeType) &&
		a.ClockRate == b.ClockRate &&
		strings.EqualFold(strings.TrimSpace(a.SDPFmtpLine), strings.TrimSpace(b.SDPFmtpLine))
}

// publicationCodec 返回发布轨的真实 codec。以 videoPublication.codec（onTrack 里
// 由 remote.Codec().RTPCodecCapability 写入）为准，兜底读轨自身，二者在正常路径上
// 完全相同；这样测试里直接塞 pubs 的假发布轨（codec 未填）也能工作。
func publicationCodec(pub *videoPublication) webrtc.RTPCodecCapability {
	if pub == nil {
		return webrtc.RTPCodecCapability{}
	}
	if pub.codec.MimeType != "" {
		return pub.codec
	}
	if pub.track != nil {
		return pub.track.Codec()
	}
	return webrtc.RTPCodecCapability{}
}

// publisherCodecLocked 返回目标成员当前发布轨的真实 codec（调用方必须持有 r.mu）。
// 这是「发布者 codec 表」的查询入口：发布轨不存在时 ok=false，调用方必须把它当作
// 「codec 未知」处理，不能拿占位 codec 冒充。
func (r *VideoRoom) publisherCodecLocked(target string) (webrtc.RTPCodecCapability, bool) {
	codec := publicationCodec(r.pubs[target])
	if codec.MimeType == "" {
		return webrtc.RTPCodecCapability{}, false
	}
	return codec, true
}

// PublisherCodec 返回目标成员当前发布轨的真实 codec（测试/诊断；并发安全）。
func (r *VideoRoom) PublisherCodec(target string) (webrtc.RTPCodecCapability, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.publisherCodecLocked(target)
}

// ReceiveSessionCodec 返回该接收会话当前本端轨的 codec（测试/诊断；并发安全）。
func (r *VideoRoom) ReceiveSessionCodec(sessionID string) (webrtc.RTPCodecCapability, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	session := r.receives[sessionID]
	if session == nil || session.track == nil {
		return webrtc.RTPCodecCapability{}, false
	}
	return session.track.Codec(), true
}

// bindReceiveTrackLocked 把接收会话的本端轨换成发布者真实 codec 的轨。
//
// 调用方必须持有 r.mu，并且必须保证此刻发布者的读循环还没有开始写包（生产路径
// 里它只在 onTrack 启动读循环之前被调用），因此不会出现「旧 codec 的轨写出新
// codec 的 payload」的窗口。返回是否真的换了轨。
func (r *VideoRoom) bindReceiveTrackLocked(session *videoReceive, codec webrtc.RTPCodecCapability) bool {
	if session == nil || session.closed || codec.MimeType == "" {
		return false
	}
	if session.track != nil && sameRTPCodec(session.track.Codec(), codec) {
		session.hasTrack = true
		return false
	}
	track, err := webrtc.NewTrackLocalStaticRTP(codec, receiveTrackID(session.id), "partyup-relay-video")
	if err != nil {
		r.log.Error("new receive track for publisher codec", "session", session.id,
			"codec", codec.MimeType, "err", err)
		return false
	}
	if session.sender != nil {
		// 与旧槽位路径同一个 Pion 机制：ReplaceTrack 会用新轨的 codec 重新绑定
		// payload type（TrackLocalStaticRTP.writeRTP 写出时用绑定 PT），所以换轨后
		// 包头 PT 与 payload 的 codec 重新一致。
		if err := session.sender.ReplaceTrack(track); err != nil {
			r.log.Error("replace receive track", "session", session.id,
				"codec", codec.MimeType, "err", err)
			return false
		}
	}
	session.track = track
	session.hasTrack = true
	return true
}

// prepareReceivesForPublisherLocked 在发布轨 attach 时，把所有指向该发布者的接收
// 会话的本端轨换成发布者真实 codec，并把「发布者 codec vs 本端轨 codec」写进日志。
//
// 为什么必须在读循环启动之前：Pion 的 TrackLocalStaticRTP.writeRTP 会用**绑定时的
// payload type** 覆盖每个包的包头 PT（track_local_static.go 的 writeRTP）；本地轨
// codec 一旦与 payload 的真实 codec 不一致，观看端会按包头 PT 指向的错误解码器解包，
// 表现正是「传输连通、RTP 计数增长、却 0 帧」（本缺陷的现场）。
//
// 调用方必须持有 r.mu，且在 onTrack 启动读循环之前调用。
func (r *VideoRoom) prepareReceivesForPublisherLocked(target string, codec webrtc.RTPCodecCapability) {
	for id, session := range r.receives {
		if session.closed || session.target != target {
			continue
		}
		replaced := r.bindReceiveTrackLocked(session, codec)
		trackCodec := webrtc.RTPCodecCapability{}
		if session.track != nil {
			trackCodec = session.track.Codec()
		}
		r.log.Info("screen receive track codec", "target", target, "session", id,
			"publisher_codec", codec.MimeType, "track_codec", trackCodec.MimeType,
			"match", sameRTPCodec(trackCodec, codec), "replaced", replaced)
	}
}

// logReceiveSendStats 在 delay 后打印该接收会话 PC 的真实发送侧统计。
//
// 为什么不能用 WriteRTP 返回值判断「发出去了」：TrackLocalStaticRTP.writeRTP 在没有
// binding 时 writeErrs 为空并返回 nil —— 「写入成功」不等于「sender 有 binding、
// 包真的离开服务端」。outbound-rtp.packetsSent>0 / transport.bytesSent 才是决定性证据：
//   - packetsSent>0 ⇒ 服务端真在发，0 帧的原因在客户端收/解/渲染；
//   - packetsSent==0 ⇒ 服务端没有绑定或没连上，问题还在服务端这一层。
func (r *VideoRoom) logReceiveSendStats(sessionID string, delay time.Duration) {
	time.Sleep(delay)

	r.mu.Lock()
	session := r.receives[sessionID]
	if session == nil || session.closed || session.pc == nil {
		r.mu.Unlock()
		return
	}
	pc := session.pc
	written := session.packetsWritten.Load()
	writeErrs := session.writeErrors.Load()
	codec := ""
	if session.track != nil {
		codec = session.track.Codec().MimeType
	}
	r.mu.Unlock()

	// GetStats 可能会等对端 RTCP 报告（Pion 内部有超时），绝不能在 r.mu 内调用。
	for id, stat := range pc.GetStats() {
		switch s := stat.(type) {
		case webrtc.OutboundRTPStreamStats:
			r.log.Info("receive send stats", "session", sessionID, "stat_id", id,
				"kind", s.Kind, "ssrc", uint32(s.SSRC),
				"packets_sent", s.PacketsSent, "bytes_sent", s.BytesSent,
				"track_writes", written, "write_errors", writeErrs, "codec", codec)
		case webrtc.TransportStats:
			r.log.Info("receive transport stats", "session", sessionID,
				"packets_sent", s.PacketsSent, "packets_received", s.PacketsReceived,
				"bytes_sent", s.BytesSent, "bytes_received", s.BytesReceived)
		}
	}
}

// decodeSignalPayload 解码客户端透传的信令信封（与旧路径同一套结构）。
func decodeSignalPayload(payload []byte, out any) error {
	if err := json.Unmarshal(payload, out); err != nil {
		return fmt.Errorf("decode receive signal: %w", err)
	}
	return nil
}
