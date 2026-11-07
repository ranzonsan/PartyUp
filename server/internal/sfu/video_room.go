// Package sfu 的**屏幕共享视频中继**：与音频 Room 平级，但**独立 PeerConnection**。
//
// 为什么单独一个 room（而不是在音频 PC 上加视频 m-line）：
//   - 客户端把语音与屏幕拆成了两个 PeerConnection（voice_session_ / screen_session_），
//     一个服务端 PC 无法同时被两个客户端 PC 应答，所以服务端也必须一成员两 PC；
//   - 两套生命周期互不牵连：停共享只拆视频 room 的转发，语音 room 的 PC/槽位完全不动。
//
// 结构与音频同构（见 room.go 顶部注释）：
//   - 每成员的视频 PC 预协商 1 条 recvonly 视频 m-line（上传自己的屏幕）
//   - MaxScreenShares 条 sendonly 视频槽（看别人的屏幕，占位轨 id partyup-video-slot-<n>）；
//   - 共享者不换 PC：它往自己的 recvonly 视频 m-line 上传，服务端 OnTrack 收到后
//     ReplaceTrack 进其他成员的槽，零重协商；
//   - 观看者中途加入：offer 发出去后再把已有共享回填进它的空槽。
//
// v1.1 起支持**显式单流观看**（screen.watch / screen.unwatch）：
//   - 旧客户端（不声明 screen.watch.v1）：保持 v1 行为，Join 后自动绑定所有共享；
//   - 新客户端：manual 模式，未 watch 前不绑定任何共享；Watch 把目标绑到观看者自己的
//     一个 sendonly 槽（与上一目标的槽不同），并解绑旧目标；Unwatch 解绑并停止转发。
//     「一次一路」是观看者的约束，不是共享者的：一个共享者仍可被多个观看者同时看。
//
// 并发/生命周期（修复 -race 报告）：
//   - Join/Leave/Close/Refresh/Watch/Unwatch 全部由 lifecycleMu 串行化，房间关闭后
//     不会再插入新 peer；PC 在发布到 peers 映射**之前**创建、发布后不再改写 p.pc，
//     因此 HandleSignal/Close 读 p.pc 不会再与 Join 的写竞争。
//   - OnTrack 闭包捕获 peer 代次，旧 PC 的迟到回调不会把发布挂到新 peer 上。
package sfu

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/pion/rtcp"
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
	// codec 是发布者真实上传的 codec（onTrack 里的 remote.Codec().RTPCodecCapability）。
	// 严格接收会话的本端轨必须用它，否则 Pion 会按占位轨的 codec 绑定 payload type，
	// 把 VP8 payload 当成 H264 发出去，观看端解码器解不出帧。
	codec webrtc.RTPCodecCapability
	// ssrc 是发布者 RTP 流的 SSRC：转给发布者的 PLI/FIR 要指向它才会被当成本流
	// 的关键帧请求（屏幕共享编码器只在开始/被请求时编关键帧，中途加入者必须请求）。
	ssrc uint32
}

type videoPeer struct {
	id           string
	gen          uint64 // 创建代次：旧 PC 的 OnTrack 回调不得影响新 peer
	pc           *webrtc.PeerConnection
	slots        []*webrtc.RTPSender
	placeholders []*webrtc.TrackLocalStaticRTP
	bound        map[string]int // 发布者 memberID → 占用槽位下标

	// manual=true：只有显式 Watch 才绑定；false：v1 自动绑定所有共享。
	manual bool
	// target/watchSlot 是 manual 模式的当前目标与它占用的观看者槽位（-1 = 无）。
	// Watch 换目标时同时换槽（0↔1），保证新目标的 RTP 落在与旧目标不同的 track 上，
	// 客户端原生侧可以按「目标 + 槽位 + 代次」丢弃旧目标的迟到帧。
	target    string
	watchSlot int
}

// VideoRoom 是**一频道**的屏幕共享视频中继房间。
type VideoRoom struct {
	channelID string
	log       *slog.Logger
	api       *webrtc.API
	send      SignalSender
	onChange  func() // 视频槽位映射变化时通知 hub（可为 nil）

	// lifecycleMu 串行化 Join/Leave/Close/Refresh/Watch/Unwatch：关闭后不再接受新 peer，
	// 也避免 Join 与 Close 同时改 peers/pc。它不保护 map 内容（那是 mu）。
	lifecycleMu sync.Mutex
	// recvMu 串行化严格接收会话的 open/signal/close（B2）。锁序：recvMu → mu，
	// 反过来不允许；onTrack 只取 mu，因此不会与接收 PC 的关闭互相等待。
	recvMu sync.Mutex

	mu         sync.Mutex
	peers      map[string]*videoPeer
	pubs       map[string]*videoPublication
	slotOwners []string // 槽位序号 → 共享者 memberID（"" = 空），供旧式自动绑定使用
	// B2：严格接收会话（session_id → 独立 receive-only PC）与观看者当前会话。
	receives        map[string]*videoReceive
	receiveByViewer map[string]string
	nextGen         uint64
	done            bool
}

// newVideoAPIWithICE 构造视频房间默认使用的 webrtc.API（注册默认编解码），
// 并按 ICEConfig 注入 SettingEngine（端口区间 / host 候选对外 IP）。
func newVideoAPIWithICE(ice ICEConfig) (*webrtc.API, error) {
	media := &webrtc.MediaEngine{}
	if err := media.RegisterDefaultCodecs(); err != nil {
		return nil, fmt.Errorf("register default codecs: %w", err)
	}
	return newAPIWithICE(media, ice)
}

// NewVideo 构造一个视频房间。
func NewVideo(channelID string, log *slog.Logger, send SignalSender, onChange func()) (*VideoRoom, error) {
	return NewVideoWithICE(channelID, log, send, onChange, ICEConfig{})
}

// NewVideoWithICE 与 NewVideo 相同，但把媒体 ICE 的端口范围/对外 IP 配置注入
// Pion。语音房间（NewWithICE）与视频房间都必须走同一条注入路径，否则 relay
// 语音或屏幕共享会有一条拿不到固定端口/对外 IP。
func NewVideoWithICE(channelID string, log *slog.Logger, send SignalSender, onChange func(), ice ICEConfig) (*VideoRoom, error) {
	api, err := newVideoAPIWithICE(ice)
	if err != nil {
		return nil, err
	}
	return newVideoWithAPI(channelID, log, send, onChange, api)
}

// newVideoWithAPI 与 NewVideo 相同，只是用调用方提供的 webrtc.API。
//
// 唯一用途是回归测试注入 SettingEngine：Go 侧要证明「旧会话加密 RTP datagram 被
// 暂扣后在新区间重放不产生交付」，就得让本房间所有 PeerConnection 的 ICE 走同一条
// 受控 UDP socket（ice.UDPMux）。生产代码只经 NewVideo；房间的 map 初始化、锁与
// 生命周期语义与这里完全一致，没有第二条会话实现。
func newVideoWithAPI(channelID string, log *slog.Logger, send SignalSender, onChange func(), api *webrtc.API) (*VideoRoom, error) {
	if api == nil {
		return nil, errors.New("video room requires a webrtc api")
	}
	if log == nil {
		log = slog.Default()
	}
	return &VideoRoom{
		channelID:       channelID,
		log:             log.With("sfu-video", channelID),
		api:             api,
		send:            send,
		onChange:        onChange,
		peers:           make(map[string]*videoPeer, 4),
		pubs:            make(map[string]*videoPublication, 2),
		slotOwners:      make([]string, MaxScreenShares),
		receives:        make(map[string]*videoReceive, 2),
		receiveByViewer: make(map[string]string, 2),
	}, nil
}

// Join 为成员建立视频 PC 并主动发 offer（幂等），保持 v1 自动观看行为。
func (r *VideoRoom) Join(memberID string) error {
	return r.JoinWithMode(memberID, false)
}

// JoinWithMode 是 Join 的显式模式版本：
//   - manual=false（旧客户端）：和 v1 一样，自动把频道内已有共享绑定给该成员；
//   - manual=true（新客户端）：只建 PC，不绑定任何共享，等 screen.watch。
//
// 只有 --screen-mode=relay 时 hub 才会调用它：direct 模式下不能凭空多建一条到服务端的
// 视频连接（用户以为屏幕是 P2P，实际却连服务器，既是资源浪费也是隐私问题）。
func (r *VideoRoom) JoinWithMode(memberID string, manual bool) error {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
	if err := r.joinLocked(memberID, manual, "", -1); err != nil {
		return err
	}
	if !manual {
		// legacy 客户端 join 即自动观看已有共享：绑定完成后请求关键帧（锁外发 RTCP）。
		r.mu.Lock()
		owners := make([]string, 0, len(r.pubs))
		for owner := range r.pubs {
			owners = append(owners, owner)
		}
		r.mu.Unlock()
		if len(owners) > 0 {
			go r.nudgeLegacyKeyframes(owners)
		}
	}
	return nil
}

// joinLocked 在 lifecycleMu 内建 peer；target/watchSlot 用于 Refresh 保留观看状态。
func (r *VideoRoom) joinLocked(memberID string, manual bool, target string, watchSlot int) error {
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return errors.New("video room is closed")
	}
	if p, exists := r.peers[memberID]; exists {
		// 幂等：同一模式的重复 Join（重订阅 / 同一成员的另一条连接订阅）不得
		// 清掉已有 watch。只有「legacy → manual」升级时才需要撤掉自动绑定。
		if p.manual == manual {
			r.mu.Unlock()
			return nil
		}
		p.manual = manual
		if manual {
			for owner := range p.bound {
				r.unbindLocked(p, owner)
			}
			p.target = ""
			p.watchSlot = -1
		} else {
			p.target = ""
			p.watchSlot = -1
			for owner, pub := range r.pubs {
				r.bindLocked(p, owner, pub.track)
			}
		}
		r.mu.Unlock()
		return nil
	}
	r.nextGen++
	gen := r.nextGen
	r.mu.Unlock()

	pc, err := r.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return fmt.Errorf("new video peer connection: %w", err)
	}
	p := &videoPeer{
		id:        memberID,
		gen:       gen,
		pc:        pc,
		bound:     make(map[string]int, MaxScreenShares),
		manual:    manual,
		target:    target,
		watchSlot: watchSlot,
	}
	closeOnError := func(err error) error {
		_ = pc.Close()
		return err
	}
	pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		r.onTrack(memberID, gen, track)
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		r.log.Info("video peer state", "member", memberID, "state", state.String())
	})

	// 1) 服务端收该成员的屏幕：recvonly 视频 m-line（共享者往这里上传）。
	if _, err := pc.AddTransceiverFromKind(
		webrtc.RTPCodecTypeVideo,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionRecvonly},
	); err != nil {
		return closeOnError(fmt.Errorf("add screen recv transceiver: %w", err))
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
			return closeOnError(fmt.Errorf("new video slot placeholder %d: %w", i, err))
		}
		tr, err := pc.AddTransceiverFromTrack(
			placeholder,
			webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly},
		)
		if err != nil {
			return closeOnError(fmt.Errorf("add video slot %d: %w", i, err))
		}
		p.slots = append(p.slots, tr.Sender())
		p.placeholders = append(p.placeholders, placeholder)
	}

	// 3) 服务端 offer（非 trickle，候选进 SDP；与音频一致）。
	offer, err := pc.CreateOffer(nil)
	if err != nil {
		return closeOnError(fmt.Errorf("create video offer: %w", err))
	}
	gathered := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(offer); err != nil {
		return closeOnError(fmt.Errorf("set video local description: %w", err))
	}
	<-gathered
	local := pc.LocalDescription()
	if local == nil {
		return closeOnError(errors.New("video local description is nil after gathering"))
	}

	// 4) PC 完全就绪后才发布到 peers 映射：p.pc 从此刻起不可变，Close/HandleSignal
	//    读它不会与 Join 的写竞争（-race 修复的核心）。
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		_ = pc.Close()
		return errors.New("video room is closed")
	}
	r.peers[memberID] = p
	if manual {
		if target != "" && watchSlot >= 0 {
			if pub := r.pubs[target]; pub != nil {
				r.bindSlotLocked(p, target, pub.track, watchSlot)
			}
		}
	} else {
		for owner, pub := range r.pubs {
			r.bindLocked(p, owner, pub.track)
		}
	}
	r.mu.Unlock()

	r.send(memberID, "offer", mustJSON(map[string]any{
		"type":       "offer",
		"sdp":        local.SDP,
		"session_id": "relay-video/" + memberID,
	}))
	r.log.Info("video offer sent", "member", memberID,
		"slots", len(p.slots), "shares", len(r.pubs), "sdp_bytes", len(local.SDP), "manual", manual)
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
		if port, codecs, err := videoCodecMap(msg.SDP); err == nil {
			r.log.Info("video answer codecs", "member", memberID, "port", port,
				"codecs", strings.Join(codecs, " | "))
		}
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

// Watch 把观看者的目标切到 owner，返回它占用的观看者槽位。
//
// 一次一路：先解绑该观看者当前所有绑定，再绑定新目标。槽位在 0/1 之间与上一目标
// 交替（目标还没开始上传时也先占住槽位），换台后旧目标的迟到 RTP 落在另一个 track 上。
// 成员未加入/房间关闭时返回错误，调用方应明确报错，不能假装已订阅。
func (r *VideoRoom) Watch(memberID, owner string) (int, error) {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
	// subscribe 的视频 PC 是异步建的（ICE gathering 几十毫秒）。watch 可能在它完成前到达：
	// 这里在发布序列内补建一次（幂等），避免把「还没建好」误报成永久失败。
	r.mu.Lock()
	_, exists := r.peers[memberID]
	r.mu.Unlock()
	if !exists {
		if err := r.joinLocked(memberID, true, "", -1); err != nil {
			return -1, err
		}
	}
	slot, err := r.watchLocked(memberID, owner)
	if err == nil && slot >= 0 {
		// 槽位绑定后请求关键帧（锁外发 RTCP）：中途切台的观看者也需要新的关键帧。
		go r.nudgeLegacyKeyframes([]string{owner})
	}
	return slot, err
}

func (r *VideoRoom) watchLocked(memberID, owner string) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.done {
		return -1, errors.New("video room is closed")
	}
	p := r.peers[memberID]
	if p == nil || p.pc == nil {
		return -1, fmt.Errorf("%w: viewer has no video peer in this channel", ErrNotInRoom)
	}
	p.manual = true
	if p.target == owner && p.watchSlot >= 0 {
		return p.watchSlot, nil // 幂等：重复 watch 同一目标不换槽、不中断画面
	}
	slot := 0
	if p.watchSlot == 0 {
		slot = 1
	}
	for boundOwner := range p.bound {
		r.unbindLocked(p, boundOwner)
	}
	p.target = owner
	p.watchSlot = slot
	if pub := r.pubs[owner]; pub != nil {
		r.bindSlotLocked(p, owner, pub.track, slot)
	}
	r.log.Info("video watch armed", "member", memberID, "target", owner, "slot", slot)
	return slot, nil
}

// Unwatch 停止该观看者的当前目标：解绑槽位、停止转发（RTP 不再写入该观看者）。
// 成员保持 manual 模式（不会退回自动观看），重复调用是幂等的。
func (r *VideoRoom) Unwatch(memberID string) {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
	r.unwatchLocked(memberID)
}

func (r *VideoRoom) unwatchLocked(memberID string) {
	r.mu.Lock()
	p := r.peers[memberID]
	if p == nil {
		r.mu.Unlock()
		return
	}
	for owner := range p.bound {
		r.unbindLocked(p, owner)
	}
	p.target = ""
	p.watchSlot = -1
	p.manual = true
	r.mu.Unlock()
	r.log.Info("video watch cleared", "member", memberID)
}

// StopShare 在客户端 screen.stop 时调用：立刻摘掉该共享者占用的槽位并清空，
// 否则观看者会一直收到最后一帧（看起来像画面卡死）。约束 #2。
//
// 同时撤掉所有观看者指向这路共享的 manual target：重新开共享后必须重新 watch，
// 不能凭旧状态自动续看。
func (r *VideoRoom) StopShare(memberID string) {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
	r.mu.Lock()
	if _, ok := r.pubs[memberID]; !ok {
		r.mu.Unlock()
		return
	}
	delete(r.pubs, memberID)
	r.releaseSlotLocked(memberID)
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
		if other.target == memberID {
			other.target = ""
			other.watchSlot = -1
		}
	}
	r.mu.Unlock()
	// B2：共享停止后，所有指向它的严格接收会话一并关闭（观看者不能停在最后一帧）。
	closed := r.CloseReceivesForTarget(memberID)
	r.log.Info("screen share stopped", "member", memberID, "receive_sessions_closed", closed)
	r.notifyChange()
}

// Leave 让成员离开视频房间：关 PC、清掉它的共享与它占的槽位。
func (r *VideoRoom) Leave(memberID string) {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
	r.mu.Lock()
	p := r.peers[memberID]
	delete(r.peers, memberID)
	if _, ok := r.pubs[memberID]; ok {
		delete(r.pubs, memberID)
		r.releaseSlotLocked(memberID)
	}
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
		if other.target == memberID {
			other.target = ""
			other.watchSlot = -1
		}
	}
	empty := len(r.peers) == 0
	if empty {
		r.done = true
	}
	r.mu.Unlock()

	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
	// B2：该成员作为观看者或发布者的严格接收会话都要随 leave 关闭。
	closedAsViewer := r.CloseReceivesForViewer(memberID)
	closedAsTarget := r.CloseReceivesForTarget(memberID)
	r.log.Info("video member left", "member", memberID, "empty", empty,
		"receive_closed_viewer", closedAsViewer, "receive_closed_target", closedAsTarget)
}

// Close 关闭房间与所有连接。
func (r *VideoRoom) Close() {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()
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

	// 先关所有严格接收 PC，再关发布 PC；两者互不影响。
	r.CloseAllReceives()
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

// WatchState 返回某观看者当前的 manual 目标与槽位（测试/诊断用；没有则 ok=false）。
func (r *VideoRoom) WatchState(memberID string) (target string, slot int, manual bool, ok bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	p := r.peers[memberID]
	if p == nil {
		return "", -1, false, false
	}
	return p.target, p.watchSlot, p.manual, true
}

// Bound 返回某观看者当前绑定的发布者集合（测试用；manual 下一路，legacy 下多路）。
func (r *VideoRoom) Bound(memberID string) []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	p := r.peers[memberID]
	if p == nil {
		return nil
	}
	out := make([]string, 0, len(p.bound))
	for owner := range p.bound {
		out = append(out, owner)
	}
	return out
}

// ------------------------------------------------------------------
// 内部实现
// ------------------------------------------------------------------

// onTrack 在共享者开始上传屏幕时触发：建本地静态 RTP 轨，挂到观看者的槽并持续转发。
// gen 是该 peer 的创建代次；旧 PC 的迟到回调直接丢弃。
func (r *VideoRoom) onTrack(memberID string, gen uint64, remote *webrtc.TrackRemote) {
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

	r.mu.Lock()
	p := r.peers[memberID]
	if r.done || p == nil || p.gen != gen {
		r.mu.Unlock()
		r.log.Info("stale video track ignored", "member", memberID, "gen", gen)
		return
	}
	r.mu.Unlock()

	track, err := webrtc.NewTrackLocalStaticRTP(remote.Codec().RTPCodecCapability, "screen-"+memberID, "partyup-relay-video")
	if err != nil {
		r.log.Error("new video forward track", "member", memberID, "err", err)
		return
	}

	pub := &videoPublication{owner: memberID, track: track, codec: remote.Codec().RTPCodecCapability, ssrc: uint32(remote.SSRC())}
	r.mu.Lock()
	p = r.peers[memberID]
	if r.done || p == nil || p.gen != gen {
		r.mu.Unlock()
		return
	}
	r.pubs[memberID] = pub
	// B2：发布轨已就绪。严格接收会话的本端轨必须换成发布者真实 codec——观看者的
	// screen.watch 可能早于发布轨 attach（实测早约 1.5s），那时 OpenReceive 只能用
	// 占位轨；这里在读循环启动**之前**换轨，保证不会有占位 codec 的包被发出去。
	r.markReceivesHasTrackLocked(memberID)
	r.prepareReceivesForPublisherLocked(memberID, pub.codec)
	assigned := false
	if slot := r.assignSlotLocked(memberID); slot < 0 {
		r.log.Warn("no free video slot", "member", memberID, "slots", MaxScreenShares)
	} else {
		assigned = true
		r.log.Info("video slot assigned", "member", memberID, "slot", slot,
			"track_id", ScreenSlotTrackID(slot))
	}
	subscribers := 0
	for id, other := range r.peers {
		if id == memberID {
			continue
		}
		if other.manual {
			// 新客户端：只有 target 指向本发布者才绑定，未点不接收。
			if other.target == memberID && other.watchSlot >= 0 {
				r.bindSlotLocked(other, memberID, track, other.watchSlot)
				subscribers++
			}
			continue
		}
		r.bindLocked(other, memberID, track)
		subscribers++
	}
	r.mu.Unlock()

	r.log.Info("screen publisher attached", "member", memberID,
		"codec", remote.Codec().MimeType, "subscribers", subscribers, "gen", gen)
	if assigned {
		r.notifyChange()
	}
	// 已有观看者直接绑上了这条轨（legacy 槽位/手动槽位）：它们从半截 GOP 开始收，
	// 必须让发布者立刻编一个关键帧，否则永远等不到可解码的起点。
	if subscribers > 0 {
		r.requestPublisherKeyframe(memberID)
	}

	go func() {
		// 诊断：前 3 个包的 PT/SSRC/载荷前缀。PT 由发布者的 SDP 协商决定，载荷前缀
		// 直接区分 VP8（首个字节是 payload descriptor）与 H264（0x67/0x65/0x41 NAL 头）：
		// 若二者不一致，中继转发出去的流观看端永远解不出帧。
		loggedPackets := 0
		for {
			pkt, _, err := remote.ReadRTP()
			if err != nil {
				r.log.Info("screen publisher read ended", "member", memberID, "err", err)
				return
			}
			if loggedPackets < 3 {
				loggedPackets++
				prefix := pkt.Payload
				if len(prefix) > 12 {
					prefix = prefix[:12]
				}
				r.log.Info("screen publisher rtp", "member", memberID, "n", loggedPackets,
					"seq", pkt.SequenceNumber, "pt", pkt.PayloadType, "ssrc", pkt.SSRC,
					"codec", remote.Codec().MimeType, "payload_prefix", hex.EncodeToString(prefix))
			}
			// 停发/换源后不再把旧发布者的包写进任何槽（track 本身可能未被解绑干净）。
			r.mu.Lock()
			current, ok := r.pubs[memberID]
			stopped := !ok || current.track != track
			if !stopped {
				// B2：同一包也写给所有指向该发布者的严格接收会话。每个会话有自己的
				// TrackLocalStaticRTP 与发送状态，关闭后即从 receives 摘除，不会续写。
				r.writeToReceivesLocked(memberID, pkt)
			}
			r.mu.Unlock()
			if stopped {
				return
			}
			if err := track.WriteRTP(pkt); err != nil {
				r.log.Info("screen forward ended", "member", memberID, "err", err)
				return
			}
		}
	}()
}

// requestPublisherKeyframe 向目标发布者请求一个关键帧（PLI）。
//
// 为什么必须有：屏幕共享编码器只在开始/被请求时编关键帧（真机实测一次 55s 分享
// 只编了 2 个 keyframe）。观看者中途加入时收到的第一包是半截 GOP，没有关键帧就
// 解不出任何帧——表现正是「receive PC connected、RTP 在涨、观看端 0 帧」。
// Pion 不会跨 PeerConnection 转发 RTCP，SFU 必须替观看者把关键帧请求转给发布者。
//
// 返回是否成功写入了 RTCP。
func (r *VideoRoom) requestPublisherKeyframe(target string) bool {
	r.mu.Lock()
	pub := r.pubs[target]
	peer := r.peers[target]
	var (
		pc   *webrtc.PeerConnection
		ssrc uint32
	)
	if pub != nil && peer != nil {
		pc, ssrc = peer.pc, pub.ssrc
	}
	r.mu.Unlock()
	if pc == nil {
		return false
	}
	if err := pc.WriteRTCP([]rtcp.Packet{
		&rtcp.PictureLossIndication{MediaSSRC: ssrc},
	}); err != nil {
		r.log.Warn("request publisher keyframe", "target", target, "ssrc", ssrc, "err", err)
		return false
	}
	r.log.Info("requested publisher keyframe", "target", target, "ssrc", ssrc)
	return true
}

// nudgeLegacyKeyframes 在 legacy/手动槽位绑定完成后，退避向发布者请求关键帧。
//
// 槽位绑定发生在 Join/watch 的锁内，而 RTCP 必须在锁外发；绑定那一刻观看者 PC 可能
// 还没连上（RTCP 会被丢），所以重试两次。没有这一步，中途加入的 legacy 观看者会和
// 严格接收会话一样卡在「等待首帧」——同一个根因（发布端不编关键帧、SFU 不请求）。
func (r *VideoRoom) nudgeLegacyKeyframes(owners []string) {
	for _, delay := range []time.Duration{400 * time.Millisecond, 1500 * time.Millisecond} {
		time.Sleep(delay)
		for _, owner := range owners {
			r.requestPublisherKeyframe(owner)
		}
	}
}

// bindLocked 是 v1 自动观看路径：用发布者的全局槽位绑定。
func (r *VideoRoom) bindLocked(p *videoPeer, owner string, track *webrtc.TrackLocalStaticRTP) {
	if _, exists := p.bound[owner]; exists {
		return
	}
	slot := r.slotIndexOfLocked(owner)
	if slot < 0 {
		r.log.Warn("screen publisher has no slot", "member", p.id, "publisher", owner)
		return
	}
	r.bindSlotLocked(p, owner, track, slot)
}

// bindSlotLocked 把发布者轨绑定到指定槽（manual 用观看者自己的槽位）。
func (r *VideoRoom) bindSlotLocked(p *videoPeer, owner string, track *webrtc.TrackLocalStaticRTP, slot int) {
	if slot < 0 || slot >= len(p.slots) {
		r.log.Warn("invalid video slot", "member", p.id, "slot", slot)
		return
	}
	// 必须用 comma-ok：槽位 0 是合法值，而缺失键的零值也是 0。
	// 直接比较 p.bound[owner] == slot 会让「首次绑定到槽 0」被误判成已绑定而直接返回，
	// 结果是 Bound() 为空、观看者永远收不到画面。
	if current, exists := p.bound[owner]; exists && current == slot {
		return
	}
	if err := p.slots[slot].ReplaceTrack(track); err != nil {
		r.log.Error("replace video track", "member", p.id, "publisher", owner, "slot", slot, "err", err)
		return
	}
	p.bound[owner] = slot
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

// Refresh 重建某成员的视频 PC 并重新发 offer。
//
// 为什么需要它：客户端媒体核心的被动屏幕会话**必须在本端 answer 之前**把视频轨挂到
// recvonly m-line 上（ABI v2.11 不支持事后重新协商）。而观看者为了能随时看到画面，
// 在订阅时就已经把视频 PC 答完了——等到它真要共享时，那条 m-line 早已协商成「本端不发」。
// 所以共享开始时由服务端重建这个成员的视频 PC 并发新 offer，客户端在收到 offer 前
// 已用 pu_screen_start_ex(passive) 建好带轨的新会话，answer 时轨就挂上了。
//
// 代价：该成员自己正在收的其它共享会短暂中断（v1 同时最多 1 路共享，可接受）。
// 修复点：重建时保留该成员的 manual/target/watchSlot——共享者自己可能正在看别人，
// 不能因为开始共享就把它变成自动观看或丢掉目标。
func (r *VideoRoom) Refresh(memberID string) error {
	r.lifecycleMu.Lock()
	defer r.lifecycleMu.Unlock()

	r.mu.Lock()
	p := r.peers[memberID]
	manual := false
	target := ""
	watchSlot := -1
	if p != nil {
		manual = p.manual
		target = p.target
		watchSlot = p.watchSlot
	}
	delete(r.peers, memberID)
	if _, ok := r.pubs[memberID]; ok {
		delete(r.pubs, memberID)
		r.releaseSlotLocked(memberID)
	}
	for _, other := range r.peers {
		r.unbindLocked(other, memberID)
		if other.target == memberID {
			other.target = ""
			other.watchSlot = -1
		}
	}
	r.mu.Unlock()

	if p != nil && p.pc != nil {
		_ = p.pc.Close()
	}
	r.log.Info("video pc refreshed for screen share", "member", memberID,
		"manual", manual, "target", target, "watchSlot", watchSlot)
	return r.joinLocked(memberID, manual, target, watchSlot)
}
