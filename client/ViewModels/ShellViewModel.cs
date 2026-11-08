using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PartyUp.Client.Localization;
using PartyUp.Client.Models;
using PartyUp.Client.Services;

namespace PartyUp.Client.ViewModels;

/// <summary>一条待显示的频道消息（已把 member_id 解析成展示名、时间戳格式化成 HH:mm）。</summary>
public sealed record ShellMessageItem(string Author, string Body, string TimeText, bool IsSelf);

/// <summary>
/// v1.1 重构外壳的视图模型：真实登录 → 频道浏览/文字 → 全服名册 → 语音/共享 → 设置。
///
/// 与旧 <see cref="MainViewModel"/> 的关键区别是**浏览与通话彻底分离**：
///   * <c>_browseChannelId</c> 只决定读哪个频道的文字与草稿；
///   * <c>_callChannelId</c> 只决定语音/共享归属，并且持有唯一的 <see cref="ShareSession"/>；
///   * 单击频道只浏览（不会 dispose 通话会话），双击/显式加入才转移通话。
/// 旧的 OpenChannelAsync 在切频道时会重建 ShareSession，因此不能拿来当浏览语义的实现。
///
/// 线程约定：网络事件在 WS 接收线程触发、媒体事件在 media pump 线程触发，
/// 所有集合/属性修改都在 <see cref="Dispatcher.UIThread"/> 上执行。
/// </summary>
public sealed partial class ShellViewModel : ObservableObject
{
    /// <summary>浏览频道历史一次拉取的条数（服务端上限内）。</summary>
    private const int HistoryLimit = 50;

    // ---- 服务与生命周期 ----
    private PartyUpConnection? _connection;
    private MediaEngine? _media;
    private MediaPump? _pump;
    private RemoteScreenPresenter? _remoteScreen;
    /// <summary>当前通话频道的会话（语音 + 共享发布/观看）。只有加入语音后才存在。</summary>
    private ShareSession? _call;
    private Uri? _baseUri;
    private MemberDto? _self;
    private bool _busy;
    private bool _frameAcquireWarned;
    private bool _loggingOut;

    // ---- 频道/订阅状态 ----
    private readonly Dictionary<string, ChannelDto> _channelsById = new(StringComparer.Ordinal);
    private readonly HashSet<string> _subscriptions = new(StringComparer.Ordinal);
    private readonly Dictionary<string, VoiceStateDto> _voiceByChannel = new(StringComparer.Ordinal);
    private readonly Dictionary<string, ScreenStateDto> _screenByChannel = new(StringComparer.Ordinal);
    private IReadOnlyList<MemberRosterEntryDto> _roster = Array.Empty<MemberRosterEntryDto>();
    private readonly Dictionary<string, string> _drafts = new(StringComparer.Ordinal);
    private readonly HashSet<string> _messageIds = new(StringComparer.Ordinal);

    /// <summary>消息按 id 去重时保留的插入顺序，用于在裁剪时同步释放 id 集合。</summary>
    private readonly Queue<string> _messageIdOrder = new();

    /// <summary>单频道内存上限：长驻频道不会线性增长（最老的消息先裁）。</summary>
    private const int MessageCap = 2000;

    /// <summary>
    /// 「上一次登录尝试」的序号：只用于丢弃迟到的 HTTP 响应，由 LoginCoreAsync/AutoLogin 推进。
    /// 建立会话（AttachSessionAsync）有自己的 <see cref="_attachGeneration"/>，绝不能共用——
    /// 共用会让登录尝试的成功收尾被自己的 attach 判定为过期（曾导致登录后忙态永不复位）。
    /// </summary>
    private int _loginGeneration;

    /// <summary>「上一次建会话尝试」的序号：丢弃迟到的 WS 建连结果。</summary>
    private int _attachGeneration;

    /// <summary>当前会话的服务器与 token；主动退出要调 /api/logout，记住我要按它落盘/清除。</summary>
    private Uri? _sessionBaseUri;
    private string? _sessionToken;

    private int _browseGeneration;
    private int _callGeneration;
    private string? _browseChannelId;
    private string? _callChannelId;
    private string? _pendingTransferChannelId;
    private string? _previousCallChannelId;

    public ShellViewModel()
    {
        ServerUrl = Program.Options.Server;
        Registration = new RegistrationViewModel(OnRegistrationCompleted, OnRegistrationCancelled);

        if (Program.Options.NoMedia)
        {
            MediaStatusText = "媒体引擎：已禁用（--no-media）";
        }
        else
        {
            TryInitMedia();
        }

        FooterText = "填写服务器地址后用账号登录，或通过邀请链接注册。";

        // 外观与语言从本机设置回填。语言本身已在 Program.Main 里应用到 L（XAML 载入前），
        // 这里只是让选择器显示当前值；改语言写盘后提示需要重启。
        var saved = AppSettings.Instance;
        ThemeChoice = ThemeChoices[Math.Max(0, Array.IndexOf(ThemeKeys, saved.Theme))];
        TextSizeChoice = TextSizeChoices[Math.Max(0, Array.IndexOf(TextSizeKeys, saved.TextSize))];
        LanguageOption = LanguageChoices.FirstOrDefault(o => o.Tag == saved.Language) ?? LanguageChoices[0];

        // 窗口与启动：关闭到托盘直接读设置文件；开机启动的真源是注册表，设置文件只做缓存。
        // 初始化时不能触发写入（会在每次启动时改写注册表），所以先屏蔽变更回调。
        _suppressStartupChange = true;
        try
        {
            CloseToTray = saved.CloseToTray;
            LaunchAtStartup = StartupRegistrar.IsEnabled();
        }
        finally
        {
            _suppressStartupChange = false;
        }

        // 自动更新开关：默认开；初始化时同样屏蔽回调，避免每次启动都写盘。
        _suppressAutoCheckChange = true;
        try
        {
            AutoCheckUpdates = saved.AutoCheckUpdates;
        }
        finally
        {
            _suppressAutoCheckChange = false;
        }

        // 空态提示需要跟随集合变化刷新；CollectionChanged 只在构造时订阅一次。
        Messages.CollectionChanged += (_, _) => OnPropertyChanged(nameof(HasMessages));

        // 注册流程自己控制 IsActive；阶段属性必须跟着它的变化重新求值，
        // 否则点了「注册新账户」后注册面板不会出现（IsActive=true 但 ShowRegistration 不刷新）。
        Registration.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(RegistrationViewModel.IsActive))
            {
                NotifyStage();
            }
        };

        // 第一屏服务器列表：载入本机记住的服务器；有上次使用的就预选，但**不自动跳过第一屏**——
        // 用户要看到「自己在哪台服务器上」，多服务器场景下自动跳转很容易连错服。
        RefreshServers();
        if (_serverBook.Selected is { } last)
        {
            ActiveServer = last;
            ServerUrl = last.Url;
        }
        else if (Servers.Count == 0 && !string.IsNullOrWhiteSpace(Program.Options.Server))
        {
            // 首次启动但命令行给了地址（例如验收脚本）：直接把它当待添加目标预填。
            AddServerInput = Program.Options.Server;
        }

        // 用户要求「启动后静默检查」：主窗口显示后延迟约 8 秒跑一次（失败只记日志）。
        ScheduleStartupUpdateCheck();
    }

    // ------------------------------------------------------------------
    // 认证 / 启动阶段
    // ------------------------------------------------------------------

    [ObservableProperty]
    private string _serverUrl = "http://127.0.0.1:8080";

    [ObservableProperty]
    private string _username = string.Empty;

    [ObservableProperty]
    private string _password = string.Empty;

    [ObservableProperty]
    private string _loginError = string.Empty;

    /// <summary>
    /// 会话被服务端终止后留在登录页的说明（顶号/被踢/被封禁各一句）。
    /// 与 <see cref="LoginError"/> 分开：那是"这次登录没成功"，这是"上次会话为什么结束"。
    /// </summary>
    [ObservableProperty]
    private string _sessionNotice = string.Empty;

    public bool HasSessionNotice => SessionNotice.Length > 0;

    partial void OnSessionNoticeChanged(string value) => OnPropertyChanged(nameof(HasSessionNotice));

    [ObservableProperty]
    private bool _isLoggingIn;

    [ObservableProperty]
    private bool _isLoggedIn;

    [ObservableProperty]
    private string _selfDisplayName = string.Empty;

    [ObservableProperty]
    private string _selfLoginName = string.Empty;

    /// <summary>服务端自我介绍里的名字（auth.ok.server.name），未连接时为空。</summary>
    [ObservableProperty]
    private string _serverName = string.Empty;

    [ObservableProperty]
    private string _statusText = "未连接";

    [ObservableProperty]
    private string _mediaStatusText = "媒体引擎：初始化中…";

    [ObservableProperty]
    private string _connectionStatusText = string.Empty;

    [ObservableProperty]
    private bool _isConnectionReconnecting;

    [ObservableProperty]
    private string _footerText = string.Empty;

    /// <summary>分步注册流程（P1b）。真实邀请预检 + 真实注册，不再是模拟。</summary>
    public RegistrationViewModel Registration { get; }

    // ==================================================================
    // 第一屏：我的服务器（服务器列表 + 添加服务器）
    //
    // 设计意图：登录不再是「填一串地址 + 账号」，而是「先选一台服务器，再在这台服务器上登录」。
    // 添加服务器时把用户粘进来的东西（地址 / 邀请链接 / 裸邀请码）统一识别，
    // 再按服务端真实状态分流：未初始化 → owner 创建向导；带邀请 → 直接注册；否则 → 登录。
    // ==================================================================

    private readonly ServerBook _serverBook = ServerBook.Instance;
    private Uri? _bootstrapBaseUri;

    /// <summary>已知服务器（最近使用倒序），第一屏列表的数据源。</summary>
    public ObservableCollection<KnownServer> Servers { get; } = new();

    [ObservableProperty]
    private string _addServerInput = string.Empty;

    [ObservableProperty]
    private string _addServerError = string.Empty;

    [ObservableProperty]
    private bool _isAddingServer;

    /// <summary>「记住我」默认开启（用户要求）：成功后把 token 交给 DPAPI 保险箱。</summary>
    [ObservableProperty]
    private bool _rememberMe = true;

    /// <summary>当前选中的服务器；为 null 表示还停在第一屏。</summary>
    [ObservableProperty]
    private KnownServer? _activeServer;

    // ---- 未初始化服务器的 owner 创建向导 ----
    [ObservableProperty]
    private bool _isBootstrapping;

    [ObservableProperty]
    private string _bootstrapServerName = string.Empty;

    [ObservableProperty]
    private string _bootstrapToken = string.Empty;

    [ObservableProperty]
    private string _bootstrapUsername = string.Empty;

    [ObservableProperty]
    private string _bootstrapPassword = string.Empty;

    [ObservableProperty]
    private string _bootstrapError = string.Empty;

    public bool ShowServerPicker => !IsLoggedIn && !Registration.IsActive && !IsBootstrapping && ActiveServer is null;

    public bool ShowLogin => !IsLoggedIn && !Registration.IsActive && !IsBootstrapping && ActiveServer is not null;

    public bool ShowRegistration => !IsLoggedIn && Registration.IsActive;

    public bool ShowBootstrap => !IsLoggedIn && !Registration.IsActive && IsBootstrapping;

    public bool HasAddServerError => AddServerError.Length > 0;

    public bool HasBootstrapError => BootstrapError.Length > 0;

    public bool HasActiveServer => ActiveServer is not null;

    public bool HasServers => Servers.Count > 0;

    public string ActiveServerCaption => ActiveServer?.Name ?? string.Empty;

    public string ActiveServerUrl => ActiveServer?.Url ?? string.Empty;

    partial void OnAddServerErrorChanged(string value) => OnPropertyChanged(nameof(HasAddServerError));

    partial void OnBootstrapErrorChanged(string value) => OnPropertyChanged(nameof(HasBootstrapError));

    partial void OnActiveServerChanged(KnownServer? value)
    {
        NotifyStage();
        OnPropertyChanged(nameof(HasActiveServer));
        OnPropertyChanged(nameof(ActiveServerCaption));
        OnPropertyChanged(nameof(ActiveServerUrl));
    }

    partial void OnIsBootstrappingChanged(bool value) => NotifyStage();

    // ==================================================================
    // 服务器管理（owner/admin）。页面做成覆盖层而不是设置抽屉里的一屏：
    // 管理操作要能看着频道/成员列表做，而且它是"服务器级"的，不是本机偏好。
    // ==================================================================

    [ObservableProperty]
    private AdminViewModel? _admin;

    [ObservableProperty]
    private bool _showAdmin;

    /// <summary>当前账号是否有管理后台入口（owner 或 admin）。只是界面裁剪，授权在服务端。</summary>
    [ObservableProperty]
    private bool _hasServerAdminRights;

    partial void OnShowAdminChanged(bool value) => OnPropertyChanged(nameof(IsAdminOverlayVisible));

    public bool IsAdminOverlayVisible => ShowAdmin && Admin is not null;

    [RelayCommand]
    private async Task OpenAdminAsync()
    {
        if (_sessionBaseUri is null || _sessionToken is null || _self is null)
        {
            return;
        }
        if (!HasServerAdminRights)
        {
            StatusText = "需要管理员权限才能打开服务器管理。";
            return;
        }

        var admin = new AdminViewModel(_sessionBaseUri, _sessionToken,
            new AdminRoleSnapshot(IsAdmin: true, IsOwner: false), _self.Id);
        Admin = admin;
        ShowAdmin = true;
        OnPropertyChanged(nameof(IsAdminOverlayVisible));
        AppLog.Info("gui: admin panel opened");
        await admin.RefreshAsync();
    }

    [RelayCommand]
    private async Task CloseAdminAsync()
    {
        ShowAdmin = false;
        Admin = null;
        OnPropertyChanged(nameof(IsAdminOverlayVisible));
        AppLog.Info("gui: admin panel closed");

        // 面板里可能刚建/改名/删了频道：关掉后把左栏列表同步一次。
        // 只重建列表、不重新 browse 当前频道（同 id 时 OnBrowseChannelChanged 直接返回，不会闪）。
        if (_connection is { } connection)
        {
            try
            {
                await RefreshChannelListOnlyAsync(connection);
            }
            catch (Exception ex)
            {
                AppLog.Warn("gui: refresh channel list after admin failed: " + ex.Message);
            }
        }
    }

    /// <summary>
    /// 只刷新左栏频道列表，不切换当前浏览频道（管理面板建/改名/删频道后回填用）。
    /// 当前频道还在就保持选中；被删了就退到第一条。
    /// </summary>
    private async Task RefreshChannelListOnlyAsync(PartyUpConnection connection)
    {
        var list = await connection.ListChannelsAsync();
        var currentId = _browseChannelId;
        _channelsById.Clear();
        Channels.Clear();
        foreach (var channel in list.Channels)
        {
            _channelsById[channel.Id] = channel;
            Channels.Add(new ShellChannelItem(channel.Id, channel.Name));
        }
        BrowseChannel = Channels.FirstOrDefault(c => c.Id == currentId) ?? Channels.FirstOrDefault();
        OnPropertyChanged(nameof(HasBrowseChannel));
        OnPropertyChanged(nameof(BrowseChannelName));
        FooterText = $"已加载 {Channels.Count} 个频道";
        AppLog.Pass($"gui: channel list refreshed after admin ({Channels.Count} channels)");
    }

    private void RefreshServers()
    {
        Servers.Clear();
        foreach (var server in _serverBook.Servers)
        {
            Servers.Add(server);
        }
        OnPropertyChanged(nameof(HasServers));
    }

    /// <summary>
    /// 添加服务器：识别输入 → 探测 /api/server-info → 按真实状态分流。
    /// 注意：这里的 initialized 只决定「给哪个界面」，不构成任何授权；
    /// 真正的 bootstrap 授权在服务端（成员数为 0 + token + 连接可信性）。
    /// </summary>
    [RelayCommand]
    private async Task AddServerAsync()
    {
        if (IsAddingServer)
        {
            return;
        }
        AddServerError = string.Empty;
        if (!ServerTargetParser.TryParse(AddServerInput, out var target, out var parseError) || target is null)
        {
            AddServerError = parseError ?? "无法识别输入。";
            return;
        }

        var baseUri = target.BaseUri;
        if (baseUri is null)
        {
            // 裸邀请码不含服务器信息：用当前选中/上次使用的服务器补，猜不出来就明确要求地址。
            var fallbackUrl = ActiveServer?.Url ?? _serverBook.Selected?.Url ?? Servers.FirstOrDefault()?.Url;
            if (fallbackUrl is null || !Uri.TryCreate(fallbackUrl, UriKind.Absolute, out var fallback))
            {
                AddServerError = "只输入邀请码时，需要先有这台服务器的地址（或先在列表里选一台）。";
                return;
            }
            baseUri = fallback;
            target = target.WithBase(fallback);
        }

        IsAddingServer = true;
        try
        {
            using var api = new PartyUpApi(baseUri);
            var info = await api.GetServerInfoAsync();
            AppLog.Info($"gui: server-info name='{info.Name}' initialized={info.Initialized} " +
                        $"registration={info.Registration} kind={target.Kind}");

            var entry = _serverBook.Upsert(baseUri, info.Name, ActiveServer?.LastUser);
            RefreshServers();
            AddServerInput = string.Empty;

            if (!info.Initialized)
            {
                EnterBootstrap(baseUri, entry, info);
                return;
            }

            ActiveServer = entry;
            ServerUrl = entry.Url;

            if (target.IsJoinFlow && !string.IsNullOrWhiteSpace(target.InviteCode))
            {
                // 带邀请 → 直接进注册流程（注册流程自己解析链接、自己做非消费预检）。
                // 必须传**完整邀请链接**而不是裸码：注册第一步要求"服务器地址或邀请链接"，
                // 裸码会被判成非法地址（真机 UIA 抓到：点下一步报 URL 格式错误）。
                var invite = baseUri.ToString().TrimEnd('/');
                Registration.Begin($"{invite}/?code={Uri.EscapeDataString(target.InviteCode)}");
                return;
            }

            Username = entry.LastUser ?? string.Empty;
            Password = string.Empty;
            StatusText = "请登录";
        }
        catch (PartyUpApiException ex)
        {
            AddServerError = ex.Code switch
            {
                "NOT_FOUND" => "这个地址上没有 PartyUp 服务端（检查端口或反代路径）。",
                _ => $"服务器拒绝了请求：{ex.Message}",
            };
            AppLog.Warn($"gui: server-info rejected code={ex.Code} status={ex.Status}");
        }
        catch (Exception ex)
        {
            AddServerError = "连不上这台服务器：" + ex.Message;
            AppLog.Warn("gui: server-info failed: " + ex.Message);
        }
        finally
        {
            IsAddingServer = false;
        }
    }

    /// <summary>点列表里的一台服务器：直接用它登录（账号从上次记录预填）。</summary>
    [RelayCommand]
    private void UseServer(KnownServer? server)
    {
        if (server is null)
        {
            return;
        }
        _serverBook.Select(server.Url);
        ActiveServer = server;
        ServerUrl = server.Url;
        Username = server.LastUser ?? string.Empty;
        Password = string.Empty;
        LoginError = string.Empty;
        StatusText = "请登录";
    }

    /// <summary>从列表里移除（只删本机记录，不碰服务端任何数据）。</summary>
    [RelayCommand]
    private void RemoveServer(KnownServer? server)
    {
        if (server is null)
        {
            return;
        }
        _serverBook.Remove(server.Url);
        RefreshServers();
        if (ActiveServer?.Url == server.Url)
        {
            ActiveServer = null;
        }
    }

    /// <summary>回到第一屏（换服务器）。</summary>
    [RelayCommand]
    private void BackToServerList()
    {
        ActiveServer = null;
        IsBootstrapping = false;
        LoginError = string.Empty;
        BootstrapError = string.Empty;
    }

    private void EnterBootstrap(Uri baseUri, KnownServer entry, ServerPublicInfoDto info)
    {
        _bootstrapBaseUri = baseUri;
        ActiveServer = entry;
        ServerUrl = entry.Url;
        IsBootstrapping = true;
        BootstrapServerName = info.Name;
        BootstrapToken = string.Empty;
        BootstrapUsername = string.Empty;
        BootstrapPassword = string.Empty;
        BootstrapError = string.Empty;
        AppLog.Info($"gui: entering owner bootstrap wizard for '{info.Name}'");
    }

    /// <summary>
    /// 未初始化服务器：用服务端终端打印的一次性 token 创建 owner。
    /// 客户端这里只做输入校验；token 是否有效、连接是否可信由服务端裁决。
    /// </summary>
    [RelayCommand]
    private async Task CompleteBootstrapAsync()
    {
        BootstrapError = string.Empty;
        var baseUri = _bootstrapBaseUri;
        if (baseUri is null)
        {
            BootstrapError = "服务器地址丢失，请返回列表重试。";
            return;
        }
        if (string.IsNullOrWhiteSpace(BootstrapToken))
        {
            BootstrapError = "请填入服务端启动时打印的 bootstrap token。";
            return;
        }
        if (string.IsNullOrWhiteSpace(BootstrapUsername))
        {
            BootstrapError = "请填写管理员用户名。";
            return;
        }
        if (BootstrapPassword.Length < 8)
        {
            BootstrapError = "密码至少 8 位。";
            return;
        }

        try
        {
            using var api = new PartyUpApi(baseUri);
            var session = await api.BootstrapAsync(
                BootstrapToken.Trim(), BootstrapUsername.Trim(), BootstrapPassword);
            if (!await AttachSessionAsync(baseUri, session.Token, session.Member))
            {
                BootstrapError = "服务器已创建，但建立实时连接失败，请重试用登录进入。";
                return;
            }

            _serverBook.RememberUser(baseUri, BootstrapUsername.Trim());
            RefreshServers();
            var normalized = ServerEndpoint.NormalizeBase(baseUri).ToString().TrimEnd('/');
            ActiveServer = Servers.FirstOrDefault(s => s.Url == normalized) ?? ActiveServer;
            if (RememberMe)
            {
                TokenVault.TrySave(baseUri, BootstrapUsername.Trim(), session.Token, out _);
            }
            BootstrapToken = string.Empty;
            BootstrapPassword = string.Empty;
            IsBootstrapping = false;
            AppLog.Info("gui: owner bootstrap completed");
        }
        catch (PartyUpApiException ex)
        {
            BootstrapError = ex.Code switch
            {
                "ALREADY_BOOTSTRAPPED" => "这台服务器已经初始化过了，请改用登录。",
                "OWNER_ONLY" or "ADMIN_ONLY" => "当前身份无权执行初始化。",
                "UNAUTHENTICATED" => "bootstrap token 不正确或已过期（token 只用一次，24 小时有效）。",
                "NOT_BOOTSTRAPPED" => "服务端还没准备好初始化，请重启服务端并取新的 token。",
                _ => $"初始化失败：{ex.Message}",
            };
            AppLog.Warn($"gui: bootstrap rejected code={ex.Code} status={ex.Status}");
        }
        catch (Exception ex)
        {
            BootstrapError = "初始化失败（网络）：" + ex.Message;
            AppLog.Warn("gui: bootstrap failed: " + ex.Message);
        }
    }

    /// <summary>登录失败原因是否可见（避免在 XAML 里写字符串转换器）。</summary>
    public bool HasLoginError => LoginError.Length > 0;

    partial void OnLoginErrorChanged(string value) => OnPropertyChanged(nameof(HasLoginError));

    /// <summary>聊天区是否有消息（空态提示用）。</summary>
    public bool HasMessages => Messages.Count > 0;

    /// <summary>语音/共享路径历史是否非空（诊断区空态用）。</summary>
    public bool HasPathHistory => PathHistoryLines.Count > 0;

    /// <summary>底部通话条的一行模式说明（语音/共享各自独立的服务端策略）。</summary>
    public string ModeLine => VoiceModeText + " · " + ScreenModeText;

    partial void OnIsLoggedInChanged(bool value)
    {
        NotifyStage();
        // CanUseMedia 由 IsLoggedIn 派生：编译绑定不会跟着 IsLoggedIn 自动重算，
        // 不显式通知就会让「加入语音/共享屏幕」在登录后仍然保持禁用（本轮真机测试抓到）。
        OnPropertyChanged(nameof(CanUseMedia));
        NotifyMediaActionsChanged();
    }

    partial void OnServerNameChanged(string value) => OnPropertyChanged(nameof(ServerCaption));

    public string ServerCaption =>
        string.IsNullOrWhiteSpace(ServerName) ? "尚未连接服务器" : ServerName;

    /// <summary>服务端版本（auth.ok 的 server.version）；未知时为空串。</summary>
    [ObservableProperty]
    private string _serverVersion = string.Empty;

    partial void OnServerVersionChanged(string value) => OnPropertyChanged(nameof(ServerVersionText));

    /// <summary>
    /// 设置页显示的服务端版本。版本不一致只提示、不阻断（协议版本才是硬约束）。
    /// </summary>
    public string ServerVersionText => ServerVersion.Length > 0
        ? L.Format("Update_ServerVersion", ServerVersion)
        : L.Get("Update_ServerVersionUnknown");

    private void NotifyStage()
    {
        OnPropertyChanged(nameof(ShowServerPicker));
        OnPropertyChanged(nameof(ShowLogin));
        OnPropertyChanged(nameof(ShowRegistration));
        OnPropertyChanged(nameof(ShowBootstrap));
        OnPropertyChanged(nameof(CanSend));
    }

    private void OnRegistrationCancelled()
    {
        OnPropertyChanged(nameof(ShowLogin));
        OnPropertyChanged(nameof(ShowRegistration));
    }

    private void OnRegistrationCompleted(RegistrationSession session)
    {
        // 注册成功后直接进入已登录界面；注册流程自己的 IsActive 由它自己复位。
        // 顺手把服务器与账号记进服务器簿，并按「记住我」把 token 交给 DPAPI 保险箱。
        _serverBook.RememberUser(session.BaseUri, session.Member.Name);
        RefreshServers();
        if (RememberMe && !TokenVault.TrySave(session.BaseUri, session.Member.Name, session.Token, out var vaultError))
        {
            AppLog.Warn("gui: remember-me save failed (registration): " + vaultError);
        }
        _ = AttachSessionAsync(session.BaseUri, session.Token, session.Member);
    }

    [RelayCommand]
    private Task LoginAsync() => LoginCoreAsync();

    [RelayCommand]
    private void BeginRegistration() => Registration.Begin(null);

    [RelayCommand]
    private Task LogoutAsync() => EndSessionAsync(
        statusText: "已退出登录",
        footerText: "已断开连接并释放通话；可以重新登录。",
        notice: string.Empty,
        callApiLogout: true,
        backToServerList: true);

    /// <summary>
    /// 统一的「结束本会话」路径：用户主动退出 + 被服务端终止（顶号/踢/封禁）共用。
    /// 差别只在：主动退出要先调 <c>POST /api/logout</c>（否则服务端会话还活着，
    /// "退出"就成了假的）；被终止时 token 已被撤销，再调只会 401，所以跳过。
    /// </summary>
    private async Task EndSessionAsync(
        string statusText, string footerText, string notice, bool callApiLogout, bool backToServerList)
    {
        _loggingOut = true;
        try
        {
            // 服务端撤销会话：失败不阻塞本地清理（本地状态必须清干净，否则界面假在线）。
            if (callApiLogout && _sessionBaseUri is { } logoutUri && _sessionToken is { } logoutToken)
            {
                try
                {
                    using var api = new PartyUpApi(logoutUri);
                    await api.LogoutAsync(logoutToken);
                    AppLog.Info("gui: logout revoked on server");
                }
                catch (Exception ex)
                {
                    AppLog.Warn("gui: logout call failed (continuing local cleanup): " + ex.Message);
                }
            }
            // 退出前停掉独立输入采集：原生在语音占用时会拒绝 monitor_start，但退出登录后
            // 残留的采集没有意义，也会让下次打开设置页看到一个"正在采集"的假状态。
            StopMicTest();
            await TearDownCallAsync(logout: true);
            if (_connection is { } connection)
            {
                _connection = null;
                await connection.DisposeAsync();
            }
            _channelsById.Clear();
            _subscriptions.Clear();
            _voiceByChannel.Clear();
            _screenByChannel.Clear();
            _roster = Array.Empty<MemberRosterEntryDto>();
            Channels.Clear();
            CurrentMembers.Clear();
            OtherOnlineMembers.Clear();
            OfflineMembers.Clear();
            Messages.Clear();
            _messageIds.Clear();
            _messageIdOrder.Clear();
            _drafts.Clear();
            _browseChannelId = null;
            BrowseChannel = null;
            _pendingTransferChannelId = null;
            _previousCallChannelId = null;
            _watchingMemberId = null;
            _browseGeneration++;
            _callGeneration++;
            _loginGeneration++;
            _attachGeneration++;
            // 退出登录是终态转换：忙态必须在这里复位，否则登录页的按钮会被永久禁用。
            _busy = false;
            IsLoggingIn = false;
            IsCallJoining = false;
            IsTransferPending = false;
            PendingTransferText = string.Empty;
            HasTransferFailure = false;
            TransferFailureText = string.Empty;
            ShareStatusText = "共享：未开始";
            PathHistoryLines.Clear();
            ShowSettings = false;
            IsChatVisible = true;
            IsLoggedIn = false;
            IsConnectionReconnecting = false;
            ConnectionStatusText = string.Empty;
            ServerName = string.Empty;
            ServerVersion = string.Empty;
            SelfDisplayName = string.Empty;
            SelfLoginName = string.Empty;
            StatusText = statusText;
            FooterText = footerText;
            SessionNotice = notice;
            Password = string.Empty;

            // 会话结束就作废「记住我」在这台服务器上的 token——否则"退出"之后重启又能免密进去，
            // 且服务端已经把它撤销了。服务器条目保留（下次还要用），只清凭据。
            if (_sessionBaseUri is { } forgottenUri)
            {
                TokenVault.Forget(forgottenUri, SelfLoginName.Length > 0 ? SelfLoginName : Username);
            }
            _sessionToken = null;
            _sessionBaseUri = null;
            HasServerAdminRights = false;
            ShowAdmin = false;
            Admin = null;
            RefreshServers();
            if (backToServerList)
            {
                ActiveServer = null;
            }
            IsBootstrapping = false;
            OnPropertyChanged(nameof(BrowseChannelName));
            OnPropertyChanged(nameof(HasBrowseChannel));
            RebuildMemberGroups();
            AppLog.Info("gui: logged out");
        }
        finally
        {
            _loggingOut = false;
        }
    }

    private async Task LoginCoreAsync()
    {
        if (_busy || IsLoggingIn)
        {
            return;
        }
        LoginError = string.Empty;
        if (!TryParseServerUri(ServerUrl, out var baseUri, out var addressError))
        {
            LoginError = addressError;
            return;
        }
        if (string.IsNullOrWhiteSpace(Username))
        {
            LoginError = "请输入用户名。";
            return;
        }
        if (Password.Length == 0)
        {
            LoginError = "请输入密码。";
            return;
        }

        var generation = ++_loginGeneration;
        _busy = true;
        IsLoggingIn = true;
        StatusText = "正在连接服务器…";
        try
        {
            using var api = new PartyUpApi(baseUri);
            var session = await api.LoginAsync(Username.Trim(), Password);
            if (generation != _loginGeneration)
            {
                return;
            }
            if (!await AttachSessionAsync(baseUri, session.Token, session.Member))
            {
                // 建会话失败：保留用户名/密码供直接重试，失败原因已在 LoginError。
                StatusText = "登录未完成";
                return;
            }
            if (generation != _loginGeneration)
            {
                return;
            }
            Password = string.Empty;
            LoginError = string.Empty;

            // 记住这台服务器 + 上次账号；「记住我」开启时把 token 交给 DPAPI 保险箱。
            var account = Username.Trim();
            _serverBook.RememberUser(baseUri, account);
            RefreshServers();
            var normalizedUrl = ServerEndpoint.NormalizeBase(baseUri).ToString().TrimEnd('/');
            ActiveServer = Servers.FirstOrDefault(s => s.Url == normalizedUrl) ?? ActiveServer;
            if (RememberMe)
            {
                if (!TokenVault.TrySave(baseUri, account, session.Token, out var vaultError))
                {
                    AppLog.Warn("gui: remember-me save failed: " + vaultError);
                }
            }
            else
            {
                TokenVault.Forget(baseUri, account);
            }
        }
        catch (PartyUpApiException ex)
        {
            LoginError = DescribeAuthError(ex);
            StatusText = "登录失败";
            AppLog.Fail($"gui: login rejected code={ex.Code} status={ex.Status}");
        }
        catch (Exception ex)
        {
            LoginError = "无法连接服务器：" + ex.Message;
            StatusText = "连接失败";
            AppLog.Fail("gui: login failed: " + ex.Message);
        }
        finally
        {
            // 忙态属于「本次登录尝试」：只有最新一次尝试负责复位；若期间被更晚的尝试取代，
            // 由后者复位，避免把新尝试的忙态提前清掉。
            if (generation == _loginGeneration)
            {
                _busy = false;
                IsLoggingIn = false;
            }
            if (generation == _loginGeneration)
            {
                NotifyStage();
            }
        }
    }

    /// <summary>把服务端错误码翻译成就近可读的登录失败原因。</summary>
    /// <summary>
    /// 把服务端 auth.err 的 code 翻成用户能看懂的一句话。
    /// 每加一个服务端会返回的 code 都要在这里补一条——真机验收里 "MEMBER_BANNED"
    /// 曾落进默认分支，界面显示成「登录失败：MEMBER_BANNED: this account is banned」。
    /// </summary>
    private static string DescribeAuthError(PartyUpApiException ex) => ex.Code switch
    {
        "UNAUTHENTICATED" => "用户名或密码不正确。",
        "INVALID_ARGUMENT" => "用户名或密码格式不符合服务端要求。",
        "NOT_FOUND" => "服务器地址不正确（服务端没有这个 API 路径）。",
        "MEMBER_BANNED" => "该账号已被服务器管理员封禁，无法登录；请联系管理员解封。",
        "SESSION_REPLACED" => "账号已在别处登录，这个会话已结束。",
        "SERVER_FULL" => "服务器成员已满，请联系管理员。",
        "REGISTRATION_CLOSED" => "这台服务器已关闭注册，需要邀请码。",
        "INVITE_INVALID" => "邀请码无效或已被使用。",
        "RATE_LIMITED" => "尝试过于频繁，请稍后再试。",
        _ => $"登录失败（{ex.Code}）：{ex.Message}",
    };

    /// <summary>
    /// 解析服务器地址。邀请链接形式（带 ?code=）在这里只取 base，不消费码：
    /// 登录页不需要邀请码，注册流程自己解析链接。
    /// </summary>
    private static bool TryParseServerUri(string raw, out Uri baseUri, out string error)
    {
        baseUri = null!;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(raw))
        {
            error = "请输入服务器地址，例如 http://127.0.0.1:8080。";
            return false;
        }
        if (!Uri.TryCreate(raw.Trim(), UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
        {
            error = "服务器地址必须是带 http:// 或 https:// 的完整地址。";
            return false;
        }
        if (string.IsNullOrEmpty(uri.Host) || !string.IsNullOrEmpty(uri.UserInfo))
        {
            error = "服务器地址缺少主机名，或不应包含用户名/密码。";
            return false;
        }
        baseUri = ServerEndpoint.NormalizeBase(uri);
        return true;
    }

    /// <summary>
    /// `--token` 自动登录：先 /api/me 校验 token 再建 WS。
    /// 用于无头/脚本取证；正常用户路径仍然是账号密码登录。
    /// </summary>
    public async Task AutoLoginWithTokenAsync(string token)
    {
        if (!TryParseServerUri(ServerUrl, out var baseUri, out var addressError))
        {
            LoginError = addressError;
            return;
        }
        var generation = ++_loginGeneration;
        try
        {
            using var api = new PartyUpApi(baseUri);
            var member = await api.MeAsync(token);
            if (generation != _loginGeneration)
            {
                return;
            }
            await AttachSessionAsync(baseUri, token, member);
        }
        catch (Exception ex)
        {
            LoginError = "自动登录失败：" + ex.Message;
            AppLog.Fail("gui: auto-login failed: " + ex.Message);
        }
    }

    /// <summary>
    /// 用已经拿到的 token 建立 WS 会话（登录成功、注册成功、--token 自动登录共用）。
    /// 返回 true 表示会话可用且频道已加载；false 表示失败（UI 已给出就近原因，调用方不要清密码）。
    /// </summary>
    public async Task<bool> AttachSessionAsync(Uri baseUri, string token, MemberDto member)
    {
        var generation = ++_attachGeneration;
        StatusText = "正在建立实时连接…";
        try
        {
            if (_connection is { } existing)
            {
                _connection = null;
                await existing.DisposeAsync();
            }

            var isolated = _media?.HasIsolatedReceiveSupport == true;
            var publish = _media?.HasDirectPublishSupport == true;
            var watch = _media?.HasScreenWatchSupport == true || isolated || publish;
            var connection = await PartyUpConnection.ConnectAsync(
                baseUri, token, watch, isolated, publish);

            if (generation != _attachGeneration)
            {
                await connection.DisposeAsync();
                return false;
            }

            connection.TextReceived += OnTextReceived;
            connection.PresenceChanged += OnPresenceChanged;
            connection.MembersStateChanged += OnMembersStateChanged;
            connection.VoiceStateChanged += OnVoiceStateChanged;
            connection.ScreenStateChanged += OnScreenStateChanged;
            connection.SessionTerminated += OnSessionTerminated;
            connection.Closed += OnConnectionClosed;
            connection.ConnectionLost += OnConnectionLost;
            connection.Reconnecting += OnReconnecting;
            connection.Reconnected += OnReconnected;
            _connection = connection;
            _baseUri = baseUri;
            _self = member;
            HasServerAdminRights = member.Admin;
            // 主动退出要撤销服务端会话；「记住我」要按服务器+账号落盘/清除。
            _sessionBaseUri = baseUri;
            _sessionToken = token;

            SelfDisplayName = member.EffectiveDisplayName;
            SelfLoginName = member.Name;
            // 服务端名可能为空（旧数据/未配置）：用地址兜底，不要显示"尚未连接服务器"。
            ServerName = connection.Server?.Name is { Length: > 0 } name ? name : baseUri.Authority;
            ServerVersion = connection.Server?.Version ?? string.Empty;
            StatusText = $"已连接：{SelfDisplayName}";
            FooterText = $"已连接 {baseUri}";

            // 会话对应的服务器必须进入服务器簿并设为当前：否则 --token 直连、
            // 或从注册流程进来的会话没有 ActiveServer，一旦被顶号/踢下线，
            // 界面会退回"选择服务器"而把具体原因丢掉（真机验收抓到过）。
            var known = _serverBook.Upsert(baseUri, ServerName, member.Name);
            RefreshServers();
            ActiveServer = Servers.FirstOrDefault(s => s.Url == known.Url) ?? ActiveServer;

            // 先加载频道/名册，全部成功后才把界面切到「已登录」；
            // 中途失败会走 catch 清理连接，避免出现"已登录但没有频道"的半连接状态。
            await RefreshChannelsAsync(connection);
            if (generation != _attachGeneration)
            {
                await connection.DisposeAsync();
                return false;
            }
            await RefreshRosterAsync(connection);
            if (generation != _attachGeneration)
            {
                await connection.DisposeAsync();
                return false;
            }

            IsLoggedIn = true;
            AppLog.Pass($"gui: connected as {SelfLoginName} ({SelfDisplayName}) @ {baseUri} " +
                        $"watch={watch} isolated={isolated} publish={publish}");

            // --voice-join：自动化验收要求登录后直接入会（旧 MainWindow 曾有该语义，
            // 新外壳此前漏接，导致脚本以为已入会但实际只在浏览）。失败不阻塞登录。
            if (Program.Options.VoiceAutoJoin && _browseChannelId is { } autoJoinChannelId)
            {
                AppLog.Info($"gui: --voice-join auto joining channel={autoJoinChannelId}");
                await JoinVoiceAsync(autoJoinChannelId);
            }
            return true;
        }
        catch (Exception ex)
        {
            // 半个会话不能留在界面上：清掉连接与「已登录」标志，让用户看到失败原因后重试。
            if (_connection is { } failed)
            {
                _connection = null;
                try { await failed.DisposeAsync(); } catch { }
            }
            _self = null;
            IsLoggedIn = false;
            StatusText = "连接失败：" + ex.Message;
            LoginError = "无法建立实时连接：" + ex.Message;
            AppLog.Fail("gui: attach session failed: " + ex.Message);
            return false;
        }
    }

    private async Task RefreshChannelsAsync(PartyUpConnection connection)
    {
        var list = await connection.ListChannelsAsync();
        _channelsById.Clear();
        Channels.Clear();
        foreach (var channel in list.Channels)
        {
            _channelsById[channel.Id] = channel;
            Channels.Add(new ShellChannelItem(channel.Id, channel.Name));
        }
        FooterText = $"已加载 {Channels.Count} 个频道";
        AppLog.Pass($"gui: channel list loaded ({Channels.Count} channels)");

        if (Channels.Count == 0)
        {
            // 空服务器：没有任何频道可浏览，界面明确提示而不是留白。
            OnPropertyChanged(nameof(BrowseChannelName));
            OnPropertyChanged(nameof(HasBrowseChannel));
            BrowseSummary = "这个服务器还没有频道。需要管理员先创建频道。";
            return;
        }

        var wanted = Program.Options.Channel;
        var target = Channels[0];
        if (Program.Options.ChannelSpecified && !string.IsNullOrWhiteSpace(wanted))
        {
            var match = Channels.FirstOrDefault(c =>
                string.Equals(c.Name, wanted, StringComparison.OrdinalIgnoreCase));
            if (match is not null)
            {
                target = match;
                AppLog.Pass($"gui: auto-select channel by --channel: {target.Name}");
            }
            else
            {
                AppLog.Warn($"gui: --channel {wanted} not found, fallback to {target.Name}");
            }
        }
        await BrowseChannelAsync(target, alsoJoinVoice: false);
    }

    private async Task RefreshRosterAsync(PartyUpConnection connection)
    {
        var roster = await connection.ListMembersAsync();
        _roster = roster.Members;
        RebuildMemberGroups();
        AppLog.Pass($"gui: roster loaded ({_roster.Count} members, " +
                    $"{_roster.Count(m => m.Online)} online)");
    }

    // ------------------------------------------------------------------
    // 频道浏览（只影响文字阅读，不影响通话）
    // ------------------------------------------------------------------

    public ObservableCollection<ShellChannelItem> Channels { get; } = new();

    [ObservableProperty]
    private ShellChannelItem? _browseChannel;

    [ObservableProperty]
    private bool _channelPickerOpen = true;

    /// <summary>是否已选中一个浏览频道（决定标题/输入区是否可用）。</summary>
    public bool HasBrowseChannel => _browseChannelId is not null;

    public string BrowseChannelName =>
        _browseChannelId is { } id && _channelsById.TryGetValue(id, out var channel)
            ? "#" + channel.Name
            : "未选择频道";

    [ObservableProperty]
    private string _browseSummary = string.Empty;

    /// <summary>成员面板可见性：频道列表与成员列表共用左栏（CR-01）。</summary>
    public bool ShowMembersPanel => !ChannelPickerOpen;

    partial void OnChannelPickerOpenChanged(bool value) => OnPropertyChanged(nameof(ShowMembersPanel));

    partial void OnBrowseChannelChanged(ShellChannelItem? value)
    {
        if (value is null || value.Id == _browseChannelId)
        {
            return;
        }
        _ = BrowseChannelAsync(value, alsoJoinVoice: false);
    }

    /// <summary>左栏顶部按钮：在频道列表与成员视图之间切换（标签随状态变，行为不变）。</summary>
    [RelayCommand]
    private void ChooseChannel() => ChannelPickerOpen = !ChannelPickerOpen;

    [RelayCommand]
    private void ShowMembers() => ChannelPickerOpen = false;

    /// <summary>
    /// 浏览一个频道：只做文字订阅/历史/草稿。**绝不**触碰通话会话（这是与旧 OpenChannelAsync
    /// 最关键的区别）。双击入口会再显式调用 <see cref="JoinVoiceAsync"/>。
    /// </summary>
    public async Task BrowseChannelAsync(ShellChannelItem item, bool alsoJoinVoice)
    {
        if (_connection is not { } connection)
        {
            return;
        }
        var previousId = _browseChannelId;
        var generation = ++_browseGeneration;
        if (previousId is not null)
        {
            _drafts[previousId] = MessageInput;
        }

        try
        {
            var subscribedHere = false;
            if (!_subscriptions.Contains(item.Id))
            {
                await connection.SubscribeAsync(item.Id);
                _subscriptions.Add(item.Id);
                subscribedHere = true;
            }
            if (generation != _browseGeneration)
            {
                // 迟到的订阅请求：回滚，避免服务端/本地订阅集合留下没人用的条目。
                if (subscribedHere && item.Id != _browseChannelId && item.Id != _callChannelId)
                {
                    await TryUnsubscribeAsync(connection, item.Id);
                }
                return;
            }

            _browseChannelId = item.Id;
            foreach (var channel in Channels)
            {
                channel.IsBrowsing = channel.Id == item.Id;
            }
            BrowseChannel = item;
            OnPropertyChanged(nameof(BrowseChannelName));
            OnPropertyChanged(nameof(HasBrowseChannel));

            // 旧浏览频道如果不是通话频道，退订以释放服务端订阅；通话频道必须保持订阅。
            if (previousId is not null && previousId != item.Id && previousId != _callChannelId)
            {
                await TryUnsubscribeAsync(connection, previousId);
            }

            // 草稿与消息随频道切换：该频道的草稿只在本次运行内保留。
            MessageInput = _drafts.TryGetValue(item.Id, out var draft) ? draft : string.Empty;
            Messages.Clear();
            _messageIds.Clear();
            _messageIdOrder.Clear();
            HasSendFailure = false;
            SendError = string.Empty;
            HasNewMessages = false;
            ChatAtEnd = true;

            var history = await connection.HistoryAsync(item.Id, 0, HistoryLimit);
            if (generation != _browseGeneration)
            {
                return;
            }
            foreach (var message in history)
            {
                AppendMessage(message);
            }
            UpdateBrowseSummary();
            AppLog.Pass($"gui: browsing #{item.Name} (history={history.Count})");
            // 单击频道即进入该频道：频道列表收起，左栏直接显示该频道的三组名册。
            ChannelPickerOpen = false;

            if (alsoJoinVoice)
            {
                await JoinVoiceAsync(item.Id);
            }
        }
        catch (Exception ex)
        {
            if (generation == _browseGeneration)
            {
                StatusText = "打开频道失败：" + ex.Message;
                AppLog.Fail("gui: browse channel failed: " + ex.Message);
            }
        }
    }

    private async Task TryUnsubscribeAsync(PartyUpConnection connection, string channelId)
    {
        if (!_subscriptions.Remove(channelId))
        {
            return;
        }
        try
        {
            await connection.UnsubscribeAsync(channelId);
        }
        catch (Exception ex)
        {
            AppLog.Warn($"gui: unsubscribe {channelId} failed: {ex.Message}");
        }
        _voiceByChannel.Remove(channelId);
        _screenByChannel.Remove(channelId);
    }

    private void UpdateBrowseSummary()
    {
        if (_browseChannelId is not { } id)
        {
            // 空服务器时这里不能被清成空串，否则「还没有频道」的提示会被名册刷新覆盖掉。
            BrowseSummary = Channels.Count == 0
                ? "这个服务器还没有频道。需要管理员先创建频道。"
                : string.Empty;
            return;
        }
        var channelName = _channelsById.TryGetValue(id, out var channel) ? channel.Name : id;
        var voiceCount = _voiceByChannel.TryGetValue(id, out var voice) ? voice.Members.Count : 0;
        var shareCount = _screenByChannel.TryGetValue(id, out var screen) ? screen.Shares.Count : 0;
        var callNote = _callChannelId == id ? " · 当前通话频道" : string.Empty;
        BrowseSummary = $"{channelName} · 语音 {voiceCount} 人 · 共享 {shareCount} 路{callNote}";
        OnPropertyChanged(nameof(BrowseChannelName));
        OnPropertyChanged(nameof(HasBrowseChannel));
    }

    // ------------------------------------------------------------------
    // 消息
    // ------------------------------------------------------------------

    public ObservableCollection<ShellMessageItem> Messages { get; } = new();

    [ObservableProperty]
    private string _messageInput = string.Empty;

    [ObservableProperty]
    private bool _hasSendFailure;

    [ObservableProperty]
    private string _sendError = string.Empty;

    [ObservableProperty]
    private bool _hasNewMessages;

    /// <summary>聊天区当前是否停在底部（由窗口的 ScrollViewer 回报）。</summary>
    public bool ChatAtEnd { get; private set; } = true;

    partial void OnMessageInputChanged(string value)
    {
        OnPropertyChanged(nameof(CanSend));
        if (_browseChannelId is { } id)
        {
            _drafts[id] = value;
        }
    }

    /// <summary>断线期间禁止发送新消息（仍可写草稿）；没有浏览频道也不可发。</summary>
    public bool CanSend =>
        IsLoggedIn && !IsConnectionReconnecting && _browseChannelId is not null && MessageInput.Trim().Length > 0;

    partial void OnIsConnectionReconnectingChanged(bool value)
    {
        OnPropertyChanged(nameof(CanSend));
        OnPropertyChanged(nameof(CanUseMedia));
        NotifyMediaActionsChanged();
    }

    /// <summary>窗口在滚动位置变化时回报是否停在底部。</summary>
    public void NotifyChatAtEnd(bool atEnd)
    {
        ChatAtEnd = atEnd;
        if (atEnd)
        {
            HasNewMessages = false;
        }
    }

    [RelayCommand]
    private async Task SendMessageAsync()
    {
        if (_connection is not { } connection || _browseChannelId is not { } channelId)
        {
            return;
        }
        var body = MessageInput.Trim();
        if (body.Length == 0)
        {
            return;
        }
        var generation = _browseGeneration;
        try
        {
            HasSendFailure = false;
            SendError = string.Empty;
            MessageInput = string.Empty;
            await connection.SendTextAsync(channelId, body);
        }
        catch (Exception ex)
        {
            if (generation == _browseGeneration)
            {
                // 发送失败保留原文（不清空输入框），供用户直接重试。
                MessageInput = body;
                HasSendFailure = true;
                SendError = "发送失败：" + ex.Message;
                AppLog.Fail("gui: send failed: " + ex.Message);
            }
        }
    }

    [RelayCommand]
    private void JumpToLatest()
    {
        HasNewMessages = false;
        ScrollToLatestRequested?.Invoke();
    }

    /// <summary>窗口订阅它来把聊天区滚到底部。</summary>
    public event Action? ScrollToLatestRequested;

    private void OnTextReceived(TextMessageDto message)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (message.ChannelId != _browseChannelId)
            {
                return;
            }
            AppendMessage(message);
            if (!ChatAtEnd)
            {
                HasNewMessages = true;
            }
        });
    }

    private void AppendMessage(TextMessageDto message)
    {
        // 历史 + 实时两条路径可能给同一条消息：按消息 id 去重，不用显示名当键。
        if (!_messageIds.Add(message.Id))
        {
            return;
        }
        var author = message.EffectiveAuthor;
        if (string.IsNullOrWhiteSpace(message.MemberDisplayName) &&
            string.IsNullOrWhiteSpace(message.MemberName))
        {
            author = message.MemberId.Length >= 6 ? message.MemberId[..6] : message.MemberId;
        }
        var time = DateTimeOffset.FromUnixTimeMilliseconds(message.Ts).ToLocalTime().ToString("HH:mm");
        var isSelf = _self is { } self && message.MemberId == self.Id;
        Messages.Add(new ShellMessageItem(author, message.Body, time, isSelf));
        _messageIdOrder.Enqueue(message.Id);
        while (Messages.Count > MessageCap && _messageIdOrder.Count > 0)
        {
            Messages.RemoveAt(0);
            _messageIds.Remove(_messageIdOrder.Dequeue());
        }
    }

    // ------------------------------------------------------------------
    // 全服名册 → 三组（MEM-01/02）
    // ------------------------------------------------------------------

    public ObservableCollection<ShellMemberRow> CurrentMembers { get; } = new();

    public ObservableCollection<ShellMemberRow> OtherOnlineMembers { get; } = new();

    public ObservableCollection<ShellMemberRow> OfflineMembers { get; } = new();

    public string CurrentGroupTitle => $"频道中 · {CurrentMembers.Count}";

    public string OnlineGroupTitle => $"其他在线 · {OtherOnlineMembers.Count}";

    public string OfflineGroupTitle => $"离线 · {OfflineMembers.Count}";

    public bool HasCurrentMembers => CurrentMembers.Count > 0;

    public bool HasOtherOnlineMembers => OtherOnlineMembers.Count > 0;

    public bool HasOfflineMembers => OfflineMembers.Count > 0;

    private void OnMembersStateChanged(MemberListDto roster)
    {
        Dispatcher.UIThread.Post(() =>
        {
            _roster = roster.Members;
            RebuildMemberGroups();
        });
    }

    private void RebuildMemberGroups()
    {
        var browseId = _browseChannelId;
        var callId = _callChannelId;
        var browseVoice = browseId is not null && _voiceByChannel.TryGetValue(browseId, out var v) ? v : null;
        var browseShares = browseId is not null && _screenByChannel.TryGetValue(browseId, out var s) ? s : null;
        var callShares = callId is not null && _screenByChannel.TryGetValue(callId, out var cs) ? cs : null;

        CurrentMembers.Clear();
        OtherOnlineMembers.Clear();
        OfflineMembers.Clear();

        foreach (var entry in _roster)
        {
            var row = new ShellMemberRow(entry.Id, entry.EffectiveDisplayName, WatchCommand)
            {
                IsSelf = _self is { } self && entry.Id == self.Id,
                Admin = entry.Admin,
                IsOnline = entry.Online,
                VoiceChannelIds = entry.VoiceChannelIds,
            };
            row.IsInVoice = entry.Online && entry.VoiceChannelIds.Count > 0;
            row.VoiceChannelLabel = DescribeVoiceChannels(entry.VoiceChannelIds);
            row.MicrophoneEnabled = !row.IsInVoice || !IsMutedInVoice(entry.Id, browseId, callId);
            row.IsMuted = IsMutedInVoice(entry.Id, browseId, callId);
            row.IsDeafened = IsDeafenedInVoice(entry.Id, browseId, callId);
            row.IsSpeaking = _call is { } call && call.IsSpeaking(entry.Id);
            var share = FindShare(browseShares, entry.Id) ?? FindShare(callShares, entry.Id);
            row.IsSharing = share is not null;
            row.SharingTitle = share?.Source.Title ?? string.Empty;
            row.ShowWatchButton = callShares is not null && entry.Id != _self?.Id &&
                                  FindShare(callShares, entry.Id) is not null &&
                                  callId is not null && entry.VoiceChannelIds.Contains(callId);
            row.CanWatch = row.ShowWatchButton && _call is { IsInVoice: true } && CanWatchShares;

            var inBrowseVoice = browseId is not null && entry.VoiceChannelIds.Contains(browseId);
            if (!entry.Online)
            {
                OfflineMembers.Add(row);
            }
            else if (inBrowseVoice)
            {
                CurrentMembers.Add(row);
            }
            else
            {
                OtherOnlineMembers.Add(row);
            }
        }

        OnPropertyChanged(nameof(CurrentGroupTitle));
        OnPropertyChanged(nameof(OnlineGroupTitle));
        OnPropertyChanged(nameof(OfflineGroupTitle));
        OnPropertyChanged(nameof(HasCurrentMembers));
        OnPropertyChanged(nameof(HasOtherOnlineMembers));
        OnPropertyChanged(nameof(HasOfflineMembers));
        UpdateBrowseSummary();
    }

    private string DescribeVoiceChannels(IReadOnlyList<string> channelIds)
    {
        if (channelIds.Count == 0)
        {
            return string.Empty;
        }
        var names = channelIds
            .Select(id => _channelsById.TryGetValue(id, out var channel) ? channel.Name : id)
            .ToArray();
        return string.Join("、", names);
    }

    private bool IsMutedInVoice(string memberId, string? browseId, string? callId)
    {
        if (callId is { } cid && _voiceByChannel.TryGetValue(cid, out var callVoice))
        {
            var state = callVoice.Members.FirstOrDefault(m => m.MemberId == memberId);
            if (state is not null)
            {
                return state.Muted;
            }
        }
        if (browseId is { } bid && _voiceByChannel.TryGetValue(bid, out var browseVoice))
        {
            var state = browseVoice.Members.FirstOrDefault(m => m.MemberId == memberId);
            if (state is not null)
            {
                return state.Muted;
            }
        }
        return false;
    }

    private bool IsDeafenedInVoice(string memberId, string? browseId, string? callId)
    {
        if (callId is { } cid && _voiceByChannel.TryGetValue(cid, out var callVoice))
        {
            var state = callVoice.Members.FirstOrDefault(m => m.MemberId == memberId);
            if (state is not null)
            {
                return state.Deafened;
            }
        }
        if (browseId is { } bid && _voiceByChannel.TryGetValue(bid, out var browseVoice))
        {
            var state = browseVoice.Members.FirstOrDefault(m => m.MemberId == memberId);
            if (state is not null)
            {
                return state.Deafened;
            }
        }
        return false;
    }

    private static ShareDto? FindShare(ScreenStateDto? state, string memberId) =>
        state?.Shares.FirstOrDefault(s => s.SharerId == memberId);

    private void OnPresenceChanged(PresenceDto presence)
    {
        // presence 是「订阅该频道的连接」快照，不是语音归属；名册分组只认真实
        // member.list/voice.state，因此这里只留诊断日志，不缓存成第二份状态。
        Dispatcher.UIThread.Post(() =>
        {
            if (presence.ChannelId == _browseChannelId)
            {
                AppLog.Info($"gui: presence {presence.Members.Count} member(s) in #{presence.ChannelId}");
            }
        });
    }

    private void OnVoiceStateChanged(VoiceStateDto state)
    {
        // 事件来自 WS 接收线程：快照写入与读侧（名册/摘要）都在 UI 线程，避免 Dictionary 竞态。
        Dispatcher.UIThread.Post(() =>
        {
            _voiceByChannel[state.ChannelId] = state;
            if (state.ChannelId == _browseChannelId || state.ChannelId == _callChannelId)
            {
                RebuildMemberGroups();
                SyncCallState();
            }
        });
    }

    private void OnScreenStateChanged(ScreenStateDto state)
    {
        Dispatcher.UIThread.Post(() =>
        {
            _screenByChannel[state.ChannelId] = state;
            if (state.ChannelId == _browseChannelId || state.ChannelId == _callChannelId)
            {
                RebuildMemberGroups();
                SyncShares();
            }
        });
    }

    private void OnConnectionClosed(string reason)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (_loggingOut)
            {
                return;
            }
            // 连接终止（不是自动重连中）：本地通话/媒体状态必须一起降级，
            // 否则会出现「已回登录页但仍在通话/共享」的假状态。
            var call = _call;
            _call = null;
            _callChannelId = null;
            _watchingMemberId = null;
            MarkCallChannel(null);
            IsInVoice = false;
            IsSharing = false;
            IsWatching = false;
            IsWaitingForFrame = false;
            _remoteScreen?.Clear();
            if (call is not null)
            {
                call.Changed -= OnCallChanged;
                _ = call.DisposeAsync();   // 尽力收尾；连接已断，失败不影响回到登录页
            }
            _subscriptions.Clear();
            _voiceByChannel.Clear();
            _screenByChannel.Clear();
            _self = null;
            SyncShares();
            // 回到登录界面并保留可读原因，用户可以重试。
            IsConnectionReconnecting = false;
            ConnectionStatusText = string.Empty;
            if (SessionNotice.Length > 0)
            {
                // 会话已被服务端终止（顶号/踢/封禁）：保留具体原因，不用通用断线文案覆盖。
                StatusText = "会话已结束";
            }
            else
            {
                StatusText = "连接已断开：" + reason;
                LoginError = "连接已断开：" + reason;
            }
            IsLoggedIn = false;
            NotifyStage();
            AppLog.Warn("gui: connection closed: " + reason);
        });
    }

    /// <summary>
    /// 服务端通知会话已结束（单端登录 / 被踢 / 被封禁 / 主动退出）。这是**终态**：
    /// 连接层已停止重连，这里把界面切回登录页并给出**可区分**的原因。
    /// </summary>
    private void OnSessionTerminated(string reason)
    {
        var notice = reason switch
        {
            "login_elsewhere" => "你的账号在另一台设备上登录了，这里的会话已结束。",
            "kicked" => "你已被管理员移出这台服务器。",
            "banned" => "你已被管理员封禁，无法再登录这台服务器。",
            "logout" => "已退出登录。",
            _ => $"会话已结束（{reason}）。",
        };
        AppLog.Warn($"gui: session terminated reason={reason}");
        Dispatcher.UIThread.Post(() => _ = EndSessionAsync(
            statusText: "会话已结束",
            footerText: "连接已断开；请重新登录。",
            notice: notice,
            callApiLogout: false,        // token 已被服务端撤销，再调只会 401
            backToServerList: false));   // 留在登录页，让用户看到原因
    }

    /// <summary>
    /// 启动时按「记住我」自动登录：只在有上次服务器 + 账号、且能解出 token 时尝试。
    /// token 失效（换机器/被撤销）就清掉并正常显示登录页，不拿死 token 反复重试或弹错。
    /// </summary>
    public async Task TryRestoreRememberedSessionAsync()
    {
        var server = ActiveServer;
        if (server is null || !Uri.TryCreate(server.Url, UriKind.Absolute, out var baseUri))
        {
            return;
        }
        var account = server.LastUser;
        if (string.IsNullOrWhiteSpace(account))
        {
            return;
        }

        var token = TokenVault.TryLoad(baseUri, account, out var vaultError);
        if (token is null)
        {
            if (!string.IsNullOrEmpty(vaultError))
            {
                AppLog.Info("gui: no remembered token: " + vaultError);
            }
            return;
        }

        try
        {
            using var api = new PartyUpApi(baseUri);
            var member = await api.MeAsync(token);
            Username = account;
            if (await AttachSessionAsync(baseUri, token, member))
            {
                AppLog.Info($"gui: auto-login with remembered token as {account}");
            }
            else
            {
                TokenVault.Forget(baseUri, account);
            }
        }
        catch (PartyUpApiException ex)
        {
            AppLog.Warn($"gui: remembered token rejected code={ex.Code}");
            TokenVault.Forget(baseUri, account);
        }
        catch (Exception ex)
        {
            AppLog.Warn("gui: remembered auto-login failed: " + ex.Message);
        }
    }

    private void OnConnectionLost(string reason)
    {
        Dispatcher.UIThread.Post(() =>
        {
            IsConnectionReconnecting = true;
            ConnectionStatusText = "连接已断开：正在重连…";
            StatusText = ConnectionStatusText;
            FooterText = "断线期间可继续写草稿；恢复后不会自动发送草稿。";
            AppLog.Warn("gui: connection lost: " + reason);
        });
    }

    private void OnReconnecting(int attempt, int delayMs)
    {
        Dispatcher.UIThread.Post(() =>
        {
            IsConnectionReconnecting = true;
            ConnectionStatusText = $"重连中…（第 {attempt} 次，{delayMs}ms 后重试）";
            StatusText = ConnectionStatusText;
        });
    }

    private void OnReconnected()
    {
        Dispatcher.UIThread.Post(async () =>
        {
            IsConnectionReconnecting = false;
            ConnectionStatusText = string.Empty;
            StatusText = "已重新连接";
            AppLog.Pass("gui: reconnected");
            if (_connection is not { } connection)
            {
                return;
            }
            // 连接层已重放频道订阅；名册与当前通话的语音协商需要重新对齐。
            try
            {
                await RefreshRosterAsync(connection);
                if (_call is { } call)
                {
                    // 重连后接收 lease 已随旧连接失效：ShareSession 会清空观看本地状态，
                    // 这里按用户原本的目标重看一次；失败则明确告知需要重新点「观看」。
                    var wantedWatch = _watchingMemberId;
                    await call.HandleConnectionRestoredAsync();
                    SyncCallState();
                    if (wantedWatch is not null)
                    {
                        try
                        {
                            await call.WatchAsync(wantedWatch);
                            call.ClearLastError();
                            AppLog.Pass($"gui: watch restored after reconnect: {wantedWatch}");
                        }
                        catch (Exception ex)
                        {
                            _watchingMemberId = null;
                            ShareStatusText = "重连后观看已停止，需要重新点「观看」：" + ex.Message;
                            AppLog.Warn("gui: restore watch failed: " + ex.Message);
                        }
                    }
                }
                SyncShares();
            }
            catch (Exception ex)
            {
                AppLog.Warn("gui: post-reconnect refresh failed: " + ex.Message);
            }
        });
    }

    // ------------------------------------------------------------------
    // 通话（语音 + 共享）：浏览频道与通话频道彻底解耦
    // ------------------------------------------------------------------

    [ObservableProperty]
    private bool _isInVoice;

    [ObservableProperty]
    private bool _isVoiceMuted;

    [ObservableProperty]
    private bool _isVoiceDeafened;

    [ObservableProperty]
    private bool _isCallJoining;

    [ObservableProperty]
    private string _callStatusText = "未加入频道语音";

    [ObservableProperty]
    private string _voiceModeText = "语音传输：—";

    [ObservableProperty]
    private string _screenModeText = "共享传输：—";

    partial void OnVoiceModeTextChanged(string value) => OnPropertyChanged(nameof(ModeLine));

    partial void OnScreenModeTextChanged(string value) => OnPropertyChanged(nameof(ModeLine));

    [ObservableProperty]
    private bool _isTransferPending;

    [ObservableProperty]
    private string _pendingTransferText = string.Empty;

    [ObservableProperty]
    private bool _hasTransferFailure;

    [ObservableProperty]
    private string _transferFailureText = string.Empty;

    public string CallChannelName =>
        _callChannelId is { } id && _channelsById.TryGetValue(id, out var channel)
            ? "通话：" + channel.Name
            : "通话：未加入";

    public bool CanWatchShares =>
        _call is { IsInVoice: true } call &&
        (call.CanWatchStrictly || call.CanWatchExplicitly);

    /// <summary>加入/离开语音、静音等本地媒体动作在断线时是否可用（降低暴露的操作不受影响）。</summary>
    public bool CanLeaveCall => _call is { IsInVoice: true };

    partial void OnIsInVoiceChanged(bool value)
    {
        OnPropertyChanged(nameof(CanLeaveCall));
        NotifyMediaActionsChanged();
    }

    private void NotifyMediaActionsChanged()
    {
        OnPropertyChanged(nameof(CanWatchShares));
        RebuildMemberGroups();
    }

    /// <summary>底部通话条的「加入语音」：加入当前**浏览**频道的语音（Q9：显式操作才转移）。</summary>
    [RelayCommand]
    private Task JoinCallAsync() =>
        _browseChannelId is { } id ? JoinVoiceAsync(id) : Task.CompletedTask;

    /// <summary>
    /// 加入指定频道的语音。若当前在别的频道且正在共享，先进入确认态而不是直接切断
    /// （Q9/CR-02：共享中转移需提示会停止共享）。
    /// </summary>
    public async Task JoinVoiceAsync(string channelId)
    {
        if (_connection is not { } connection || _media is not { IsCreated: true })
        {
            CallStatusText = _media is { IsCreated: true }
                ? "请先登录"
                : "媒体引擎不可用，无法加入语音";
            return;
        }
        if (_callChannelId == channelId && _call is { IsInVoice: true })
        {
            return;
        }
        if (_call is { IsSharing: true } && _callChannelId != channelId)
        {
            _pendingTransferChannelId = channelId;
            IsTransferPending = true;
            PendingTransferText = $"转移到「{ChannelNameOf(channelId)}」语音会停止当前共享，确认？";
            return;
        }
        await TransferCallAsync(channelId);
    }

    [RelayCommand]
    private Task ConfirmTransferAsync()
    {
        var target = _pendingTransferChannelId;
        IsTransferPending = false;
        _pendingTransferChannelId = null;
        return target is null ? Task.CompletedTask : TransferCallAsync(target);
    }

    [RelayCommand]
    private void CancelTransfer()
    {
        IsTransferPending = false;
        _pendingTransferChannelId = null;
        PendingTransferText = string.Empty;
    }

    /// <summary>转移失败后的「返回原频道」：只恢复浏览，不暗中重入通话（Q20）。</summary>
    [RelayCommand]
    private async Task ReturnToPreviousChannelAsync()
    {
        HasTransferFailure = false;
        TransferFailureText = string.Empty;
        if (_previousCallChannelId is { } previous &&
            Channels.FirstOrDefault(c => c.Id == previous) is { } item)
        {
            await BrowseChannelAsync(item, alsoJoinVoice: false);
        }
        _previousCallChannelId = null;
    }

    private async Task TransferCallAsync(string channelId)
    {
        if (_connection is not { } connection || _media is not { IsCreated: true })
        {
            return;
        }
        var generation = ++_callGeneration;
        var previous = _callChannelId;
        // 语音会占用同一个 ADM：原生在语音占用时拒绝 monitor_start，所以入会前先停掉测试，
        // 让 UI 状态与库状态一致（否则设置页会留着一个"正在采集"的假状态）。
        StopMicTest();
        IsCallJoining = true;
        HasTransferFailure = false;
        TransferFailureText = string.Empty;
        CallStatusText = $"正在加入「{ChannelNameOf(channelId)}」语音…";
        try
        {
            await TearDownCallAsync(logout: false);

            if (!_subscriptions.Contains(channelId))
            {
                await connection.SubscribeAsync(channelId);
                _subscriptions.Add(channelId);
            }
            if (generation != _callGeneration)
            {
                return;
            }

            var share = new ShareSession(_media, connection, channelId,
                Program.Options.VoicePassive, Program.Options.VoiceActive);
            share.Changed += OnCallChanged;
            _call = share;
            _callChannelId = channelId;
            MarkCallChannel(channelId);

            await share.JoinVoiceAsync();
            if (generation != _callGeneration)
            {
                return;
            }
            if (!share.IsInVoice)
            {
                // 加入失败 = 落到「未加入通话」，不自动反复跳转（Q20）。
                var reason = share.LastError ?? "未知原因";
                await TearDownCallAsync(logout: false);
                _previousCallChannelId = previous;
                HasTransferFailure = true;
                TransferFailureText = previous is null
                    ? $"加入「{ChannelNameOf(channelId)}」语音失败：{reason}"
                    : $"转移到「{ChannelNameOf(channelId)}」失败，已回到未加入状态：{reason}";
                CallStatusText = "未加入频道语音";
                AppLog.Fail($"gui: join voice failed channel={channelId}: {reason}");
                return;
            }
            AppLog.Pass($"gui: voice joined channel={channelId} mode={share.VoiceMode}");
            SyncCallState();
            RebuildMemberGroups();
        }
        catch (Exception ex)
        {
            if (generation == _callGeneration)
            {
                await TearDownCallAsync(logout: false);
                _previousCallChannelId = previous;
                HasTransferFailure = true;
                TransferFailureText = "加入语音失败：" + ex.Message;
                CallStatusText = "未加入频道语音";
                AppLog.Fail("gui: transfer call failed: " + ex.Message);
            }
        }
        finally
        {
            if (generation == _callGeneration)
            {
                IsCallJoining = false;
            }
        }
    }

    private string ChannelNameOf(string channelId) =>
        _channelsById.TryGetValue(channelId, out var channel) ? channel.Name : channelId;

    private void MarkCallChannel(string? channelId)
    {
        foreach (var channel in Channels)
        {
            channel.IsCallChannel = channel.Id == channelId;
        }
        OnPropertyChanged(nameof(CallChannelName));
    }

    private async Task TearDownCallAsync(bool logout)
    {
        var previousCall = _call;
        _call = null;
        if (previousCall is not null)
        {
            previousCall.Changed -= OnCallChanged;
            // 离开/转移/注销都必须先停共享：只 LeaveVoice 不会关屏幕 PC、也不会发 screen.stop，
            // 否则本机采集继续、旧频道留 ghost share（CR-02 的确认文案必须兑现）。
            if (previousCall.IsSharing)
            {
                try
                {
                    await previousCall.StopSharingAsync(CancellationToken.None);
                    AppLog.Pass("gui: sharing stopped before leaving call");
                }
                catch (Exception ex)
                {
                    AppLog.Warn("gui: stop sharing before leave failed: " + ex.Message);
                }
            }
            try
            {
                if (previousCall.IsInVoice)
                {
                    await previousCall.LeaveVoiceAsync();
                }
            }
            catch (Exception ex)
            {
                AppLog.Warn("gui: leave voice failed: " + ex.Message);
            }
            await previousCall.DisposeAsync();
        }
        var previousId = _callChannelId;
        _callChannelId = null;
        _watchingMemberId = null;
        MarkCallChannel(null);
        IsInVoice = false;
        IsVoiceMuted = false;
        IsVoiceDeafened = false;
        IsSharing = false;
        IsWatching = false;
        IsWaitingForFrame = false;
        _remoteScreen?.Clear();
        SyncShares();
        if (!logout && previousId is not null && previousId != _browseChannelId &&
            _connection is { } connection)
        {
            await TryUnsubscribeAsync(connection, previousId);
        }
        if (!logout)
        {
            CallStatusText = "未加入频道语音";
            VoiceModeText = "语音传输：—";
            ScreenModeText = "共享传输：—";
        }
        RebuildMemberGroups();
    }

    [RelayCommand]
    private async Task LeaveCallAsync()
    {
        await TearDownCallAsync(logout: false);
        HasTransferFailure = false;
        AppLog.Info("gui: call left");
    }

    [RelayCommand]
    private void ToggleMute()
    {
        if (_call is not { } call || !call.IsInVoice)
        {
            return;
        }
        var rc = call.ToggleVoiceMute();
        if (rc != 0)
        {
            CallStatusText = $"麦克风静音失败 rc={rc}";
            return;
        }
        SyncCallState();
    }

    [RelayCommand]
    private void ToggleDeafen()
    {
        if (_call is not { } call || !call.IsInVoice)
        {
            return;
        }
        var rc = call.ToggleVoiceDeafen();
        if (rc != 0)
        {
            CallStatusText = $"扬声器切换失败 rc={rc}";
            return;
        }
        SyncCallState();
    }

    private void OnCallChanged() => Dispatcher.UIThread.Post(() =>
    {
        SyncCallState();
        RebuildMemberGroups();
        SyncShares();
    });

    /// <summary>把 ShareSession 的真实语音状态同步到可绑定属性（UI 线程）。</summary>
    private void SyncCallState()
    {
        if (_call is not { } call)
        {
            IsInVoice = false;
            CallStatusText = "未加入频道语音";
            VoiceModeText = "语音传输：—";
            ScreenModeText = "共享传输：—";
            return;
        }
        IsInVoice = call.IsInVoice;
        IsVoiceMuted = call.IsVoiceMuted;
        IsVoiceDeafened = call.IsVoiceDeafened;
        VoiceModeText = "语音传输：" + call.VoiceModeText + "（服务端策略）";
        ScreenModeText = "共享传输：" + call.ScreenModeText + "（服务端策略）";
        if (call.IsInVoice)
        {
            CallStatusText = $"通话中 · {ChannelNameOf(_callChannelId ?? string.Empty)}" +
                             $"（{call.VoiceMemberCount} 人在语音）" +
                             (call.IsVoiceMuted ? "，麦克风静音" : string.Empty) +
                             (call.IsVoiceDeafened ? "，扬声器关闭" : string.Empty);
        }
        else
        {
            CallStatusText = "未加入频道语音";
        }
        OnPropertyChanged(nameof(CanWatchShares));
        OnPropertyChanged(nameof(CanLeaveCall));
        OnPropertyChanged(nameof(CallChannelName));
    }

    // ------------------------------------------------------------------
    // 屏幕共享（服务端决定 transport；客户端只消费）
    // ------------------------------------------------------------------

    public ObservableCollection<MediaSource> Sources { get; } = new();

    [ObservableProperty]
    private MediaSource? _selectedSource;

    [ObservableProperty]
    private bool _isSharing;

    [ObservableProperty]
    private string _shareStatusText = "共享：未开始";

    partial void OnShareStatusTextChanged(string value) => OnPropertyChanged(nameof(HasShareProblem));

    /// <summary>媒体卡片默认只留图标；只有真的出问题时才在卡片里显示一行错误（失败不静默）。</summary>
    public bool HasShareProblem =>
        ShareStatusText.StartsWith("共享失败", StringComparison.Ordinal) ||
        ShareStatusText.StartsWith("共享错误", StringComparison.Ordinal) ||
        ShareStatusText.StartsWith("共享：请先", StringComparison.Ordinal) ||
        ShareStatusText.StartsWith("共享：没有", StringComparison.Ordinal) ||
        ShareStatusText.StartsWith("观看失败", StringComparison.Ordinal);

    [ObservableProperty]
    private bool _isWatching;

    [ObservableProperty]
    private bool _isWaitingForFrame;

    [ObservableProperty]
    private string _watchingTitle = "未在观看";

    [ObservableProperty]
    private string _remoteFpsText = "画面：等待远端帧";

    [ObservableProperty]
    private bool _isFullScreen;

    /// <summary>正在观看的成员 id（用于重连后恢复观看）。</summary>
    private string? _watchingMemberId;

    public bool ShowWatchStage => IsWatching || IsWaitingForFrame;

    /// <summary>断线期间暂停媒体操作（简报 §5）；发送由 <see cref="CanSend"/> 单独管。</summary>
    public bool CanUseMedia => IsLoggedIn && !IsConnectionReconnecting;

    partial void OnIsWatchingChanged(bool value)
    {
        OnPropertyChanged(nameof(ShowWatchStage));
        if (!value)
        {
            IsWaitingForFrame = false;
        }
    }

    partial void OnIsWaitingForFrameChanged(bool value) => OnPropertyChanged(nameof(ShowWatchStage));

    private void SyncShares()
    {
        if (_call is not { } call)
        {
            IsSharing = false;
            IsWatching = false;
            IsWaitingForFrame = false;
            WatchingTitle = "未在观看";
            ShareStatusText = "共享：未开始";
            return;
        }
        IsSharing = call.IsSharing;
        if (call.IsWatching)
        {
            IsWatching = true;
            IsWaitingForFrame = call.RemoteFrames == 0;
            WatchingTitle = $"正在观看：{call.WatchingName}";
            RemoteFpsText = call.RemoteWidth > 0
                ? $"画面：{call.RemoteWidth}x{call.RemoteHeight} · {call.RemoteFrames} 帧"
                : "画面：等待首帧…";
        }
        else if (IsWatching)
        {
            IsWatching = false;
            IsWaitingForFrame = false;
            WatchingTitle = "未在观看";
            RemoteFpsText = "画面：等待远端帧";
            _remoteScreen?.Clear();
        }
        ShareStatusText = call.IsSharing
            ? $"共享中：{call.SharingSourceTitle}"
            : "共享：未开始";
        // 成功路径会清 LastError；这里只展示真实失败，状态行在 XAML 里常显。
        if (call.LastError is { } error)
        {
            ShareStatusText = "共享错误：" + error;
        }
    }

    /// <summary>共享前设置弹窗是否打开（点共享图标 → 选源 → 开始共享）。</summary>
    [ObservableProperty]
    private bool _showShareSetup;

    [RelayCommand]
    private void OpenShareSetup()
    {
        if (!IsInVoice)
        {
            ShareStatusText = "共享：请先加入频道语音";
            return;
        }
        if (Sources.Count == 0)
        {
            ShareStatusText = "共享：没有可用的屏幕源（媒体核心未就绪）";
            return;
        }
        ShareStatusText = "共享：选择屏幕后点「开始共享」";
        ShowShareSetup = true;
    }

    [RelayCommand]
    private void CancelShareSetup() => ShowShareSetup = false;

    [RelayCommand]
    private async Task StartSharingAsync()
    {
        if (_call is not { } call || !call.IsInVoice)
        {
            ShowShareSetup = false;
            ShareStatusText = "共享：请先加入频道语音";
            return;
        }
        if (SelectedSource is null)
        {
            ShareStatusText = "共享：请选择要共享的屏幕";
            return;
        }
        try
        {
            ShareStatusText = $"共享：正在启动「{SelectedSource.Title}」…";
            await call.StartSharingAsync(SelectedSource);
            call.ClearLastError();
            SyncShares();
            RebuildMemberGroups();
            ShowShareSetup = false;
            AppLog.Pass($"gui: sharing started source={SelectedSource.Id}");
        }
        catch (Exception ex)
        {
            IsSharing = false;
            ShareStatusText = "共享失败：" + ex.Message;
            AppLog.Fail("gui: start sharing failed: " + ex.Message);
        }
    }

    [RelayCommand]
    private async Task StopSharingAsync()
    {
        if (_call is not { } call || !call.IsSharing)
        {
            return;
        }
        try
        {
            await call.StopSharingAsync();
            call.ClearLastError();
            SyncShares();
            RebuildMemberGroups();
            AppLog.Info("gui: sharing stopped");
        }
        catch (Exception ex)
        {
            ShareStatusText = "停止共享失败：" + ex.Message;
            AppLog.Fail("gui: stop sharing failed: " + ex.Message);
        }
    }

    /// <summary>
    /// 观看某成员的共享。只允许观看**当前通话频道**里的共享（Q19）；停止只停这一路接收，
    /// 不影响自己的发布/语音（由 ShareSession 的严格接收控制器保证）。
    /// </summary>
    [RelayCommand]
    private async Task WatchAsync(ShellMemberRow? row)
    {
        if (row is null || _call is not { } call)
        {
            return;
        }
        if (!call.IsInVoice)
        {
            ShareStatusText = "观看：请先加入该频道语音";
            return;
        }
        try
        {
            IsWaitingForFrame = true;
            WatchingTitle = $"正在观看：{row.DisplayName}";
            await call.WatchAsync(row.Id);
            _watchingMemberId = row.Id;
            call.ClearLastError();
            SyncShares();
            AppLog.Pass($"gui: watching {row.Id}");
        }
        catch (Exception ex)
        {
            _watchingMemberId = null;
            IsWatching = false;
            IsWaitingForFrame = false;
            WatchingTitle = "未在观看";
            ShareStatusText = "观看失败：" + ex.Message;
            AppLog.Fail("gui: watch failed: " + ex.Message);
        }
    }

    [RelayCommand]
    private async Task StopWatchingAsync()
    {
        if (_call is not { } call)
        {
            return;
        }
        _watchingMemberId = null;
        try
        {
            await call.StopWatchingAsync();
        }
        catch (Exception ex)
        {
            ShareStatusText = "停止观看失败：" + ex.Message;
            AppLog.Fail("gui: stop watching failed: " + ex.Message);
        }
        // 只有确实不再接收时才复位 UI：停止失败必须保留「仍在接收」的可见状态（M6）。
        if (call.IsWatching)
        {
            IsWatching = true;
            IsWaitingForFrame = false;
            WatchingTitle = $"仍在观看：{call.WatchingName}（停止未确认）";
            ShareStatusText = ShareStatusText.StartsWith("停止观看失败", StringComparison.Ordinal)
                ? ShareStatusText
                : "停止观看未确认：仍在接收该成员画面，请重试";
        }
        else
        {
            IsWatching = false;
            IsWaitingForFrame = false;
            WatchingTitle = "未在观看";
            _remoteScreen?.Clear();
            SyncShares();
        }
    }

    [RelayCommand]
    private void ToggleFullScreen() => IsFullScreen = !IsFullScreen;

    [RelayCommand]
    private void ToggleChatVisible() => IsChatVisible = !IsChatVisible;

    [ObservableProperty]
    private bool _isChatVisible = true;

    // ------------------------------------------------------------------
    // 媒体引擎
    // ------------------------------------------------------------------

    private void TryInitMedia()
    {
        try
        {
            _media = MediaEngine.Load();
            if (_media.MissingExports.Count > 0)
            {
                MediaStatusText = $"媒体引擎：缺 {_media.MissingExports.Count}/{MediaEngine.KnownExportCount} 个导出";
                AppLog.Fail($"media: missing exports: {string.Join(", ", _media.MissingExports)}");
                return;
            }
            _media.Create("partyup-shell");
            // 这是「客户端期望使用到的导出」命中数，不是 DLL 导出总数；不要读成 ABI 全部能力就绪。
            MediaStatusText =
                $"媒体引擎：已就绪（客户端所需导出 {_media.PresentExportCount}/{MediaEngine.KnownExportCount} 可用）";
            AppLog.Pass($"media: pu_engine_create ok, dll={_media.LibraryPath}, " +
                        $"exports={_media.PresentExportCount}/{MediaEngine.KnownExportCount}");
            LoadSources();
        }
        catch (Exception ex)
        {
            MediaStatusText = "媒体引擎不可用：" + ex.Message;
            AppLog.Warn("media: " + ex.Message);
        }
    }

    private void LoadSources()
    {
        if (_media is not { IsCreated: true })
        {
            return;
        }
        try
        {
            Sources.Clear();
            foreach (var source in _media.EnumerateSources())
            {
                Sources.Add(source);
            }
            SelectedSource ??= Sources.FirstOrDefault();
            AppLog.Pass($"share: sources = {Sources.Count} ({string.Join(", ", Sources.Select(s => s.Title))})");
        }
        catch (Exception ex)
        {
            AppLog.Warn("share: enumerate sources failed: " + ex.Message);
        }
    }

    /// <summary>启动媒体事件泵（窗口在 XAML 加载后调用）。</summary>
    public void StartMediaPump()
    {
        if (_media is not { IsCreated: true })
        {
            AppLog.Warn("media pump: 引擎未就绪，未启动");
            return;
        }
        if (_pump is not null)
        {
            return;
        }
        _pump = new MediaPump(_media, OnMediaEvent);
        _pump.Start();
    }

    public void StopMediaPump()
    {
        _pump?.Dispose();
        _pump = null;
    }

    /// <summary>把远端画面渲染器接到 ViewModel（窗口在 XAML 加载后调用，仅 UI 线程）。</summary>
    public void AttachRemoteScreen(RemoteScreenPresenter presenter)
    {
        _remoteScreen = presenter;
        presenter.FpsChanged += fps => RemoteFpsText = $"画面：{fps:F1} fps（{presenter.Width}x{presenter.Height}）";
        AppLog.Info("remote screen: presenter attached");
    }

    /// <summary>媒体泵线程回调。帧路径在 UI 线程取帧+拷贝；非帧事件 Post 回 UI 线程。</summary>
    private void OnMediaEvent(MediaEvent ev)
    {
        if (ev.Kind == MediaEventKind.RemoteFrame)
        {
            // Invoke（阻塞）而不是 Post：UI 跟不上时让泵线程等，native 队列自然丢旧帧。
            Dispatcher.UIThread.Invoke(() =>
            {
                _call?.HandleMediaEvent(ev);
                PresentRemoteFrame();
            });
            return;
        }

        Dispatcher.UIThread.Post(() =>
        {
            _call?.HandleMediaEvent(ev);
            switch (ev.Kind)
            {
                case MediaEventKind.State:
                    if (MediaStatusText.StartsWith("媒体引擎：初始化", StringComparison.Ordinal))
                    {
                        MediaStatusText = $"媒体引擎：{ev.PreviousStateValue} → {ev.StateValue}";
                    }
                    AppLog.Info($"media: state {ev.PreviousStateValue} -> {ev.StateValue} (reason={ev.Reason})");
                    break;
                case MediaEventKind.Error:
                    MediaStatusText = "媒体引擎错误：" + ev.ErrorMessage;
                    AppLog.Fail("media: " + ev.ErrorMessage);
                    break;
            }
        });
    }

    private void PresentRemoteFrame()
    {
        // 已经停止观看后仍在飞/滞留的远端帧一律不贴图：否则"停止未确认"时会用旧帧假装还在看。
        if (!IsWatching)
        {
            return;
        }
        if (_media is not { IsCreated: true } || _remoteScreen is not { } presenter)
        {
            return;
        }
        var status = _media.FrameAcquire(MediaEngine.RemoteStreamId, out var frame);
        if (status == (int)PuStatus.Timeout)
        {
            return;
        }
        if (status != (int)PuStatus.Ok || frame.Pixels == IntPtr.Zero)
        {
            if (!_frameAcquireWarned)
            {
                _frameAcquireWarned = true;
                AppLog.Warn($"remote screen: 帧未就绪（pu_frame_acquire={status}，{_media.LastErrorText()}）");
            }
            return;
        }
        try
        {
            if (presenter.Present(frame.Pixels, (int)frame.Width, (int)frame.Height, (int)frame.Stride))
            {
                IsWaitingForFrame = false;
            }
        }
        finally
        {
            _media.FrameRelease(ref frame);
        }
    }

    /// <summary>窗口关闭时调用：释放通话/连接/媒体泵与引擎。</summary>
    public async Task ShutdownAsync()
    {
        // 泵线程可能在等 UI 线程（帧路径用阻塞 Invoke）；不能在 UI 线程上 Join 它，
        // 否则关窗/退出时 UI 与泵互等到超时。把停止动作挪到线程池再 await。
        var pump = _pump;
        _pump = null;
        if (pump is not null)
        {
            await Task.Run(pump.Dispose);
        }
        await TearDownCallAsync(logout: true);
        if (_connection is { } connection)
        {
            _connection = null;
            await connection.DisposeAsync();
        }
        _media?.Dispose();
        _media = null;
    }

    // ------------------------------------------------------------------
    // 设置（外观部分真实生效；未接能力禁用并说明）
    // ------------------------------------------------------------------

    [ObservableProperty]
    private bool _showSettings;

    [ObservableProperty]
    private string _themeChoice = ThemeChoices[2];

    [ObservableProperty]
    private string _textSizeChoice = TextSizeChoices[0];

    [ObservableProperty]
    private LanguageOption _languageOption = LanguageChoices[0];

    /// <summary>主题显示名（随界面语言变化），索引与 <see cref="ThemeKeys"/> 一一对应。</summary>
    public static string[] ThemeChoices { get; } =
        [L.Get("Theme_Light"), L.Get("Theme_Dark"), L.Get("Theme_System")];

    /// <summary>持久化用的稳定键。存档里不能写显示名，否则切语言后读回会错位。</summary>
    public static string[] ThemeKeys { get; } = ["light", "dark", "system"];

    public static string[] TextSizeChoices { get; } =
        [L.Get("TextSize_Normal"), L.Get("TextSize_Large")];

    public static string[] TextSizeKeys { get; } = ["normal", "large"];

    public static IReadOnlyList<LanguageOption> LanguageChoices => L.Languages;

    public ObservableCollection<string> PathHistoryLines { get; } = new();

    public string SettingsAudioNote => L.Get("Settings_AudioNote");

    public string SettingsStartupNote => L.Get("Settings_StartupNote");

    /// <summary>关于页版本行。版本号从运行时取，不写死在 XAML 里。</summary>
    public string SettingsAboutLine => L.Format(
        "Settings_AboutLine",
        typeof(ShellViewModel).Assembly.GetName().Version?.ToString(3) ?? "1.1",
        "12.1.4",
        "12.1.0.1");

    // ------------------------------------------------------------------
    // 自动更新（Velopack；便携版 / 调试桥构建只有「检查」能力，没有下载与重启）
    //
    // 用户 2026-11-07 的四条要求：启动后静默检查（开关默认开）→ 只有发现新版才提示 →
    // 用户确认后才下载 → 下载完重启应用完成更新。
    // 静默检查从构造时排期（延迟 8 秒），不订阅窗口事件——ShellWindow.axaml.cs 归其它任务。
    // ------------------------------------------------------------------

    private readonly UpdateService _updateService = new();
    private bool _suppressAutoCheckChange;

    /// <summary>「启动后自动检查更新」：默认开，改了就落盘到 <see cref="AppSettings.AutoCheckUpdates"/>。</summary>
    [ObservableProperty]
    private bool _autoCheckUpdates = true;

    /// <summary>设置页「更新」区块的状态文字。</summary>
    [ObservableProperty]
    private string _updateStatusText = string.Empty;

    /// <summary>检查/下载进行中（按钮禁用，避免重复触发）。</summary>
    [ObservableProperty]
    private bool _isUpdateBusy;

    /// <summary>发现的新版本号；空 = 当前没有可用更新。</summary>
    [ObservableProperty]
    private string _availableUpdateVersion = string.Empty;

    /// <summary>当前客户端版本（程序集版本，csproj 从仓库根 VERSION 注入）。</summary>
    public string CurrentClientVersionText => L.Format("Update_Current", _updateService.CurrentVersion);

    /// <summary>更新提示框正文（版本号变化时重新求值）。</summary>
    public string UpdatePromptMessage => L.Format(
        "UpdatePrompt_Message", AvailableUpdateVersion, _updateService.CurrentVersion);

    partial void OnAvailableUpdateVersionChanged(string value) =>
        OnPropertyChanged(nameof(UpdatePromptMessage));

    partial void OnAutoCheckUpdatesChanged(bool value)
    {
        if (_suppressAutoCheckChange)
        {
            return;
        }
        var saved = AppSettings.Instance;
        saved.AutoCheckUpdates = value;
        saved.Save();
        AppLog.Info("settings: AutoCheckUpdates=" + (value ? "on" : "off"));
    }

    /// <summary>主窗口显示后延迟约 8 秒静默检查一次；关闭「自动检查更新」时完全不排期。</summary>
    public void ScheduleStartupUpdateCheck()
    {
        if (!AutoCheckUpdates)
        {
            AppLog.Info("update: 启动静默检查已关闭（AutoCheckUpdates=false）");
            return;
        }
        _ = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(8)).ConfigureAwait(false);
            await RunUpdateCheckAsync(silent: true).ConfigureAwait(false);
        });
    }

    [RelayCommand]
    private Task CheckForUpdatesAsync() => RunUpdateCheckAsync(silent: false);

    /// <summary>下载已发现的新版本；成功时 Velopack 结束当前进程并重启完成替换。</summary>
    [RelayCommand]
    private async Task DownloadUpdateAsync()
    {
        if (IsUpdateBusy)
        {
            return;
        }
        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            IsUpdateBusy = true;
            UpdateStatusText = L.Get("Update_Downloading");
        });
        var ok = await _updateService.DownloadAndApplyAsync(percent =>
        {
            if (percent > 0)
            {
                Dispatcher.UIThread.Post(() => UpdateStatusText = L.Format("Update_DownloadProgress", percent));
            }
        }).ConfigureAwait(false);
        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            IsUpdateBusy = false;
            if (!ok)
            {
                UpdateStatusText = L.Get("Update_DownloadFailed");
            }
        });
    }

    /// <summary>检查更新；silent=true 时只在发现新版本才提示（启动静默检查走这条）。</summary>
    private async Task RunUpdateCheckAsync(bool silent)
    {
        if (silent && !AutoCheckUpdates)
        {
            return;
        }
        if (!silent)
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                IsUpdateBusy = true;
                UpdateStatusText = L.Get("Update_Checking");
            });
        }

        // 便携版 / 调试桥构建：明确告知要手动下载，且**不发起任何网络请求**。
        UpdateCheckResult result;
        if (!_updateService.IsInstalled)
        {
            AppLog.Info("update: 非 Velopack 安装版，跳过网络检查");
            result = new UpdateCheckResult(UpdateCheckStatus.NotInstalled);
        }
        else
        {
            result = await _updateService.CheckAsync().ConfigureAwait(false);
        }

        UpdateCheckResult finalResult = result;
        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            IsUpdateBusy = false;
            ApplyUpdateResult(finalResult, silent);
        });
    }

    /// <summary>把检查结果落到界面（已在 UI 线程）。</summary>
    private void ApplyUpdateResult(UpdateCheckResult result, bool silent)
    {
        switch (result.Status)
        {
            case UpdateCheckStatus.Available:
                AvailableUpdateVersion = result.Version ?? string.Empty;
                UpdateStatusText = L.Format("Update_Available", AvailableUpdateVersion);
                if (silent)
                {
                    _ = ShowUpdatePromptAsync();
                }
                break;
            case UpdateCheckStatus.UpToDate:
                AvailableUpdateVersion = string.Empty;
                UpdateStatusText = L.Get("Update_UpToDate");
                break;
            case UpdateCheckStatus.NotInstalled:
                UpdateStatusText = L.Get("Update_Portable");
                break;
            default:
                AvailableUpdateVersion = string.Empty;
                UpdateStatusText = L.Get("Update_Failed");
                break;
        }
    }

    /// <summary>发现新版本时弹确认框：用户点「立即更新」才下载（确认后才下载）。</summary>
    private async Task ShowUpdatePromptAsync()
    {
        if (Application.Current?.ApplicationLifetime is not IClassicDesktopStyleApplicationLifetime { MainWindow: { } owner })
        {
            return;
        }
        try
        {
            await new PartyUp.Client.Views.UpdatePromptWindow { DataContext = this }.ShowDialog(owner);
        }
        catch (Exception ex)
        {
            AppLog.Fail("update: 更新提示框显示失败: " + ex.Message);
        }
    }

    partial void OnThemeChoiceChanged(string value)
    {
        ThemeChanged?.Invoke(value);
        SaveAppearance();
    }

    partial void OnTextSizeChoiceChanged(string value)
    {
        TextSizeChanged?.Invoke(value);
        SaveAppearance();
    }

    partial void OnLanguageOptionChanged(LanguageOption value)
    {
        // 语言只在启动时应用（XAML 文案在窗口载入时求值），因此这里只落盘，
        // 设置页会明确告诉用户"重启后生效"。
        var saved = AppSettings.Instance;
        saved.Language = value.Tag;
        saved.Save();
    }

    private void SaveAppearance()
    {
        var saved = AppSettings.Instance;
        saved.Theme = ThemeKeys[Math.Max(0, Array.IndexOf(ThemeChoices, ThemeChoice))];
        saved.TextSize = TextSizeKeys[Math.Max(0, Array.IndexOf(TextSizeChoices, TextSizeChoice))];
        saved.Save();
    }

    // ------------------------------------------------------------------
    // 音频设备与麦克风测试（B1：真实枚举、选择与电平；不显示假设备值）
    // ------------------------------------------------------------------

    private IReadOnlyList<AudioDeviceInfo> _audioInputs = Array.Empty<AudioDeviceInfo>();
    private IReadOnlyList<AudioDeviceInfo> _audioOutputs = Array.Empty<AudioDeviceInfo>();
    private readonly List<string?> _inputOptionGuids = new();
    private readonly List<string?> _outputOptionGuids = new();
    private DispatcherTimer? _micTestTimer;
    private DateTime _micTestStartedAt;
    private bool _micTestEverValid;
    private bool _suppressDeviceChange;

    /// <summary>输入设备下拉项；第 0 项固定是「跟随系统默认」（原生 index=-1）。</summary>
    public ObservableCollection<string> InputDeviceOptions { get; } = new();

    public ObservableCollection<string> OutputDeviceOptions { get; } = new();

    [ObservableProperty]
    private string _inputDeviceChoice = string.Empty;

    [ObservableProperty]
    private string _outputDeviceChoice = string.Empty;

    [ObservableProperty]
    private string _audioDeviceNote = string.Empty;

    [ObservableProperty]
    private bool _micTestActive;

    /// <summary>0..100，给进度条用（原生 peak 是 0..32767 的样本幅值）。</summary>
    [ObservableProperty]
    private double _micLevel;

    [ObservableProperty]
    private string _micTestStatusText = string.Empty;

    [ObservableProperty]
    private bool _micTestAvailable;

    /// <summary>枚举设备并回填下拉框。打开设置页时调用；热插拔后由「刷新设备」再调。</summary>
    [RelayCommand]
    private void RefreshAudioDevices()
    {
        if (_media is not { IsCreated: true } || !_media.HasAudioDeviceSupport)
        {
            AudioDeviceNote = L.Get("Settings_AudioUnavailable");
            return;
        }

        IReadOnlyList<AudioDeviceInfo> inputs;
        IReadOnlyList<AudioDeviceInfo> outputs;
        try
        {
            inputs = _media.GetAudioDevices(0);
            outputs = _media.GetAudioDevices(1);
        }
        catch (Exception ex)
        {
            AppLog.Warn("audio: enumerate devices failed: " + ex.Message);
            AudioDeviceNote = L.Get("Settings_AudioUnavailable");
            return;
        }

        _audioInputs = inputs;
        _audioOutputs = outputs;

        var saved = AppSettings.Instance;
        _suppressDeviceChange = true;
        try
        {
            var missing = FillDeviceOptions(InputDeviceOptions, _inputOptionGuids, _audioInputs, saved.InputDevice,
                out var inputIndex);
            missing |= FillDeviceOptions(OutputDeviceOptions, _outputOptionGuids, _audioOutputs, saved.OutputDevice,
                out var outputIndex);
            InputDeviceChoice = InputDeviceOptions[inputIndex];
            OutputDeviceChoice = OutputDeviceOptions[outputIndex];

            var note = L.Get(_media.HasAudioHotSwitch ? "Settings_AudioHotSwitchOn" : "Settings_AudioHotSwitchOff");
            AudioDeviceNote = missing ? L.Get("Settings_AudioDeviceMissing") + "\n" + note : note;
        }
        finally
        {
            _suppressDeviceChange = false;
        }

        // 把已保存的选择推给引擎：不在通话中时只登记，下次入会生效（见头文件语义）。
        ApplyAudioDevice(0, saved.InputDevice, announce: false);
        ApplyAudioDevice(1, saved.OutputDevice, announce: false);
        MicTestAvailable = _media.HasAudioMonitorSupport;
    }

    /// <summary>填充下拉项；返回 true 表示「保存的设备已不存在」（调用方要提示回落）。</summary>
    private static bool FillDeviceOptions(ObservableCollection<string> options, List<string?> guids,
        IReadOnlyList<AudioDeviceInfo> devices, string? savedGuid, out int selectedIndex)
    {
        options.Clear();
        guids.Clear();
        options.Add(L.Get("Settings_AudioDefault"));
        guids.Add(null);

        foreach (var device in devices)
        {
            var label = string.IsNullOrWhiteSpace(device.Name) ? "#" + device.Index : device.Name;
            // 声卡常常暴露两个同名端点；重名时带序号，否则用户在下拉里分不清选的是哪个。
            if (devices.Count(d => string.Equals(d.Name, device.Name, StringComparison.Ordinal)) > 1)
            {
                label += "  (#" + device.Index + ")";
            }
            options.Add(label);
            guids.Add(device.Guid);
        }

        selectedIndex = 0;
        var missing = false;
        if (!string.IsNullOrWhiteSpace(savedGuid))
        {
            var found = guids.IndexOf(savedGuid);
            if (found < 0)
            {
                missing = true;
            }
            else
            {
                selectedIndex = found;
            }
        }
        return missing;
    }

    partial void OnInputDeviceChoiceChanged(string value) =>
        OnDeviceChoiceChanged(0, InputDeviceOptions, _inputOptionGuids, value);

    partial void OnOutputDeviceChoiceChanged(string value) =>
        OnDeviceChoiceChanged(1, OutputDeviceOptions, _outputOptionGuids, value);

    private void OnDeviceChoiceChanged(int kind, ObservableCollection<string> options, List<string?> guids, string value)
    {
        if (_suppressDeviceChange)
        {
            return;
        }
        var optionIndex = options.IndexOf(value);
        var guid = optionIndex >= 0 && optionIndex < guids.Count ? guids[optionIndex] : null;

        var saved = AppSettings.Instance;
        if (kind == 0)
        {
            saved.InputDevice = guid;
        }
        else
        {
            saved.OutputDevice = guid;
        }
        saved.Save();

        ApplyAudioDevice(kind, guid, announce: true);
    }

    /// <summary>把选择推给媒体核心。索引 -1 = 跟随系统默认（原生约定）。</summary>
    private void ApplyAudioDevice(int kind, string? guid, bool announce)
    {
        if (_media is not { IsCreated: true } || !_media.HasAudioDeviceSupport)
        {
            return;
        }

        var devices = kind == 0 ? _audioInputs : _audioOutputs;
        var index = -1;
        if (!string.IsNullOrWhiteSpace(guid))
        {
            var match = devices.FirstOrDefault(d => string.Equals(d.Guid, guid, StringComparison.Ordinal));
            // 设备消失时回到系统默认，而不是把旧枚举序号塞进新列表——序号不稳定，头文件明确要求用 guid。
            index = match?.Index ?? -1;
        }

        try
        {
            _media.SetAudioDevice(kind, index);
            AppLog.Info($"audio: device kind={kind} index={index} guid={(guid ?? "(system default)")}");
            if (announce && IsInVoice && !_media.HasAudioHotSwitch)
            {
                AudioDeviceNote = L.Get("Settings_AudioHotSwitchOff");
            }
        }
        catch (Exception ex)
        {
            AppLog.Warn("audio: set device failed: " + ex.Message);
            AudioDeviceNote = ex.Message;
        }
    }

    /// <summary>麦克风测试：开始/停止独立输入电平采集（不进语音、不建 PeerConnection）。</summary>
    [RelayCommand]
    private void ToggleMicTest()
    {
        if (MicTestActive)
        {
            StopMicTest();
            return;
        }

        if (_media is not { IsCreated: true } || !_media.HasAudioMonitorSupport)
        {
            MicTestStatusText = L.Get("Settings_MicTestUnavailable");
            return;
        }
        if (IsInVoice)
        {
            // 原生语义：语音占用时 monitor_start 会被拒绝（PU_ERR_STATE）。先说明原因，
            // 而不是让用户点一下看到库内部冒出来的错误。
            MicTestStatusText = L.Get("Settings_MicTestInCall");
            return;
        }

        try
        {
            _media.StartAudioMonitor();
        }
        catch (Exception ex)
        {
            AppLog.Warn("audio: monitor start failed: " + ex.Message);
            MicTestStatusText = ex.Message;
            return;
        }

        _micTestEverValid = false;
        _micTestStartedAt = DateTime.UtcNow;
        MicLevel = 0;
        MicTestStatusText = L.Get("Settings_MicTestWaiting");
        MicTestActive = true;

        _micTestTimer ??= new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _micTestTimer.Tick -= OnMicTestTick;
        _micTestTimer.Tick += OnMicTestTick;
        _micTestTimer.Start();
    }

    private void OnMicTestTick(object? sender, EventArgs e)
    {
        if (_media is not { IsCreated: true })
        {
            StopMicTest();
            return;
        }

        try
        {
            var status = _media.GetAudioMonitorStatus();
            if (status.Valid)
            {
                _micTestEverValid = true;
                MicLevel = ToMeterPercent(status.Peak);
                MicTestStatusText = $"{L.Get("Settings_MicTestLevel")} {MicLevel:0}%";
            }
            else if (status.State == 2)
            {
                // Faulted：如实报告仍可能在跑，不能当成"已停止"。
                MicTestStatusText = L.Get("Settings_MicTestNoSignal") + $" (faulted, running={status.Running})";
            }
            else if (!_micTestEverValid && DateTime.UtcNow - _micTestStartedAt > TimeSpan.FromSeconds(2))
            {
                MicTestStatusText = L.Get("Settings_MicTestNoSignal");
            }
        }
        catch (Exception ex)
        {
            AppLog.Warn("audio: monitor poll failed: " + ex.Message);
            StopMicTest();
        }
    }

    /// <summary>
    /// 把 0..32767 的峰值映射成 0..100 的电平条读数。
    /// 用线性幅度会让正常说话只占到 3–10%（看起来像坏了），所以按 dBFS 映射：
    /// -60 dBFS → 0%，0 dBFS → 100%，与常见调音台表头一致。
    /// </summary>
    private static double ToMeterPercent(uint peak)
    {
        if (peak <= 1)
        {
            return 0;
        }
        var dbfs = 20.0 * Math.Log10(peak / 32767.0);
        return Math.Clamp((dbfs + 60.0) / 60.0 * 100.0, 0.0, 100.0);
    }

    private void StopMicTest()
    {
        try
        {
            _media?.StopAudioMonitor();
        }
        catch (Exception ex)
        {
            // Stop 失败时原生留在 Faulted 并如实报告 running；这里只记日志，不假装已停。
            AppLog.Warn("audio: monitor stop failed: " + ex.Message);
        }
        _micTestTimer?.Stop();
        MicTestActive = false;
        MicLevel = 0;
        MicTestStatusText = string.Empty;
    }

    // ------------------------------------------------------------------
    // 窗口与启动（B1）：关闭到托盘 + 开机启动
    // ------------------------------------------------------------------

    private bool _suppressStartupChange;

    [ObservableProperty]
    private bool _closeToTray;

    [ObservableProperty]
    private bool _launchAtStartup;

    /// <summary>开机启动写入失败的原因（空 = 正常）。</summary>
    [ObservableProperty]
    private string _startupNote = string.Empty;

    public bool HasStartupNote => !string.IsNullOrEmpty(StartupNote);

    partial void OnStartupNoteChanged(string value) => OnPropertyChanged(nameof(HasStartupNote));

    partial void OnCloseToTrayChanged(bool value)
    {
        // ShellWindow.OnClosing 每次都实时读 AppSettings，所以这里只要落盘即可。
        var saved = AppSettings.Instance;
        saved.CloseToTray = value;
        saved.Save();
    }

    partial void OnLaunchAtStartupChanged(bool value)
    {
        if (_suppressStartupChange)
        {
            return;
        }

        if (StartupRegistrar.SetEnabled(value, out var error))
        {
            var saved = AppSettings.Instance;
            saved.LaunchAtStartup = value; // 仅缓存，真源是注册表
            saved.Save();
            StartupNote = string.Empty;
            return;
        }

        StartupNote = L.Format("Startup_RegisterFailed", error ?? "unknown");
        // 写失败时把勾选回滚到真实状态：不能让界面显示"已开启"而注册表里没有。
        _suppressStartupChange = true;
        try
        {
            LaunchAtStartup = StartupRegistrar.IsEnabled();
        }
        finally
        {
            _suppressStartupChange = false;
        }
    }

    /// <summary>窗口订阅：应用主题（跟随系统用 Default）。</summary>
    public event Action<string>? ThemeChanged;

    /// <summary>窗口订阅：应用文字大小。</summary>
    public event Action<string>? TextSizeChanged;

    [RelayCommand]
    private void OpenSettings()
    {
        ShowSettings = true;
        SyncPathHistory();
        // 设备列表在打开设置时枚举：用户可能刚插拔耳机，缓存一份旧的设备表会让他选到不存在的设备。
        RefreshAudioDevices();
    }

    [RelayCommand]
    private void CloseSettings() => ShowSettings = false;

    private void SyncPathHistory()
    {
        var snapshot = _call?.PathHistory.Select(p => p.ToString()).ToList() ?? [];
        if (snapshot.Count == PathHistoryLines.Count &&
            !snapshot.Where((line, i) => line != PathHistoryLines[i]).Any())
        {
            return;
        }
        PathHistoryLines.Clear();
        foreach (var line in snapshot)
        {
            PathHistoryLines.Add(line);
        }
        OnPropertyChanged(nameof(HasPathHistory));
    }
}
