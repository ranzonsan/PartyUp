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

public sealed record TextMessageDto(string ChannelId, string Id, long Seq, string MemberId, string Body, long Ts);

public sealed record TextHistoryDto(IReadOnlyList<TextMessageDto> Messages);

public sealed record PresenceMemberDto(string MemberId, string Name, string State);

public sealed record PresenceDto(string ChannelId, IReadOnlyList<PresenceMemberDto> Members);

/// <summary>通用信封。v=1 由协议固定；Payload 原样保留，按消息类型再反序列化。</summary>
public sealed record EnvelopeDto(int V, string Id, string Type, long Ts, JsonElement? Payload);
