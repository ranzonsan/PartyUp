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

// SignalSender 把一条信令投递给某个成员。
// hub 注入的实现负责把它包成 media.signal（from=sfu, target=sfu）。
type SignalSender func(memberID, kind, payload string)

type publication struct {
	owner string
	track *webrtc.TrackLocalStaticRTP
}

type peer struct {
	id    string
	pc    *webrtc.PeerConnection
	slots []*webrtc.RTPSender // 预协商的 sendonly 槽，下标即槽位
	bound map[string]int      // 发布者 memberID → 占用槽位下标
}

// Room 是**一频道**的语音中继房间。
type Room struct {
	channelID string
	log       *slog.Logger
	api       *webrtc.API
	send      SignalSender

	mu    sync.Mutex
	peers map[string]*peer
	pubs  map[string]*publication
	done  bool
}

// New 构造一个房间。send 负责把服务端产出的信令送给成员。
func New(channelID string, log *slog.Logger, send SignalSender) (*Room, error) {
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		return nil, fmt.Errorf("register default codecs: %w", err)
	}
	api := webrtc.NewAPI(webrtc.WithMediaEngine(media))
	if log == nil {
		log = slog.Default()
	}
	return &Room{
		channelID: channelID,
		log:       log.With("sfu", channelID),
		api:       api,
		send:      send,
		peers:     make(map[string]*peer, 4),
		pubs:      make(map[string]*publication, 4),
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
	for i := 0; i < MaxRemoteSlots; i++ {
		tr, err := pc.AddTransceiverFromKind(
			webrtc.RTPCodecTypeAudio,
			webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly},
		)
		if err != nil {
			pc.Close()
			r.removePeer(memberID)
			return fmt.Errorf("add slot %d: %w", i, err)
		}
		p.slots = append(p.slots, tr.Sender())
	}

	// 3) 已经在说话的成员，直接填进空槽（新人一进来就能听见）。
	r.mu.Lock()
	for owner, pub := range r.pubs {
		if owner == memberID {
			continue
		}
		r.bindLocked(p, owner, pub.track)
	}
	r.mu.Unlock()

	// 4) 服务端 offer。非 trickle：等 ICE 收集完，把候选一起塞进 SDP，
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
	used := make(map[int]struct{}, len(p.bound))
	for _, idx := range p.bound {
		used[idx] = struct{}{}
	}
	slot := -1
	for i := range p.slots {
		if _, busy := used[i]; !busy {
			slot = i
			break
		}
	}
	if slot < 0 {
		r.log.Warn("no free slot for publisher", "member", p.id, "publisher", owner,
			"slots", len(p.slots))
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
