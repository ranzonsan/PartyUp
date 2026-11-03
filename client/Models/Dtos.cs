// 与 docs/PROTOCOL.md 对应的 DTO。
//
// 序列化统一走 Services/JsonDefaults：属性用 PascalCase，落盘/上线是 snake_case
// （channel_id / client_msg_id / member_id …），与 Go 服务的 JSON tag 一一对应。
// 只声明客户端真正用到的字段；服务端多给的字段由 System.Text.Json 忽略。
using System.Text.Json;

namespace PartyUp.Client.Models;

public sealed record MemberDto(string Id, string Name, bool Admin);

public sealed record SessionDto(MemberDto Member, string Token);

public sealed record ErrorDto(string Code, string Message);

public sealed record ServerInfoDto(string Name, string Version, int ProtocolVersion, IReadOnlyList<string> Capabilities);

public sealed record AuthOkDto(MemberDto Member, ServerInfoDto Server);

public sealed record ChannelDto(string Id, string Name);

public sealed record ChannelListDto(IReadOnlyList<ChannelDto> Channels);

public sealed record ChannelCreateDto(ChannelDto Channel);

public sealed record SubscribeAckDto(string ChannelId);

public sealed record TextSendAckDto(string Id, long Seq, long Ts, string? ClientMsgId);

public sealed record TextMessageDto(string ChannelId, string Id, long Seq, string MemberId, string MemberName, string Body, long Ts);

public sealed record TextHistoryDto(IReadOnlyList<TextMessageDto> Messages);

public sealed record PresenceMemberDto(string MemberId, string Name, string State);

public sealed record PresenceDto(string ChannelId, IReadOnlyList<PresenceMemberDto> Members);

// ---- 屏幕共享 / 媒体信令（docs/PROTOCOL.md §4.2、§5）----

/// <summary>共享源（对应媒体核心 pu_source_info）。</summary>
public sealed record ScreenSourceDto(string Id, string Title, int Width, int Height, string Kind);

/// <summary>screen.state 里的一路共享。</summary>
public sealed record ShareDto(string SharerId, string MemberName, ScreenSourceDto Source, string Mode, IReadOnlyList<string> Viewers);

public sealed record ScreenStateDto(string ChannelId, IReadOnlyList<ShareDto> Shares);

public sealed record ScreenAckDto(string ChannelId);

/// <summary>media.signal：payload 是媒体核心的原始信令 JSON，客户端不解析、只透传。</summary>
public sealed record MediaSignalDto(
    string ChannelId,
    string? Target,
    string? To,
    string? From,
    string? FromName,
    string Media,
    string Kind,
    string Payload);

/// <summary>voice.state 里的一个语音成员（PROTOCOL.md §4）。</summary>
public sealed record VoiceMemberDto(string MemberId, bool Muted, bool Deafened);

/// <summary>
/// voice.state.slots 的一项：第 <paramref name="Index"/> 个远端音频槽位转发的是哪个成员。
/// 客户端据此把 native 侧 id 为 "partyup-slot-&lt;index&gt;" 的远端音轨映射到具体的人。
/// </summary>
public sealed record VoiceSlotDto(int Index, string MemberId, bool Muted);

/// <summary>voice.state 推送：频道当前语音快照（mode + 成员 + relay 槽位映射）。</summary>
public sealed record VoiceStateDto(
    string ChannelId,
    string Mode,
    IReadOnlyList<VoiceMemberDto> Members,
    IReadOnlyList<VoiceSlotDto>? Slots);

/// <summary>voice.join / voice.leave / voice.set_mode / set_mute / set_deafen 的响应。</summary>
public sealed record VoiceAckDto(string ChannelId, string? Mode, string? Transport);

/// <summary>通用信封。v=1 由协议固定；Payload 原样保留，按消息类型再反序列化。</summary>
public sealed record EnvelopeDto(int V, string Id, string Type, long Ts, JsonElement? Payload);
