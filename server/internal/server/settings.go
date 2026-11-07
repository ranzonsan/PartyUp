package server

// 服务器设置（2026-11-07，规格 `.docs/identity-and-onboarding-20261107.md` §3）。
//
// 存储：单行 JSON 存在现有 `settings` 表（key = settingsJSONKey），读写整份快照。
// 为什么整份存而不是每字段一行：设置是一次性整体替换（PATCH 语义），整份 JSON
// 让「读-改-写」天然原子，且不会出现半份旧值半份新值；字段校验在 server 层做。
//
// 不进这里的项（§3B/§3C）：会话 TTL、登录限速、监听地址/端口、数据目录、TLS、
// STUN/TURN、bootstrap token、日志级别——它们属于进程与信任边界，改错会把人
// 锁在门外，只能改命令行/配置文件后重启。

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"partyup/internal/protocol"
	"partyup/internal/store"
)

const settingsJSONKey = "server_settings"

// defaultSettings 返回出厂设置：服务器名与传输模式沿用命令行参数（--name /
// --voice-mode / --screen-mode）作为**默认值**；首次启动写入 DB 后以 DB 为准。
func defaultSettings(name, voiceMode, screenMode string) protocolSettings {
	return protocolSettings{
		DisplayName:     name,
		Registration:    "invite",
		InviteCreators:  "admin",
		VoiceMode:       NormalizeVoiceMode(voiceMode),
		ScreenMode:      NormalizeScreenMode(screenMode),
		MaxScreenShares: 4,
	}
}

// protocolSettings 是持久化形态；与 protocol.SettingsState 字段一致。
// 单独起名是为了在 server 包内做校验/默认值，序列化时用同一个 JSON tag。
type protocolSettings = protocol.SettingsState

// settings 读取当前设置；库里没有（首次启动）时返回默认值。
func (s *Server) settings(ctx context.Context) (protocolSettings, error) {
	raw, err := s.store.GetSetting(ctx, settingsJSONKey)
	if errors.Is(err, store.ErrNotFound) {
		return defaultSettings(s.name, s.voiceModeDefault, s.screenModeDefault), nil
	}
	if err != nil {
		return protocolSettings{}, err
	}
	var st protocolSettings
	if err := json.Unmarshal([]byte(raw), &st); err != nil {
		return protocolSettings{}, fmt.Errorf("decode settings: %w", err)
	}
	st = s.normalizeSettings(st)
	return st, nil
}

// ensureSettings 在启动时把默认设置落库（幂等）：首次启动后 DB 就是唯一真源，
// 之后改 --voice-mode 不会覆盖管理员在 GUI 里的选择（要改请先在管理页改回）。
func (s *Server) ensureSettings() {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := s.store.GetSetting(ctx, settingsJSONKey); err == nil {
		return
	} else if !errors.Is(err, store.ErrNotFound) {
		s.log.Warn("read settings", "err", err)
		return
	}
	st := defaultSettings(s.name, s.voiceModeDefault, s.screenModeDefault)
	data, err := json.Marshal(st)
	if err != nil {
		s.log.Warn("encode default settings", "err", err)
		return
	}
	if err := s.store.SetSetting(ctx, settingsJSONKey, string(data)); err != nil {
		s.log.Warn("persist default settings", "err", err)
	}
}

// saveSettings 覆盖整份设置并广播给在线客户端。
func (s *Server) saveSettings(ctx context.Context, st protocolSettings) error {
	data, err := json.Marshal(st)
	if err != nil {
		return fmt.Errorf("encode settings: %w", err)
	}
	if err := s.store.SetSetting(ctx, settingsJSONKey, string(data)); err != nil {
		return err
	}
	s.hub.BroadcastSettings(st)
	return nil
}

// normalizeSettings 修正越界/空值，保证对外永远是一份合法快照。
func (s *Server) normalizeSettings(st protocolSettings) protocolSettings {
	def := defaultSettings(s.name, s.voiceModeDefault, s.screenModeDefault)
	if strings.TrimSpace(st.DisplayName) == "" {
		st.DisplayName = def.DisplayName
	}
	st.VoiceMode = NormalizeVoiceMode(st.VoiceMode)
	st.ScreenMode = NormalizeScreenMode(st.ScreenMode)
	switch st.Registration {
	case "invite", "closed", "open":
	default:
		st.Registration = "invite"
	}
	switch st.InviteCreators {
	case "owner", "admin", "all":
	default:
		st.InviteCreators = "admin"
	}
	if st.InviteMaxUsesPerCode < 0 {
		st.InviteMaxUsesPerCode = 0
	}
	if st.MemberLimit < 0 {
		st.MemberLimit = 0
	}
	if st.HistoryRetentionDays < 0 {
		st.HistoryRetentionDays = 0
	}
	if st.MaxScreenShares <= 0 {
		st.MaxScreenShares = def.MaxScreenShares
	}
	return st
}

// settingsPatch 是 PATCH /api/admin/settings 的请求体：只允许出现字段白名单里的键。
// 用 map[string]json.RawMessage 是为了能区分「没传」与「显式传零值」。
type settingsPatch map[string]json.RawMessage

// dangerousSettingKeys 是「危险设置」：仅 owner 可改（规格 §2/§3A）。
// 语音/共享模式会改变全服务器的媒体路径，改错影响所有在会的人；最大共享路数是
// 资源上限，调大可能压垮服务端。
var dangerousSettingKeys = map[string]bool{
	"voice_mode":        true,
	"screen_mode":       true,
	"max_screen_shares": true,
}

// applySettingsPatch 把白名单字段合并进 st。返回被改动的键与错误。
// 校验失败时返回 INVALID_ARGUMENT 文案，调用方回 400。
func (s *Server) applySettingsPatch(st *protocolSettings, patch settingsPatch) ([]string, error) {
	changed := make([]string, 0, len(patch))
	for key, raw := range patch {
		switch key {
		case "display_name":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("display_name must be a string")
			}
			v = strings.TrimSpace(v)
			if v == "" || len([]rune(v)) > 64 {
				return nil, fmt.Errorf("display_name must be 1-64 characters")
			}
			if st.DisplayName != v {
				st.DisplayName = v
				changed = append(changed, key)
			}
		case "announcement":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("announcement must be a string")
			}
			if len([]rune(v)) > 2000 {
				return nil, fmt.Errorf("announcement must be at most 2000 characters")
			}
			if st.Announcement != v {
				st.Announcement = v
				changed = append(changed, key)
			}
		case "welcome_message":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("welcome_message must be a string")
			}
			if len([]rune(v)) > 500 {
				return nil, fmt.Errorf("welcome_message must be at most 500 characters")
			}
			if st.WelcomeMessage != v {
				st.WelcomeMessage = v
				changed = append(changed, key)
			}
		case "registration":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil || (v != "invite" && v != "closed" && v != "open") {
				return nil, fmt.Errorf("registration must be invite, closed or open")
			}
			if st.Registration != v {
				st.Registration = v
				changed = append(changed, key)
			}
		case "invite_creators":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil || (v != "owner" && v != "admin" && v != "all") {
				return nil, fmt.Errorf("invite_creators must be owner, admin or all")
			}
			if st.InviteCreators != v {
				st.InviteCreators = v
				changed = append(changed, key)
			}
		case "invite_max_uses_per_code":
			var v int
			if err := json.Unmarshal(raw, &v); err != nil || v < 0 || v > 1000 {
				return nil, fmt.Errorf("invite_max_uses_per_code must be 0-1000")
			}
			if st.InviteMaxUsesPerCode != v {
				st.InviteMaxUsesPerCode = v
				changed = append(changed, key)
			}
		case "member_limit":
			var v int
			if err := json.Unmarshal(raw, &v); err != nil || v < 0 || v > 10000 {
				return nil, fmt.Errorf("member_limit must be 0-10000")
			}
			if st.MemberLimit != v {
				st.MemberLimit = v
				changed = append(changed, key)
			}
		case "history_retention_days":
			var v int
			if err := json.Unmarshal(raw, &v); err != nil || v < 0 || v > 36500 {
				return nil, fmt.Errorf("history_retention_days must be 0-36500")
			}
			if st.HistoryRetentionDays != v {
				st.HistoryRetentionDays = v
				changed = append(changed, key)
			}
		case "global_mute":
			var v bool
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("global_mute must be a boolean")
			}
			if st.GlobalMute != v {
				st.GlobalMute = v
				changed = append(changed, key)
			}
		case "voice_mode":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("voice_mode must be a string")
			}
			v = NormalizeVoiceMode(v)
			if st.VoiceMode != v {
				st.VoiceMode = v
				changed = append(changed, key)
			}
		case "screen_mode":
			var v string
			if err := json.Unmarshal(raw, &v); err != nil {
				return nil, fmt.Errorf("screen_mode must be a string")
			}
			v = NormalizeScreenMode(v)
			if st.ScreenMode != v {
				st.ScreenMode = v
				changed = append(changed, key)
			}
		case "max_screen_shares":
			var v int
			if err := json.Unmarshal(raw, &v); err != nil || v < 1 || v > 32 {
				return nil, fmt.Errorf("max_screen_shares must be 1-32")
			}
			if st.MaxScreenShares != v {
				st.MaxScreenShares = v
				changed = append(changed, key)
			}
		default:
			return nil, fmt.Errorf("unknown settings field %q", key)
		}
	}
	return changed, nil
}
