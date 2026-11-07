using System.ComponentModel;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.AspNetCore.WebUtilities;

namespace PartyUp.UiPrototype;

/// <summary>Purely local registration storyboard. No HTTP calls or credential storage.</summary>
public partial class RegistrationFlow : ObservableObject
{
    private readonly Action<string, string, string> _returnToLogin;
    private Uri? _server;
    private int _generation;
    private static readonly HashSet<string> Derived =
    [nameof(TargetStep), nameof(InviteStep), nameof(AccountStep), nameof(DoneStep),
     nameof(StepTitle), nameof(CanContinue), nameof(CanVerify), nameof(CanCreate), nameof(SafeState), nameof(PasswordFeedback)];

    [ObservableProperty] private bool isActive;
    [ObservableProperty] private int step;
    [ObservableProperty] private string target = "https://example.invalid";
    [ObservableProperty] private string inviteCode = string.Empty;
    [ObservableProperty] private bool inviteVerified;
    [ObservableProperty] private bool busy;
    [ObservableProperty] private string error = string.Empty;
    [ObservableProperty] private string serverLabel = string.Empty;
    [ObservableProperty] private string newUsername = string.Empty;
    [ObservableProperty] private string displayName = string.Empty;
    [ObservableProperty] private string newPassword = string.Empty;
    [ObservableProperty] private string confirmPassword = string.Empty;

    public RegistrationFlow(Action<string, string, string> returnToLogin) => _returnToLogin = returnToLogin;
    public bool TargetStep => Step == 0;
    public bool InviteStep => Step == 1;
    public bool AccountStep => Step == 2;
    public bool DoneStep => Step == 3;
    public string StepTitle => Step switch
    {
        0 => "先找到你的服务器",
        1 => "确认加入资格",
        2 => "创建你的账户",
        _ => "演示流程已完成",
    };
    public bool CanContinue => IsActive && TargetStep && !Busy && !string.IsNullOrWhiteSpace(Target);
    public bool CanVerify => IsActive && InviteStep && !Busy && _server is not null && !string.IsNullOrWhiteSpace(InviteCode);
    public bool CanCreate => IsActive && AccountStep && !Busy && InviteVerified && _server is not null &&
        NewUsername.Trim().Length is >= 2 and <= 32 && !string.IsNullOrWhiteSpace(DisplayName) &&
        Encoding.UTF8.GetByteCount(NewPassword) is >= 6 and <= 256 &&
        string.Equals(NewPassword, ConfirmPassword, StringComparison.Ordinal);
    public string PasswordFeedback => string.IsNullOrEmpty(ConfirmPassword) ? string.Empty :
        NewPassword == ConfirmPassword ? "两次密码一致" : "两次密码不一致，暂不能创建账户";
    // Deliberately excludes invitation contents, URLs, usernames and both passwords.
    public string SafeState => $"registration active={IsActive} step={Step} busy={Busy} verified={InviteVerified} passwordsMatch={!string.IsNullOrEmpty(NewPassword) && NewPassword == ConfirmPassword} simulationOnly=true";

    partial void OnTargetChanged(string value)
    {
        Invalidate();
        _server = null;
        ServerLabel = string.Empty;
        Step = 0;
    }
    partial void OnInviteCodeChanged(string value)
    {
        Invalidate();
        if (Step >= 2) Step = 1;
    }

    private void Invalidate()
    {
        _generation++;
        Busy = false;
        InviteVerified = false;
        Error = string.Empty;
    }

    [RelayCommand]
    private void Begin()
    {
        ResetDraft();
        IsActive = true;
    }

    [RelayCommand]
    private void Cancel()
    {
        IsActive = false;
        ResetDraft();
    }

    private void ResetDraft()
    {
        Invalidate();
        Step = 0;
        _server = null;
        ServerLabel = string.Empty;
        InviteCode = string.Empty;
        Target = "https://example.invalid";
        NewUsername = DisplayName = string.Empty;
        NewPassword = ConfirmPassword = string.Empty;
    }

    [RelayCommand]
    private void Back()
    {
        Invalidate();
        NewPassword = ConfirmPassword = string.Empty;
        Step = Math.Max(0, Step - 1);
    }

    [RelayCommand(CanExecute = nameof(CanContinue))]
    private async Task ContinueAsync()
    {
        Error = string.Empty;
        var raw = Target.Trim();
        if (!Uri.TryCreate(raw, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
        {
            Error = "请输入带 HTTP/HTTPS 的完整服务器地址或邀请链接。";
            return;
        }
        if (string.IsNullOrWhiteSpace(uri.Host) || !string.IsNullOrEmpty(uri.UserInfo))
        {
            Error = "服务器须为 HTTP/HTTPS 地址，不能在地址中放用户名和密码。";
            return;
        }
        if (!string.IsNullOrEmpty(uri.Fragment))
        {
            Error = "此预览只演示查询参数邀请链接，不支持片段路由形式。";
            return;
        }

        // URI and query syntax are parsed only by framework/library parsers.
        var query = QueryHelpers.ParseQuery(uri.Query);
        if (query.TryGetValue("code", out var codes) && codes.Count != 1)
        {
            Error = "邀请链接包含重复邀请码，请检查链接。";
            return;
        }
        var linkedCode = query.TryGetValue("code", out codes) ? codes.ToString() : string.Empty;
        // Draft invite format: the server base URI itself, with a code query.
        // Preserve reverse-proxy/base paths instead of dropping them for links.
        var server = uri.GetLeftPart(UriPartial.Path).TrimEnd('/');
        _server = new Uri(server, UriKind.Absolute);
        ServerLabel = _server.AbsoluteUri;
        InviteCode = linkedCode;
        Step = 1;
        if (!string.IsNullOrWhiteSpace(linkedCode))
            await VerifyAsync();
    }

    [RelayCommand(CanExecute = nameof(CanVerify))]
    private async Task VerifyAsync()
    {
        if (_server is null || string.IsNullOrWhiteSpace(InviteCode))
        {
            Error = "请先选择服务器并输入邀请码。";
            return;
        }
        var generation = ++_generation;
        var code = InviteCode.Trim();
        Busy = true;
        Error = string.Empty;
        try
        {
            // Explicitly simulated: the real service has no non-consuming validation endpoint.
            await Task.Delay(450);
            if (generation != _generation || !IsActive) return;
            if (!string.Equals(code, DemoState.DemoInviteCode, StringComparison.OrdinalIgnoreCase))
            {
                Error = "演示校验未通过。请使用 DEMO-2026；未请求真实服务器，也未消耗邀请码。";
                return;
            }
            InviteVerified = true;
            Step = 2;
        }
        finally
        {
            if (generation == _generation) Busy = false;
        }
    }

    [RelayCommand(CanExecute = nameof(CanCreate))]
    private async Task CreateAsync()
    {
        if (!CanCreate)
        {
            Error = "请先校验邀请码，填写用户名和显示名称，并确认两次密码一致（6–256 UTF-8字节）。";
            return;
        }
        var generation = ++_generation;
        var user = NewUsername;
        var name = DisplayName;
        var password = NewPassword;
        Busy = true;
        Error = string.Empty;
        try
        {
            await Task.Delay(300);
            if (generation != _generation || !IsActive || !InviteVerified) return;
            if (user != NewUsername || name != DisplayName || password != NewPassword || password != ConfirmPassword)
            {
                Error = "账户信息已经变化，请重新提交。";
                return;
            }
            NewPassword = ConfirmPassword = string.Empty;
            Step = 3;
        }
        finally
        {
            if (generation == _generation) Busy = false;
        }
    }

    [RelayCommand]
    private void ReturnToLogin()
    {
        if (Step != 3 || _server is null) return;
        _returnToLogin(_server.AbsoluteUri, NewUsername.Trim(), DisplayName.Trim());
        Cancel();
    }

    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.PropertyName is not null && Derived.Contains(e.PropertyName)) return;
        foreach (var name in Derived) base.OnPropertyChanged(new PropertyChangedEventArgs(name));
        ContinueCommand.NotifyCanExecuteChanged();
        VerifyCommand.NotifyCanExecuteChanged();
        CreateCommand.NotifyCanExecuteChanged();
    }
}
