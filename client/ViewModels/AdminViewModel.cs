using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 「服务器管理」页的视图模型（owner / admin 共用一套界面，按角色裁剪能力）。
///
/// 职责边界：
///  * 这里只做**展示与请求**；所有"谁能做什么"的最终裁决在服务端
///    （owner-only 危险设置、owner 不可被管理、admin 不能改 owner 等）。
///  * 界面上的 <c>Can*</c> 属性只是为了避免把注定 403 的按钮摆出来，
///    不能当成权限校验——服务端拒绝时这里必须把错误如实显示出来。
/// </summary>
public sealed partial class AdminViewModel : ObservableObject
{
    private readonly AdministrationApi _api;
    private readonly AdminRoleSnapshot _role;
    private readonly string _selfMemberId;

    public AdminViewModel(Uri baseUri, string token, AdminRoleSnapshot role, string selfMemberId)
    {
        _api = new AdministrationApi(baseUri, token);
        _role = role;
        _selfMemberId = selfMemberId;
        ServerAddress = baseUri.ToString().TrimEnd('/');
    }

    public string ServerAddress { get; }

    /// <summary>
    /// 是否是所有者。初值只按 member.Admin 猜（owner 也是 admin=true），
    /// 首次加载成员列表后用自报 id 精确判定并通知——所以它可能从 false 变 true。
    /// 仍然只是**界面裁剪**，不是授权。
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(RoleCaption))]
    [NotifyPropertyChangedFor(nameof(CanChangeRole))]
    [NotifyPropertyChangedFor(nameof(CanTransferOwnership))]
    [NotifyPropertyChangedFor(nameof(CanEditDangerous))]
    private bool _isOwner;

    public bool IsAdmin => _role.IsAdmin;

    /// <summary>页签标题里显示角色，避免用户误以为自己是 owner。</summary>
    public string RoleCaption => IsOwner ? "所有者" : _role.IsAdmin ? "管理员" : "成员";

    // ---------------- 概览 ----------------
    [ObservableProperty]
    private string _overviewLine = "正在读取服务器概览…";

    [ObservableProperty]
    private string _overviewDetail = string.Empty;

    // ---------------- 成员 ----------------
    public ObservableCollection<AdminMemberDto> Members { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CanKick))]
    [NotifyPropertyChangedFor(nameof(CanBan))]
    [NotifyPropertyChangedFor(nameof(CanUnban))]
    [NotifyPropertyChangedFor(nameof(CanChangeRole))]
    [NotifyPropertyChangedFor(nameof(CanTransferOwnership))]
    [NotifyPropertyChangedFor(nameof(SelectedMemberCaption))]
    private AdminMemberDto? _selectedMember;

    [ObservableProperty]
    private string _banReason = string.Empty;

    public string SelectedMemberCaption =>
        SelectedMember is null ? "未选择成员" : $"{SelectedMember.EffectiveName}（{SelectedMember.RoleText}）";

    /// <summary>owner 不可被任何管理操作作用（服务端同样拒绝，这里只是不误导）。</summary>
    public bool CanKick => SelectedMember is { IsOwner: false };
    public bool CanBan => SelectedMember is { IsOwner: false, Banned: false };
    public bool CanUnban => SelectedMember is { IsOwner: false, Banned: true };

    /// <summary>任免管理员仅 owner（服务端 requireRole(owner)）；不能改自己的 owner 角色。</summary>
    public bool CanChangeRole => IsOwner && SelectedMember is { IsOwner: false };

    public bool CanTransferOwnership => IsOwner && SelectedMember is { IsOwner: false };

    // ---------------- 邀请 ----------------
    public ObservableCollection<AdminInviteDto> Invites { get; } = new();

    [ObservableProperty]
    private string _newInviteCode = string.Empty;

    /// <summary>邀请码生成权限由服务端设置决定（owner/admin/all），界面不预判，失败就显示原因。</summary>
    public bool CanCreateInvite => true;

    // ---------------- 审计 ----------------
    public ObservableCollection<string> AuditLines { get; } = new();

    // ---------------- 设置 ----------------
    [ObservableProperty]
    private string _announcement = string.Empty;

    [ObservableProperty]
    private string _welcomeMessage = string.Empty;

    [ObservableProperty]
    private string _registration = "invite";

    [ObservableProperty]
    private string _inviteCreators = "admin";

    [ObservableProperty]
    private int _memberLimit;

    [ObservableProperty]
    private int _historyRetentionDays;

    [ObservableProperty]
    private bool _globalMute;

    // 危险设置：仅 owner 可改（客户端 hide + 服务端拒绝双保险）
    [ObservableProperty]
    private string _voiceMode = "relay";

    [ObservableProperty]
    private string _screenMode = "relay";

    [ObservableProperty]
    private int _maxScreenShares = 1;

    public bool CanEditDangerous => IsOwner;

    public static IReadOnlyList<string> RegistrationChoices { get; } = new[] { "invite", "closed" };
    public static IReadOnlyList<string> InviteCreatorChoices { get; } = new[] { "owner", "admin", "all" };
    public static IReadOnlyList<string> ModeChoices { get; } = new[] { "relay", "direct" };

    // ---------------- 状态 ----------------
    [ObservableProperty]
    private string _statusText = string.Empty;

    [ObservableProperty]
    private string _errorText = string.Empty;

    [ObservableProperty]
    private string _temporaryPassword = string.Empty;

    [ObservableProperty]
    private bool _isBusy;

    public bool HasError => ErrorText.Length > 0;
    public bool HasTemporaryPassword => TemporaryPassword.Length > 0;

    partial void OnErrorTextChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnTemporaryPasswordChanged(string value) => OnPropertyChanged(nameof(HasTemporaryPassword));

    /// <summary>打开页面时的首轮加载：概览 + 成员 + 邀请 + 审计 + 设置一次拉齐。</summary>
    [RelayCommand]
    public async Task RefreshAsync()
    {
        if (IsBusy)
        {
            return;
        }
        IsBusy = true;
        ErrorText = string.Empty;
        StatusText = "正在读取…";
        try
        {
            await LoadAllAsync();
            StatusText = "已更新 " + DateTime.Now.ToString("HH:mm:ss");
        }
        catch (Exception ex)
        {
            ErrorText = DescribeError(ex);
            StatusText = "读取失败";
            AppLog.Warn("admin: refresh failed: " + ex.Message);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task LoadAllAsync()
    {
        var overview = await _api.GetOverviewAsync();
        OverviewLine = $"{overview.Name}  ·  v{overview.Version}";
        OverviewDetail =
            $"成员 {overview.MemberCount}（在线 {overview.OnlineCount}） · 频道 {overview.ChannelCount} · " +
            $"语音房间 {overview.VoiceRooms} · 进行中的共享 {overview.ActiveShares} · " +
            $"注册策略 {overview.Registration} · 语音 {overview.VoiceMode} / 共享 {overview.ScreenMode}";

        var members = await _api.GetMembersAsync();
        Members.Clear();
        foreach (var member in members)
        {
            Members.Add(member);
        }

        // 精确判定自己的角色（owner 也是 admin=true，只有看 role 才分得清）。
        var myself = Members.FirstOrDefaultById(_selfMemberId);
        if (myself is not null)
        {
            IsOwner = myself.IsOwner;
        }

        if (SelectedMember is not null)
        {
            SelectedMember = Members.FirstOrDefaultById(SelectedMember.Id) ?? Members.FirstOrDefault();
        }
        else
        {
            SelectedMember = Members.FirstOrDefault();
        }

        var invites = await _api.GetInvitesAsync();
        Invites.Clear();
        foreach (var invite in invites)
        {
            Invites.Add(invite);
        }

        var audit = await _api.GetAuditAsync(limit: 100);
        AuditLines.Clear();
        foreach (var ev in audit)
        {
            var at = DateTimeOffset.FromUnixTimeMilliseconds(ev.AtMs).ToLocalTime().ToString("MM-dd HH:mm:ss");
            var actor = string.IsNullOrWhiteSpace(ev.ActorName) ? (ev.ActorId ?? "?") : ev.ActorName;
            var target = string.IsNullOrWhiteSpace(ev.Target) ? string.Empty : " → " + ev.Target;
            var detail = string.IsNullOrWhiteSpace(ev.Detail) ? string.Empty : "（" + ev.Detail + "）";
            AuditLines.Add($"{at}  {actor}  {ev.Action}{target}{detail}");
        }

        var settings = await _api.GetSettingsAsync();
        Announcement = settings.Announcement;
        WelcomeMessage = settings.WelcomeMessage;
        Registration = settings.Registration;
        InviteCreators = settings.InviteCreators;
        MemberLimit = settings.MemberLimit;
        HistoryRetentionDays = settings.HistoryRetentionDays;
        GlobalMute = settings.GlobalMute;
        VoiceMode = settings.VoiceMode;
        ScreenMode = settings.ScreenMode;
        MaxScreenShares = settings.MaxScreenShares;
    }

    [RelayCommand]
    private async Task KickAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync($"已把 {target.EffectiveName} 移出服务器", () => _api.KickAsync(target.Id));
    }

    [RelayCommand]
    private async Task BanAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        var reason = BanReason.Trim();
        await RunAsync(
            $"已封禁 {target.EffectiveName}（踢下线 {0} 条连接）",
            async () =>
            {
                var result = await _api.BanAsync(target.Id, reason);
                StatusText = $"已封禁 {target.EffectiveName}（断开连接 {result.ClosedConns} 条）";
            });
    }

    [RelayCommand]
    private async Task UnbanAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync($"已解封 {target.EffectiveName}", () => _api.UnbanAsync(target.Id));
    }

    [RelayCommand]
    private async Task PromoteAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync($"已把 {target.EffectiveName} 设为管理员", () => _api.SetRoleAsync(target.Id, "admin"));
    }

    [RelayCommand]
    private async Task DemoteAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync($"已把 {target.EffectiveName} 降为普通成员", () => _api.SetRoleAsync(target.Id, "user"));
    }

    [RelayCommand]
    private async Task ResetPasswordAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync(
            string.Empty,
            async () =>
            {
                var result = await _api.ResetPasswordAsync(target.Id);
                TemporaryPassword = result.Password;
                StatusText = $"已为 {target.EffectiveName} 生成临时密码（只显示这一次）";
            });
    }

    [RelayCommand]
    private async Task CreateInviteAsync()
    {
        await RunAsync(
            string.Empty,
            async () =>
            {
                var created = await _api.CreateInviteAsync();
                NewInviteCode = created.Code;
                StatusText = "已生成邀请码（复制发给朋友）";
                await RefreshInvitesAsync();
            });
    }

    private async Task RefreshInvitesAsync()
    {
        var invites = await _api.GetInvitesAsync();
        Invites.Clear();
        foreach (var invite in invites)
        {
            Invites.Add(invite);
        }
    }

    [RelayCommand]
    private async Task RevokeInviteAsync(AdminInviteDto? invite)
    {
        if (invite is null)
        {
            return;
        }
        await RunAsync(
            $"已撤销邀请码 {invite.Code}",
            async () =>
            {
                await _api.RevokeInviteAsync(invite.Code);
                await RefreshInvitesAsync();
            });
    }

    /// <summary>保存设置：只提交改动过的字段（PATCH 白名单合并）。</summary>
    [RelayCommand]
    private async Task SaveSettingsAsync()
    {
        var patch = new Dictionary<string, object?>
        {
            ["announcement"] = Announcement,
            ["welcome_message"] = WelcomeMessage,
            ["registration"] = Registration,
            ["invite_creators"] = InviteCreators,
            ["member_limit"] = MemberLimit,
            ["history_retention_days"] = HistoryRetentionDays,
            ["global_mute"] = GlobalMute,
        };
        if (IsOwner)
        {
            patch["voice_mode"] = VoiceMode;
            patch["screen_mode"] = ScreenMode;
            patch["max_screen_shares"] = MaxScreenShares;
        }

        await RunAsync(
            "设置已保存",
            async () =>
            {
                var saved = await _api.PatchSettingsAsync(patch);
                VoiceMode = saved.VoiceMode;
                ScreenMode = saved.ScreenMode;
                MaxScreenShares = saved.MaxScreenShares;
                if (IsOwner && saved.MaxScreenShares != MaxScreenShares)
                {
                    StatusText = "设置已保存（最大共享路数需重启服务端生效）";
                }
            });
    }

    [RelayCommand]
    private async Task TransferOwnershipAsync()
    {
        if (SelectedMember is not { } target)
        {
            return;
        }
        await RunAsync(
            $"所有权已转交给 {target.EffectiveName}（你需要重新登录）",
            async () =>
            {
                await _api.TransferOwnershipAsync(target.Id);
                StatusText = "所有权已转交；你已不是所有者，部分操作将不可用";
            });
    }

    /// <summary>统一执行：异常转成可读文案，成功则刷新列表。</summary>
    private async Task RunAsync(string successText, Func<Task> action)
    {
        if (IsBusy)
        {
            return;
        }
        IsBusy = true;
        ErrorText = string.Empty;
        TemporaryPassword = string.Empty;
        try
        {
            await action();
            if (successText.Length > 0)
            {
                StatusText = successText;
            }
            await LoadAllAsync();
        }
        catch (Exception ex)
        {
            ErrorText = DescribeError(ex);
            StatusText = "操作失败";
            AppLog.Warn("admin: action failed: " + ex.Message);
        }
        finally
        {
            IsBusy = false;
        }
    }

    /// <summary>把服务端错误码翻译成"为什么不行"，不要只显示 HTTP 403。</summary>
    private static string DescribeError(Exception ex) => ex switch
    {
        PartyUpApiException api => api.Code switch
        {
            "OWNER_ONLY" => "只有服务器所有者可以执行这个操作。",
            "ADMIN_ONLY" => "需要管理员权限。",
            "FORBIDDEN" => "服务端拒绝了这次操作（权限不足）。",
            "MEMBER_BANNED" => "该账号已被封禁。",
            "NOT_FOUND" => "目标不存在（可能已被删除或列表过期）。",
            "INVALID_ARGUMENT" => "参数不合法：" + api.Message,
            "UNAUTHENTICATED" => "登录已失效，请重新登录。",
            _ => $"{api.Code}：{api.Message}",
        },
        _ => ex.Message,
    };
}

internal static class AdminMemberListExtensions
{
    public static AdminMemberDto? FirstOrDefaultById(this IEnumerable<AdminMemberDto> members, string id)
    {
        foreach (var member in members)
        {
            if (string.Equals(member.Id, id, StringComparison.Ordinal))
            {
                return member;
            }
        }
        return null;
    }
}
