// Package sfu 是 PartyUp 的服务端语音中继（SFU，Selective Forwarding Unit）。
//
// 设计要点（见 docs/PROTOCOL.md §4.1）：
//
//   - 每个加入语音的成员对**服务端**建一个 PeerConnection（不是与其他客户端互联）。
//     中继模式下 offer 的对方是服务端，客户端只负责 answer。
//   - 每个成员的 PeerConnection 里预协商：
//     1 个 recvonly 音频 m-line（服务端收他的麦克风）+
//     MaxRemoteSlots 个 sendonly 音频槽（服务端把其他成员的声音塞进这些槽）。
//     成员进出只在空槽上 ReplaceTrack，**不触发重协商**——客户端一辈子只处理一次
//     offer/answer，这对 ABI v2「无重协商」的媒体核心很关键。
//   - 服务端 OnTrack 收到某成员的声音后，写进一个 TrackLocalStaticRTP，再把这个 track
//     挂到其他每个成员的空槽上：这就是「A 的声音同时转给 B 和 C」。
//   - 服务端是 offerer：连接建立、成员变化都不需要客户端二次发 offer。
package sfu

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sync"

	"github.com/pion/webrtc/v4"
)

// MaxRemoteSlots 是每个成员能同时收到的其他人音频路数上限。
// 频道上限 12 人，留一路余量便于未来加「屏幕共享音频」之类的轨。
const MaxRemoteSlots = 12

// ErrNotInRoom 表示该成员没有在这个房间里（比如已经 leave 或者从没 join）。
var ErrNotInRoom = errors.New("member is not in this relay room")

// SlotBinding 是「槽位序号 → 发布者」的公开映射（随 voice.state 下发）。
// 槽位序号 == 客户端收到的 sendonly 音频 m-line 顺序（减去第 0 条麦克风 m-line，
// 即第 i 条远端音轨对应槽位 i）。
type SlotBinding struct {
	Index    int
	MemberID string
}

// SlotTrackID 是第 index 个槽位占位轨的 id。占位轨在建 PC 时就参与 SDP 协商，
// 所以它（而不是后来 ReplaceTrack 进去的发布者轨）决定了客户端看到的远端 track id：
// 客户端因此能靠 "partyup-slot-<n>" 稳定地把第 n 条远端音轨映射到槽位 n，
// 不受「后绑定的槽没有 msid、id 随机」影响。
func SlotTrackID(index int) string {
	return fmt.Sprintf("partyup-slot-%02d", index)
}

// SignalSender 把一条信令投递给某个成员。
// hub 注入的实现负责把它包成 media.signal（from=sfu, target=sfu）。
type SignalSender func(memberID, kind, payload string)

type publication struct {
	owner string
	track *webrtc.TrackLocalStaticRTP
}

type peer struct {
	id           string
	pc           *webrtc.PeerConnection
	slots        []*webrtc.RTPSender           // 预协商的 sendonly 槽，下标即槽位
	placeholders []*webrtc.TrackLocalStaticRTP // 槽位占位轨（决定客户端看到的 track id）
	bound        map[string]int                // 发布者 memberID → 占用槽位下标
}

// Room 是**一频道**的语音中继房间。
type Room struct {
	channelID string
	log       *slog.Logger
	api       *webrtc.API
	send      SignalSender

	onChange func() // 槽位映射变化时通知 hub 重播 voice.state（可为 nil）

	mu         sync.Mutex
	peers      map[string]*peer
	pubs       map[string]*publication
	slotOwners []string // 槽位序号 → 发布者 memberID（"" = 空），全房间统一，保证每个成员看到的槽位映射一致
	done       bool
}

// New 构造一个房间。send 负责把服务端产出的信令送给成员；
// onChange 在「槽位 → 成员」映射变化时被调用（hub 用它重播 voice.state）。
func New(channelID string, log *slog.Logger, send SignalSender, onChange func()) (*Room, error) {
	return NewWithICE(channelID, log, send, onChange, ICEConfig{})
}

// NewWithICE 与 New 相同，但把媒体 ICE 的端口范围/对外 IP 配置注入 Pion。
// 生产路径由 hub 传入 CLI 解析出的配置；New 保留为「不限制」的便捷入口
// （既有测试与不关心部署细节的调用方继续可用）。
func NewWithICE(channelID string, log *slog.Logger, send SignalSender, onChange func(), ice ICEConfig) (*Room, error) {
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		return nil, fmt.Errorf("register default codecs: %w", err)
	}
	api, err := newAPIWithICE(media, ice)
	if err != nil {
		return nil, fmt.Errorf("webrtc api: %w", err)
	}
	if log == nil {
		log = slog.Default()
	}
	return &Room{
		channelID:  channelID,
		log:        log.With("sfu", channelID),
		api:        api,
		send:       send,
		onChange:   onChange,
		peers:      make(map[string]*peer, 4),
		pubs:       make(map[string]*publication, 4),
		slotOwners: make([]string, MaxRemoteSlots),
	}, nil
}

// Join 为成员建立到服务端的 PeerConnection，并主动发一条 offer。
// 重复 join 是幂等的（已存在就直接返回）。
func (r *Room) Join(memberID string) error {
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return errors.New("relay room is closed")
	}
	if _, exists := r.peers[memberID]; exists {
		r.mu.Unlock()
		return nil
	}
	p := &peer{id: memberID, bound: make(map[string]int, MaxRemoteSlots)}
	r.peers[memberID] = p
	r.mu.Unlock()

	pc, err := r.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		r.removePeer(memberID)
		return fmt.Errorf("new peer connection: %w", err)
	}
	p.pc = pc

	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		r.onTrack(memberID, track)
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		r.log.Info("peer state", "member", memberID, "state", state.String())
	})

	// 1) 服务端收该成员的麦克风：recvonly 音频 m-line。
	if _, err := pc.AddTransceiverFromKind(
		webrtc.RTPCodecTypeAudio,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionRecvonly},
	); err != nil {
		pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("add mic transceiver: %w", err)
	}

	// 2) 预协商收别人的槽位：sendonly 音频 m-line × MaxRemoteSlots。
	//    每条 m-line 先挂一条**占位轨**，它的 id 固定为 "partyup-slot-<n>"：
	//    这样 msid 在 offer 时就写进 SDP，客户端收到的远端 track id 稳定可解析；
	//    之后 ReplaceTrack 换成真实发布者轨不会改 msid（不重协商）。
	for i := 0; i < MaxRemoteSlots; i++ {
		placeholder, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
			MimeType:  webrtc.MimeTypeOpus,
			ClockRate: 48000,
			Channels:  2,
		}, SlotTrackID(i), "partyup-relay")
		if err != nil {
			pc.Close()
			r.removePeer(memberID)
			return fmt.Errorf("new slot placeholder %d: %w", i, err)
		}
		tr, err := pc.AddTransceiverFromTrack(
			placeholder,
			webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly},
		)
		if err != nil {
			pc.Close()
			r.removePeer(memberID)
			return fmt.Errorf("add slot %d: %w", i, err)
		}
		p.slots = append(p.slots, tr.Sender())
		p.placeholders = append(p.placeholders, placeholder)
	}

	// 3) 服务端 offer。非 trickle：等 ICE 收集完，把候选一起塞进 SDP，
	//    客户端一次 SetRemoteDescription 就够（ABI v2 的收信令路径能直接吃）。
	offer, err := pc.CreateOffer(nil)
	if err != nil {
		pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("create offer: %w", err)
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(offer); err != nil {
		pc.Close()
		r.removePeer(memberID)
		return fmt.Errorf("set local description: %w", err)
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		pc.Close()
		r.removePeer(memberID)
		return errors.New("local description is nil after gathering")
	}

	r.send(memberID, "offer", mustJSON(map[string]any{
		"type":       "offer",
		"sdp":        local.SDP,
		"session_id": sessionID(memberID),
	}))
	r.log.Info("offer sent", "member", memberID, "slots", len(p.slots),
		"publishers", len(r.pubs), "sdp_bytes", len(local.SDP))

	// 4) offer 发出去之后才把已在说话的成员填进槽位。顺序很重要：
	//    如果在 CreateOffer 之前就 ReplaceTrack，那条 m-line 的 msid 会变成发布者
	//    member_id，客户端只能靠 id 猜是谁；先发 offer 再绑定，则 12 条 m-line 的
	//    msid 全是占位轨 id（partyup-slot-<n>），客户端可以稳定按槽位号映射成员。
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

// HandleSignal 处理成员发给服务端的信令（relay 模式下只接受 answer / candidate）。
func (r *Room) HandleSignal(memberID, kind string, payload []byte) error {
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
			return fmt.Errorf("decode answer: %w", err)
		}
		if msg.SDP == "" {
			return errors.New("answer.sdp is empty")
		}
		if err := p.pc.SetRemoteDescription(webrtc.SessionDescription{
			Type: webrtc.SDPTypeAnswer,
			SDP:  msg.SDP,
		}); err != nil {
			return fmt.Errorf("set remote answer: %w", err)
		}
		r.log.Info("answer applied", "member", memberID, "sdp_bytes", len(msg.SDP))
		return nil
	case "candidate":
		var msg struct {
			Candidate     string  `json:"candidate"`
			SDPMid        *string `json:"sdpMid"`
			SDPMLineIndex *uint16 `json:"sdpMLineIndex"`
		}
		if err := json.Unmarshal(payload, &msg); err != nil {
			return fmt.Errorf("decode candidate: %w", err)
		}
		if msg.Candidate == "" {
			return errors.New("candidate is empty")
		}
		if err := p.pc.AddICECandidate(webrtc.ICECandidateInit{
			Candidate:     msg.Candidate,
			SDPMid:        msg.SDPMid,
			SDPMLineIndex: msg.SDPMLineIndex,
		}); err != nil {
			return fmt.Errorf("add ice candidate: %w", err)
		}
		return nil
	case "offer":
		// relay 模式下服务端才是 offerer；客户端发 offer 说明客户端选错了模式。
		return errors.New("relay mode: server is the offerer, client must answer, not offer")
	default:
		return fmt.Errorf("unsupported signal kind %q for relay", kind)
	}
}

// Leave 让成员离开：关掉他的 PC、释放他占的槽位、停止转发他的声音。
func (r *Room) Leave(memberID string) {
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
	r.log.Info("member left", "member", memberID, "empty", empty)
}

// Close 关闭房间与所有连接（服务停机或频道切回 direct 时调用）。
func (r *Room) Close() {
	r.mu.Lock()
	peers := make([]*peer, 0, len(r.peers))
	for _, p := range r.peers {
		peers = append(peers, p)
	}
	r.peers = make(map[string]*peer)
	for id := range r.pubs {
		delete(r.pubs, id)
	}
	r.slotOwners = make([]string, MaxRemoteSlots)
	r.done = true
	r.mu.Unlock()

	for _, p := range peers {
		if p.pc != nil {
			_ = p.pc.Close()
		}
	}
	r.log.Info("room closed")
}

// Members 返回当前房间里的成员 id（测试与日志用）。
func (r *Room) Members() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, 0, len(r.peers))
	for id := range r.peers {
		out = append(out, id)
	}
	return out
}

// Publishers 返回当前正在发布音频的成员 id（测试与日志用）。
func (r *Room) Publishers() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, 0, len(r.pubs))
	for id := range r.pubs {
		out = append(out, id)
	}
	return out
}

// Slots 返回「槽位序号 → 发布者」映射（按序号升序，空槽不返回）。
// 广播给客户端后，客户端按远端音轨 id "partyup-slot-<n>" 反查成员。
func (r *Room) Slots() []SlotBinding {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]SlotBinding, 0, MaxRemoteSlots)
	for i, owner := range r.slotOwners {
		if owner == "" {
			continue
		}
		out = append(out, SlotBinding{Index: i, MemberID: owner})
	}
	return out
}

// notifyChange 在锁外通知 hub：槽位映射变了，voice.state 需要重播。
func (r *Room) notifyChange() {
	if r.onChange != nil {
		r.onChange()
	}
}

// assignSlotLocked 给新发布者分配一个空槽（最小可用序号）。
func (r *Room) assignSlotLocked(memberID string) int {
	for i, owner := range r.slotOwners {
		if owner == "" {
			r.slotOwners[i] = memberID
			return i
		}
	}
	return -1
}

// releaseSlotLocked 释放某发布者占的槽位。
func (r *Room) releaseSlotLocked(memberID string) {
	for i, owner := range r.slotOwners {
		if owner == memberID {
			r.slotOwners[i] = ""
			return
		}
	}
}

// slotIndexOfLocked 查某发布者的槽位序号（-1 = 没有）。
func (r *Room) slotIndexOfLocked(memberID string) int {
	for i, owner := range r.slotOwners {
		if owner == memberID {
			return i
		}
	}
	return -1
}

// ------------------------------------------------------------------
// 内部实现
// ------------------------------------------------------------------

// onTrack 在服务端收到某成员的音频时触发：建一条本地静态 RTP 轨，
// 挂到其他所有成员的空槽上，并把这路 RTP 持续写进去。
func (r *Room) onTrack(memberID string, remote *webrtc.TrackRemote) {
	if remote.Kind() != webrtc.RTPCodecTypeAudio {
		r.log.Warn("ignoring non-audio track", "member", memberID, "kind", remote.Kind().String())
		go func() {
			for {
				if _, _, err := remote.ReadRTP(); err != nil {
					return
				}
			}
		}()
		return
	}

	track, err := webrtc.NewTrackLocalStaticRTP(remote.Codec().RTPCodecCapability, memberID, "partyup-relay")
	if err != nil {
		r.log.Error("new local track", "member", memberID, "err", err)
		return
	}

	pub := &publication{owner: memberID, track: track}
	r.mu.Lock()

	r.pubs[memberID] = pub
	assigned := false
	if slot := r.assignSlotLocked(memberID); slot < 0 {
		r.log.Warn("no free slot for publisher", "member", memberID, "slots", MaxRemoteSlots)
	} else {
		assigned = true
		r.log.Info("slot assigned", "member", memberID, "slot", slot,
			"track_id", SlotTrackID(slot))
	}
	for id, other := range r.peers {
		if id == memberID {
			continue
		}
		r.bindLocked(other, memberID, track)
	}
	subscribers := len(r.peers) - 1
	r.mu.Unlock()

	r.log.Info("publisher attached", "member", memberID,
		"subscribers", subscribers, "sdp_tag", memberID)
	if assigned {
		r.notifyChange()
	}

	go func() {
		for {
			pkt, _, err := remote.ReadRTP()
			if err != nil {
				r.log.Info("publisher read ended", "member", memberID, "err", err)
				return
			}
			if err := track.WriteRTP(pkt); err != nil {
				r.log.Info("forward ended", "member", memberID, "err", err)
				return
			}
		}
	}()
}

// bindLocked 把一个发布者的轨挂到某个成员的空槽上。调用方必须持有 r.mu。
func (r *Room) bindLocked(p *peer, owner string, track *webrtc.TrackLocalStaticRTP) {
	if _, exists := p.bound[owner]; exists {
		return
	}
	// 槽位序号由房间级的发布者→槽位映射决定（不是「本 peer 里第一个空槽」），
	// 这样同一个发布者在所有成员那里占用同一个序号，voice.state.slots 才能广播一份。
	slot := r.slotIndexOfLocked(owner)
	if slot < 0 {
		r.log.Warn("publisher has no assigned slot", "member", p.id, "publisher", owner)
		return
	}
	p.bound[owner] = slot
	if err := p.slots[slot].ReplaceTrack(track); err != nil {
		r.log.Error("replace track", "member", p.id, "publisher", owner, "slot", slot, "err", err)
		delete(p.bound, owner)
		return
	}
	r.log.Info("slot bound", "member", p.id, "publisher", owner, "slot", slot)
}

// unbindLocked 释放某成员占用的槽位（发布者离开时）。调用方必须持有 r.mu。
func (r *Room) unbindLocked(p *peer, owner string) {
	slot, ok := p.bound[owner]
	if !ok {
		return
	}
	delete(p.bound, owner)
	if slot >= 0 && slot < len(p.slots) {
		if err := p.slots[slot].ReplaceTrack(nil); err != nil {
			r.log.Warn("clear slot", "member", p.id, "slot", slot, "err", err)
		}
	}
	r.log.Info("slot unbound", "member", p.id, "publisher", owner, "slot", slot)
}

// removePeer 在 join 失败时把半成品状态擦掉。
func (r *Room) removePeer(memberID string) {
	r.mu.Lock()
	p := r.peers[memberID]
	delete(r.peers, memberID)
	r.mu.Unlock()
	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
}

func sessionID(memberID string) string {
	return "relay/" + memberID
}

func mustJSON(v any) string {
	data, err := json.Marshal(v)
	if err != nil {
		return "{}"
	}
	return string(data)
}
