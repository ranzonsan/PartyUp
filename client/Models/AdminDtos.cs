using System.Collections.Generic;

namespace PartyUp.Client.Models;

// 服务器管理（Admin）相关的传输对象，字段与 server/internal/server/admin.go 的
// adminMemberDTO / 各 handler 的响应一一对应。序列化统一由 JsonDefaults 做
// PascalCase ↔ snake_case 转换，这里不写任何 JSON 特性。

/// <summary>GET /api/admin/overview。</summary>
public sealed record AdminOverviewDto(
    string Name,
    string Version,
    int ProtocolVersion,
    bool Initialized,
    int MemberCount,
    int OnlineCount,
    int ChannelCount,
    int VoiceRooms,
    int ActiveShares,
    string VoiceMode,
    string ScreenMode,
    string Registration);

/// <summary>GET /api/admin/members 的一行。</summary>
public sealed record AdminMemberDto(
    string Id,
    string Name,
    string? DisplayName,
    string Role,
    bool Admin,
    long CreatedAt,
    bool Banned,
    long BannedAt,
    string? BanReason)
{
    public string EffectiveName => string.IsNullOrWhiteSpace(DisplayName) ? Name : DisplayName;

    public bool IsOwner => string.Equals(Role, "owner", System.StringComparison.Ordinal);

    /// <summary>owner 或 admin。仅用于界面分组，不构成任何授权判断。</summary>
    public bool IsAdminRole => Role is "owner" or "admin";

    public string RoleText => Role switch
    {
        "owner" => "所有者",
        "admin" => "管理员",
        _ => "成员",
    };
}

/// <summary>GET /api/admin/audit 的一条审计事件。</summary>
public sealed record AdminAuditEventDto(
    long Id,
    long AtMs,
    string? ActorId,
    string? ActorName,
    string Action,
    string? Target,
    string? Detail,
    string? Ip);

/// <summary>GET /api/admin/invites 的一条。</summary>
public sealed record AdminInviteDto(string Code, string? CreatedBy, long CreatedAt);

/// <summary>POST /api/admin/invites 的响应。</summary>
public sealed record AdminInviteCreateDto(string Code, string? CreatedBy, long CreatedAt);

/// <summary>POST /api/admin/members/{id}/ban 的响应。</summary>
public sealed record AdminBanResultDto(bool Banned, int ClosedConns);

/// <summary>POST /api/admin/members/{id}/password 的响应：一次性临时密码，只显示一次。</summary>
public sealed record AdminPasswordResetDto(string Password);

/// <summary>
/// GET/PATCH /api/admin/settings。危险项（voice_mode/screen_mode/max_screen_shares）
/// 仅 owner 可改——授权在服务端拦截，客户端只是不把按钮给 admin。
/// </summary>
public sealed record ServerSettingsDto(
    string DisplayName,
    string Announcement,
    string WelcomeMessage,
    string Registration,
    string InviteCreators,
    int InviteMaxUsesPerCode,
    int MemberLimit,
    int HistoryRetentionDays,
    bool GlobalMute,
    string VoiceMode,
    string ScreenMode,
    int MaxScreenShares);

/// <summary>服务器管理页需要的角色快照（用于决定显示哪些页签）。</summary>
public sealed record AdminRoleSnapshot(bool IsAdmin, bool IsOwner);

/// <summary>
/// 一条频道。GET /api/admin/channels 返回带壳对象 {channels:[...]}；
/// POST 与 PATCH 返回单条。仅 owner/admin 可取（服务端 requireRole(RoleAdmin)）。
/// </summary>
public sealed record AdminChannelDto(
    string Id,
    string Name,
    string Description,
    int Position,
    long CreatedAt);

