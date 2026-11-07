// 与 docs/PROTOCOL.md 对应的 DTO。
//
// 序列化统一走 Services/JsonDefaults：属性用 PascalCase，落盘/上线是 snake_case
// （channel_id / client_msg_id / member_id …），与 Go 服务的 JSON tag 一一对应。
// 只声明客户端真正用到的字段；服务端多给的字段由 System.Text.Json 忽略。
using System.Text.Json;

namespace PartyUp.Client.Models;

/// <summary>
/// 成员身份。Name 是**唯一登录名**，DisplayName 是展示名（REG-02，2026-11-06 补入）。
/// 服务端对旧库回退 display_name = name；客户端应优先显示 DisplayName，
/// 但任何按成员去重/归属的地方都必须用 Id，不能拿显示名当键。
/// </summary>
public sealed record MemberDto(string Id, string Name, bool Admin, string? DisplayName = null)
{
    /// <summary>UI 展示名：有 display_name 用它，否则回退唯一登录名。</summary>
    public string EffectiveDisplayName =>
        string.IsNullOrWhiteSpace(DisplayName) ? Name : DisplayName;
}

public sealed record SessionDto(MemberDto Member, string Token);

public sealed record ErrorDto(string Code, string Message);

public sealed record ServerInfoDto(string Name, string Version, int ProtocolVersion, IReadOnlyList<string> Capabilities);

/// <summary>
/// GET /api/server-info（匿名）的响应，对应 server/internal/protocol/admin.go 的 ServerInfoPublic。
/// 注意 <see cref="Initialized"/> 与 <see cref="BootstrapAllowed"/> 都只是 UI 提示：
/// 真正的授权在服务端（成员数为 0 + bootstrap token + 连接可信性），客户端不据此放行任何操作。
/// </summary>
public sealed record ServerPublicInfoDto(
    string Name,
    string Version,
    int ProtocolVersion,
    bool Initialized,
    string Registration,
    string VoiceMode,
    string ScreenMode,
    bool BootstrapAllowed);

public sealed record AuthOkDto(MemberDto Member, ServerInfoDto Server);

/// <summary>
/// session.replaced 的载荷（服务端 → 客户端）：本连接所属会话已结束。
/// Reason: login_elsewhere / logout / kicked / banned；关闭码 4001/4001/4002/4003
/// 是同义兜底，**文案以 reason 为准**（关闭码只用于诊断）。
/// </summary>
public sealed record SessionReplacedDto(string Reason);

public sealed record ChannelDto(string Id, string Name);

public sealed record ChannelListDto(IReadOnlyList<ChannelDto> Channels);

public sealed record ChannelCreateDto(ChannelDto Channel);

public sealed record SubscribeAckDto(string ChannelId);

public sealed record TextSendAckDto(string Id, long Seq, long Ts, string? ClientMsgId);

public sealed record TextMessageDto(string ChannelId, string Id, long Seq, string MemberId, string MemberName, string Body, long Ts)
{
    /// <summary>展示名（REG-02）：优先显示它，MemberName 仍是唯一登录名语义。</summary>
    public string? MemberDisplayName { get; init; }

    /// <summary>作者展示名：display_name → member_name → id 前缀（调用方兜底）。</summary>
    public string EffectiveAuthor =>
        !string.IsNullOrWhiteSpace(MemberDisplayName) ? MemberDisplayName
        : !string.IsNullOrWhiteSpace(MemberName) ? MemberName
        : MemberId;
}

/// <summary>
/// 频道范围 presence（旧语义：该频道当前订阅者）。注意它不是「谁在这个频道的语音里」，
/// 语音归属只看 voice.state 与 member.list 的 voice_channel_ids。
/// </summary>
public sealed record PresenceMemberDto(string MemberId, string Name, string State)
{
    /// <summary>展示名（REG-02）。</summary>
    public string? DisplayName { get; init; }

    public string EffectiveDisplayName =>
        string.IsNullOrWhiteSpace(DisplayName) ? Name : DisplayName;
}

public sealed record PresenceDto(string ChannelId, IReadOnlyList<PresenceMemberDto> Members);

public sealed record TextHistoryDto(IReadOnlyList<TextMessageDto> Messages);

/// <summary>
/// 全服务器名册的一项（MEM-01/02，含离线）。State 是服务端推导的
/// offline / idle / voice：idle = 在线但不在任何频道语音里（不是键盘活动），
/// voice = 在线且至少在一个频道语音里。
/// </summary>
/// <remarks>
/// <see cref="VoiceChannelIds"/> 是**实际**语音频道集合（按 id 排序），可能多项——同一成员
/// 可以通过多条连接分别进入不同频道。不能用单个字段冒充唯一频道，也不能用文字订阅推断语音。
/// </remarks>
public sealed record MemberRosterEntryDto(
    string Id,
    string Name,
    bool Admin,
    bool Online,
    string State,
    IReadOnlyList<string> VoiceChannelIds)
{
    /// <summary>展示名（REG-02）：优先显示，但从不用来当身份键。</summary>
    public string? DisplayName { get; init; }

    public string EffectiveDisplayName =>
        string.IsNullOrWhiteSpace(DisplayName) ? Name : DisplayName;
}

/// <summary>member.list.ok 与 members.state 推送共用同一载荷。</summary>
public sealed record MemberListDto(IReadOnlyList<MemberRosterEntryDto> Members);


// ---- 屏幕共享 / 媒体信令（docs/PROTOCOL.md §4.2、§5）----

/// <summary>共享源（对应媒体核心 pu_source_info）。</summary>
public sealed record ScreenSourceDto(string Id, string Title, int Width, int Height, string Kind);

/// <summary>screen.state 里的一路共享。</summary>
public sealed record ShareDto(string SharerId, string MemberName, ScreenSourceDto Source, string Mode, IReadOnlyList<string> Viewers);

public sealed record ScreenStateDto(string ChannelId, string Mode, IReadOnlyList<ShareDto> Shares);

public sealed record ScreenAckDto(string ChannelId);

/// <summary>
/// screen.watch.ok（v1.1）：服务端选定的目标 + relay 槽位 + 单调代次。
/// Slot 只有 relay 有意义（direct = -1）；客户端把 (SharerId, Slot, Generation)
/// 原样交给媒体核心 pu_screen_watch_ex，用于「目标 + 代次」的帧归属。
/// </summary>
public sealed record ScreenWatchDto(string ChannelId, string SharerId, int Slot, ulong Generation,
    string Transport)
{
    /// <summary>B2：严格接收会话 id（客户端生成的 receive_id，服务端原样回显）。</summary>
    public string? SessionId { get; init; }

    /// <summary>B2：服务端生成的会话 nonce，后续 answer/ICE 必须带回。</summary>
    public string? Nonce { get; init; }

    /// <summary>B2：本次 watch 是否走独立 receive-only 传输。</summary>
    public bool Isolated { get; init; }
}

public sealed record ScreenUnwatchDto(string ChannelId, string SharerId);

/// <summary>media.signal：payload 是媒体核心的原始信令 JSON，客户端不解析、只透传。</summary>
public sealed record MediaSignalDto(
    string ChannelId,
    string? Target,
    string? To,
    string? From,
    string? FromName,
    string Media,
    string Kind,
    string Payload)
{
    /// <summary>B2：严格接收会话 id；旧路径/voice 为 null。</summary>
    public string? SessionId { get; init; }

    /// <summary>B2：服务端会话 nonce（offer 推送携带，answer/ICE 回带）。</summary>
    public string? SessionNonce { get; init; }

    /// <summary>B2：观看代次（viewer_close/viewer_arm 也携带）。</summary>
    public ulong Generation { get; init; }
}

/// <summary>voice.state 里的一个语音成员（PROTOCOL.md §4）。</summary>
/// <summary>
/// voice.state.members 的一项。Role 是**服务端裁决的直连协商角色**（offerer / answerer）：
/// 客户端不再按「谁先来」推断，否则两端同时加入会都判 answerer、没人发 offer。
/// </summary>
/// <remarks>
/// Transport / TransportReason 是**按成员下发的事实**（v1 全频道相同）：
/// Transport 表示这个人实际在用什么路径，与 voice.state.voice_mode（服务端策略）语义不同。
/// 逐成员混合路径是 v1 之后的方向，客户端从现在就按成员读，避免届时改协议。
/// </remarks>
public sealed record VoiceMemberDto(
    string MemberId,
    bool Muted,
    bool Deafened,
    string? Role,
    string? Transport = null,
    string? TransportReason = null);

/// <summary>
/// voice.state.slots 的一项：第 <paramref name="Index"/> 个远端音频槽位转发的是哪个成员。
/// 客户端据此把 native 侧 id 为 "partyup-slot-&lt;index&gt;" 的远端音轨映射到具体的人。
/// </summary>
public sealed record VoiceSlotDto(int Index, string MemberId, bool Muted);

/// <summary>voice.state 推送：频道当前语音快照（mode + 成员 + relay 槽位映射）。</summary>
public sealed record VoiceStateDto(
    string ChannelId,
    string VoiceMode,
    string ScreenMode,
    IReadOnlyList<VoiceMemberDto> Members,
    IReadOnlyList<VoiceSlotDto>? Slots);

/// <summary>
/// voice.join / voice.leave / voice.set_mode / set_mute / set_deafen 的响应。
/// Role 只有 voice.join.ok 会填（offerer / answerer）：客户端在建 PC 之前/之后都用它对齐。
/// </summary>
public sealed record VoiceAckDto(string ChannelId, string? Mode, string? Transport, string? Role);

/// <summary>
/// server.info 的响应（PROTOCOL.md §1.4）：服务端配置。目前只有 STUN 地址。
/// 注意与 auth.ok 里的 <see cref="ServerInfoDto"/>（服务端身份/能力）区分：两条消息不同。
/// </summary>
public sealed record ServerConfigDto(IReadOnlyList<string> StunUrls);

/// <summary>通用信封。v=1 由协议固定；Payload 原样保留，按消息类型再反序列化。</summary>
public sealed record EnvelopeDto(int V, string Id, string Type, long Ts, JsonElement? Payload);
