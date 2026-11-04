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

	// CodeNotImplemented 表示服务端当前配置不支持该能力（例如纯 SFU 中继模式下的屏幕共享）。
	// 与 INTERNAL 区分开：这是**可预期的不支持**，客户端应把 message 直接展示给用户。
	CodeNotImplemented = "NOT_IMPLEMENTED"

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
	TypeVoiceSetMute   = "voice.set_mute"
	TypeVoiceSetDeafen = "voice.set_deafen"
	TypeVoiceState     = "voice.state"

	// 服务器级信息（PROTOCOL.md §1.4）。客户端在鉴权后请求一次，缓存起来；
	// 与语音/屏幕房间无关，任何 PC 创建之前都可以先拿到（这是 stun_urls 的时序要求）。
	TypeServerInfo = "server.info"

	// TypeVoiceTransportFailed 是客户端**上报事实**的消息（PROTOCOL.md §4.4）：
	// 「我这边直连 ICE 失败/超时」。是否有必要降级、怎么降级，由服务端决定。
	TypeVoiceTransportFailed = "voice.transport_failed"
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
	ChannelID string `json:"channel_id"`
	// Mode 是服务端屏幕共享传输策略（--screen-mode），与 VoiceMode 独立。
	// 客户端据此决定共享信令 target（relay → sfu，direct → 成员）以及本地角色。
	Mode   string       `json:"mode"` // relay | direct
	Shares []ShareState `json:"shares"`
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
