// Package sfu 的**屏幕共享视频中继**：与音频 Room 平级，但**独立 PeerConnection**。
//
// 为什么单独一个 room（而不是在音频 PC 上加视频 m-line）：
//   - 客户端把语音与屏幕拆成了两个 PeerConnection（voice_session_ / screen_session_），
//     一个服务端 PC 无法同时被两个客户端 PC 应答，所以服务端也必须一成员两 PC；
//   - 两套生命周期互不牵连：停共享只拆视频 room 的转发，语音 room 的 PC/槽位完全不动。
//
// 结构与音频完全同构（见 room.go 顶部注释）：
//   - 每成员的视频 PC 预协商 1 条 recvonly 视频 m-line（上传自己的屏幕）
//   - MaxScreenShares 条 sendonly 视频槽（看别人的屏幕，占位轨 id partyup-video-slot-<n>）；
//   - 共享者不换 PC：它往自己的 recvonly 视频 m-line 上传，服务端 OnTrack 收到后
//     ReplaceTrack 进其他成员的空槽，零重协商；
//   - 观看者中途加入：offer 发出去后再把已有共享回填进它的空槽（约束「已有共享要能直接挂上」）。
package sfu

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sync"

	"github.com/pion/webrtc/v4"
)

// MaxScreenShares 是单频道同时可见的屏幕共享路数上限（v1 规模目标：最多 2 路）。
// 首轮只验收 1 路，但槽位结构按 2 路预留（多开槽不增加重协商成本，只是 SDP 多两行）。
const MaxScreenShares = 2

// ScreenSlotTrackID 是第 index 个视频槽占位轨的 id。占位轨参与 SDP 协商，
// 因此客户端看到的远端视频 track id 稳定为 "partyup-video-slot-<n>"。
func ScreenSlotTrackID(index int) string {
	return fmt.Sprintf("partyup-video-slot-%02d", index)
}

type videoPublication struct {
	owner string
	track *webrtc.TrackLocalStaticRTP
}

type videoPeer struct {
	id           string
	pc           *webrtc.PeerConnection
	slots        []*webrtc.RTPSender
	placeholders []*webrtc.TrackLocalStaticRTP
	bound        map[string]int // 共享者 memberID → 占用槽位下标
}

// VideoRoom 是**一频道**的屏幕共享视频中继房间。
type VideoRoom struct {
	channelID string
	log       *slog.Logger
	api       *webrtc.API
	send      SignalSender
	onChange  func() // 视频槽位映射变化时通知 hub（可为 nil）

	mu         sync.Mutex
	peers      map[string]*videoPeer
	pubs       map[string]*videoPublication
	slotOwners []string // 槽位序号 → 共享者 memberID（"" = 空）
	done       bool
}

// NewVideo 构造一个视频房间。
func NewVideo(channelID string, log *slog.Logger, send SignalSender, onChange func()) (*VideoRoom, error) {
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		return nil, fmt.Errorf("register default codecs: %w", err)
	}
	api := webrtc.NewAPI(webrtc.WithMediaEngine(media))
	if log == nil {
		log = slog.Default()
	}
	return &VideoRoom{
		channelID:  channelID,
		log:        log.With("sfu-video", channelID),
		api:        api,
		send:       send,
		onChange:   onChange,
		peers:      make(map[string]*videoPeer, 4),
		pubs:       make(map[string]*videoPublication, 2),
		slotOwners: make([]string, MaxScreenShares),
	}, nil
}

// Join 为成员建立视频 PC 并主动发 offer（幂等）。
//
// 只有 --screen-mode=relay 时 hub 才会调用它：direct 模式下不能凭空多建一条到服务端的
// 视频连接（用户以为屏幕是 P2P，实际却连服务器，既是资源浪费也是隐私问题）。
func (r *VideoRoom) Join(memberID string) error {
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return errors.New("video room is closed")
	}
	if _, exists := r.peers[memberID]; exists {
		r.mu.Unlock()
		return nil
	}
	p := &videoPeer{id: memberID, bound: make(map[string]int, MaxScreenShares)}
	r.peers[memberID] = p
	r.mu.Unlock()

	pc, err := r.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		r.removePeer(memberID)
		return fmt.Errorf("new video peer connection: %w", err)
	}
	p.pc = pc
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		r.onTrack(memberID, track)
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		r.log.Info("video peer state", "member", memberID, "state", state.String())
	})

	// 1) 服务端收该成员的屏幕：recvonly 视频 m-line（共享者往这里上传）。
	if _, err := pc.AddTransceiverFromKind(
		webrtc.RTPCodecTypeVideo,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionRecvonly},
	); err != nil {
		_ = pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("add screen recv transceiver: %w", err)
	}

	// 2) 预协商「看别人屏幕」的视频槽：sendonly 视频 m-line × MaxScreenShares。
	//    占位轨 id 固定为 partyup-video-slot-<n>，与音频槽同一套「先占位后替换」思路。
	//    占位轨用 H264：客户端媒体核心优先硬件 H264；VP8 回落在 SFU 视频首轮不支持（见文档）。
	for i := 0; i < MaxScreenShares; i++ {
		placeholder, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
			MimeType:  webrtc.MimeTypeH264,
			ClockRate: 90000,
		}, ScreenSlotTrackID(i), "partyup-relay-video")
		if err != nil {
			_ = pc.Close()
			r.removePeer(memberID)
			return fmt.Errorf("new video slot placeholder %d: %w", i, err)
		}
		tr, err := pc.AddTransceiverFromTrack(
			placeholder,
			webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly},
		)
		if err != nil {
			_ = pc.Close()
			r.removePeer(memberID)
			return fmt.Errorf("add video slot %d: %w", i, err)
		}
		p.slots = append(p.slots, tr.Sender())
		p.placeholders = append(p.placeholders, placeholder)
	}

	// 3) 服务端 offer（非 trickle，候选进 SDP；与音频一致）。
	offer, err := pc.CreateOffer(nil)
	if err != nil {
		_ = pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("create video offer: %w", err)
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(offer); err != nil {
		_ = pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("set video local description: %w", err)
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		_ = pc.Close()
		r.removePeer(memberID)
		return errors.New("video local description is nil after gathering")
	}

	r.send(memberID, "offer", mustJSON(map[string]any{
		"type":       "offer",
		"sdp":        local.SDP,
		"session_id": "relay-video/" + memberID,
	}))
	r.log.Info("video offer sent", "member", memberID,
		"slots", len(p.slots), "shares", len(r.pubs), "sdp_bytes", len(local.SDP))

	// 4) offer 发出后再把**已有共享**回填进它的槽位（约束：观看者中途加入要能看到已开始的共享）。
	r.mu.Lock()
	for owner, pub := range r.pubs {
		if owner == memberID {
			continue
		}
		r.bindLocked(p, owner, pub.track)
	}
	r.mu.Unlock()
	return nil
}

// HandleSignal 处理视频 PC 的 answer / candidate。
func (r *VideoRoom) HandleSignal(memberID, kind string, payload []byte) error {
	r.mu.Lock()
	p := r.peers[memberID]
	r.mu.Unlock()
	if p == nil || p.pc == nil {
		return ErrNotInRoom
	}
	switch kind {
	case "answer":
		var msg struct {
			Type string `json:"type"`
			SDP  string `json:"sdp"`
		}
		if err := json.Unmarshal(payload, &msg); err != nil {
			return fmt.Errorf("decode video answer: %w", err)
		}
		if msg.SDP == "" {
			return errors.New("video answer.sdp is empty")
		}
		if err := p.pc.SetRemoteDescription(webrtc.SessionDescription{
			Type: webrtc.SDPTypeAnswer,
			SDP:  msg.SDP,
		}); err != nil {
			return fmt.Errorf("set video remote answer: %w", err)
		}
		r.log.Info("video answer applied", "member", memberID, "sdp_bytes", len(msg.SDP))
		return nil
	case "candidate":
		var msg struct {
			Candidate     string  `json:"candidate"`
			SDPMid        *string `json:"sdpMid"`
			SDPMLineIndex *uint16 `json:"sdpMLineIndex"`
		}
		if err := json.Unmarshal(payload, &msg); err != nil {
			return fmt.Errorf("decode video candidate: %w", err)
		}
		if msg.Candidate == "" {
			return errors.New("video candidate is empty")
		}
		return p.pc.AddICECandidate(webrtc.ICECandidateInit{
			Candidate:     msg.Candidate,
			SDPMid:        msg.SDPMid,
			SDPMLineIndex: msg.SDPMLineIndex,
		})
	case "offer":
		return errors.New("screen relay: server is the offerer, client must answer, not offer")
	default:
		return fmt.Errorf("unsupported signal kind %q for video relay", kind)
	}
}

// StopShare 在客户端 screen.stop 时调用：立刻摘掉该共享者占用的槽位并清空，
// 否则观看者会一直收到最后一帧（看起来像画面卡死）。约束 #2。
func (r *VideoRoom) StopShare(memberID string) {
	r.mu.Lock()
	pub, ok := r.pubs[memberID]
	if !ok {
		r.mu.Unlock()
		return
	}
	delete(r.pubs, memberID)
	r.releaseSlotLocked(memberID)
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
	}
	r.mu.Unlock()
	_ = pub
	r.log.Info("screen share stopped", "member", memberID)
	r.notifyChange()
}

// Leave 让成员离开视频房间：关 PC、清掉它的共享与它占的槽位。
func (r *VideoRoom) Leave(memberID string) {
	r.mu.Lock()
	p := r.peers[memberID]
	delete(r.peers, memberID)
	if _, ok := r.pubs[memberID]; ok {
		delete(r.pubs, memberID)
		r.releaseSlotLocked(memberID)
	}
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
	}
	empty := len(r.peers) == 0
	if empty {
		r.done = true
	}
	r.mu.Unlock()

	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
	r.log.Info("video member left", "member", memberID, "empty", empty)
}

// Close 关闭房间与所有连接。
func (r *VideoRoom) Close() {
	r.mu.Lock()
	peers := make([]*videoPeer, 0, len(r.peers))
	for _, p := range r.peers {
		peers = append(peers, p)
	}
	r.peers = make(map[string]*videoPeer)
	r.pubs = make(map[string]*videoPublication)
	r.slotOwners = make([]string, MaxScreenShares)
	r.done = true
	r.mu.Unlock()

	for _, p := range peers {
		if p.pc != nil {
			_ = p.pc.Close()
		}
	}
	r.log.Info("video room closed")
}

// Members 返回视频房间成员（测试/日志用）。
func (r *VideoRoom) Members() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, 0, len(r.peers))
	for id := range r.peers {
		out = append(out, id)
	}
	return out
}

// Publishers 返回正在共享的成员（测试/日志用）。
func (r *VideoRoom) Publishers() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, 0, len(r.pubs))
	for id := range r.pubs {
		out = append(out, id)
	}
	return out
}

// Slots 返回「视频槽位序号 → 共享者」映射。
func (r *VideoRoom) Slots() []SlotBinding {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]SlotBinding, 0, MaxScreenShares)
	for i, owner := range r.slotOwners {
		if owner == "" {
			continue
		}
		out = append(out, SlotBinding{Index: i, MemberID: owner})
	}
	return out
}

// ------------------------------------------------------------------
// 内部实现
// ------------------------------------------------------------------

// onTrack 在共享者开始上传屏幕时触发：建本地静态 RTP 轨，挂到其他成员的空槽并持续转发。
func (r *VideoRoom) onTrack(memberID string, remote *webrtc.TrackRemote) {
	if remote.Kind() != webrtc.RTPCodecTypeVideo {
		r.log.Warn("ignoring non-video track in video room", "member", memberID, "kind", remote.Kind().String())
		go func() {
			for {
				if _, _, err := remote.ReadRTP(); err != nil {
					return
				}
			}
		}()
		return
	}

	track, err := webrtc.NewTrackLocalStaticRTP(remote.Codec().RTPCodecCapability, "screen-"+memberID, "partyup-relay-video")
	if err != nil {
		r.log.Error("new video forward track", "member", memberID, "err", err)
		return
	}

	pub := &videoPublication{owner: memberID, track: track}
	r.mu.Lock()
	r.pubs[memberID] = pub
	assigned := false
	if slot := r.assignSlotLocked(memberID); slot < 0 {
		r.log.Warn("no free video slot", "member", memberID, "slots", MaxScreenShares)
	} else {
		assigned = true
		r.log.Info("video slot assigned", "member", memberID, "slot", slot,
			"track_id", ScreenSlotTrackID(slot))
	}
	for id, other := range r.peers {
		if id == memberID {
			continue
		}
		r.bindLocked(other, memberID, track)
	}
	subscribers := len(r.peers) - 1
	r.mu.Unlock()

	r.log.Info("screen publisher attached", "member", memberID,
		"codec", remote.Codec().MimeType, "subscribers", subscribers)
	if assigned {
		r.notifyChange()
	}

	go func() {
		for {
			pkt, _, err := remote.ReadRTP()
			if err != nil {
				r.log.Info("screen publisher read ended", "member", memberID, "err", err)
				return
			}
			if err := track.WriteRTP(pkt); err != nil {
				r.log.Info("screen forward ended", "member", memberID, "err", err)
				return
			}
		}
	}()
}

func (r *VideoRoom) bindLocked(p *videoPeer, owner string, track *webrtc.TrackLocalStaticRTP) {
	if _, exists := p.bound[owner]; exists {
		return
	}
	slot := r.slotIndexOfLocked(owner)
	if slot < 0 {
		r.log.Warn("screen publisher has no slot", "member", p.id, "publisher", owner)
		return
	}
	p.bound[owner] = slot
	if err := p.slots[slot].ReplaceTrack(track); err != nil {
		r.log.Error("replace video track", "member", p.id, "publisher", owner, "slot", slot, "err", err)
		delete(p.bound, owner)
		return
	}
	r.log.Info("video slot bound", "member", p.id, "publisher", owner, "slot", slot)
}

func (r *VideoRoom) unbindLocked(p *videoPeer, owner string) {
	slot, ok := p.bound[owner]
	if !ok {
		return
	}
	delete(p.bound, owner)
	if slot >= 0 && slot < len(p.slots) {
		if err := p.slots[slot].ReplaceTrack(nil); err != nil {
			r.log.Warn("clear video slot", "member", p.id, "slot", slot, "err", err)
		}
	}
	r.log.Info("video slot unbound", "member", p.id, "publisher", owner, "slot", slot)
}

func (r *VideoRoom) assignSlotLocked(memberID string) int {
	for i, owner := range r.slotOwners {
		if owner == "" {
			r.slotOwners[i] = memberID
			return i
		}
	}
	return -1
}

func (r *VideoRoom) releaseSlotLocked(memberID string) {
	for i, owner := range r.slotOwners {
		if owner == memberID {
			r.slotOwners[i] = ""
			return
		}
	}
}

func (r *VideoRoom) slotIndexOfLocked(memberID string) int {
	for i, owner := range r.slotOwners {
		if owner == memberID {
			return i
		}
	}
	return -1
}

func (r *VideoRoom) notifyChange() {
	if r.onChange != nil {
		r.onChange()
	}
}

func (r *VideoRoom) removePeer(memberID string) {
	r.mu.Lock()
	p := r.peers[memberID]
	delete(r.peers, memberID)
	r.mu.Unlock()
	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
}

// Refresh 重建某成员的视频 PC 并重新发 offer。
//
// 为什么需要它：客户端媒体核心的被动屏幕会话**必须在本端 answer 之前**把视频轨挂到
// recvonly m-line 上（ABI v2.11 不支持事后重新协商）。而观看者为了能随时看到画面，
// 在订阅时就已经把视频 PC 答完了——等到它真要共享时，那条 m-line 早已协商成「本端不发」。
// 所以共享开始时由服务端重建这个成员的视频 PC 并发新 offer，客户端在收到 offer 前
// 已用 pu_screen_start_ex(passive) 建好带轨的新会话，answer 时轨就挂上了。
//
// 代价：该成员自己正在收的其它共享会短暂中断（v1 同时最多 1 路共享，可接受）。
func (r *VideoRoom) Refresh(memberID string) error {
	r.mu.Lock()
	p := r.peers[memberID]
	delete(r.peers, memberID)
	if _, ok := r.pubs[memberID]; ok {
		delete(r.pubs, memberID)
		r.releaseSlotLocked(memberID)
	}
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
	}
	r.mu.Unlock()

	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
	r.log.Info("video pc refreshed for screen share", "member", memberID)
	return r.Join(memberID)
}
