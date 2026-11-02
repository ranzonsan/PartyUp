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
	MaxBodyBytes    = 4096 // text.send.body 的 UTF-8 字节上限
	MaxChannelName  = 32   // 频道名 rune 数上限
	MaxHistoryLimit = 200  // text.history.limit 上限
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

// Share 是共享区的一路共享占位。骨架不实现屏幕共享，恒为空数组。
type Share struct {
	SharerID string   `json:"sharer_id"`
	Source   string   `json:"source"`
	Mode     string   `json:"mode"`
	Viewers  []string `json:"viewers"`
}

// Channel 是频道。三区同时存在：文字区（消息流）、语音区、共享区。
type Channel struct {
	ID     string        `json:"id"`
	Name   string        `json:"name"`
	Voice  []VoiceMember `json:"voice"`
	Shares []Share       `json:"shares"`
}

// TextMessage 是一条文字消息。text.new 的载荷与 text.history 的元素同形。
type TextMessage struct {
	ChannelID string `json:"channel_id"`
	ID        string `json:"id"`
	Seq       int64  `json:"seq"`
	MemberID  string `json:"member_id"`
	Body      string `json:"body"`
	TS        int64  `json:"ts"`
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
