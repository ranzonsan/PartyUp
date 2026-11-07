using System.ComponentModel;
using System.Globalization;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.ViewModels;

/// <summary>
/// 注册成功后的会话快照。Token 只经内存交给宿主，不落盘、不写日志；
/// <see cref="BaseUri"/> 是 <see cref="PartyUpApi"/> 规范化后的服务器 base（保留反代前缀）。
/// </summary>
public sealed record RegistrationSession(Uri BaseUri, string Token, MemberDto Member);

/// <summary>
/// 分步注册流程（REG-01..REG-04 的客户端接线，替代 Semi 预览里的本地走查版 RegistrationFlow）：
/// 目标服务器/邀请链接 → 真实非消费邀请码预检 → 账户信息 → 真实注册并自动登录。
///
/// 真实请求只有两处：<see cref="PartyUpApi.ValidateInviteAsync"/>（只读预检，不建成员、不消费邀请码）
/// 与 <see cref="PartyUpApi.RegisterWithInviteAsync"/>（服务端在同一事务里重新校验并消费邀请码，
/// 成功后返回 token）。预检绝不用注册接口代做。
///
/// 生命周期：<see cref="Begin"/> 激活并重置草稿；<see cref="Cancel"/>、返回上一步、修改目标或邀请码
/// 都会推进 generation 并取消在途请求，迟到的响应不会把新流程推进到下一步。
/// 密码只作为局部变量参与请求，不写日志、不持久化；注册成功后清空输入框。
/// </summary>
public sealed partial class RegistrationViewModel : ObservableObject
{
    private const int MinUsernameChars = 2;
    private const int MaxUsernameChars = 32;
    private const int MinDisplayNameChars = 1;
    private const int MaxDisplayNameChars = 32;
    private const int MinPasswordBytes = 6;
    private const int MaxPasswordBytes = 256;

    /// <summary>派生属性的通知名单；避免它们再触发一轮派生通知。</summary>
    private static readonly HashSet<string> DerivedProperties = new(StringComparer.Ordinal)
    {
        nameof(TargetStep),
        nameof(InviteStep),
        nameof(AccountStep),
        nameof(DoneStep),
        nameof(ShowBack),
        nameof(StepTitle),
        nameof(BackLabel),
        nameof(InviteHint),
        nameof(VerifiedNote),
        nameof(BusyText),
        nameof(PasswordFeedback),
        nameof(HasError),
        nameof(CanContinue),
        nameof(CanVerify),
        nameof(CanCreateAccount),
    };

    private readonly Action<RegistrationSession> _onCompleted;
    private readonly Action _onCancelled;

    private Uri? _serverBase;
    private string _verifiedCode = string.Empty;
    private CancellationTokenSource? _request;
    private int _generation;
    private bool _isActive;

    [ObservableProperty] private int _step;
    [ObservableProperty] private string _target = string.Empty;
    [ObservableProperty] private string _inviteCode = string.Empty;
    [ObservableProperty] private bool _inviteVerified;
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = string.Empty;
    [ObservableProperty] private string _serverLabel = string.Empty;
    [ObservableProperty] private string _newUsername = string.Empty;
    [ObservableProperty] private string _displayName = string.Empty;
    [ObservableProperty] private string _newPassword = string.Empty;
    [ObservableProperty] private string _confirmPassword = string.Empty;
    [ObservableProperty] private string _completedAccountLabel = string.Empty;

    public RegistrationViewModel(Action<RegistrationSession> onCompleted, Action onCancelled)
    {
        ArgumentNullException.ThrowIfNull(onCompleted);
        ArgumentNullException.ThrowIfNull(onCancelled);
        _onCompleted = onCompleted;
        _onCancelled = onCancelled;
    }

    /// <summary>宿主用它控制注册视图的可见性：<see cref="Begin"/> 置 true，取消/成功置 false。</summary>
    public bool IsActive
    {
        get => _isActive;
        private set => SetProperty(ref _isActive, value);
    }

    public bool TargetStep => Step == 0;
    public bool InviteStep => Step == 1;
    public bool AccountStep => Step == 2;
    public bool DoneStep => Step == 3;

    /// <summary>「上一步」在步骤 1/2 可用；步骤 0 的退出入口是右上角「返回登录」。</summary>
    public bool ShowBack => Step is 1 or 2;

    public string StepTitle => Step switch
    {
        0 => "先找到你的服务器",
        1 => "确认加入资格",
        2 => "创建你的账户",
        _ => "账户已创建",
    };

    public string BackLabel => Step == 2 ? "重新选择服务器或邀请码" : "上一步";

    public string InviteHint => InviteCode.Length > 0
        ? "已填入邀请码。确认是只读校验，不会消耗邀请码；服务器确认可用后才会进入账户信息。"
        : "邀请链接没带邀请码时在下面补填。只有服务器确认可用后，才会进入账户信息。";

    public string VerifiedNote => InviteVerified
        ? "邀请码已确认可用（只读预检，尚未消耗）· 创建账户后会自动登录"
        : string.Empty;

    public string BusyText => Step == 2 ? "正在创建账户并登录…" : "正在与服务器确认邀请码…";

    public bool HasError => Error.Length > 0;

    public bool CanContinue =>
        IsActive && TargetStep && !Busy && !string.IsNullOrWhiteSpace(Target);

    public bool CanVerify =>
        IsActive && InviteStep && !Busy && _serverBase is not null && !string.IsNullOrWhiteSpace(InviteCode);

    public bool CanCreateAccount =>
        IsActive && AccountStep && !Busy && InviteVerified && _serverBase is not null &&
        !string.IsNullOrWhiteSpace(NewUsername) &&
        !string.IsNullOrWhiteSpace(NewPassword) &&
        !string.IsNullOrWhiteSpace(ConfirmPassword);

    public string PasswordFeedback
    {
        get
        {
            if (NewPassword.Length == 0 && ConfirmPassword.Length == 0)
            {
                return string.Empty;
            }
            var bytes = Encoding.UTF8.GetByteCount(NewPassword);
            if (bytes < MinPasswordBytes)
            {
                return $"密码至少 {MinPasswordBytes} 个 UTF-8 字节（当前 {bytes}）。";
            }
            if (bytes > MaxPasswordBytes)
            {
                return $"密码最多 {MaxPasswordBytes} 个 UTF-8 字节（当前 {bytes}）。";
            }
            if (ConfirmPassword.Length == 0)
            {
                return "请再次输入密码。";
            }
            return string.Equals(NewPassword, ConfirmPassword, StringComparison.Ordinal)
                ? "两次密码一致。"
                : "两次密码不一致。";
        }
    }

    /// <summary>激活并重置草稿。<paramref name="initialTarget"/> 只预填第一步，不自动提交。</summary>
    public void Begin(string? initialTarget)
    {
        ResetDraft();
        IsActive = true;
        if (!string.IsNullOrWhiteSpace(initialTarget))
        {
            Target = initialTarget.Trim();
        }
    }

    [RelayCommand]
    private void Cancel()
    {
        Invalidate();
        IsActive = false;
        ResetDraft();
        _onCancelled();
    }

    [RelayCommand]
    private void Back()
    {
        if (!IsActive || Busy)
        {
            return;
        }
        if (Step <= 0)
        {
            // 步骤 0 的返回即退出注册；与「返回登录」同一语义。
            Cancel();
            return;
        }
        Invalidate();
        NewPassword = string.Empty;
        ConfirmPassword = string.Empty;
        Step--;
    }

    /// <summary>已完成态的收尾按钮（宿主通常已切走视图）；幂等，不重复回调 onCompleted。</summary>
    [RelayCommand]
    private void Finish() => IsActive = false;

    [RelayCommand(CanExecute = nameof(CanContinue))]
    private async Task ContinueAsync()
    {
        Error = string.Empty;
        var raw = Target.Trim();
        if (raw.Length == 0)
        {
            Error = "请输入服务器地址或邀请链接。";
            return;
        }
        if (!Uri.TryCreate(raw, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
        {
            Error = "请输入带 http:// 或 https:// 的完整服务器地址或邀请链接。";
            return;
        }

        // 邀请链接只由 InviteLink.TryParse 解析（System.Uri + QueryHelpers），UI 层不手写解析。
        if (InviteLink.TryParse(raw, out var link, out var linkError))
        {
            var inviteBase = link!.ServerBaseUri;
            _serverBase = inviteBase;
            ServerLabel = "服务器：" + inviteBase.AbsoluteUri;
            Step = 1;
            InviteCode = link.Code;
            // 链接自带邀请码：用户已确认这一步，直接做只读预检（不消费）。
            await VerifyInviteAsync();
            return;
        }

        // 带查询参数但不是合法邀请链接：按解析器的原因就近报错，不静默丢掉参数。
        if (uri.Query.Length > 0)
        {
            Error = linkError ?? "邀请链接无效，请检查后重试。";
            return;
        }
        if (uri.Fragment.Length > 0)
        {
            Error = "服务器地址不能包含 # 片段。";
            return;
        }
        if (uri.UserInfo.Length > 0)
        {
            Error = "服务器地址不能包含用户名或密码。";
            return;
        }

        // 不带 code 的地址按服务器 base 接受；邀请码在下一步填。
        var normalizedBase = ServerEndpoint.NormalizeBase(uri);
        _serverBase = normalizedBase;
        ServerLabel = "服务器：" + normalizedBase.AbsoluteUri;
        Step = 1;
        InviteCode = string.Empty;
    }

    [RelayCommand(CanExecute = nameof(CanVerify))]
    private async Task VerifyInviteAsync()
    {
        if (_serverBase is not { } serverBase)
        {
            Error = "请先选择服务器地址。";
            Step = 0;
            return;
        }
        var code = InviteLink.NormalizeCode(InviteCode);
        if (code.Length == 0)
        {
            Error = "请输入邀请码。";
            return;
        }

        var generation = StartRequest(out var ct);
        Busy = true;
        Error = string.Empty;
        InviteVerified = false;
        try
        {
            using var api = new PartyUpApi(serverBase);
            // 真实只读预检：不创建成员、不消费邀请码；最终注册仍由服务端事务重新裁决。
            await api.ValidateInviteAsync(code, ct);
            if (generation != _generation)
            {
                return;
            }
            _verifiedCode = code;
            InviteVerified = true;
            Step = 2;
            AppLog.Pass($"registration: invite preflight ok @ {api.BaseUri}");
        }
        catch (PartyUpApiException ex)
        {
            if (generation != _generation)
            {
                return;
            }
            AppLog.Warn($"registration: invite preflight rejected ({ex.Code})");
            Error = ex.Code switch
            {
                "NOT_FOUND" => "邀请码不存在或已失效。请检查后重新输入，或向服务器管理员索取新的邀请码。",
                "CONFLICT" => "这个邀请码已经被使用过了。请向服务器管理员索取新的邀请码。",
                "INVALID_ARGUMENT" => "服务器认为邀请码格式不合法，请确认复制完整后重试。",
                _ => $"服务器拒绝了邀请码预检（{ex.Code}）：{ex.Message}",
            };
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            if (generation != _generation)
            {
                return;
            }
            Error = "连接服务器超时。请检查服务器地址与网络后重试。";
        }
        catch (OperationCanceledException)
        {
            // 流程已被取消/重置：不展示错误，也不推进。
        }
        catch (HttpRequestException ex)
        {
            if (generation != _generation)
            {
                return;
            }
            AppLog.Fail($"registration: invite preflight network error: {ex.Message}");
            Error = "无法连接服务器。请检查服务器地址、端口与网络后重试。";
        }
        finally
        {
            if (generation == _generation)
            {
                Busy = false;
                EndRequest();
            }
        }
    }

    [RelayCommand(CanExecute = nameof(CanCreateAccount))]
    private async Task CreateAccountAsync()
    {
        if (!CanCreateAccount)
        {
            Error = "请先确认邀请码，并填写用户名、密码与重复密码。";
            return;
        }
        if (_serverBase is not { } serverBase)
        {
            Invalidate();
            Step = 0;
            Error = "请先选择服务器地址。";
            return;
        }

        var username = NewUsername.Trim();
        var displayName = DisplayName.Trim();
        var password = NewPassword;

        // 先本地校验（与服务端规则同口径）；服务端仍是最终裁决者。
        if (ValidateUsername(username) is { } usernameError)
        {
            Error = usernameError;
            return;
        }
        if (displayName.Length > 0 && ValidateDisplayName(displayName) is { } displayError)
        {
            Error = displayError;
            return;
        }
        var passwordBytes = Encoding.UTF8.GetByteCount(password);
        if (passwordBytes is < MinPasswordBytes or > MaxPasswordBytes)
        {
            Error = $"密码需为 {MinPasswordBytes}–{MaxPasswordBytes} 个 UTF-8 字节（当前 {passwordBytes}）。";
            return;
        }
        if (!string.Equals(password, ConfirmPassword, StringComparison.Ordinal))
        {
            Error = "两次输入的密码不一致。";
            return;
        }
        if (_verifiedCode.Length == 0)
        {
            Step = 1;
            Error = "请先确认邀请码。";
            return;
        }

        var generation = StartRequest(out var ct);
        var code = _verifiedCode;
        Busy = true;
        Error = string.Empty;
        try
        {
            using var api = new PartyUpApi(serverBase);
            // 真实注册：服务端在事务里重新校验邀请码并消费；成功后直接返回 token。
            var session = await api.RegisterWithInviteAsync(
                code,
                username,
                password,
                displayName.Length == 0 ? null : displayName,
                ct);
            if (generation != _generation)
            {
                return;
            }
            // Busy 期间输入被禁用，这里仍对一次值：防御程序化改动导致「提交的账户」和「界面显示」不一致。
            if (!string.Equals(username, NewUsername.Trim(), StringComparison.Ordinal))
            {
                Error = "账户信息在提交后发生了变化，请重新提交。";
                return;
            }

            NewPassword = string.Empty;
            ConfirmPassword = string.Empty;
            CompletedAccountLabel = $"{session.Member.EffectiveDisplayName}（@{session.Member.Name}）";
            AppLog.Pass($"registration: account created as {session.Member.Name} @ {api.BaseUri}");
            Deliver(new RegistrationSession(api.BaseUri, session.Token, session.Member));
        }
        catch (PartyUpApiException ex)
        {
            if (generation != _generation)
            {
                return;
            }
            ApplyRegisterError(ex);
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            if (generation != _generation)
            {
                return;
            }
            // 超时时服务端可能已经提交；引导用户去登录而不是盲目重复注册。
            Error = "创建账户超时，服务器可能已经处理。请先尝试用新账户登录，不要重复提交。";
        }
        catch (OperationCanceledException)
        {
            // 流程已被取消/重置：不推进。
        }
        catch (HttpRequestException ex)
        {
            if (generation != _generation)
            {
                return;
            }
            AppLog.Fail($"registration: network error: {ex.Message}");
            Error = "无法连接服务器。请检查服务器地址、端口与网络后重试。";
        }
        finally
        {
            if (generation == _generation)
            {
                Busy = false;
                EndRequest();
            }
        }
    }

    /// <summary>
    /// 注册失败的服务端错误定位：用户名占用留在账户步骤，邀请码被抢/失效退回邀请步骤。
    /// 服务端对「邀请码已用」和「用户名占用」都返回 CONFLICT，只能按 message 区分；
    /// 协议如果以后给出更细的错误码，这里应改成按 Code 分支。
    /// </summary>
    private void ApplyRegisterError(PartyUpApiException ex)
    {
        AppLog.Warn($"registration: register rejected ({ex.Code}) status={ex.Status}");
        switch (ex.Code)
        {
            case "NOT_FOUND":
                ResetInviteVerification();
                Step = 1;
                Error = "邀请码在确认之后已被使用或失效。请重新确认邀请码。";
                break;
            case "CONFLICT" when ex.Message.Contains("username", StringComparison.OrdinalIgnoreCase):
                Error = "这个用户名已经被占用，请换一个登录名。";
                break;
            case "CONFLICT":
                ResetInviteVerification();
                Step = 1;
                Error = "邀请码刚被其他人使用了。请向服务器管理员索取新的邀请码。";
                break;
            case "INVALID_ARGUMENT":
                // 本地校验与服务端规则有出入时的兜底：按服务端消息把问题定位到字段（仍留在账户步骤）。
                Error = ex.Message.Contains("display", StringComparison.OrdinalIgnoreCase)
                    ? "服务器不接受这个显示名称：需为 1–32 个可打印字符。"
                    : ex.Message.Contains("username", StringComparison.OrdinalIgnoreCase)
                        ? "服务器不接受这个用户名：需为 2–32 个字母、数字、下划线、连字符或中文。"
                        : ex.Message.Contains("password", StringComparison.OrdinalIgnoreCase)
                            ? $"服务器不接受这个密码：需为 {MinPasswordBytes}–{MaxPasswordBytes} 个 UTF-8 字节。"
                            : $"服务器拒绝了这次注册：{ex.Message}";
                break;
            default:
                Error = $"服务器拒绝了这次注册（{ex.Code}）：{ex.Message}";
                break;
        }
    }

    /// <summary>
    /// 成功：先把注册视图收起，再把会话交给宿主（A 方案——不多一步确认）。
    /// 服务端注册接口已经返回 token，宿主收到后直接进入已登录布局。
    /// </summary>
    private void Deliver(RegistrationSession session)
    {
        Step = 3;
        IsActive = false;
        _onCompleted(session);
    }

    private void ResetInviteVerification()
    {
        InviteVerified = false;
        _verifiedCode = string.Empty;
    }

    private void ResetDraft()
    {
        Invalidate();
        Step = 0;
        _serverBase = null;
        _verifiedCode = string.Empty;
        ServerLabel = string.Empty;
        Target = string.Empty;
        InviteCode = string.Empty;
        NewUsername = string.Empty;
        DisplayName = string.Empty;
        NewPassword = string.Empty;
        ConfirmPassword = string.Empty;
        CompletedAccountLabel = string.Empty;
    }

    /// <summary>
    /// 让当前流程作废：推进 generation、取消在途请求、清掉忙碌与错误态。
    /// 在途命令的 catch/finally 会比对 generation，旧响应既不显示错误也不推进步骤。
    /// </summary>
    private void Invalidate()
    {
        _generation++;
        var request = _request;
        _request = null;
        request?.Cancel(); // 不在这里 Dispose：在途请求仍可能收尾，交给它的 finally 或 GC。
        Busy = false;
        InviteVerified = false;
        _verifiedCode = string.Empty;
        Error = string.Empty;
    }

    private int StartRequest(out CancellationToken token)
    {
        var previous = _request;
        _request = new CancellationTokenSource();
        previous?.Cancel();
        token = _request.Token;
        return ++_generation;
    }

    private void EndRequest()
    {
        var request = _request;
        _request = null;
        request?.Dispose();
    }

    partial void OnTargetChanged(string value)
    {
        Invalidate();
        _serverBase = null;
        ServerLabel = string.Empty;
        Step = 0;
    }

    partial void OnInviteCodeChanged(string value)
    {
        Invalidate();
        if (Step >= 2)
        {
            Step = 1;
        }
    }

    /// <summary>
    /// 用户名规则与服务端 auth.ValidName 对齐：2–32 rune，只允许 ASCII 字母/数字、下划线、连字符与 CJK。
    /// 返回 null 表示通过。
    /// </summary>
    private static string? ValidateUsername(string value)
    {
        var count = value.EnumerateRunes().Count();
        if (count is < MinUsernameChars or > MaxUsernameChars)
        {
            return $"用户名需为 {MinUsernameChars}–{MaxUsernameChars} 个字符（登录名，当前 {count} 个）。";
        }
        foreach (var rune in value.EnumerateRunes())
        {
            var c = rune.Value;
            var allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '_' || c == '-' || (c >= 0x4E00 && c <= 0x9FFF);
            if (!allowed)
            {
                return "用户名只能用字母、数字、下划线、连字符或中文。";
            }
        }
        return null;
    }

    /// <summary>
    /// 显示名规则（仅当用户填了内容时检查）：1–32 rune 且不接受控制/不可见格式字符，
    /// 与服务端 auth.ValidDisplayName 的 unicode.IsPrint 口径尽量一致；留空表示回退登录名。
    /// </summary>
    private static string? ValidateDisplayName(string value)
    {
        var count = value.EnumerateRunes().Count();
        if (count is < MinDisplayNameChars or > MaxDisplayNameChars)
        {
            return $"显示名称需为 {MinDisplayNameChars}–{MaxDisplayNameChars} 个字符（当前 {count} 个）。";
        }
        foreach (var rune in value.EnumerateRunes())
        {
            var category = CharUnicodeInfo.GetUnicodeCategory(rune.Value);
            if (category is UnicodeCategory.Control or UnicodeCategory.Format or UnicodeCategory.Surrogate
                or UnicodeCategory.OtherNotAssigned or UnicodeCategory.LineSeparator
                or UnicodeCategory.ParagraphSeparator)
            {
                return "显示名称不能包含控制字符或不可见格式字符。";
            }
        }
        return null;
    }

    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.PropertyName is not null && DerivedProperties.Contains(e.PropertyName))
        {
            return;
        }
        foreach (var name in DerivedProperties)
        {
            base.OnPropertyChanged(new PropertyChangedEventArgs(name));
        }
        ContinueCommand.NotifyCanExecuteChanged();
        VerifyInviteCommand.NotifyCanExecuteChanged();
        CreateAccountCommand.NotifyCanExecuteChanged();
    }
}
