package server

// 语音（PROTOCOL.md §4）。
//
// 两条传输路径并存，互不影响：
//
//   - **direct**：完全沿用老路径——客户端之间经 media.signal 广播 offer、定向回
//     answer/candidate，服务端只当不透明转发器。1:1 下的既有行为零改动。
//   - **relay**：服务端 SFU（internal/sfu）为每个成员建一个 PeerConnection，成员之间
//     不直连。信令目标是服务端（media.signal + target="sfu"），SDP 由服务端生成。
//
// 成员状态（谁在语音、谁静音）与频道的传输模式统一由 voice.state 广播，
// 取代 2026-11-03 客户端临时用 media.signal 捎带 partyup.voice.presence 的做法。

import (
	"errors"
	"fmt"
	"sort"
	"strings"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
)

// NormalizeVoiceMode 归一化服务端语音传输策略：只接受 relay / direct，
// 空值或无法识别的值一律回落到默认 relay（ADR-0002：语音默认中继）。
//
// 这是**服务端配置**，不是客户端请求参数；客户端无权选择传输方式。
// NormalizeScreenMode 归一化屏幕共享传输策略。与语音策略完全独立（ADR-0002）。
// SFU 视频通道尚未实现，所以默认 direct：默认配置下屏幕共享必须可用；
// 视频通道落地后把 main.go 的默认值切到 relay。
func NormalizeScreenMode(mode string) string {
	if strings.EqualFold(strings.TrimSpace(mode), "relay") {
		return "relay"
	}
	return "direct"
}

func NormalizeVoiceMode(mode string) string {
	if strings.EqualFold(strings.TrimSpace(mode), "direct") {
		return "direct"
	}
	return "relay"
}

// ensureRoom 取得（必要时创建）某个频道的 SFU 房间。房间方法不持 h.mu 调用。
func (h *Hub) ensureRoom(channelID string) (*sfu.Room, error) {
	h.mu.RLock()
	if room := h.rooms[channelID]; room != nil {
		h.mu.RUnlock()
		return room, nil
	}
	h.mu.RUnlock()

	room, err := sfu.New(channelID, h.log, func(memberID, kind, payload string) {
		h.deliverVoiceSignal(channelID, memberID, kind, payload)
	}, func() {
		// 发布者首次发声 → 槽位分配完成 → 广播新的槽位映射。
		h.broadcastVoiceState(channelID)
	})
	if err != nil {
		return nil, err
	}

	h.mu.Lock()
	defer h.mu.Unlock()
	// 并发加入时可能已经有人建好了，优先用现成的。
	if existing := h.rooms[channelID]; existing != nil {
		return existing, nil
	}
	h.rooms[channelID] = room
	return room, nil
}

// deliverVoiceSignal 把服务端 SFU 产出的信令投递给指定成员。
// 成员断线/退订时找不到连接，静默丢弃（房间会在 Leave/Close 时收尾）。
func (h *Hub) deliverVoiceSignal(channelID, memberID, kind, payload string) {
	h.mu.RLock()
	var target *conn
	for c := range h.subs[channelID] {
		if c.authed && c.member.ID == memberID {
			target = c
			break
		}
	}
	h.mu.RUnlock()
	if target == nil {
		h.log.Warn("voice signal dropped, member not connected",
			"channel", channelID, "member", memberID, "kind", kind)
		return
	}
	target.enqueue(protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
		ChannelID: channelID,
		Target:    "sfu",
		From:      "sfu",
		FromName:  "sfu",
		Media:     "voice",
		Kind:      kind,
		Payload:   payload,
	}))
}

// handleVoiceSignal 把客户端的 relay 信令（answer/candidate）交给 SFU。
func (h *Hub) handleVoiceSignal(memberID, channelID, kind, payload string) error {
	h.mu.RLock()
	room := h.rooms[channelID]
	h.mu.RUnlock()
	if room == nil {
		return fmt.Errorf("%w: channel has no relay room", sfu.ErrNotInRoom)
	}
	return room.HandleSignal(memberID, kind, []byte(payload))
}

// broadcastVoiceState 把频道当前语音快照推给所有订阅者。
func (h *Hub) broadcastVoiceState(channelID string) {
	h.mu.RLock()
	mode := h.voiceMode
	screenMode := h.screenMode
	members := make([]protocol.VoiceMemberState, 0, len(h.voice[channelID]))
	for _, m := range h.voice[channelID] {
		members = append(members, m)
	}
	targets := make([]*conn, 0, len(h.subs[channelID]))
	for peer := range h.subs[channelID] {
		targets = append(targets, peer)
	}
	room := h.rooms[channelID]
	h.mu.RUnlock()

	// 顺序稳定：成员按 id 排序，测试与客户端 diff 都不会因为 map 顺序抖动。
	sort.Slice(members, func(i, j int) bool { return members[i].MemberID < members[j].MemberID })

	// relay 模式下补「槽位 → 成员」映射（direct 没有中继槽位，保持空）。
	var slots []protocol.VoiceSlotState
	if mode == "relay" && room != nil {
		muted := make(map[string]bool, len(members))
		for _, m := range members {
			muted[m.MemberID] = m.Muted
		}
		for _, b := range room.Slots() {
			slots = append(slots, protocol.VoiceSlotState{
				Index:    b.Index,
				MemberID: b.MemberID,
				Muted:    muted[b.MemberID],
			})
		}
	}

	data, err := protocol.MustNew(protocol.TypeVoiceState, protocol.VoiceStatePayload{
		ChannelID:  channelID,
		VoiceMode:  mode,
		ScreenMode: screenMode,
		Members:    members,
		Slots:      slots,
	}).Marshal()
	if err != nil {
		h.log.Error("marshal voice.state", "err", err)
		return
	}
	for _, peer := range targets {
		if peer.authed {
			peer.enqueueBytes(data)
		}
	}
}

// detachVoiceMemberLocked 把成员从频道语音里摘掉。调用方必须持有 h.mu。
// 返回：需要 Leave 的房间（频道还有别人）、需要 Close 的房间（频道空了。
// 已从 map 里摘除，调用方在锁外关闭）。
func (h *Hub) detachVoiceMemberLocked(channelID, memberID string) (leave, close *sfu.Room) {
	members := h.voice[channelID]
	if members == nil {
		return nil, nil
	}
	if _, ok := members[memberID]; !ok {
		return nil, nil
	}
	delete(members, memberID)
	mode := h.voiceMode
	if len(members) > 0 {
		if mode == "relay" {
			return h.rooms[channelID], nil
		}
		return nil, nil
	}
	delete(h.voice, channelID)
	delete(h.voiceSeq, channelID) // 频道空了：加入序号从头开始，避免序号无限增长
	room := h.rooms[channelID]
	delete(h.rooms, channelID)
	if room != nil {
		return nil, room
	}
	return nil, nil
}

// leaveVoiceChannel 让成员离开某个频道的语音（退订/断线/显式 leave 都用这条）。
func (h *Hub) leaveVoiceChannel(memberID, channelID string) {
	h.mu.Lock()
	leave, closeRoom := h.detachVoiceMemberLocked(channelID, memberID)
	h.mu.Unlock()

	if leave != nil {
		leave.Leave(memberID)
	}
	if closeRoom != nil {
		closeRoom.Close()
	}
	if leave != nil || closeRoom != nil {
		h.log.Info("voice: member left", "channel", channelID, "member", memberID)
		h.broadcastVoiceState(channelID)
	}
}

// leaveVoiceEverywhere 在连接断开时清掉该成员在所有频道的语音状态。
func (h *Hub) leaveVoiceEverywhere(memberID string) {
	h.mu.Lock()
	type change struct {
		channelID string
		leave     *sfu.Room
		closeRoom *sfu.Room
	}
	changes := make([]change, 0, 2)
	for channelID := range h.voice {
		if _, ok := h.voice[channelID][memberID]; !ok {
			continue
		}
		leave, closeRoom := h.detachVoiceMemberLocked(channelID, memberID)
		changes = append(changes, change{channelID: channelID, leave: leave, closeRoom: closeRoom})
	}
	h.mu.Unlock()

	for _, ch := range changes {
		if ch.leave != nil {
			ch.leave.Leave(memberID)
		}
		if ch.closeRoom != nil {
			ch.closeRoom.Close()
		}
		h.log.Info("voice: member dropped", "channel", ch.channelID, "member", memberID)
		h.broadcastVoiceState(ch.channelID)
	}
}

// ------------------------------------------------------------------
// 请求处理
// ------------------------------------------------------------------

func (c *conn) handleVoiceJoin(env protocol.Envelope) {
	var req protocol.VoiceJoinRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}

	// 传输方式只由服务端策略决定；客户端请求里就算带了 mode 也不会被读取。
	h := c.hub
	memberID := c.member.ID

	h.mu.Lock()
	members := h.voice[channelID]
	if members == nil {
		members = make(map[string]protocol.VoiceMemberState, 4)
		h.voice[channelID] = members
	}
	_, already := members[memberID]
	if !already {
		// 角色由服务端单一来源裁决（客户端只消费，不自行推断）：
		//   - relay：服务端 SFU 是 offerer，所有成员都是 answerer；
		//   - direct：频道里还没人 → answerer（先等着，别对着空气发 offer）；
		//     只有一人且他还不是 offerer → 新来者当 offerer（两人一进一退，确定）；
		//     已有一对在通话 → 新来者 answerer（三人及以上的 mesh 暂不支持，
		//     但绝不能因为第三个人进来就把已在通话的两人的 PC 拆了）。
		role := "answerer"
		if h.voiceMode == "direct" {
			others := 0
			hasOfferer := false
			for id, m := range members {
				if id == memberID {
					continue
				}
				others++
				if m.Role == "offerer" {
					hasOfferer = true
				}
			}
			if others == 1 && !hasOfferer {
				role = "offerer"
			}
		}
		h.voiceSeq[channelID]++
		members[memberID] = protocol.VoiceMemberState{
			MemberID:  memberID,
			Role:      role,
			JoinedSeq: h.voiceSeq[channelID],
		}
	}
	h.mu.Unlock()

	if h.voiceMode == "relay" {
		room, err := h.ensureRoom(channelID)
		if err != nil {
			// 加入失败要回滚刚写进 voice 表的成员，避免幽灵成员。
			c.hub.leaveVoiceChannel(memberID, channelID)
			c.replyError(env.Type, protocol.CodeInternal, "create relay room: "+err.Error(), env.ID)
			return
		}
		if err := room.Join(memberID); err != nil {
			c.hub.leaveVoiceChannel(memberID, channelID)
			c.replyError(env.Type, protocol.CodeInternal, "join relay room: "+err.Error(), env.ID)
			return
		}
	}

	// 在 voice.join.ok 里直接回自己的角色：客户端在「建 PC 之前」就要知道 passive 与否，
	// 不能等 voice.state 广播（那会晚一步，先建错 PC 再拆）。
	c.replyOK(env, protocol.VoiceJoinOK{
		ChannelID: channelID,
		Transport: h.voiceMode,
		Role:      h.memberRole(channelID, memberID),
	})
	if !already {
		h.log.Info("voice: joined", "channel", channelID, "member", memberID, "transport", h.voiceMode)
		h.broadcastVoiceState(channelID)
	}
}

// memberRole 读取某成员在当前频道的服务端角色（不在语音里返回空）。
func (h *Hub) memberRole(channelID, memberID string) string {
	h.mu.RLock()
	defer h.mu.RUnlock()
	if m, ok := h.voice[channelID][memberID]; ok {
		return m.Role
	}
	return ""
}

func (c *conn) handleVoiceLeave(env protocol.Envelope) {
	var req protocol.VoiceLeaveRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	c.hub.leaveVoiceChannel(c.member.ID, channelID)
	c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID})
}

func (c *conn) handleVoiceSetMute(env protocol.Envelope) {
	var req protocol.VoiceSetMuteRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	c.updateVoiceFlag(env, strings.TrimSpace(req.ChannelID), func(m *protocol.VoiceMemberState) {
		m.Muted = req.Muted
	})
}

func (c *conn) handleVoiceSetDeafen(env protocol.Envelope) {
	var req protocol.VoiceSetDeafenRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	c.updateVoiceFlag(env, strings.TrimSpace(req.ChannelID), func(m *protocol.VoiceMemberState) {
		m.Deafened = req.Deafened
	})
}

// updateVoiceFlag 是 mute/deafen 的公共部分：只允许改自己在该频道的条目。
func (c *conn) updateVoiceFlag(env protocol.Envelope, channelID string, apply func(*protocol.VoiceMemberState)) {
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	h := c.hub
	h.mu.Lock()
	members := h.voice[channelID]
	state, ok := members[c.member.ID]
	if ok {
		apply(&state)
		members[c.member.ID] = state
	}
	h.mu.Unlock()

	if !ok {
		c.replyError(env.Type, protocol.CodeConflict, "not in voice for this channel", env.ID)
		return
	}
	c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID})
	h.broadcastVoiceState(channelID)
}

// voiceMember 是测试用的内部访问器（避免测试把 hub 内部结构抄一遍）。
func (h *Hub) voiceMember(channelID, memberID string) (protocol.VoiceMemberState, bool) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	m, ok := h.voice[channelID][memberID]
	return m, ok
}

// roomFor 是测试用的内部访问器。
func (h *Hub) roomFor(channelID string) *sfu.Room {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.rooms[channelID]
}

var _ = errors.Is // errors 在 media.signal 的 target=sfu 分支里用（本文件保留引用检查）

// ------------------------------------------------------------------
// 屏幕共享视频中继（sfu.VideoRoom，仅 --screen-mode=relay）
// ------------------------------------------------------------------

// ensureVideoRoom 取得（必要时创建）某频道的视频房间。
func (h *Hub) ensureVideoRoom(channelID string) (*sfu.VideoRoom, error) {
	h.mu.RLock()
	if room := h.videoRooms[channelID]; room != nil {
		h.mu.RUnlock()
		return room, nil
	}
	h.mu.RUnlock()

	room, err := sfu.NewVideo(channelID, h.log, func(memberID, kind, payload string) {
		h.deliverScreenSignal(channelID, memberID, kind, payload)
	}, nil)
	if err != nil {
		return nil, err
	}

	h.mu.Lock()
	defer h.mu.Unlock()
	if existing := h.videoRooms[channelID]; existing != nil {
		return existing, nil
	}
	h.videoRooms[channelID] = room
	return room, nil
}

// joinVideoRoom 在**订阅**频道时把成员接进视频房间（仅 screen_mode=relay）。
// 观看者必须提前就绪，才能在共享开始的瞬间就收到槽位；direct 模式不建视频 PC。
func (h *Hub) joinVideoRoom(channelID, memberID string) {
	h.mu.RLock()
	mode := h.screenMode
	h.mu.RUnlock()
	if mode != "relay" {
		return
	}
	room, err := h.ensureVideoRoom(channelID)
	if err != nil {
		h.log.Error("ensure video room", "channel", channelID, "err", err)
		return
	}
	// Join 里有 ICE gathering（几十毫秒），放在 goroutine 里避免阻塞 subscribe 应答。
	go func() {
		if err := room.Join(memberID); err != nil {
			h.log.Error("join video room", "channel", channelID, "member", memberID, "err", err)
		}
	}()
}

// leaveVideoRoom 让成员离开视频房间；频道空了就关房间（仅 relay）。
func (h *Hub) leaveVideoRoom(channelID, memberID string) {
	h.mu.RLock()
	room := h.videoRooms[channelID]
	h.mu.RUnlock()
	if room == nil {
		return
	}
	room.Leave(memberID)
	h.mu.Lock()
	if existing := h.videoRooms[channelID]; existing != nil && len(existing.Members()) == 0 {
		delete(h.videoRooms, channelID)
		existing.Close()
	}
	h.mu.Unlock()
}

// leaveVideoRoomsEverywhere 在连接断开时把该成员从所有视频房间摘掉。
func (h *Hub) leaveVideoRoomsEverywhere(memberID string, channels []string) {
	for _, ch := range channels {
		h.leaveVideoRoom(ch, memberID)
	}
}

// handleScreenSignal 把客户端的 answer/candidate 交给视频房间。
func (h *Hub) handleScreenSignal(memberID, channelID, kind, payload string) error {
	h.mu.RLock()
	room := h.videoRooms[channelID]
	h.mu.RUnlock()
	if room == nil {
		return fmt.Errorf("%w: channel has no video relay room", sfu.ErrNotInRoom)
	}
	return room.HandleSignal(memberID, kind, []byte(payload))
}

// StopVideoShare 在 screen.stop 时摘掉视频房间里的共享槽位并清空，
// 否则观看者会一直收到最后一帧（看起来像画面卡死）。
func (h *Hub) StopVideoShare(channelID, memberID string) {
	h.mu.RLock()
	room := h.videoRooms[channelID]
	h.mu.RUnlock()
	if room != nil {
		room.StopShare(memberID)
	}
}

// deliverScreenSignal 把服务端视频 PC 的信令投递给某个成员（media.signal, media=screen, target=sfu）。
func (h *Hub) deliverScreenSignal(channelID, memberID, kind, payload string) {
	h.mu.RLock()
	var target *conn
	for c := range h.subs[channelID] {
		if c.authed && c.member.ID == memberID {
			target = c
			break
		}
	}
	h.mu.RUnlock()
	if target == nil {
		h.log.Warn("screen signal dropped, member not connected",
			"channel", channelID, "member", memberID, "kind", kind)
		return
	}
	target.enqueue(protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
		ChannelID: channelID,
		Target:    "sfu",
		From:      "sfu",
		FromName:  "sfu",
		Media:     "screen",
		Kind:      kind,
		Payload:   payload,
	}))
}
