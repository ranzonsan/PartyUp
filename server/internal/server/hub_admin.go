package server

// Hub 的管理面配套能力（2026-11-07 身份/权限第一批）：
//   - 传输模式的运行时热更（危险设置，仅 owner 可改）；
//   - settings.state 广播；
//   - 管理概览统计；
//   - 频道是否仍被占用（删除前检查）。

import "partyup/internal/protocol"

// modeSnapshot 返回当前传输模式快照。用独立的 modesMu 而不是 h.mu：
// 管理页改模式只需要与 SetModes 互斥，不应和在线路径的 h.mu 抢锁。
func (h *Hub) modeSnapshot() (voiceMode, screenMode string) {
	h.modesMu.RLock()
	defer h.modesMu.RUnlock()
	return h.voiceMode, h.screenMode
}

// SetModes 热更传输模式。返回是否真的发生变化。
//
// 只影响**之后**新建的语音房间/屏幕共享：已建立的 PeerConnection 已经协商完
// 传输路径，不会因为改配置而重建（避免把在线用户直接踢掉）。因此管理页改模式
// 后，正在进行的通话要重进语音/重开共享才会走新路径——这是有意的保守语义。
func (h *Hub) SetModes(voiceMode, screenMode string) bool {
	voiceMode = NormalizeVoiceMode(voiceMode)
	screenMode = NormalizeScreenMode(screenMode)
	h.modesMu.Lock()
	changed := h.voiceMode != voiceMode || h.screenMode != screenMode
	h.voiceMode = voiceMode
	h.screenMode = screenMode
	h.modesMu.Unlock()
	if changed {
		h.log.Info("transport modes changed", "voice_mode", voiceMode, "screen_mode", screenMode)
	}
	return changed
}

// BroadcastSettings 把设置快照推给所有已鉴权连接（settings.state）。
// 客户端收到后整体覆盖本地缓存；不改协议版本、不要求应答。
func (h *Hub) BroadcastSettings(st protocol.SettingsState) {
	env := protocol.MustNew(protocol.TypeSettingsState, st)
	data, err := env.Marshal()
	if err != nil {
		h.log.Error("marshal settings.state", "err", err)
		return
	}
	h.mu.RLock()
	targets := make([]*conn, 0, len(h.conns))
	for c := range h.conns {
		if c.authed {
			targets = append(targets, c)
		}
	}
	h.mu.RUnlock()
	for _, c := range targets {
		c.enqueueBytes(data)
	}
}

// HubStats 是管理概览用的运行时统计。
type HubStats struct {
	Online       int // 已鉴权连接数（不是去重成员数，供容量观察）
	VoiceRooms   int // 当前有人的语音房间数
	ActiveShares int // 当前正在进行的屏幕共享路数
}

// Stats 取一份运行时统计快照。
func (h *Hub) Stats() HubStats {
	h.mu.RLock()
	defer h.mu.RUnlock()
	st := HubStats{}
	for c := range h.conns {
		if c.authed {
			st.Online++
		}
	}
	for _, members := range h.voice {
		if len(members) > 0 {
			st.VoiceRooms++
		}
	}
	for _, shares := range h.shares {
		st.ActiveShares += len(shares)
	}
	return st
}

// ChannelBusy 报告频道是否仍有在线订阅、语音成员或进行中的共享。
// 删除频道前必须为 false，否则会留下悬空的内存状态（订阅者/语音/共享）。
func (h *Hub) ChannelBusy(channelID string) bool {
	h.mu.RLock()
	defer h.mu.RUnlock()
	if len(h.subs[channelID]) > 0 {
		return true
	}
	if len(h.voice[channelID]) > 0 {
		return true
	}
	if len(h.shares[channelID]) > 0 {
		return true
	}
	return false
}
