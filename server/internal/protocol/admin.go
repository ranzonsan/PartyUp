package protocol

// 身份/权限第一批（2026-11-07）：角色常量、设置推送、匿名 server-info、管理页 DTO。
// 规格：`.docs/identity-and-onboarding-20261107.md` §3/§4/§7.3/§9。

// 角色值。与 store.Role 的字符串一致；协议层单独列一份避免 server/protocol 循环依赖
// （store 不依赖 protocol，protocol 也不依赖 store）。
const (
	RoleOwner = "owner"
	RoleAdmin = "admin"
	RoleUser  = "user"
)

// TypeSettingsState 是**服务端 → 客户端**的设置变更推送（PROTOCOL.md §1.5）。
//
// 单向通知、没有 .ok/.err 响应；只在 owner/admin 修改设置成功后广播给所有已鉴权
// 连接。客户端收到后覆盖本地缓存即可（整份快照，不做增量）。
const TypeSettingsState = "settings.state"

// SettingsState 是 settings.state 的载荷，也是 GET /api/admin/settings 的响应体。
//
// 只包含 §3A 的「GUI 可改、立即生效」项；§3B/§3C 的项（会话 TTL、监听地址、
// STUN、TLS 等）不进这里——它们属于进程/信任边界，改错会把人锁在门外。
type SettingsState struct {
	// DisplayName 是服务器展示名（--name 是出厂默认值）。
	DisplayName string `json:"display_name"`
	// Announcement 是服务器公告；空串表示没有。
	Announcement string `json:"announcement"`
	// WelcomeMessage 是新成员注册后的欢迎语；空串表示不发。
	WelcomeMessage string `json:"welcome_message"`
	// Registration 是注册策略："invite"（默认，必须邀请码）| "closed"（关闭注册）。
	Registration string `json:"registration"`
	// InviteCreators 是「谁能生成邀请码」："owner" | "admin" | "all"。
	InviteCreators string `json:"invite_creators"`
	// InviteMaxUsesPerCode 是单个邀请码可注册的人数上限（0 = 无限制）。
	InviteMaxUsesPerCode int `json:"invite_max_uses_per_code"`
	// MemberLimit 是成员上限（0 = 无限制）。
	MemberLimit int `json:"member_limit"`
	// HistoryRetentionDays 是消息历史保留天数（0 = 永久保留）。
	HistoryRetentionDays int `json:"history_retention_days"`
	// GlobalMute 为 true 时强制所有在线成员闭麦（客户端执行，服务端广播设置）。
	GlobalMute bool `json:"global_mute"`
	// VoiceMode / ScreenMode 是**危险设置**（仅 owner 可改）：relay | direct。
	VoiceMode  string `json:"voice_mode"`
	ScreenMode string `json:"screen_mode"`
	// MaxScreenShares 是最大并发共享路数（规格 §3B：改后需重启生效，这里只存值）。
	MaxScreenShares int `json:"max_screen_shares"`
}

// ServerInfoPublic 是 GET /api/server-info 的响应（匿名端点，PROTOCOL.md §7.3）。
//
// bootstrap_allowed 只是 UI 提示：真正的授权在 POST /api/bootstrap 内部
// （成员数为 0 + bootstrap token + TTL/用后即焚 + 回环或 TLS 连接）。
type ServerInfoPublic struct {
	Name            string `json:"name"`
	Version         string `json:"version"`
	ProtocolVersion int    `json:"protocol_version"`
	// Initialized 表示库里已有至少一个成员（每次请求重新查库，不做进程内缓存）。
	Initialized bool `json:"initialized"`
	// Registration 是当前注册策略："invite" | "closed"。
	Registration string `json:"registration"`
	VoiceMode    string `json:"voice_mode"`
	ScreenMode   string `json:"screen_mode"`
	// BootstrapAllowed 表示「这条连接 + 当前状态允许 bootstrap」。
	BootstrapAllowed bool `json:"bootstrap_allowed"`
}

// RoleString 归一化任意角色字符串；未知值回退 user（只用于展示，不用于授权）。
func RoleString(role string) string {
	switch role {
	case RoleOwner, RoleAdmin:
		return role
	default:
		return RoleUser
	}
}

// RoleIsAdmin 报告角色是否具备管理员及以上权限。
func RoleIsAdmin(role string) bool { return role == RoleOwner || role == RoleAdmin }
