// Package protocol 是 PartyUp 控制面协议（docs/PROTOCOL.md）的 Go 表示。
//
// 它是服务端与测试共享的唯一定义处：信封形状、消息类型名、错误码、领域对象
// 都在这里，避免 HTTP 层与 WebSocket 层各写一份字符串常量。
//
// 与文档的差异集中记录在 .docs/spikes/2026-11-02-skeleton.md 的「协议补充」一节。
package protocol

import (
	"encoding/json"
	"fmt"
	"time"

	"github.com/oklog/ulid/v2"
)

// Version 是信封的 v 字段。docs/PROTOCOL.md §0：当前恒为 1。
const Version = 1

// 能力协商字符串（加法式，不改变信封版本）。
//
// 旧客户端不认识它们：auth 请求里不带 capabilities，服务端按 v1 行为处理
// （订阅即自动观看全部共享）。新客户端带 CapabilityScreenWatch 表示「我会用
// screen.watch / screen.unwatch 显式选台，未选台时不要给我绑定任何视频」。
const (
	// CapabilityScreenWatch 是**客户端 → 服务端**的 opt-in，也是服务端 → 客户端
	// 的能力广告（auth.ok.server.capabilities）。它同时要求媒体核心有显式的
	// watch/帧代次能力（由 pu_screen_watch_caps 探测，不能只看导出是否存在）。
	CapabilityScreenWatch = "screen.watch.v1"
	// CapabilityScreenWatchSlots 表示服务端会在 screen.watch.ok 里下发 relay
	// 槽位号与单调 generation，供原生把帧归属绑定到「目标 + 代次」。
	CapabilityScreenWatchSlots = "screen.watch.slots.v1"
	// CapabilityScreenWatchIsolated 是 B2 严格接收传输的显式 opt-in：
	// 客户端在 auth 里声明它 + 媒体核心有 receive-only 会话能力后，screen.watch
	// 必须带 receive_id；服务端为每次 watch 新建一条**只接收**的 PeerConnection
	//（新 ICE/DTLS/SSRC、session + nonce），不再复用发布 PC 上的观看槽。
	//
	// 它独立于 CapabilityScreenWatch：只声明旧能力、不声明本能力的客户端继续走
	// v1.1 的混合 PC 槽位路径，行为不变。旧服务端/旧 DLL 不广告本能力时客户端
	// 必须明确报不支持，不能退回「换槽/贴代次」冒充严格隔离。
	CapabilityScreenWatchIsolated = "screen.watch.isolated.v1"
	// CapabilityScreenPublishSessions 是**客户端 → 服务端**的 opt-in（B2b direct）：
	// 本连接（作为共享者）能为单个 viewer 建立**独立的发送传输/会话**——新
	// PeerConnection（新 ICE/DTLS/SSRC），复用已存在的采集源与视频轨，per-viewer
	// open/close，不重启采集、不全局 screen.stop。
	//
	// direct 严格 watch 必须同时满足：viewer 声明 CapabilityScreenWatchIsolated +
	// 共享者的**发布连接**声明本能力。共享者不具备时服务端明确 NOT_IMPLEMENTED，
	// 绝不悄悄退回旧共享 PC 的暂停/恢复冒充隔离。relay 不使用本能力（接收传输在
	// 服务端 SFU）。
	CapabilityScreenPublishSessions = "screen.publish.sessions.v1"
)

// MediaSignalKind 是 media.signal.kind 的取值。
const (
	MediaSignalKindOffer     = "offer"
	MediaSignalKindAnswer    = "answer"
	MediaSignalKindCandidate = "candidate"
	// MediaSignalKindViewerClose / ViewerArm 是服务端 → 共享者的**会话控制**，
	// 只用于 direct（P2P 1v1）：观看者停看/重看时通知共享者停止或恢复向该
	// viewer 的发送传输。共享者保留采集源、视频轨与 screen.state 的发布状态，
	// 绝不等价于全局 screen.stop。
	//
	// 两种载荷形态（用 session_id 区分）：
	//   - 空 session_id：legacy 观看者，共享者只暂停/恢复既有 PC 对该 viewer 的发送；
	//   - 非空 session_id：B2b 严格观看者，共享者建立/关闭**该会话自己的发送传输**
	//     （新 PC），session_nonce/generation 来自服务端 arm，不能由客户端自报。
	MediaSignalKindViewerClose = "viewer_close"
	MediaSignalKindViewerArm   = "viewer_arm"
	// MediaSignalKindReceiveClose 是服务端 → 观看者的**会话控制**（B2b direct）：
	// 共享者停发/断线/会话被撤销时，通知观看者关闭对应 receive 会话。观看者只
	// 销毁自己的接收传输，不碰发布、采集或语音。
	MediaSignalKindReceiveClose = "receive_close"
)

// 载荷上限。骨架先用固定值，后续按 PROTOCOL.md §8 的扩展位调整。
const (
	MaxBodyBytes     = 4096  // text.send.body 的 UTF-8 字节上限
	MaxChannelName   = 32    // 频道名 rune 数上限
	MaxHistoryLimit  = 200   // text.history.limit 上限
	MaxSignalPayload = 65536 // media.signal.payload 的 UTF-8 字节上限（SDP 有几 KB）
)

// 错误码，见 docs/PROTOCOL.md §0「第一批」。
const (
	CodeUnauthenticated = "UNAUTHENTICATED"
	CodeForbidden       = "FORBIDDEN"
	CodeNotFound        = "NOT_FOUND"
	CodeConflict        = "CONFLICT"
	CodeRateLimited     = "RATE_LIMITED"
	CodeInternal        = "INTERNAL"

	// CodeNotImplemented 表示服务端当前配置不支持该能力（例如纯 SFU 中继模式下的屏幕共享）。
	// 与 INTERNAL 区分开：这是**可预期的不支持**，客户端应把 message 直接展示给用户。
	CodeNotImplemented = "NOT_IMPLEMENTED"

	// CodeInvalidArgument 是骨架新增的码：载荷形状/字段非法。
	// PROTOCOL.md 的第一批里没有它，而 HTTP 400 与 WS 参数错误需要一个稳定名字，
	// 建议下一版文档补入（见 spike 文档）。
	CodeInvalidArgument = "INVALID_ARGUMENT"

	// CodeSessionReplaced 表示这个 token 已被该账号**更新的一次登录**撤销
	// （单端登录：新登录踢掉旧会话）。与 UNAUTHENTICATED 分开是为了让客户端能
	// 把「账号已在另一处登录」和「token 是乱填的/已过期」区分开提示用户。
	// 见 PROTOCOL.md §1.1 的 auth.err 码表与 §1.2 的 session.replaced。
	CodeSessionReplaced = "SESSION_REPLACED"

	// 身份/权限第一批错误码（2026-11-07，规格 `.docs/identity-and-onboarding-20261107.md` §9）。
	// 与既有码一样用 UPPER_SNAKE；规格里的 member_banned 等小写名是同一批码的口语写法。
	//
	// CodeMemberBanned：该账号已被封禁（登录/WS 鉴权//api/me 一律拒绝）。
	CodeMemberBanned = "MEMBER_BANNED"
	// CodeOwnerOnly：该操作只有 owner 能做（任免管理员、危险设置、转让所有权）。
	CodeOwnerOnly = "OWNER_ONLY"
	// CodeAdminOnly：该操作至少需要 admin（普通用户访问管理端点）。
	CodeAdminOnly = "ADMIN_ONLY"
	// CodeNotBootstrapped：服务器还没有 owner（未初始化），需要先 bootstrap。
	CodeNotBootstrapped = "NOT_BOOTSTRAPPED"
	// CodeAlreadyBootstrapped：服务器已初始化，bootstrap 被拒绝。
	CodeAlreadyBootstrapped = "ALREADY_BOOTSTRAPPED"
)

// WebSocket 关闭码（RFC 6455 §7.4.2：4000–4999 是应用私有段）。
//
// 关闭码与 session.replaced 的 reason 一一对应，客户端可以只按码做兜底、
// 也可以按应用层 reason 显示更准确的文案（两处都提供是为了排障友好）：
//
//	4001 login_elsewhere —— 同一账号在别处登录，旧会话被替换（batch 1 已验收，语义不变）
//	4002 kicked          —— 被管理员踢下线（2026-11-07 补）
//	4003 banned          —— 被管理员封禁（2026-11-07 补）
const (
	// CloseSessionReplaced：连接因会话被替换或主动退出而由服务端结束。
	// 这一帧之前服务端一定先发过一条 session.replaced 应用层消息——客户端应以
	// 应用层消息的 reason 决定提示文案，关闭码只用于诊断与兜底。
	CloseSessionReplaced = 4001
	// CloseSessionKicked：被管理员踢下线（会话已撤销，不能自动重连）。
	CloseSessionKicked = 4002
	// CloseSessionBanned：被管理员封禁（账号级，之后登录会被 403 MEMBER_BANNED）。
	CloseSessionBanned = 4003
)

// 消息类型（type 字段）。命名规则 `<域>.<动作>`，响应为 `+".ok"` / `+".err"`。
const (
	TypeAuth           = "auth"
	TypePing           = "ping"
	TypePong           = "pong"
	TypeChannelList    = "channel.list"
	TypeChannelCreate  = "channel.create"
	TypeChannelSub     = "channel.subscribe"
	TypeChannelUnsub   = "channel.unsubscribe"
	TypeTextSend       = "text.send"
	TypeTextNew        = "text.new"
	TypeTextHistory    = "text.history"
	TypePresence       = "presence"
	TypeMemberRegister = "member.register"
	// 全服务器名册：member.list 是认证后的请求，members.state 是变化推送。
	// presence 仍是频道内订阅者列表（兼容旧客户端），两者不互相替代。
	TypeMemberList   = "member.list"
	TypeMembersState = "members.state"

	// 屏幕共享与媒体信令（PROTOCOL.md §4.2 / §5）。
	TypeScreenStart = "screen.start"
	TypeScreenStop  = "screen.stop"
	TypeScreenState = "screen.state"
	TypeMediaSignal = "media.signal"
	// v1.1：观看者显式选择/停止一路共享（一次一路）。
	// 旧客户端不发这两条消息 = 保留 v1 自动观看行为；新客户端在 auth 里声明
	// CapabilityScreenWatch 后，服务端只为它绑定已 watch 的目标。
	TypeScreenWatch   = "screen.watch"
	TypeScreenUnwatch = "screen.unwatch"

	// 语音（PROTOCOL.md §4）。relay 模式由服务端 SFU 中继，direct 模式走 media.signal 直连。
	TypeVoiceJoin      = "voice.join"
	TypeVoiceLeave     = "voice.leave"
	TypeVoiceSetMute   = "voice.set_mute"
	TypeVoiceSetDeafen = "voice.set_deafen"
	TypeVoiceState     = "voice.state"

	// 服务器级信息（PROTOCOL.md §1.4）。客户端在鉴权后请求一次，缓存起来；
	// 与语音/屏幕房间无关，任何 PC 创建之前都可以先拿到（这是 stun_urls 的时序要求）。
	TypeServerInfo = "server.info"

	// TypeVoiceTransportFailed 是客户端**上报事实**的消息（PROTOCOL.md §4.4）：
	// 「我这边直连 ICE 失败/超时」。是否有必要降级、怎么降级，由服务端决定。
	TypeVoiceTransportFailed = "voice.transport_failed"

	// TypeSessionReplaced 是**服务端 → 客户端**的会话终止通知（单端登录/踢下线/
	// 封禁/主动退出，PROTOCOL.md §1.2）：这条连接所属的 token 已被撤销，服务端
	// 发送本消息后立即关闭连接（关闭码见 CloseCodeForSessionReason）。
	//
	// 单向通知、没有 .ok/.err 响应，客户端不应回包。收到后应停止重连循环并按
	// reason 提示用户（login_elsewhere = 「账号已在另一处登录」、kicked =
	// 「已被管理员移出」、banned = 「账号已被封禁」），否则会被下一次登录/鉴权
	// 再次踢掉。
	TypeSessionReplaced = "session.replaced"
)

// session.replaced 的 reason 取值（PROTOCOL.md §1.2）。
const (
	// SessionReasonLoginElsewhere：同一账号在别处登录成功，本会话被替换。
	SessionReasonLoginElsewhere = "login_elsewhere"
	// SessionReasonLogout：用户调用 POST /api/logout **主动退出**，同一 token 的
	// 所有连接都会收到。客户端按「主动退出」处理，不提示被顶号。
	SessionReasonLogout = "logout"
	// SessionReasonKicked：被管理员踢下线（POST /api/admin/members/{id}/kick）。
	SessionReasonKicked = "kicked"
	// SessionReasonBanned：被管理员封禁（POST /api/admin/members/{id}/ban）。
	SessionReasonBanned = "banned"
)

// CloseCodeForSessionReason 把 session.replaced 的 reason 映射成 WebSocket 关闭码。
//
// 未知 reason 回退到 4001（旧客户端只认这一个码，不能让它们因为新 reason 拿不到
// 已知关闭语义）。主动退出（logout）沿用 4001：它同样是「这条连接所属的会话结束」，
// 客户端本来就在退出流程里，不需要区分。
func CloseCodeForSessionReason(reason string) int {
	switch reason {
	case SessionReasonKicked:
		return CloseSessionKicked
	case SessionReasonBanned:
		return CloseSessionBanned
	default:
		return CloseSessionReplaced
	}
}

// TypeOK 返回请求类型对应的成功响应类型：`text.send` → `text.send.ok`。
func TypeOK(reqType string) string { return reqType + ".ok" }

// TypeError 返回请求类型对应的失败响应类型：`text.send` → `text.send.err`。
func TypeError(reqType string) string { return reqType + ".err" }

// Envelope 是所有控制消息的通用信封。
type Envelope struct {
	V       int             `json:"v"`
	ID      string          `json:"id"`
	Type    string          `json:"type"`
	TS      int64           `json:"ts"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

// ErrorPayload 是 `<type>.err` 的载荷。
type ErrorPayload struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// NewID 生成一个 ULID 字符串。PROTOCOL.md §0 要求客户端 id 是 ULID；
// 服务端推送的消息同样要带 id，这里共用一套生成器。
func NewID() string { return ulid.Make().String() }

// NowMS 返回毫秒 epoch。服务端所有对外时间戳都用它，客户端时间仅供参考。
func NowMS() int64 { return time.Now().UnixMilli() }

// New 构造一个信封，payload 为 nil 时省略该字段。
func New(msgType string, payload any) (Envelope, error) {
	env := Envelope{V: Version, ID: NewID(), Type: msgType, TS: NowMS()}
	if payload == nil {
		return env, nil
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		return Envelope{}, fmt.Errorf("marshal payload of %s: %w", msgType, err)
	}
	env.Payload = raw
	return env, nil
}

// MustNew 与 New 相同，但把序列化失败当作编码错误（payload 全部是 POD 结构，
// 失败只可能是代码写错）。用于服务端推送路径，避免层层传 error。
func MustNew(msgType string, payload any) Envelope {
	env, err := New(msgType, payload)
	if err != nil {
		panic(err)
	}
	return env
}

// Marshal 把信封序列化为一行 JSON。
func (e Envelope) Marshal() ([]byte, error) { return json.Marshal(e) }

// Decode 解析并校验一个信封。校验项：JSON 合法、v 等于当前版本、type 非空。
func Decode(data []byte) (Envelope, error) {
	var env Envelope
	if err := json.Unmarshal(data, &env); err != nil {
		return Envelope{}, fmt.Errorf("invalid envelope json: %w", err)
	}
	if env.V != Version {
		return Envelope{}, fmt.Errorf("unsupported protocol version %d (want %d)", env.V, Version)
	}
	if env.Type == "" {
		return Envelope{}, fmt.Errorf("missing type")
	}
	return env, nil
}

// DecodePayload 把信封载荷解到 out。载荷缺失按零值处理（空对象同样合法）。
func (e Envelope) DecodePayload(out any) error {
	if len(e.Payload) == 0 || string(e.Payload) == "null" {
		return nil
	}
	if err := json.Unmarshal(e.Payload, out); err != nil {
		return fmt.Errorf("invalid payload for %s: %w", e.Type, err)
	}
	return nil
}

// ------------------------------------------------------------------
// 领域对象
// ------------------------------------------------------------------

// Member 是某个 Server 上的成员（GLOSSARY.md：成员身份不跨 Server）。
type Member struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	// DisplayName 是展示名，独立于唯一的登录名 Name（2026-11-06 补入）。
	// 旧库/未设置显示名时等于 Name；客户端应优先显示它，但**不得**把它当身份 ID。
	DisplayName string `json:"display_name"`
	// Role 是服务端唯一权限真源（2026-11-07）："owner" | "admin" | "user"。
	// 旧客户端忽略未知字段即可；新客户端用它决定「服务器管理」入口。
	Role string `json:"role"`
	// Admin 是旧协议的布尔兼容位，恒等于 role 是 admin 或 owner。
	// 新代码一律读 Role；保留它是为了让旧客户端仍能工作。
	Admin bool `json:"admin"`
}

// ServerInfo 是 auth.ok 里的 server 字段：服务端自我介绍与版本范围。
type ServerInfo struct {
	Name            string   `json:"name"`
	Version         string   `json:"version"`
	ProtocolVersion int      `json:"protocol_version"`
	Capabilities    []string `json:"capabilities"`
}

// VoiceMember 是语音区的成员状态占位。骨架不实现语音，恒为空数组。
type VoiceMember struct {
	MemberID string `json:"member_id"`
	Muted    bool   `json:"muted"`
	Deafened bool   `json:"deafened"`
}

// ScreenSource 是一路可共享的源（显示器 / 窗口 / 合成源），与媒体核心的
// pu_source_info 对齐。
type ScreenSource struct {
	ID     string `json:"id"`
	Title  string `json:"title"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
	Kind   string `json:"kind"` // screen | window | synthetic
}

// ShareState 是一路正在进行的共享（screen.state 的元素）。
type ShareState struct {
	SharerID   string       `json:"sharer_id"`
	MemberName string       `json:"member_name"`
	Source     ScreenSource `json:"source"`
	Mode       string       `json:"mode"` // direct | relay
	Viewers    []string     `json:"viewers"`
}

// Channel 是频道。三区同时存在：文字区（消息流）、语音区、共享区。
type Channel struct {
	ID     string        `json:"id"`
	Name   string        `json:"name"`
	Voice  []VoiceMember `json:"voice"`
	Shares []ShareState  `json:"shares"`
}

// TextMessage 是一条文字消息。text.new 的载荷与 text.history 的元素同形。
//
// MemberName 是 PROTOCOL.md §3 之外的补充字段（原文档只有 member_id）：没有它，
// 历史消息里的离线成员只能显示 id 前缀。服务端在 text.new 与 text.history 里都填。
type TextMessage struct {
	ChannelID  string `json:"channel_id"`
	ID         string `json:"id"`
	Seq        int64  `json:"seq"`
	MemberID   string `json:"member_id"`
	MemberName string `json:"member_name"`
	// MemberDisplayName 是展示名（2026-11-06 补入）。MemberName 仍是唯一登录名，
	// 两者都下发，旧客户端继续读 member_name，新客户端优先显示 display 名。
	MemberDisplayName string `json:"member_display_name"`
	Body              string `json:"body"`
	TS                int64  `json:"ts"`
}

// PresenceMember 是 presence 里的成员条目。
type PresenceMember struct {
	MemberID string `json:"member_id"`
	Name     string `json:"name"`
	// DisplayName 与 name 同时下发：name 保持唯一登录名语义，display_name 仅用于展示。
	DisplayName string `json:"display_name"`
	State       string `json:"state"` // online | voice | sharing
}

// ------------------------------------------------------------------
// 请求/响应载荷
// ------------------------------------------------------------------

// AuthRequest 是 auth 的载荷。
type AuthRequest struct {
	Token string `json:"token"`
	// Capabilities 是客户端的**加法式**能力声明。旧客户端省略 = 空数组，
	// 服务端按 v1 兼容行为处理；新客户端用 CapabilityScreenWatch 声明
	// 「我会显式 screen.watch，未选台时不要自动观看」。
	Capabilities []string `json:"capabilities,omitempty"`
}

// AuthOK 是 auth.ok 的载荷。
type AuthOK struct {
	Member Member     `json:"member"`
	Server ServerInfo `json:"server"`
}

// SessionReplacedPayload 是 session.replaced 的载荷（服务端 → 客户端单向通知）。
//
// 字段刻意只有一个 reason：客户端不需要（也不应该）根据服务端下发的任何 id
// 决定是否断开——收到这条消息本身就意味着「当前连接所属的会话已经失效」。
type SessionReplacedPayload struct {
	// Reason 是失效原因，取值见 SessionReasonLoginElsewhere / SessionReasonKicked /
	// SessionReasonBanned（主动退出为 SessionReasonLogout）。关闭码由
	// CloseCodeForSessionReason 给出同义映射。
	Reason string `json:"reason"`
}

// ChannelListOK 是 channel.list.ok 的载荷。
type ChannelListOK struct {
	Channels []Channel `json:"channels"`
}

// ChannelCreateRequest 是 channel.create 的载荷（骨架对 PROTOCOL.md 的补充，
// 见 spike 文档：§2 只有 list/subscribe/unsubscribe，没有创建入口）。
type ChannelCreateRequest struct {
	Name string `json:"name"`
}

// ChannelCreateOK 是 channel.create.ok 的载荷。
type ChannelCreateOK struct {
	Channel Channel `json:"channel"`
}

// ChannelSubscribeRequest 是 channel.subscribe / channel.unsubscribe 的载荷。
type ChannelSubscribeRequest struct {
	ChannelID string `json:"channel_id"`
}

// ChannelSubscribeOK 是 channel.subscribe.ok / channel.unsubscribe.ok 的载荷。
type ChannelSubscribeOK struct {
	ChannelID string `json:"channel_id"`
}

// TextSendRequest 是 text.send 的载荷。
type TextSendRequest struct {
	ChannelID   string `json:"channel_id"`
	ClientMsgID string `json:"client_msg_id"`
	Body        string `json:"body"`
}

// TextSendOK 是 text.send.ok 的载荷。
//
// ClientMsgID 是 PROTOCOL.md §3 之外的补充字段：文档只写 `{ id, seq, ts }`，
// 但客户端要靠它把本地乐观显示的消息与服务端 id 对上，否则只能靠 seq 猜。
// 建议下一版文档补入。
type TextSendOK struct {
	ID          string `json:"id"`
	Seq         int64  `json:"seq"`
	TS          int64  `json:"ts"`
	ClientMsgID string `json:"client_msg_id,omitempty"`
}

// TextHistoryRequest 是 text.history 的载荷。
type TextHistoryRequest struct {
	ChannelID string `json:"channel_id"`
	BeforeSeq int64  `json:"before_seq"`
	Limit     int    `json:"limit"`
}

// TextHistoryOK 是 text.history.ok 的载荷。messages 按 seq 升序。
type TextHistoryOK struct {
	Messages []TextMessage `json:"messages"`
}

// PresencePayload 是 presence 推送的载荷。
//
// 注意这是**频道范围**列表（频道当前订阅者），语义与 v1 一致；全服务器名册见
// MemberListOK / MembersStatePayload。两者刻意分开：旧客户端只消费 presence，
// 不会因为收到全服数据而把「频道内成员」显示成全服名单。
type PresencePayload struct {
	ChannelID string           `json:"channel_id"`
	Members   []PresenceMember `json:"members"`
}

// MemberRosterEntry 是全服务器名册（含离线）的一项。
//
// Online 按 member_id 聚合该成员的所有 WebSocket；State 由服务端推导：
//   - offline：没有任何已鉴权连接；
//   - idle：在线，但不在任何频道的语音里（「空闲」不等于没有键盘活动）；
//   - voice：在线且至少在一个频道的语音里。
//
// VoiceChannelIDs 是**实际**语音频道集合，按 id 排序。它可能多于一项（同一成员
// 可通过多条连接分别进入不同频道），因此不能用单个字段冒充唯一频道。
// 文字订阅是另一套状态，不在这里、也不能拿来推断语音位置。
type MemberRosterEntry struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	DisplayName string `json:"display_name"`
	// Role 与 Member.Role 同义；Admin 是兼容位（role ∈ {owner,admin}）。
	Role            string   `json:"role"`
	Admin           bool     `json:"admin"`
	Online          bool     `json:"online"`
	State           string   `json:"state"` // offline | idle | voice
	VoiceChannelIDs []string `json:"voice_channel_ids"`
}

// MemberListOK 既是 member.list.ok 的载荷，也是 members.state 推送的载荷。
type MemberListOK struct {
	Members []MemberRosterEntry `json:"members"`
}

// ------------------------------------------------------------------
// 屏幕共享 / 媒体信令（PROTOCOL.md §4.2、§5）
// ------------------------------------------------------------------

// ScreenStartRequest 是 screen.start 的载荷：某成员在频道里开始一路共享。
type ScreenStartRequest struct {
	ChannelID string       `json:"channel_id"`
	Source    ScreenSource `json:"source"`
	Mode      string       `json:"mode,omitempty"` // direct（默认）| relay；骨架只做 direct
}

// ScreenStopRequest 是 screen.stop 的载荷。
type ScreenStopRequest struct {
	ChannelID string `json:"channel_id"`
}

// ScreenStatePayload 是 screen.state 推送的载荷：频道内当前所有共享。
type ScreenStatePayload struct {
	ChannelID string `json:"channel_id"`
	// Mode 是服务端屏幕共享传输策略（--screen-mode），与 VoiceMode 独立。
	// 客户端据此决定共享信令 target（relay → sfu，direct → 成员）以及本地角色。
	Mode   string       `json:"mode"` // relay | direct
	Shares []ShareState `json:"shares"`
}

// ScreenWatchRequest 是 screen.watch 的载荷：观看者显式选择一路共享。
//
// 一次一路：同一成员在同意频道再次 watch 会替换旧目标（服务端先解绑旧槽）。
// SharerID 必须当前正在共享且与观看者同频道；不能 watch 自己。
type ScreenWatchRequest struct {
	ChannelID string `json:"channel_id"`
	SharerID  string `json:"sharer_id"`
	// ReceiveID 是严格接收传输的客户端会话标识（B2，随机 16–64 字节的
	// URL-safe 字符串）。客户端必须在发出本请求**之前**用它建好 native 的
	// receive-only 会话，服务端的 offer 才能落到同一条接收 PC 上。
	//
	// 只有声明了 CapabilityScreenWatchIsolated 的连接才允许/必须带它：
	// 省略时按旧 v1.1 混合槽位路径处理，行为不变。
	ReceiveID string `json:"receive_id,omitempty"`
}

// ScreenWatchOK 是 screen.watch.ok 的载荷。
//
// Slot 只有旧 relay 路径有意义；严格接收路径恒为 -1（不再复用发布 PC 上的槽）。
// SessionID/Nonce 只在严格路径出现：session_id 是客户端在 watch 前生成的
// receive_id（服务端原样回显，用于把 offer/answer/ICE 路由到同一条接收 PC），
// nonce 由服务端生成，绑定 (连接, 目标, generation)；后续 media.signal 必须
// 同时带对 session_id 与 nonce，否则按会话拒绝（过期/错身份的 answer/ICE 不会
// 进入新接收器）。
//
// Generation 是该观看者在本频道的单调代次（每次成功 watch 递增）。原生用它
// 绑定「目标 + 代次」，旧代次的迟到帧即使回调晚到也不能冒充新目标。
type ScreenWatchOK struct {
	ChannelID  string `json:"channel_id"`
	SharerID   string `json:"sharer_id"`
	Slot       int    `json:"slot"`
	Generation uint64 `json:"generation"`
	Transport  string `json:"transport"` // relay | direct（服务端裁决，客户端只消费）
	SessionID  string `json:"session_id,omitempty"`
	Nonce      string `json:"nonce,omitempty"`
	// Isolated 表示这次 watch 走的是 B2 的独立接收传输。relay 下接收 PC 在服务端
	// SFU；direct 下接收 PC 在观看者本端，对端共享者为该会话新建独立的发送传输
	// （screen.publish.sessions.v1）。两种情况下 session/nonce/generation 的语义相同。
	Isolated bool `json:"isolated,omitempty"`
}

// ScreenUnwatchRequest 是 screen.unwatch 的载荷。两种互斥语义：
//
//   - ReceiveID 非空（B2 严格接收）：按**会话**精确撤销。服务端只清「本连接持有 +
//     session 完全匹配」的那条接收 lease；SharerID 不参与身份判定（目标相同不代表
//     同一会话，旧 target guard 不能冒充严格身份）。会话已关闭、属于同成员另一条
//     连接、或已被新会话取代时，按幂等成功返回空 SharerID，不动任何 lease。
//   - ReceiveID 为空：v1.1 legacy 语义不变——SharerID 省略表示「停止当前目标」；
//     若给出则必须与当前目标一致，否则按幂等成功处理（不误停别的目标）。
//
// 严格路径必须在拿到 ACK 之前也能撤销：receive_id 是客户端在 watch 前生成的会话
// 标识，客户端无需等待 watch.ok 即可精确撤租（迟到 ACK 之后还会再撤一次）。
type ScreenUnwatchRequest struct {
	ChannelID string `json:"channel_id"`
	SharerID  string `json:"sharer_id,omitempty"`
	ReceiveID string `json:"receive_id,omitempty"`
}

// ScreenUnwatchOK 是 screen.unwatch.ok 的载荷。SharerID 是实际被停止的目标
// （没有目标时为空串），客户端据此清理本地状态。
type ScreenUnwatchOK struct {
	ChannelID string `json:"channel_id"`
	SharerID  string `json:"sharer_id"`
}

// MediaSignalRequest 是客户端上行的一条媒体信令。
//
// Payload 是媒体核心吐出来的原始信令信封（{"type":"offer","sdp":...} /
// {"type":"answer",...} / {"type":"candidate",...}）的**原样字符串**：服务端不解析、
// 不改写 SDP，只按 To 定向投递或按频道广播。Kind 由客户端从 pu_signal_event.kind
// 映射（offer|answer|candidate），服务端只做白名单校验。
type MediaSignalRequest struct {
	ChannelID string `json:"channel_id"`
	To        string `json:"to,omitempty"`
	// Target 明确指定接收方：省略 = 按 To/广播处理（direct 模式，发给成员）；
	// "sfu" = 交给服务端 SFU（relay 模式，offer/answer/candidate 的对象都是服务端）。
	// 不靠猜：relay 与 direct 的信令信封长得一样，只有这个字段能区分。
	Target string `json:"target,omitempty"`
	Media  string `json:"media"` // screen | voice
	Kind   string `json:"kind"`  // offer | answer | candidate
	// SessionID / SessionNonce 是 B2 严格接收会话的归属（screen）。
	// relay 下服务端用它把信令路由到**当前**接收 lease；direct 下服务端用它把信令
	// 路由到该会话的**对端连接**（viewer ↔ sharer），并拒绝过期/错身份/错方向的
	// 信令。客户端不能只靠 payload 内容或全局 epoch。
	SessionID    string `json:"session_id,omitempty"`
	SessionNonce string `json:"session_nonce,omitempty"`
	// Generation 是 screen.watch.ok 下发的代次，严格会话的后续信令必须原样带回。
	// 旧客户端（没有 receive_id/session_id）不进入严格路径，省略该字段按 0 解码，
	// legacy 广播语义不变；严格路径要求与 arm 时的代次精确相等，0 不能冒充任何代次。
	Generation uint64 `json:"generation,omitempty"`
	Payload    string `json:"payload"`
}

// MediaSignalPush 是服务端下发的媒体信令（media.signal），字段与上行一致，
// 另加发送方身份，接收方据此决定把 answer/candidate 发给谁。
type MediaSignalPush struct {
	ChannelID string `json:"channel_id"`
	// Target 与上行一致："sfu" 表示这条信令的对象是服务端 SFU（relay 模式），
	// 空表示对象是频道内成员（direct 模式）。
	Target   string `json:"target,omitempty"`
	From     string `json:"from"`
	FromName string `json:"from_name"`
	Media    string `json:"media"`
	Kind     string `json:"kind"`
	// SessionID / SessionNonce / Generation 只在严格接收路径出现：
	// offer 携带它们让原生把 SDP 灌进正确的 receive-only 会话；viewer_close /
	// viewer_arm 携带它们让共享者只停/恢复对应会话的发送传输。
	SessionID    string `json:"session_id,omitempty"`
	SessionNonce string `json:"session_nonce,omitempty"`
	Generation   uint64 `json:"generation,omitempty"`
	Payload      string `json:"payload"`
}

// ScreenStartOK / ScreenStopOK / MediaSignalOK 是三类共享信令的成功响应载荷。
// 只回 channel_id：真实状态由 screen.state 推送负责，响应只用于配对与错误上报。
type ScreenStartOK struct {
	ChannelID string `json:"channel_id"`
}

type ScreenStopOK struct {
	ChannelID string `json:"channel_id"`
}

type MediaSignalOK struct {
	ChannelID string `json:"channel_id"`
}

// ------------------------------------------------------------------
// 语音（PROTOCOL.md §4）与 SFU 中继（§4.1）
// ------------------------------------------------------------------

// VoiceJoinRequest 是 voice.join 的上行载荷。
//
// 传输方式（relay / direct）由**服务端策略**决定（启动参数 --voice-mode），
// 客户端不提供也无法影响它；客户端只从 voice.join.ok.transport 与 voice.state.mode 读取结果。
// 旧客户端即使多传一个 "mode" 字段，服务端也会忽略（JSON 解码不认未知字段）。
type VoiceJoinRequest struct {
	ChannelID string `json:"channel_id"`
}

// VoiceLeaveRequest 是 voice.leave 的上行载荷。
type VoiceLeaveRequest struct {
	ChannelID string `json:"channel_id"`
}

// VoiceSetMuteRequest / VoiceSetDeafenRequest 上报本端静音状态，供 voice.state 广播。
// v1 的静音只作用于本地 ADM，但别人需要看到「谁静音了」，所以必须上报。
type VoiceSetMuteRequest struct {
	ChannelID string `json:"channel_id"`
	Muted     bool   `json:"muted"`
}

type VoiceSetDeafenRequest struct {
	ChannelID string `json:"channel_id"`
	Deafened  bool   `json:"deafened"`
}

// VoiceMemberState 是 voice.state 里的一个成员条目。
type VoiceMemberState struct {
	MemberID string `json:"member_id"`
	Muted    bool   `json:"muted"`
	Deafened bool   `json:"deafened"`
	// Role 是该成员在**直连（direct）协商中的角色，由服务端裁决**：
	//   offerer  = 负责向频道内其他成员发 offer
	//   answerer = 等待 offer 并应答
	// 客户端不得再按「谁先来」自行推断——两端各自推断会在同时加入时得出
	// 「双方都是 answerer」（都只看到对方、没看到自己），结果是没人发 offer、完全没声音。
	// relay 模式下恒为 answerer（服务端 SFU 才是 offerer）。
	Role string `json:"role"`
	// JoinedSeq 是同一频道内单调递增的加入序号（仅用于诊断与稳定排序，不承诺跨会话语义）。
	JoinedSeq int `json:"joined_seq,omitempty"`
	// Transport 是该成员**实际使用**的传输路径（relay | direct），按成员下发。
	//
	// v1 服务端给所有人填同一个值（行为上仍是频道级），但**协议结构必须 per-member**：
	// 逐成员混合路径是 v1 之后的方向，届时只改服务端的填充逻辑，协议与客户端都不用重设计
	// （见 PRODUCT.md 传输策略硬要求）。
	//
	// 与 VoiceStatePayload.VoiceMode 语义不同：VoiceMode 是**服务端策略**（配置成什么），
	// 这里是**这个成员实际在用什么**（事实）。
	Transport string `json:"transport"`
	// TransportReason 说明该成员为什么是这个 transport：
	//   server_policy          = 服务端策略原值
	//   direct_failed_fallback = 直连失败后的自动回落（v1 整个频道一起降级）
	TransportReason string `json:"transport_reason"`
}

// VoiceSlotState 是「第几个远端音频槽位 → 哪个成员」的映射项（仅 relay 模式有）。
// 客户端用它把 native 侧 id 为 "partyup-slot-<index>" 的远端音轨映射到具体成员：
// 槽位在设计上就决定它转发的发布者，且占位轨让每条 m-line 的 msid 在 offer 时就固定，
// 所以即使发布者是后加入、后绑定的，track id 依然稳定可解析。
type VoiceSlotState struct {
	Index    int    `json:"index"`     // 槽位序号（0 起，= 远端音轨序号）
	MemberID string `json:"member_id"` // 该槽位转发的发布者
	Muted    bool   `json:"muted"`     // 该发布者当前是否静音（元数据，不拦 RTP）
}

// VoiceStatePayload 是 voice.state 推送的载荷（频道内语音成员快照）。
type VoiceStatePayload struct {
	ChannelID string `json:"channel_id"`
	// VoiceMode / ScreenMode 是**两条独立的服务端策略**（--voice-mode / --screen-mode），
	// 客户端只消费、不参与选择。刻意拆成两个字段而不是一个 mode：
	// 语音与屏幕共享可以走不同路径（例如语音中继 + 屏幕直连）。
	// 旧字段 mode 已删除（v1 客户端同版本发布，无兼容负担）。
	VoiceMode  string             `json:"voice_mode"`  // relay | direct
	ScreenMode string             `json:"screen_mode"` // relay | direct
	Members    []VoiceMemberState `json:"members"`
	// Slots 仅 relay 模式出现：槽位 → 成员映射，供客户端做「谁在说话」与按人统计。
	Slots []VoiceSlotState `json:"slots,omitempty"`
}

// VoiceJoinOK 是 voice.join 的响应：把服务端选定的传输方式回给客户端，
// 客户端据此决定是「等 media.signal offer（relay）」还是「自己发 offer（direct）」。
type VoiceJoinOK struct {
	ChannelID string `json:"channel_id"`
	// Transport 是服务端策略决定的传输方式：relay（服务端 SFU 中继）或 direct（P2P 直连）。
	// 客户端必须按它走对应路径，不能自己选。
	Transport string `json:"transport"`
	// Role 是服务端给自己分配的直连协商角色（offerer / answerer）：
	// 客户端在**创建 PeerConnection 之前**就要用它决定 passive，不能再靠本地推断。
	// relay 模式下恒为 answerer（服务端 SFU 是 offerer）。
	Role string `json:"role"`
}

// VoiceTransportFailedRequest 是客户端上报「本端直连失败」的载荷（PROTOCOL.md §4.4）。
//
// **语义是告知，不是请求**：客户端不得据此推断自己可以指定 transport。
type VoiceTransportFailedRequest struct {
	ChannelID string `json:"channel_id"`
	Media     string `json:"media"`  // 目前只接受 "voice"
	Reason    string `json:"reason"` // ice_failed | ice_timeout
}

// VoiceTransportFailedOK 只是确认收到事实；是否降级由服务端决定并在响应里说明。
type VoiceTransportFailedOK struct {
	ChannelID string `json:"channel_id"`
	// Transport 是处理完这次上报后该频道的**实际** transport（可能刚刚被降级）。
	Transport string `json:"transport"`
	// Degraded 表示**本次**上报是否触发了降级（幂等：重复上报为 false）。
	Degraded bool `json:"degraded"`
	// Reason 是服务端说明：downgraded | already_relay | rate_limited | not_in_voice。
	Reason string `json:"reason"`
	// TriggerCount 是本次生命周期内该频道累计成功降级次数（正常只会是 1）。
	TriggerCount int `json:"trigger_count,omitempty"`
}

// ServerInfoOK 是 server.info 的响应（PROTOCOL.md §1.4）。
//
// StunURLs 是服务端下发的 STUN 地址（`stun:host:port`）。客户端必须用它作为
// PeerConnection 的 ICE servers 来做候选收集——**不得硬编码公共 STUN**：
// 自托管场景下只有服务端知道自己对外可达的地址；空数组表示运维没开 STUN，
// 此时仍可直连（只是拿不到 srflx 候选，跨 NAT 大概率连不上）。
//
// 这个字段是「配置」不是「策略」：它只影响候选收集，不决定 voice/screen 走
// relay 还是 direct（那是 voice.state.transport 的事，客户端无权选择）。
type ServerInfoOK struct {
	StunURLs []string `json:"stun_urls"`
}
