using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace PartyUp.UiPrototype;

/// <summary>
/// Local, synthetic state for the throwaway v1.1 interaction prototype.
/// Deliberately contains no network, credential, device, capture, playback,
/// global-shortcut, tray, startup or persistence code. Every transition below
/// is a UI demonstration; nothing here proves a real system capability.
/// </summary>
public partial class DemoState : ObservableObject
{
    /// <summary>Invite code accepted by the registration demonstration only.</summary>
    public const string DemoInviteCode = "DEMO-2026";

    private static readonly string[] InjectedAuthors = { "阿岚", "小满", "路人甲" };

    private static readonly HashSet<string> DerivedPropertyNames = new(StringComparer.Ordinal)
    {
        nameof(IsCallActive),
        nameof(CanSend),
        nameof(CanJoin),
        nameof(CanPublish),
        nameof(CanWatch),
        nameof(CanRetry),
        nameof(CallSummary),
        nameof(BrowseSummary),
        nameof(WatchSummary),
        nameof(IsWaitingForFrame),
        nameof(InspectorText),
    };

    private static readonly JsonSerializerOptions InspectorJsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    // Per-channel runtime state. Drafts and failures deliberately survive
    // navigation between channels; only logout clears them.
    private readonly Dictionary<string, string> _drafts = new(StringComparer.Ordinal);
    private readonly Dictionary<string, DemoMessage> _failedByChannel = new(StringComparer.Ordinal);
    private readonly HashSet<string> _unreadChannels = new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<DemoMessage>> _messagesByChannel = new(StringComparer.Ordinal);

    // UI-only private state.
    private bool _suppressDraftSave;
    private bool _atLatest = true;
    private bool _isWaitingForFrame;

    // Generation counters make late async completions harmless.
    private int _loginGeneration;
    private int _transferGeneration;
    private int _watchGeneration;
    private int _publishGeneration;
    private int _reconnectGeneration;
    private int _injectIndex;

    // ---------------------------------------------------------------- login

    [ObservableProperty]
    private string serverAddress = "example.invalid";

    [ObservableProperty]
    private string username = "demo";

    [ObservableProperty]
    private string password = "demo";

    [ObservableProperty]
    private string inviteCode = string.Empty;

    [ObservableProperty]
    private bool isRegistering;

    [ObservableProperty]
    private string loginError = string.Empty;

    // ------------------------------------------------------------- identity

    [ObservableProperty]
    private bool isLoggedIn;

    [ObservableProperty]
    private bool isBusy;

    [ObservableProperty]
    private bool isConnected = true;

    [ObservableProperty]
    private string statusText = "演示：交互原型 · 演示数据 · 模拟连接（未连接真实服务器）。";

    // -------------------------------------------------------------- browse

    private string browseChannel = "攻略与杂谈";

    /// <summary>Channel whose text is being read/typed; independent of the call.</summary>
    public string BrowseChannel
    {
        get => browseChannel;
        set
        {
            var normalized = value ?? string.Empty;
            if (string.Equals(browseChannel, normalized, StringComparison.Ordinal))
                return;

            var previous = browseChannel;
            if (!_suppressDraftSave && !string.IsNullOrEmpty(previous))
                _drafts[previous] = InputText;

            if (!SetProperty(ref browseChannel, normalized))
                return;

            LoadChannel(normalized);
        }
    }

    private string inputText = string.Empty;

    /// <summary>Draft for <see cref="BrowseChannel"/>; saved per channel on every edit.</summary>
    public string InputText
    {
        get => inputText;
        set
        {
            var normalized = value ?? string.Empty;
            if (string.Equals(inputText, normalized, StringComparison.Ordinal))
                return;

            if (!_suppressDraftSave && !string.IsNullOrEmpty(BrowseChannel))
                _drafts[BrowseChannel] = normalized;

            SetProperty(ref inputText, normalized);
        }
    }

    [ObservableProperty]
    private bool hasNewMessages;

    [ObservableProperty]
    private bool hasFailedMessage;

    // ---------------------------------------------------------------- call

    [ObservableProperty]
    private string callChannel = string.Empty;

    [ObservableProperty]
    private bool isMuted;

    [ObservableProperty]
    private bool isDeafened;

    [ObservableProperty]
    private bool isPublishing;

    [ObservableProperty]
    private string selectedSource = "显示器 1（演示源）";

    [ObservableProperty]
    private bool failNextPublish;

    [ObservableProperty]
    private bool hasPublishFailure;

    [ObservableProperty]
    private string publishError = string.Empty;

    public ObservableCollection<string> Sources { get; } = new()
    {
        "显示器 1（演示源）",
        "攻略窗口（演示源）",
    };

    public bool IsWaitingForFrame => _isWaitingForFrame;

    [ObservableProperty]
    private bool isTransferPending;

    [ObservableProperty]
    private string pendingChannel = string.Empty;

    [ObservableProperty]
    private bool hasTransferFailure;

    [ObservableProperty]
    private string lastCallChannel = string.Empty;

    // --------------------------------------------------------------- watch

    [ObservableProperty]
    private string selectedShare = "阿岚 · 桌面";

    [ObservableProperty]
    private bool isWatching;

    [ObservableProperty]
    private bool isFullScreen;

    // -------------------------------------------------------------- layout

    [ObservableProperty]
    private bool isChatVisible = true;

    [ObservableProperty]
    private bool showInspector;

    [ObservableProperty]
    private bool showSettings;

    [ObservableProperty]
    private string variant = "A";

    [ObservableProperty]
    private string themeChoice = "深色";

    [ObservableProperty]
    private string textSizeChoice = "标准";

    // ------------------------------------------------------- settings/mock

    [ObservableProperty]
    private string inputDeviceChoice = "跟随系统默认（模拟）";

    [ObservableProperty]
    private string outputDeviceChoice = "跟随系统默认（模拟）";

    [ObservableProperty]
    private string inputModeChoice = "自由说话";

    [ObservableProperty]
    private string deviceNotice = "演示：设备列表为模拟数据，未访问真实设备。";

    [ObservableProperty]
    private bool closeToTray = true;

    [ObservableProperty]
    private bool startOnLogin;

    [ObservableProperty]
    private bool startHidden;

    // ---------------------------------------------------------- demo flags

    [ObservableProperty]
    private bool failNextTransfer;

    [ObservableProperty]
    private bool failNextSend;

    // --------------------------------------------------------- collections

    public ObservableCollection<string> Channels { get; } = new()
    {
        "开黑大厅",
        "攻略与杂谈",
        "一起看",
    };

    public ObservableCollection<string> Shares { get; } = new()
    {
        "阿岚 · 桌面",
        "小满 · 窗口",
    };

    public ObservableCollection<DemoMessage> Messages { get; } = new();

    public DemoState()
    {
        Messages.CollectionChanged += (_, _) => OnPropertyChanged(nameof(InspectorText));
        SeedDemoData();
        LoadChannel(browseChannel);
    }

    // ------------------------------------------------------------ derived

    public bool IsCallActive => IsLoggedIn && !string.IsNullOrEmpty(CallChannel);

    public bool CanSend => IsLoggedIn && IsConnected && !string.IsNullOrWhiteSpace(InputText);

    public bool CanJoin =>
        IsLoggedIn &&
        IsConnected &&
        !IsTransferPending &&
        !string.IsNullOrEmpty(BrowseChannel) &&
        !string.Equals(CallChannel, BrowseChannel, StringComparison.Ordinal);

    public bool CanPublish =>
        IsLoggedIn &&
        IsCallActive &&
        (IsPublishing || (!IsTransferPending && !IsBusy && IsConnected));

    public bool CanWatch =>
        IsLoggedIn &&
        IsConnected &&
        IsCallActive &&
        !IsTransferPending &&
        !string.IsNullOrEmpty(SelectedShare) &&
        Shares.Contains(SelectedShare);

    public bool CanRetry => IsLoggedIn && IsConnected && HasFailedMessage;

    public string CallSummary
    {
        get
        {
            if (!IsLoggedIn)
                return "登录后可加入语音";

            var parts = new List<string>();
            if (!string.IsNullOrEmpty(CallChannel))
            {
                parts.Add(IsConnected ? $"通话中 · {CallChannel}" : $"重连中 · {CallChannel}（演示）");
                if (IsPublishing)
                    parts.Add("正在共享");
                if (IsWatching)
                    parts.Add($"正在观看「{SelectedShare}」");
                else if (_isWaitingForFrame)
                    parts.Add("等待共享首帧");
                if (IsMuted)
                    parts.Add("麦克风已静音");
                if (IsDeafened)
                    parts.Add("已关闭扬声器");
                if (IsTransferPending && !string.IsNullOrEmpty(PendingChannel))
                    parts.Add($"待确认转移至 {PendingChannel}");
            }
            else if (IsTransferPending && !string.IsNullOrEmpty(PendingChannel))
            {
                parts.Add($"未加入通话 · 待确认转移至 {PendingChannel}");
            }
            else if (HasTransferFailure)
            {
                parts.Add(string.IsNullOrEmpty(LastCallChannel)
                    ? "未加入通话 · 上次转移失败"
                    : $"未加入通话 · 转移失败，原频道为 {LastCallChannel}");
            }
            else
            {
                parts.Add("未加入通话 · 可在浏览的频道加入语音");
            }

            if (!IsConnected && string.IsNullOrEmpty(CallChannel))
                parts.Add("离线，媒体操作暂停");
            if (IsFullScreen)
                parts.Add("全屏观看");

            return string.Join(" · ", parts);
        }
    }

    public string BrowseSummary
    {
        get
        {
            if (!IsLoggedIn)
                return "登录后选择频道";

            var summary = $"浏览：{BrowseChannel}";
            if (IsCallActive && !string.Equals(CallChannel, BrowseChannel, StringComparison.Ordinal))
                summary += $" · 通话在 {CallChannel}";
            if (HasNewMessages)
                summary += " · 有新消息";
            return summary;
        }
    }

    public string WatchSummary
    {
        get
        {
            if (!IsLoggedIn)
                return "登录后可查看共享";
            if (!IsCallActive)
                return "未加入通话 · 加入后可观看当前通话频道的共享";
            if (_isWaitingForFrame)
                return $"等待「{SelectedShare}」首帧…（演示）";
            if (IsWatching)
            {
                var summary = $"正在观看「{SelectedShare}」 · 来自 {CallChannel}";
                if (IsFullScreen)
                    summary += " · 全屏";
                if (!IsConnected)
                    summary += " · 已断开（演示画面保持）";
                return summary;
            }
            return IsPublishing
                ? "正在共享 · 尚未观看其他共享"
                : "未观看共享 · 可从当前通话频道的共享列表选择";
        }
    }

    public string InspectorText
    {
        get
        {
            var snapshot = new Dictionary<string, object>
            {
                ["demo"] = "交互原型 / 演示数据 / 未连接服务器",
                ["generatedAt"] = DateTime.Now.ToString("HH:mm:ss"),
                ["login"] = new Dictionary<string, object>
                {
                    ["isLoggedIn"] = IsLoggedIn,
                    ["isBusy"] = IsBusy,
                    ["isRegistering"] = IsRegistering,
                    ["serverAddress"] = ServerAddress ?? string.Empty,
                    ["username"] = Username ?? string.Empty,
                    ["hasPassword"] = !string.IsNullOrEmpty(Password),
                    ["loginError"] = LoginError ?? string.Empty,
                },
                ["connection"] = new Dictionary<string, object>
                {
                    ["isConnected"] = IsConnected,
                    ["statusText"] = StatusText ?? string.Empty,
                },
                ["browse"] = new Dictionary<string, object>
                {
                    ["channel"] = BrowseChannel ?? string.Empty,
                    ["hasNewMessages"] = HasNewMessages,
                    ["summary"] = BrowseSummary,
                },
                ["call"] = new Dictionary<string, object>
                {
                    ["channel"] = CallChannel ?? string.Empty,
                    ["isActive"] = IsCallActive,
                    ["isPublishing"] = IsPublishing,
                    ["isMuted"] = IsMuted,
                    ["isDeafened"] = IsDeafened,
                    ["pendingChannel"] = PendingChannel ?? string.Empty,
                    ["isTransferPending"] = IsTransferPending,
                    ["hasTransferFailure"] = HasTransferFailure,
                    ["lastCallChannel"] = LastCallChannel ?? string.Empty,
                    ["summary"] = CallSummary,
                },
                ["publication"] = new Dictionary<string, object>
                {
                    ["source"] = SelectedSource,
                    ["isPublishing"] = IsPublishing,
                    ["hasFailure"] = HasPublishFailure,
                    ["error"] = PublishError,
                    ["failNext"] = FailNextPublish,
                },
                ["watch"] = new Dictionary<string, object>
                {
                    ["selectedShare"] = SelectedShare ?? string.Empty,
                    ["shares"] = Shares.ToArray(),
                    ["isWatching"] = IsWatching,
                    ["isFullScreen"] = IsFullScreen,
                    ["waitingForFirstFrame"] = _isWaitingForFrame,
                    ["summary"] = WatchSummary,
                },
                ["layout"] = new Dictionary<string, object>
                {
                    ["isChatVisible"] = IsChatVisible,
                    ["showInspector"] = ShowInspector,
                    ["showSettings"] = ShowSettings,
                    ["variant"] = Variant ?? string.Empty,
                    ["themeChoice"] = ThemeChoice ?? string.Empty,
                    ["textSizeChoice"] = TextSizeChoice ?? string.Empty,
                },
                ["audio"] = new Dictionary<string, object>
                {
                    ["inputDeviceChoice"] = InputDeviceChoice ?? string.Empty,
                    ["outputDeviceChoice"] = OutputDeviceChoice ?? string.Empty,
                    ["inputModeChoice"] = InputModeChoice ?? string.Empty,
                    ["deviceNotice"] = DeviceNotice ?? string.Empty,
                },
                ["window"] = new Dictionary<string, object>
                {
                    ["closeToTray"] = CloseToTray,
                    ["startOnLogin"] = StartOnLogin,
                    ["startHidden"] = StartHidden,
                },
                ["demoFlags"] = new Dictionary<string, object>
                {
                    ["failNextTransfer"] = FailNextTransfer,
                    ["failNextSend"] = FailNextSend,
                    ["readingHistory"] = !_atLatest,
                },
                ["drafts"] = new Dictionary<string, string>(_drafts),
                ["failedChannels"] = _failedByChannel.Keys.OrderBy(key => key, StringComparer.Ordinal).ToArray(),
                ["unreadChannels"] = _unreadChannels.OrderBy(key => key, StringComparer.Ordinal).ToArray(),
                ["messageCounts"] = _messagesByChannel.ToDictionary(pair => pair.Key, pair => pair.Value.Count),
            };

            try
            {
                return JsonSerializer.Serialize(snapshot, InspectorJsonOptions);
            }
            catch (Exception ex)
            {
                return "检查器序列化失败：" + ex.Message;
            }
        }
    }

    // ---------------------------------------------------------- commands

    [RelayCommand]
    private async Task LoginAsync()
    {
        if (IsBusy || IsLoggedIn)
            return;

        LoginError = string.Empty;
        var address = (ServerAddress ?? string.Empty).Trim();
        var user = (Username ?? string.Empty).Trim();
        var secret = Password ?? string.Empty;

        if (address.Length == 0)
        {
            LoginError = "请输入服务器地址（演示）。";
            return;
        }

        if (user.Length == 0)
        {
            LoginError = "请输入用户名（演示）。";
            return;
        }

        if (secret.Length == 0)
        {
            LoginError = "请输入密码（演示）。";
            return;
        }

        var wasRegistering = IsRegistering;
        if (wasRegistering)
        {
            var invite = (InviteCode ?? string.Empty).Trim();
            if (invite.Length == 0)
            {
                LoginError = "请输入邀请码（演示）。";
                return;
            }

            if (!string.Equals(invite, DemoInviteCode, StringComparison.OrdinalIgnoreCase))
            {
                LoginError = $"邀请码无效（演示）。演示邀请码为 {DemoInviteCode}。";
                return;
            }
        }

        var generation = ++_loginGeneration;
        IsBusy = true;
        StatusText = "演示：正在校验演示账号…（不会发出真实登录请求）";

        await Task.Delay(280);

        IsBusy = false;
        if (generation != _loginGeneration || IsLoggedIn)
            return;

        IsRegistering = false;
        IsLoggedIn = true;
        IsConnected = true;
        ShowSettings = false;
        IsChatVisible = true;
        IsFullScreen = false;
        IsWatching = false;
        IsPublishing = false;
        IsMuted = false;
        IsDeafened = false;
        IsTransferPending = false;
        HasTransferFailure = false;
        CallChannel = string.Empty;
        PendingChannel = string.Empty;
        LastCallChannel = string.Empty;
        _isWaitingForFrame = false;
        _atLatest = true;
        LoginError = string.Empty;

        if (_messagesByChannel.Count == 0)
            SeedDemoData();

        if (string.IsNullOrEmpty(BrowseChannel) || !Channels.Contains(BrowseChannel))
            BrowseChannel = "攻略与杂谈";
        else
            LoadChannel(BrowseChannel);

        StatusText = wasRegistering
            ? $"演示：已创建演示账号「{user}」并登录（模拟，未连接服务器）。"
            : $"演示：已登录「{user}」（模拟，未连接服务器）。聊天均为本地演示数据。";
    }

    [RelayCommand]
    private void Logout()
    {
        _loginGeneration++;
        _transferGeneration++;
        _watchGeneration++;
        _publishGeneration++;
        _reconnectGeneration++;

        IsBusy = false;
        IsLoggedIn = false;
        IsConnected = false;
        CallChannel = string.Empty;
        PendingChannel = string.Empty;
        LastCallChannel = string.Empty;
        IsTransferPending = false;
        HasTransferFailure = false;
        IsPublishing = false;
        IsWatching = false;
        IsFullScreen = false;
        _isWaitingForFrame = false;
        IsMuted = false;
        IsDeafened = false;
        ShowSettings = false;
        ShowInspector = false;
        IsChatVisible = true;
        DeviceNotice = "演示：设备列表为模拟数据，未访问真实设备。";
        HasFailedMessage = false;
        HasNewMessages = false;
        FailNextTransfer = false;
        FailNextSend = false;
        FailNextPublish = false;
        HasPublishFailure = false;
        PublishError = string.Empty;
        _atLatest = true;
        _drafts.Clear();
        _failedByChannel.Clear();
        _unreadChannels.Clear();

        _suppressDraftSave = true;
        InputText = string.Empty;
        Password = string.Empty;
        InviteCode = string.Empty;
        Username = string.Empty;
        _suppressDraftSave = false;

        _messagesByChannel.Clear();
        Messages.Clear();
        _injectIndex = 0;
        LoginError = string.Empty;
        IsRegistering = false;
        StatusText = "演示：已退出会话。未保存任何凭据、草稿或媒体状态。";
    }

    [RelayCommand]
    private void Join()
    {
        if (!IsLoggedIn)
            return;

        if (!IsConnected)
        {
            StatusText = "离线：无法加入语音，请先重新连接（演示）。";
            return;
        }

        if (IsTransferPending)
        {
            StatusText = $"演示：请先确认或取消转移到「{PendingChannel}」。";
            return;
        }

        if (IsBusy)
        {
            StatusText = "演示：操作进行中，请稍候。";
            return;
        }

        if (string.IsNullOrEmpty(BrowseChannel))
        {
            StatusText = "演示：请先选择一个频道。";
            return;
        }

        if (string.Equals(CallChannel, BrowseChannel, StringComparison.Ordinal))
        {
            StatusText = $"演示：已在「{BrowseChannel}」通话中。";
            return;
        }

        if (IsCallActive)
        {
            PendingChannel = BrowseChannel;
            IsTransferPending = true;
            HasTransferFailure = false;
            var extra = IsPublishing || IsWatching ? "将停止当前共享/观看并" : "将离开当前通话并";
            StatusText = $"演示：通话在「{CallChannel}」。确认转移{extra}加入「{BrowseChannel}」；取消则保持原状。";
            return;
        }

        CallChannel = BrowseChannel;
        IsMuted = false;
        IsDeafened = false;
        HasTransferFailure = false;
        StatusText = $"演示：已加入「{BrowseChannel}」语音（模拟，无真实媒体）。";
    }

    [RelayCommand]
    private async Task ConfirmTransferAsync()
    {
        if (!IsLoggedIn)
            return;

        if (!IsTransferPending || string.IsNullOrEmpty(PendingChannel))
        {
            StatusText = "演示：当前没有待确认的转移。";
            return;
        }

        if (!IsConnected)
        {
            StatusText = "离线：无法确认转移，请重新连接后再试（演示）。";
            return;
        }

        if (IsBusy)
            return;

        var target = PendingChannel;
        var previous = CallChannel;
        var generation = ++_transferGeneration;
        IsBusy = true;
        StatusText = $"演示：正在从「{previous}」转移到「{target}」…";

        await Task.Delay(320);

        IsBusy = false;
        if (generation != _transferGeneration)
            return;

        IsTransferPending = false;
        PendingChannel = string.Empty;
        _watchGeneration++;
        _publishGeneration++;
        _isWaitingForFrame = false;
        IsWatching = false;
        IsFullScreen = false;
        IsPublishing = false;
        IsChatVisible = true;
        CallChannel = string.Empty;
        if (!string.IsNullOrEmpty(previous))
            LastCallChannel = previous;

        if (FailNextTransfer)
        {
            FailNextTransfer = false;
            HasTransferFailure = true;
            IsMuted = false;
            IsDeafened = false;
            NotifyDerived();
            StatusText = $"演示：转移失败，已退出「{previous}」，当前未加入任何通话；" +
                         $"可返回浏览「{previous}」后手动重试，共享不会自动重开。";
            return;
        }

        CallChannel = target;
        HasTransferFailure = false;
        NotifyDerived();
        StatusText = $"演示：已转移并加入「{target}」（模拟，未恢复共享/观看）。";
    }

    [RelayCommand]
    private void CancelTransfer()
    {
        if (!IsTransferPending)
        {
            StatusText = "演示：当前没有待确认的转移。";
            return;
        }

        _transferGeneration++;
        IsBusy = false;
        IsTransferPending = false;
        PendingChannel = string.Empty;
        HasTransferFailure = false;
        StatusText = IsCallActive
            ? $"演示：已取消转移，保持「{CallChannel}」通话、发布与观看状态。"
            : "演示：已取消转移。";
    }

    [RelayCommand]
    private void ReturnPrevious()
    {
        if (!IsLoggedIn)
            return;

        if (string.IsNullOrEmpty(LastCallChannel))
        {
            StatusText = "演示：没有可返回的原频道。";
            return;
        }

        var previous = LastCallChannel;
        BrowseChannel = previous;
        HasTransferFailure = false;
        StatusText = $"演示：已返回浏览「{previous}」；未自动重新加入语音，需要时请显式加入。";
    }

    [RelayCommand]
    private void Leave()
    {
        if (!IsLoggedIn)
            return;

        var previous = CallChannel;
        _transferGeneration++;
        _watchGeneration++;
        _publishGeneration++;
        IsBusy = false;
        _isWaitingForFrame = false;

        if (!string.IsNullOrEmpty(previous))
            LastCallChannel = previous;

        CallChannel = string.Empty;
        PendingChannel = string.Empty;
        IsTransferPending = false;
        HasTransferFailure = false;
        IsPublishing = false;
        IsWatching = false;
        IsFullScreen = false;
        IsChatVisible = true;
        IsMuted = false;
        IsDeafened = false;
        NotifyDerived();

        StatusText = string.IsNullOrEmpty(previous)
            ? "演示：当前未加入通话（演示）。"
            : $"演示：已本地离开「{previous}」，未等待网络确认；重新连接后不会自动恢复。";
    }

    [RelayCommand]
    private void Mute()
    {
        if (!IsCallActive)
        {
            StatusText = "演示：未加入通话，静音仅表示本地状态，当前不可用。";
            return;
        }

        IsMuted = !IsMuted;
        StatusText = IsMuted
            ? "演示：麦克风已静音（本地演示，未操作真实设备）。"
            : "演示：麦克风已取消静音（本地演示）。";
    }

    [RelayCommand]
    private void Deafen()
    {
        if (!IsCallActive)
        {
            StatusText = "演示：未加入通话，关闭扬声器当前不可用。";
            return;
        }

        IsDeafened = !IsDeafened;
        if (IsDeafened)
            IsMuted = true;
        StatusText = IsDeafened
            ? "演示：已关闭扬声器（本地演示，未操作真实设备）。"
            : "演示：扬声器已恢复（本地演示）。";
    }

    [RelayCommand]
    private async Task PublishAsync()
    {
        if (!IsLoggedIn)
            return;

        // Stopping must always remain available, including while disconnected.
        if (IsPublishing)
        {
            _publishGeneration++;
            IsPublishing = false;
            StatusText = "演示：已停止发布（未访问真实采集）。";
            return;
        }

        if (IsBusy)
        {
            StatusText = "演示：操作进行中，请稍候再开始共享。";
            return;
        }

        if (!IsConnected)
        {
            StatusText = "离线：无法开始共享，恢复连接后请手动重试（演示）。";
            return;
        }

        if (!IsCallActive)
        {
            StatusText = "演示：未加入通话，无法共享；加入语音后可选择共享源。";
            return;
        }

        if (IsTransferPending)
        {
            StatusText = "演示：转移确认期间不能开始共享，请先确认或取消。";
            return;
        }

        if (!Sources.Contains(SelectedSource))
        {
            HasPublishFailure = true;
            PublishError = "请选择一个演示共享源。";
            return;
        }

        var source = SelectedSource;
        var fail = FailNextPublish;
        FailNextPublish = false;
        HasPublishFailure = false;
        PublishError = string.Empty;
        var generation = ++_publishGeneration;
        IsBusy = true;
        StatusText = $"演示：正在准备「{source}」…（模拟，未访问真实采集）";

        await Task.Delay(260);

        IsBusy = false;
        if (generation != _publishGeneration)
            return;

        if (!IsConnected || !IsCallActive)
        {
            StatusText = "演示：准备共享时状态已变化，未开始发布。";
            return;
        }

        if (fail)
        {
            HasPublishFailure = true;
            PublishError = $"演示：无法启动「{source}」，源选择已保留，请手动重试。";
            StatusText = PublishError;
            return;
        }

        IsPublishing = true;
        StatusText = $"演示：正在向「{CallChannel}」共享「{source}」（模拟，未访问真实采集）。";
    }

    [RelayCommand(AllowConcurrentExecutions = true)]
    private async Task WatchAsync(string? target)
    {
        if (!IsLoggedIn)
            return;

        if (!IsConnected)
        {
            StatusText = "离线：无法观看共享（演示）。";
            return;
        }

        if (!IsCallActive)
        {
            StatusText = "演示：未加入通话，无法观看共享；请先加入语音。";
            return;
        }

        if (IsTransferPending)
        {
            StatusText = "演示：转移确认期间不切换观看目标。";
            return;
        }

        var targetValue = string.IsNullOrWhiteSpace(target) ? SelectedShare : target.Trim();
        if (string.IsNullOrWhiteSpace(targetValue))
        {
            StatusText = "演示：请选择要观看的共享。";
            return;
        }

        if (!Shares.Contains(targetValue))
        {
            StatusText = $"演示：「{targetValue}」不在当前通话频道的共享列表中。";
            return;
        }

        var generation = ++_watchGeneration;
        SelectedShare = targetValue;
        _isWaitingForFrame = true;
        IsWatching = false;
        IsFullScreen = false;
        NotifyDerived();
        StatusText = $"演示：正在等待「{targetValue}」首帧…（模拟）";

        await Task.Delay(260);

        if (generation != _watchGeneration)
            return;

        _isWaitingForFrame = false;
        if (!IsConnected || !IsCallActive)
        {
            NotifyDerived();
            StatusText = "演示：观看已取消（状态变化）。";
            return;
        }

        IsWatching = true;
        NotifyDerived();
        StatusText = $"演示：正在观看「{targetValue}」· 来自「{CallChannel}」（模拟画面）。";
    }

    [RelayCommand]
    private void StopWatch()
    {
        if (!IsWatching && !_isWaitingForFrame)
        {
            StatusText = "演示：当前没有观看的共享。";
            return;
        }

        _watchGeneration++;
        var wasWatching = IsWatching;
        _isWaitingForFrame = false;
        IsWatching = false;
        IsFullScreen = false;
        IsChatVisible = true;
        NotifyDerived();
        StatusText = wasWatching
            ? "演示：已停止观看并返回聊天（浏览频道未变）。"
            : "演示：已取消等待首帧。";
    }

    [RelayCommand]
    private void EndShare()
    {
        if (!IsWatching && !_isWaitingForFrame)
        {
            StatusText = "演示：当前没有可结束的共享（没有正在观看的画面）。";
            return;
        }

        _watchGeneration++;
        _isWaitingForFrame = false;
        IsWatching = false;
        IsFullScreen = false;
        IsChatVisible = true;
        NotifyDerived();
        StatusText = "演示：共享方已结束共享，已返回聊天（未访问真实媒体）。";
    }

    [RelayCommand]
    private void ToggleFullscreen()
    {
        if (!IsWatching)
        {
            StatusText = "演示：当前没有观看内容，无法全屏。";
            return;
        }

        IsFullScreen = !IsFullScreen;
        StatusText = IsFullScreen
            ? "演示：已进入全屏观看（Escape 退出）。"
            : "演示：已退出全屏观看。";
    }

    [RelayCommand]
    private void ToggleChat()
    {
        IsChatVisible = !IsChatVisible;
        StatusText = IsChatVisible
            ? "演示：已展开聊天侧栏（观看状态保持）。"
            : "演示：已收起聊天侧栏（观看状态保持）。";
    }

    [RelayCommand]
    private void OpenSettings()
    {
        ShowSettings = true;
        if (IsFullScreen)
            IsFullScreen = false;
        StatusText = IsCallActive
            ? "演示：设置已打开，当前通话与观看保持。"
            : "演示：设置已打开。";
    }

    [RelayCommand]
    private void CloseSettings()
    {
        ShowSettings = false;
        StatusText = IsWatching
            ? $"演示：已返回观看「{SelectedShare}」。"
            : $"演示：已返回聊天「{BrowseChannel}」。";
    }

    [RelayCommand]
    private void Send()
    {
        if (!IsLoggedIn)
            return;

        if (string.IsNullOrEmpty(BrowseChannel))
        {
            StatusText = "演示：请先选择一个频道。";
            return;
        }

        var original = InputText ?? string.Empty;
        var text = original.Trim();
        if (text.Length == 0)
        {
            StatusText = "演示：请输入消息内容。";
            return;
        }

        if (!IsConnected)
        {
            StatusText = "离线：草稿已保留，恢复连接后请手动发送；不会自动发送。";
            return;
        }

        if (FailNextSend)
        {
            FailNextSend = false;
            _failedByChannel[BrowseChannel] = new DemoMessage("你", original, NowTime());
            HasFailedMessage = true;
            OnPropertyChanged(nameof(InspectorText));
            StatusText = $"演示：发送失败，原文已保留在「{BrowseChannel}」草稿中，可重试。";
            return;
        }

        AppendMessage(BrowseChannel, new DemoMessage("你", text, NowTime()));
        ClearInputForCurrentChannel();
        StatusText = $"演示：已发送到「{BrowseChannel}」（未连接服务器，无真实回执）。";
    }

    [RelayCommand]
    private void Retry()
    {
        if (!IsLoggedIn)
            return;

        if (!_failedByChannel.TryGetValue(BrowseChannel, out var failed) || failed is null)
        {
            HasFailedMessage = false;
            StatusText = "演示：当前频道没有待重试的消息。";
            return;
        }

        if (!IsConnected)
        {
            StatusText = "离线：无法重试，失败原文仍保留。";
            return;
        }

        if (FailNextSend)
        {
            FailNextSend = false;
            StatusText = "演示：重试仍失败，原文继续保留，可再次重试。";
            return;
        }

        AppendMessage(BrowseChannel, new DemoMessage("你", failed.Body, NowTime()));
        _failedByChannel.Remove(BrowseChannel);
        HasFailedMessage = false;

        if (string.Equals(InputText, failed.Body, StringComparison.Ordinal))
            ClearInputForCurrentChannel();
        else
            _drafts[BrowseChannel] = InputText;

        StatusText = $"演示：重试成功，已发送到「{BrowseChannel}」（模拟）。";
    }

    [RelayCommand]
    private void Disconnect()
    {
        if (!IsLoggedIn)
        {
            IsConnected = false;
            StatusText = "演示：当前未登录，连接状态已复位。";
            return;
        }

        if (!IsConnected)
        {
            StatusText = "演示：已经处于离线状态。";
            return;
        }

        _watchGeneration++;
        _publishGeneration++;
        _transferGeneration++;
        _reconnectGeneration++;
        _isWaitingForFrame = false;
        IsBusy = false;
        IsConnected = false;
        NotifyDerived();
        StatusText = "演示：网络已断开（模拟）。草稿保留；发送与新媒体操作暂停；" +
                     "本地静音、停止观看、离开和退出仍可用。";
    }

    [RelayCommand]
    private async Task ReconnectAsync()
    {
        if (!IsLoggedIn)
            return;

        if (IsConnected)
        {
            StatusText = "演示：当前已经是连接状态。";
            return;
        }

        if (IsBusy)
            return;

        var generation = ++_reconnectGeneration;
        IsBusy = true;
        StatusText = "演示：正在重新连接（模拟）…";

        await Task.Delay(300);

        IsBusy = false;
        if (generation != _reconnectGeneration)
            return;

        IsConnected = true;
        StatusText = "演示：已重新连接（模拟）。草稿未自动发送，已结束的通话/共享不会自动恢复。";
    }

    [RelayCommand]
    private void InjectMessage()
    {
        if (!IsLoggedIn)
            return;

        var author = InjectedAuthors[_injectIndex++ % InjectedAuthors.Length];
        var message = new DemoMessage(
            author,
            "这是一条本地注入的演示消息，用于检查新消息提示与滚动保护。",
            NowTime());

        AppendMessage(BrowseChannel, message);
        _unreadChannels.Add(BrowseChannel);
        HasNewMessages = true;
        _atLatest = false;
        OnPropertyChanged(nameof(InspectorText));
        StatusText = "演示：已注入一条新消息，未强制滚动；可用「跳到最新」回到末尾。";
    }

    [RelayCommand]
    private void MarkReadingHistory()
    {
        if (!IsLoggedIn)
            return;

        _atLatest = false;
        OnPropertyChanged(nameof(InspectorText));
        StatusText = "演示：已模拟进入历史阅读位置，不强制滚动；新消息会以提示出现。";
    }

    [RelayCommand]
    private void JumpToLatest()
    {
        if (!IsLoggedIn)
            return;

        _atLatest = true;
        _unreadChannels.Remove(BrowseChannel);
        HasNewMessages = false;
        OnPropertyChanged(nameof(InspectorText));
        StatusText = "演示：已跳到最新消息位置。";
    }

    [RelayCommand]
    private void InjectDeviceRemoval()
    {
        if (!IsLoggedIn)
            return;

        InputDeviceChoice = "跟随系统默认（模拟）";
        DeviceNotice = "演示：麦克风「USB 麦克风（模拟）」已拔出，已回退到「跟随系统默认（模拟）」；未枚举真实设备。";
        StatusText = "演示：设备拔出与默认回退仅为界面模拟，未访问真实音频设备，也未改变实际路由。";
    }

    [RelayCommand]
    private void ShowMockNotice(string? feature)
    {
        var featureName = string.IsNullOrWhiteSpace(feature) ? "该功能" : feature.Trim();
        StatusText = $"演示：{featureName} 仅为界面演示，等待真实设备/系统验收；未执行任何系统操作。";
    }

    // ----------------------------------------------------------- helpers

    partial void OnIsRegisteringChanged(bool value)
    {
        LoginError = string.Empty;
        StatusText = value
            ? $"演示：注册模式。邀请码仅用于演示，演示码为 {DemoInviteCode}。"
            : "演示：登录模式。不会发出真实登录请求。";
    }

    private void LoadChannel(string channel)
    {
        channel ??= string.Empty;

        _suppressDraftSave = true;
        InputText = _drafts.TryGetValue(channel, out var draft) ? (draft ?? string.Empty) : string.Empty;
        _suppressDraftSave = false;

        if (!_messagesByChannel.TryGetValue(channel, out var list))
        {
            list = new List<DemoMessage>();
            _messagesByChannel[channel] = list;
        }

        Messages.Clear();
        foreach (var message in list)
            Messages.Add(message);

        HasFailedMessage = _failedByChannel.ContainsKey(channel);
        HasNewMessages = _unreadChannels.Contains(channel);
        _atLatest = true;
        NotifyDerived();
    }

    private void AppendMessage(string channel, DemoMessage message)
    {
        if (string.IsNullOrEmpty(channel))
            channel = BrowseChannel;

        if (!_messagesByChannel.TryGetValue(channel, out var list))
        {
            list = new List<DemoMessage>();
            _messagesByChannel[channel] = list;
        }

        list.Add(message);
        if (string.Equals(channel, BrowseChannel, StringComparison.Ordinal))
            Messages.Add(message);
    }

    private void ClearInputForCurrentChannel()
    {
        if (!string.IsNullOrEmpty(BrowseChannel))
            _drafts[BrowseChannel] = string.Empty;

        _suppressDraftSave = true;
        InputText = string.Empty;
        _suppressDraftSave = false;
    }

    private static string NowTime() => DateTime.Now.ToString("HH:mm");

    private void SeedDemoData()
    {
        var now = DateTime.Now;

        _messagesByChannel["开黑大厅"] = new List<DemoMessage>
        {
            new("阿岚", "今晚八点开黑，老位置。", now.AddMinutes(-46).ToString("HH:mm")),
            new("小满", "收到，我先更新一下客户端。", now.AddMinutes(-41).ToString("HH:mm")),
            new("系统", "演示频道 · 消息为本地模拟数据，不来自服务器。", now.AddMinutes(-40).ToString("HH:mm")),
        };

        _messagesByChannel["攻略与杂谈"] = new List<DemoMessage>
        {
            new("阿岚", "新地图的 B 点烟位我整理了一版，等下发群里。", now.AddMinutes(-35).ToString("HH:mm")),
            new("小满", "昨天那局翻盘太离谱了。", now.AddMinutes(-28).ToString("HH:mm")),
            new("你", "我把回放链接放这，方便对照。", now.AddMinutes(-21).ToString("HH:mm")),
            new("系统", "演示会话 · 未连接服务器，输入仅保存在内存。", now.AddMinutes(-20).ToString("HH:mm")),
        };

        _messagesByChannel["一起看"] = new List<DemoMessage>
        {
            new("小满", "周末一起看比赛？", now.AddMinutes(-15).ToString("HH:mm")),
            new("阿岚", "可以，先把共享源准备好。", now.AddMinutes(-12).ToString("HH:mm")),
        };
    }

    // -------------------------------------------- derived notifications

    protected override void OnPropertyChanged(PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);

        if (e.PropertyName is null || !DerivedPropertyNames.Contains(e.PropertyName))
            NotifyDerived();
    }

    private void NotifyDerived()
    {
        OnPropertyChanged(nameof(IsCallActive));
        OnPropertyChanged(nameof(CanSend));
        OnPropertyChanged(nameof(CanJoin));
        OnPropertyChanged(nameof(CanPublish));
        OnPropertyChanged(nameof(CanWatch));
        OnPropertyChanged(nameof(CanRetry));
        OnPropertyChanged(nameof(CallSummary));
        OnPropertyChanged(nameof(BrowseSummary));
        OnPropertyChanged(nameof(WatchSummary));
        OnPropertyChanged(nameof(IsWaitingForFrame));
        OnPropertyChanged(nameof(InspectorText));
    }
}

/// <summary>
/// One synthetic chat entry. Immutable; contains only display strings.
/// </summary>
public sealed class DemoMessage
{
    public DemoMessage(string author, string body, string time)
    {
        Author = author;
        Body = body;
        Time = time;
    }

    public string Author { get; }

    public string Body { get; }

    public string Time { get; }
}
