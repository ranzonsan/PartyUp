// Package protocol 是 PartyUp 控制面协议（docs/PROTOCOL.md）的 Go 表示。
//
// 它是服务端与测试共享的唯一定义处：信封形状、消息类型名、错误码、领域对象
// 都在这里，避免 HTTP 层与 WebSocket 层各写一份字符串常量。
//
// 与文档的差异集中记录在 docs/spikes/2026-11-02-skeleton.md 的「协议补充」一节。
package protocol

import (
	"encoding/json"
	"fmt"
	"time"

	"github.com/oklog/ulid/v2"
)

// Version 是信封的 v 字段。docs/PROTOCOL.md §0：当前恒为 1。
const Version = 1

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

	// CodeInvalidArgument 是骨架新增的码：载荷形状/字段非法。
	// PROTOCOL.md 的第一批里没有它，而 HTTP 400 与 WS 参数错误需要一个稳定名字，
	// 建议下一版文档补入（见 spike 文档）。
	CodeInvalidArgument = "INVALID_ARGUMENT"
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

	// 屏幕共享与媒体信令（PROTOCOL.md §4.2 / §5）。
	TypeScreenStart = "screen.start"
	TypeScreenStop  = "screen.stop"
	TypeScreenState = "screen.state"
	TypeMediaSignal = "media.signal"

	// 语音（PROTOCOL.md §4）。relay 模式由服务端 SFU 中继，direct 模式走 media.signal 直连。
	TypeVoiceJoin      = "voice.join"
	TypeVoiceLeave     = "voice.leave"
	TypeVoiceSetMode   = "voice.set_mode"
	TypeVoiceSetMute   = "voice.set_mute"
	TypeVoiceSetDeafen = "voice.set_deafen"
	TypeVoiceState     = "voice.state"
)

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
	// Admin 标记是否由 bootstrap 创建。骨架里只有它影响邀请码管理入口。
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
	Body       string `json:"body"`
	TS         int64  `json:"ts"`
}

// PresenceMember 是 presence 里的成员条目。
type PresenceMember struct {
	MemberID string `json:"member_id"`
	Name     string `json:"name"`
	State    string `json:"state"` // online | voice | sharing
}

// ------------------------------------------------------------------
// 请求/响应载荷
// ------------------------------------------------------------------

// AuthRequest 是 auth 的载荷。
type AuthRequest struct {
	Token string `json:"token"`
}

// AuthOK 是 auth.ok 的载荷。
type AuthOK struct {
	Member Member     `json:"member"`
	Server ServerInfo `json:"server"`
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
type PresencePayload struct {
	ChannelID string           `json:"channel_id"`
	Members   []PresenceMember `json:"members"`
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
	ChannelID string       `json:"channel_id"`
	Shares    []ShareState `json:"shares"`
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
	Target  string `json:"target,omitempty"`
	Media   string `json:"media"` // screen | voice
	Kind    string `json:"kind"`  // offer | answer | candidate
	Payload string `json:"payload"`
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
	Payload  string `json:"payload"`
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

// VoiceJoinRequest 是 voice.join 的上行载荷。mode ∈ {relay, direct}：
// relay 由服务端 SFU 中继（每成员一个 PeerConnection），direct 走 1:1 直连信令。
type VoiceJoinRequest struct {
	ChannelID string `json:"channel_id"`
	Mode      string `json:"mode"`
}

// VoiceLeaveRequest 是 voice.leave 的上行载荷。
type VoiceLeaveRequest struct {
	ChannelID string `json:"channel_id"`
}

// VoiceSetModeRequest 是 voice.set_mode 的上行载荷：整频道切换中继/直连。
type VoiceSetModeRequest struct {
	ChannelID string `json:"channel_id"`
	Mode      string `json:"mode"`
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
}

// VoiceStatePayload 是 voice.state 推送的载荷（频道内语音成员快照）。
type VoiceStatePayload struct {
	ChannelID string             `json:"channel_id"`
	Mode      string             `json:"mode"` // relay | direct
	Members   []VoiceMemberState `json:"members"`
}

// VoiceJoinOK 是 voice.join 的响应：把服务端选定的传输方式回给客户端，
// 客户端据此决定是「等 media.signal offer（relay）」还是「自己发 offer（direct）」。
type VoiceJoinOK struct {
	ChannelID string `json:"channel_id"`
	Mode      string `json:"mode"`
	Transport string `json:"transport"` // relay | direct
}
