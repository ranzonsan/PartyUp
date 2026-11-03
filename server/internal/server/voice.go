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

// normalizeVoiceMode 校验并归一化传输模式。缺省 direct：老客户端不发 voice.join，
// 新客户端显式传 relay 才走中继。
func normalizeVoiceMode(mode string) (string, bool) {
	switch strings.TrimSpace(mode) {
	case "", "direct":
		return "direct", true
	case "relay":
		return "relay", true
	default:
		return "", false
	}
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
	mode := h.voiceMode[channelID]
	if mode == "" {
		mode = "direct"
	}
	members := make([]protocol.VoiceMemberState, 0, len(h.voice[channelID]))
	for _, m := range h.voice[channelID] {
		members = append(members, m)
	}
	targets := make([]*conn, 0, len(h.subs[channelID]))
	for peer := range h.subs[channelID] {
		targets = append(targets, peer)
	}
	h.mu.RUnlock()

	// 顺序稳定：成员按 id 排序，测试与客户端 diff 都不会因为 map 顺序抖动。
	sort.Slice(members, func(i, j int) bool { return members[i].MemberID < members[j].MemberID })

	data, err := protocol.MustNew(protocol.TypeVoiceState, protocol.VoiceStatePayload{
		ChannelID: channelID,
		Mode:      mode,
		Members:   members,
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
	mode := h.voiceMode[channelID]
	if len(members) > 0 {
		if mode == "relay" {
			return h.rooms[channelID], nil
		}
		return nil, nil
	}
	delete(h.voice, channelID)
	delete(h.voiceMode, channelID)
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
	mode, ok := normalizeVoiceMode(req.Mode)
	if !ok {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "mode must be relay or direct", env.ID)
		return
	}
	memberID := c.member.ID

	h := c.hub
	h.mu.Lock()
	current := h.voiceMode[channelID]
	if current == "" {
		h.voiceMode[channelID] = mode
		current = mode
	}
	members := h.voice[channelID]
	if members == nil {
		members = make(map[string]protocol.VoiceMemberState, 4)
		h.voice[channelID] = members
	}
	_, already := members[memberID]
	if !already {
		members[memberID] = protocol.VoiceMemberState{MemberID: memberID}
	}
	h.mu.Unlock()

	if current != mode {
		c.replyError(env.Type, protocol.CodeConflict,
			fmt.Sprintf("channel voice mode is %s; use voice.set_mode to switch", current), env.ID)
		return
	}

	if current == "relay" {
		room, err := h.ensureRoom(channelID)
		if err != nil {
			c.replyError(env.Type, protocol.CodeInternal, "create relay room: "+err.Error(), env.ID)
			return
		}
		if err := room.Join(memberID); err != nil {
			c.replyError(env.Type, protocol.CodeInternal, "join relay room: "+err.Error(), env.ID)
			return
		}
	}

	c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID, Mode: current, Transport: current})
	if !already {
		h.log.Info("voice: joined", "channel", channelID, "member", memberID, "mode", current)
		h.broadcastVoiceState(channelID)
	}
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

func (c *conn) handleVoiceSetMode(env protocol.Envelope) {
	var req protocol.VoiceSetModeRequest
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
	mode, ok := normalizeVoiceMode(req.Mode)
	if !ok || strings.TrimSpace(req.Mode) == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "mode must be relay or direct", env.ID)
		return
	}

	h := c.hub
	h.mu.Lock()
	if h.voiceMode[channelID] == mode {
		h.mu.Unlock()
		c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID, Mode: mode, Transport: mode})
		return
	}
	h.voiceMode[channelID] = mode
	members := make([]string, 0, len(h.voice[channelID]))
	for id := range h.voice[channelID] {
		members = append(members, id)
	}
	oldRoom := h.rooms[channelID]
	delete(h.rooms, channelID)
	h.mu.Unlock()

	// 旧房间里的人被整体断开；relay 侧由下面按新成员表重建。
	if oldRoom != nil {
		oldRoom.Close()
	}
	if mode == "relay" {
		room, err := h.ensureRoom(channelID)
		if err != nil {
			c.replyError(env.Type, protocol.CodeInternal, "create relay room: "+err.Error(), env.ID)
			return
		}
		for _, memberID := range members {
			if err := room.Join(memberID); err != nil {
				h.log.Warn("voice: rejoin relay failed", "channel", channelID, "member", memberID, "err", err)
			}
		}
	}

	h.log.Info("voice: mode switched", "channel", channelID, "mode", mode, "members", len(members))
	c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID, Mode: mode, Transport: mode})
	h.broadcastVoiceState(channelID)
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
