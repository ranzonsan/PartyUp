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
	"time"

	"partyup/internal/protocol"
	"partyup/internal/sfu"
)

// DirectMaxMembers 是 direct（P2P）语音的成员上限：P2P mesh 只实现了 1v1，
// 第 3 个人即使加入也互相听不到，所以在 voice.join 时直接拒绝（与 relay 的
// 槽位门禁同类：宁可明确报错，也不留静默降级）。
const DirectMaxMembers = 2

// NormalizeVoiceMode 归一化服务端语音传输策略：只接受 relay / direct，
// 空值或无法识别的值一律回落到默认 relay（ADR-0002：语音默认中继）。
//
// 这是**服务端配置**，不是客户端请求参数；客户端无权选择传输方式。
// NormalizeScreenMode 归一化屏幕共享传输策略。与语音策略完全独立（ADR-0002）。
// SFU 视频通道尚未实现，所以默认 direct：默认配置下屏幕共享必须可用；
// 视频通道落地后把 main.go 的默认值切到 relay。
// NormalizeScreenMode 归一到 relay|direct。**空值/未识别值一律按 relay**（默认值）：
// direct 屏幕共享只能 1v1，第三个人看不到，不能当默认。
func NormalizeScreenMode(mode string) string {
	if strings.EqualFold(strings.TrimSpace(mode), "direct") {
		return "direct"
	}
	return "relay"
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

	room, err := sfu.NewWithICE(channelID, h.log, func(memberID, kind, payload string) {
		h.deliverVoiceSignal(channelID, memberID, kind, payload)
	}, func() {
		// 发布者首次发声 → 槽位分配完成 → 广播新的槽位映射。
		h.broadcastVoiceState(channelID)
	}, h.iceConfig)
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
		if peer.authed {
			targets = append(targets, peer)
		}
	}
	room := h.rooms[channelID]
	transport, transportReason := h.channelTransportLocked(channelID)
	h.mu.RUnlock()

	// 顺序稳定：成员按 id 排序，测试与客户端 diff 都不会因为 map 顺序抖动。
	sort.Slice(members, func(i, j int) bool { return members[i].MemberID < members[j].MemberID })
	// 逐成员下发「实际 transport」（v1 全是同一个值；字段结构与将来逐成员混合一致）。
	for i := range members {
		members[i].Transport = transport
		members[i].TransportReason = transportReason
	}

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
		peer.enqueueBytes(data)
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

// addVoiceOwnerLocked 记录 c 对 (channel, member) 语音状态的持有。调用方必须持 h.mu。
func (h *Hub) addVoiceOwnerLocked(c *conn, channelID string) {
	if h.voiceOwners[channelID] == nil {
		h.voiceOwners[channelID] = make(map[string]map[*conn]struct{}, 4)
	}
	owners := h.voiceOwners[channelID][c.member.ID]
	if owners == nil {
		owners = make(map[*conn]struct{}, 2)
		h.voiceOwners[channelID][c.member.ID] = owners
	}
	owners[c] = struct{}{}
	c.voiceChannels[channelID] = struct{}{}
}

// leaveVoiceForConn 让**一条连接**释放它在某频道的语音持有权。
//
// 语音状态按 member_id 存在，但同一成员可能有多条连接（甚至同一频道）。只有该成员
// 在这个频道已无任何持有连接时，才真正把成员移出语音、释放 SFU 房间槽位。退订、
// 显式 voice.leave、断线都走这条，因此单连接断开不会清掉另一条连接的语音。
func (h *Hub) leaveVoiceForConn(c *conn, channelID string) {
	h.mu.Lock()
	memberID := c.member.ID
	delete(c.voiceChannels, channelID)
	owners := h.voiceOwners[channelID][memberID]
	if owners != nil {
		delete(owners, c)
		if len(owners) == 0 {
			delete(h.voiceOwners[channelID], memberID)
		}
	}
	if len(h.voiceOwners[channelID]) == 0 {
		delete(h.voiceOwners, channelID)
	}
	if len(owners) > 0 {
		h.mu.Unlock()
		return
	}
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
		h.broadcastMemberState()
	}
}

// leaveVoiceConnEverywhere 连接断开时释放它在所有频道的语音持有权。
func (h *Hub) leaveVoiceConnEverywhere(c *conn) {
	h.mu.RLock()
	channels := make([]string, 0, len(c.voiceChannels))
	for channelID := range c.voiceChannels {
		channels = append(channels, channelID)
	}
	h.mu.RUnlock()
	for _, channelID := range channels {
		h.leaveVoiceForConn(c, channelID)
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
	// 该频道**实际**的 transport（v1 与策略同值；直连失败降级后变 relay）。
	transport, transportReason := h.channelTransportLocked(channelID)
	_, already := members[memberID]
	// 两种传输方式都有**人数硬上限**，满了必须明确拒绝而不是静默降级：
	//   - relay：槽位 = 语音上限（sfu.MaxRemoteSlots 是每接收端预协商的 sendonly 槽数，
	//     而槽位是「发布者→槽位」的全局唯一映射：N 个发布者就要 N 个槽）。
	//     不做门禁的话第 13 个人能加入、能听见别人，但**没人能听见他**。
	//   - direct：P2P 只支持 2 人（1v1），第 3 人进来同样是「能进但互相听不到」。
	// 两类都是同一类静默降级，所以都在 join 时拦下（错误信息要给用户「出路」）。
	if !already {
		switch transport {
		case "relay":
			if len(members) >= sfu.MaxRemoteSlots {
				h.mu.Unlock()
				c.replyError(env.Type, protocol.CodeConflict,
					fmt.Sprintf("channel voice is full (max %d)", sfu.MaxRemoteSlots), env.ID)
				return
			}
		case "direct":
			if len(members) >= DirectMaxMembers {
				h.mu.Unlock()
				c.replyError(env.Type, protocol.CodeConflict,
					"当前服务器配置为直连模式，语音最多 2 人；"+
						"需要更多人请让管理员用 --voice-mode=relay 重启服务器", env.ID)
				return
			}
		}
	}
	if !already {
		// 角色由服务端单一来源裁决（客户端只消费，不自行推断）：
		//   - relay：服务端 SFU 是 offerer，所有成员都是 answerer；
		//   - direct：频道里还没人 → answerer（先等着，别对着空气发 offer）；
		//     只有一人且他还不是 offerer → 新来者当 offerer（两人一进一退，确定）；
		//     已有一对在通话 → 新来者 answerer（三人及以上的 mesh 暂不支持，
		//     但绝不能因为第三个人进来就把已在通话的两人的 PC 拆了）。
		role := "answerer"
		if transport == "direct" {
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
		if h.voiceTransport[channelID] == "" {
			h.voiceTransport[channelID] = h.voiceMode
			h.voiceTransportReason[channelID] = "server_policy"
			transport, transportReason = h.channelTransportLocked(channelID)
		}
		members[memberID] = protocol.VoiceMemberState{
			MemberID:        memberID,
			Role:            role,
			JoinedSeq:       h.voiceSeq[channelID],
			Transport:       transport,
			TransportReason: transportReason,
		}
	}
	// 无论是否首次加入，这条连接都成为该成员语音状态的一个持有者。
	h.addVoiceOwnerLocked(c, channelID)
	h.mu.Unlock()

	if transport == "relay" {
		room, err := h.ensureRoom(channelID)
		if err != nil {
			// 加入失败要回滚**这条连接**的持有权；若其他连接仍持有，语音状态保留。
			c.hub.leaveVoiceForConn(c, channelID)
			c.replyError(env.Type, protocol.CodeInternal, "create relay room: "+err.Error(), env.ID)
			return
		}
		if err := room.Join(memberID); err != nil {
			c.hub.leaveVoiceForConn(c, channelID)
			c.replyError(env.Type, protocol.CodeInternal, "join relay room: "+err.Error(), env.ID)
			return
		}
	}

	// 在 voice.join.ok 里直接回自己的角色：客户端在「建 PC 之前」就要知道 passive 与否，
	// 不能等 voice.state 广播（那会晚一步，先建错 PC 再拆）。
	c.replyOK(env, protocol.VoiceJoinOK{
		ChannelID: channelID,
		Transport: transport,
		Role:      h.memberRole(channelID, memberID),
	})
	if !already {
		h.log.Info("voice: joined", "channel", channelID, "member", memberID, "transport", transport)
		h.broadcastVoiceState(channelID)
		h.broadcastMemberState()
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
	c.hub.leaveVoiceForConn(c, channelID)
	c.replyOK(env, protocol.VoiceJoinOK{ChannelID: channelID})
}

// voiceFailedCooldown 是同一成员两次「直连失败上报」的最小间隔：网络抖动会连续触发，
// 冷却窗口内只处理第一次（防刷；幂等性另有「已经是 relay」分支兜底）。
const voiceFailedCooldown = 30 * time.Second

// handleVoiceTransportFailed 处理客户端的「本端直连失败」**事实上报**（PROTOCOL.md §4.4）。
//
// 设计要点（都是 Lead 明确要求的）：
//   - 客户端只说「我这边连不上」，降级决定权 100% 在服务端；
//   - 按频道降级（v1 direct 只支持 2 人，一对人连不上 = 这条通话连不上）；
//   - 表达为「把该频道所有成员的 transport 改成 relay」，不是改一个频道级字段；
//   - 幂等：已经是 relay 时不再降级；同一成员 30s 内重复上报直接丢弃；
//   - 单向：本进程生命周期内不再升回 direct（重启服务端才恢复策略原值）。
func (c *conn) handleVoiceTransportFailed(env protocol.Envelope) {
	var req protocol.VoiceTransportFailedRequest
	if err := env.DecodePayload(&req); err != nil {
		c.replyError(env.Type, protocol.CodeInvalidArgument, err.Error(), env.ID)
		return
	}
	channelID := strings.TrimSpace(req.ChannelID)
	if channelID == "" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "channel_id is required", env.ID)
		return
	}
	media := strings.TrimSpace(req.Media)
	if media == "" {
		media = "voice"
	}
	if media != "voice" {
		c.replyError(env.Type, protocol.CodeInvalidArgument, "media must be voice (screen 暂不支持回落)", env.ID)
		return
	}
	reason := strings.TrimSpace(req.Reason)
	switch reason {
	case "ice_failed", "ice_timeout":
	default:
		c.replyError(env.Type, protocol.CodeInvalidArgument,
			"reason must be ice_failed or ice_timeout", env.ID)
		return
	}
	if !c.hub.isSubscribed(c, channelID) {
		c.replyError(env.Type, protocol.CodeForbidden, "not a subscriber of this channel", env.ID)
		return
	}

	h := c.hub
	memberID := c.member.ID
	now := time.Now()

	h.mu.Lock()
	// 只有在语音里的人上报才可能意味着「这条通话连不上」。
	if _, ok := h.voice[channelID][memberID]; !ok {
		current, _ := h.channelTransportLocked(channelID)
		h.mu.Unlock()
		c.replyOK(env, protocol.VoiceTransportFailedOK{
			ChannelID: channelID, Transport: current, Reason: "not_in_voice",
		})
		return
	}
	// 防刷：同一成员 30s 内只处理一次。
	rateKey := channelID + "\x00" + memberID
	if last, ok := h.voiceFailedAt[rateKey]; ok && now.Sub(last) < voiceFailedCooldown {
		current, _ := h.channelTransportLocked(channelID)
		h.mu.Unlock()
		h.log.Info("voice: transport_failed ignored (rate limited)",
			"channel", channelID, "member", memberID, "reason", reason)
		c.replyOK(env, protocol.VoiceTransportFailedOK{
			ChannelID: channelID, Transport: current, Reason: "rate_limited",
		})
		return
	}
	h.voiceFailedAt[rateKey] = now

	current, _ := h.channelTransportLocked(channelID)
	if current == "relay" {
		// 幂等：已经降过了（可能是别的成员先触发），不重复降级。
		h.mu.Unlock()
		c.replyOK(env, protocol.VoiceTransportFailedOK{
			ChannelID: channelID, Transport: "relay", Reason: "already_relay",
		})
		return
	}

	// 降级：把该频道**所有成员**的 transport 表达为 relay（不是改一个频道级字段）。
	h.voiceTransport[channelID] = "relay"
	h.voiceTransportReason[channelID] = "direct_failed_fallback"
	h.voiceDowngradeCount[channelID]++
	triggerCount := h.voiceDowngradeCount[channelID]
	for id, m := range h.voice[channelID] {
		m.Transport = "relay"
		m.TransportReason = "direct_failed_fallback"
		h.voice[channelID][id] = m
	}
	h.mu.Unlock()

	h.log.Warn("voice: transport downgraded direct→relay",
		"channel", channelID, "reason", reason, "member", memberID, "trigger_count", triggerCount)
	// 广播新 voice.state：所有客户端看到自己的 transport 变 relay 后各自重建会话。
	// 重建窗口内各成员独立 leave→join（relay SFU 天然支持先后加入，槽位后绑），
	// 因此「部分已重建、部分还没有」的中间态会自愈，不需要服务端等全员确认。
	h.broadcastVoiceState(channelID)
	c.replyOK(env, protocol.VoiceTransportFailedOK{
		ChannelID:    channelID,
		Transport:    "relay",
		Degraded:     true,
		Reason:       "downgraded",
		TriggerCount: triggerCount,
	})
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

	room, err := sfu.NewVideoWithICE(channelID, h.log, func(memberID, kind, payload string) {
		h.deliverScreenSignal(channelID, memberID, kind, payload)
	}, nil, h.iceConfig)
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
//
// manual=true（新客户端声明 screen.watch.v1）：建 PC 但**不自动绑定任何共享**，
// 只有 screen.watch 才绑定目标——未点不接收。manual=false 保持 v1 自动观看。
func (h *Hub) joinVideoRoom(channelID, memberID string, manual bool) {
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
		if err := room.JoinWithMode(memberID, manual); err != nil {
			h.log.Error("join video room", "channel", channelID, "member", memberID, "err", err)
		}
	}()
}

// videoRoom 返回频道当前的视频中继房间（没有则 nil）。
func (h *Hub) videoRoom(channelID string) *sfu.VideoRoom {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.videoRooms[channelID]
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

// leaveVideoRoomForConn 在**这条连接**退出频道后发现成员是否还被同一成员的其他连接
// 订阅着；只有都不再订阅时才真正把成员从视频房间摘掉。多 WebSocket 下不能被单连接
// 断开误清另一条连接的视频 PC。
func (h *Hub) leaveVideoRoomForConn(c *conn, channelID string) {
	h.mu.RLock()
	memberID := c.member.ID
	stillSubscribed := false
	for peer := range h.subs[channelID] {
		if peer != c && peer.authed && peer.member.ID == memberID {
			stillSubscribed = true
			break
		}
	}
	h.mu.RUnlock()
	if stillSubscribed {
		return
	}
	h.leaveVideoRoom(channelID, memberID)
}

// leaveVideoRoomsForConn 在连接断开时释放它在这些频道里的视频成员资格。
func (h *Hub) leaveVideoRoomsForConn(c *conn, channels []string) {
	for _, ch := range channels {
		h.leaveVideoRoomForConn(c, ch)
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

// RefreshVideoPeer 在共享者开始共享时重建它的视频 PC 并重发 offer：
// 客户端要先把带视频轨的被动会话建好、answer 时才能把轨挂上（ABI v2.11 无重协商）。
func (h *Hub) RefreshVideoPeer(channelID, memberID string) {
	h.mu.RLock()
	room := h.videoRooms[channelID]
	h.mu.RUnlock()
	if room == nil {
		return
	}
	if err := room.Refresh(memberID); err != nil {
		h.log.Error("refresh video peer", "channel", channelID, "member", memberID, "err", err)
	}
}

// channelTransportLocked 返回频道当前**实际**的 transport 与原因（调用方必须持 h.mu）。
//
// v1 全频道一致；将来逐成员混合时，这个函数会按成员返回不同值（协议已经是 per-member）。
func (h *Hub) channelTransportLocked(channelID string) (transport, reason string) {
	if t := h.voiceTransport[channelID]; t != "" {
		if r := h.voiceTransportReason[channelID]; r != "" {
			return t, r
		}
		return t, "server_policy"
	}
	// 频道第一次有人进语音：以服务端策略为事实起点。
	return h.voiceMode, "server_policy"
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
// notifyDirectViewerClosed / Arm 是 B2 的 direct（P2P 1v1）会话控制：
// 观看者停看/重看时通知共享者停止/恢复向该 viewer 的发送传输。共享者保留
// capture/source/track 与 screen.state 的发布状态，不能因此全局 screen.stop。
// relay 不使用它：接收传输在服务端，关接收 PC 即停转发。
func (h *Hub) notifyDirectViewerClosed(channelID, sharerID, viewerID string) {
	h.notifyDirectViewerControl(channelID, sharerID, viewerID, protocol.MediaSignalKindViewerClose)
}

func (h *Hub) notifyDirectViewerArm(channelID, sharerID, viewerID string) {
	h.notifyDirectViewerControl(channelID, sharerID, viewerID, protocol.MediaSignalKindViewerArm)
}

func (h *Hub) notifyDirectViewerControl(channelID, sharerID, viewerID, kind string) {
	if sharerID == "" || viewerID == "" {
		return
	}
	h.mu.RLock()
	targets := make([]*conn, 0, 2)
	for c := range h.subs[channelID] {
		if c.authed && c.member.ID == sharerID {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	if len(targets) == 0 {
		return
	}
	data, err := protocol.MustNew(protocol.TypeMediaSignal, protocol.MediaSignalPush{
		ChannelID: channelID,
		From:      viewerID,
		Media:     "screen",
		Kind:      kind,
	}).Marshal()
	if err != nil {
		h.log.Error("marshal direct viewer control", "kind", kind, "err", err)
		return
	}
	for _, target := range targets {
		target.enqueueBytes(data)
	}
	h.log.Info("direct viewer control sent", "channel", channelID, "sharer", sharerID,
		"viewer", viewerID, "kind", kind)
}

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
