using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text.Json;
using NUlid;
using PartyUp.Client.Models;

namespace PartyUp.Client.Services;

/// <summary>
/// 一条到服务端的控制连接（docs/PROTOCOL.md §0/§1/§2/§3）。
///
/// 职责：覆盖式 auth 握手、请求/响应配对（响应 id 与请求 id 相同）、
/// 推送分发（text.new / presence）、心跳（服务端 ping → 本端 pong）。
///
/// 线程模型：一个后台接收循环独占读；发送用 SemaphoreSlim 串行化（WebSocket 不允许并发写）。
/// <see cref="TextReceived"/> / <see cref="PresenceChanged"/> 在接收循环线程上触发，
/// UI 侧必须切回 UI 线程再改控件（见 MainWindow 里的 Dispatcher.UIThread.Post）。
/// </summary>
public sealed class PartyUpConnection : IAsyncDisposable
{
    private const int ProtocolVersion = 1;
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan CloseTimeout = TimeSpan.FromSeconds(3);

    // 断线重连（v1 验收第 6 条）：退避 0.5s→1s→2s→4s→5s 封顶，无限重试直到成功或用户关闭。
    private static readonly TimeSpan ReconnectBaseDelay = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan ReconnectMaxDelay = TimeSpan.FromSeconds(5);
    // 心跳看门狗：服务端每 20s 发一次 ping；45s 收不到任何帧就认为连接已死（拔网线是静默的，
    // 读阻塞不会自己返回，必须靠这个判死）。
    private static readonly TimeSpan HeartbeatTimeout = TimeSpan.FromSeconds(45);
    private static readonly TimeSpan WatchdogInterval = TimeSpan.FromSeconds(5);

    private ClientWebSocket _socket = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly object _subscribedGate = new();
    private readonly HashSet<string> _subscribedChannels = new(StringComparer.Ordinal);
    private Uri? _baseUri;
    private string _token = string.Empty;
    // v1.1：只有显式声明 screen.watch.v1 的连接才走新观看路径；默认 false 保持旧 UI 行为。
    private bool _screenWatchCapable;
    // v2.16/B2：只有同时声明 screen.watch.isolated.v1 才允许 receive_id 严格接收路径。
    private bool _screenWatchIsolated;
    // v2.17/B2b direct：声明本连接（作为共享者）能为单个 viewer 建独立发送会话。
    private bool _screenPublishSessions;
    private long _lastInboundTicks;
    private int _reconnecting; // 0/1，Interlocked 保护
    private volatile bool _userClosed;
    /// <summary>会话已被服务端终止（顶号/踢/封禁/主动退出）：终态，不再自动重连。</summary>
    private volatile bool _sessionTerminated;
    private int _reconnectAttempt;
    private Task? _watchdog;
    private readonly ConcurrentDictionary<string, TaskCompletionSource<EnvelopeDto>> _pending = new(StringComparer.Ordinal);
    private readonly object _waiterGate = new();
    private readonly List<TextWaiter> _textWaiters = new();
    private readonly List<PresenceWaiter> _presenceWaiters = new();
    private readonly List<TextMessageDto> _receivedText = new();
    private readonly List<MediaSignalDto> _receivedSignals = new();
    private readonly List<MediaSignalWaiter> _signalWaiters = new();
    private readonly Dictionary<string, PresenceDto> _latestPresence = new(StringComparer.Ordinal);
    private readonly CancellationTokenSource _cts = new();
    private Task? _receiveLoop;

    public MemberDto Member { get; private set; } = new(string.Empty, string.Empty, false);

    public ServerInfoDto? Server { get; private set; }

    /// <summary>服务端能力名：显式单流观看（auth.ok.server.capabilities）。</summary>
    public const string ScreenWatchCapability = "screen.watch.v1";

    /// <summary>服务端能力名：严格独立接收传输（B2，每次 watch 新 receive PC/会话）。</summary>
    public const string ScreenWatchIsolatedCapability = "screen.watch.isolated.v1";

    /// <summary>
    /// 客户端能力名（B2b direct）：本连接作为共享者能为单个 viewer 建立独立发送
    /// 传输/会话（新 PC，复用采集源/视频轨）。服务端不广告它——它只用于服务端判定
    /// 「对该共享者的 direct 严格 watch 是否可 arm」。
    /// </summary>
    public const string ScreenPublishSessionsCapability = "screen.publish.sessions.v1";

    /// <summary>
    /// 服务端是否广告 screen.watch.v1。新客户端在调用 WatchScreenAsync 前必须检查它：
    /// 旧服务端不认识 screen.watch，不能假装成功。
    /// </summary>
    public bool ServerSupportsScreenWatch =>
        Server?.Capabilities?.Contains(ScreenWatchCapability, StringComparer.Ordinal) == true;

    /// <summary>
    /// 服务端是否广告严格接收传输。未广告 = 旧服务端/旧配置，严格 watch 必须明确报
    /// 不支持，不能降级成旧混合 PC 的 sink-only 成功。
    /// </summary>
    public bool ServerSupportsIsolatedScreenWatch =>
        Server?.Capabilities?.Contains(ScreenWatchIsolatedCapability, StringComparer.Ordinal) == true;

    /// <summary>本连接是否在 auth 里声明了 screen.watch.v1（决定服务端是否自动绑定）。</summary>
    public bool ScreenWatchEnabled => _screenWatchCapable;

    /// <summary>本连接是否声明了 screen.publish.sessions.v1（可作为 direct 共享者）。</summary>
    public bool ScreenPublishSessionsEnabled => _screenPublishSessions;

    /// <summary>本连接是否声明了 screen.watch.isolated.v1（严格接收传输）。</summary>
    public bool ScreenWatchIsolatedEnabled => _screenWatchIsolated;

    public bool IsConnected => _socket.State == WebSocketState.Open;

    public event Action<TextMessageDto>? TextReceived;

    public event Action<PresenceDto>? PresenceChanged;

    /// <summary>媒体信令推送（media.signal，docs/PROTOCOL.md §4.2）。在接收循环线程触发。</summary>
    public event Action<MediaSignalDto>? MediaSignalReceived;

    /// <summary>共享状态推送（screen.state，docs/PROTOCOL.md §5）。在接收循环线程触发。</summary>
    public event Action<ScreenStateDto>? ScreenStateChanged;

    /// <summary>语音状态推送（voice.state，docs/PROTOCOL.md §4）。在接收循环线程触发。</summary>
    public event Action<VoiceStateDto>? VoiceStateChanged;

    /// <summary>
    /// 全服务器名册推送（members.state，MEM-01/02）。在接收循环线程触发。
    /// 与 <see cref="PresenceChanged"/> 刻意分开：presence 是频道范围内订阅者，
    /// 名册是全服（含离线）且带 voice_channel_ids。旧服务端不发这条推送时不会触发。
    /// </summary>
    public event Action<MemberListDto>? MembersStateChanged;

    /// <summary>连接丢失（非主动关闭）；随后会自动进入重连。</summary>
    public event Action<string>? ConnectionLost;

    /// <summary>主动关闭（Dispose）或重连无法继续。</summary>
    public event Action<string>? Closed;

    /// <summary>
    /// 会话被服务端终止（login_elsewhere / kicked / banned / logout）。
    /// 这是**终态**：收到后不能再自动重连（token 已被撤销，重连只会一直 401），
    /// 上层应回到登录界面并按 reason 给出对应提示。
    /// </summary>
    public event Action<string>? SessionTerminated;

    /// <summary>重连中：(第几次尝试, 本次等待毫秒)。</summary>
    public event Action<int, int>? Reconnecting;

    /// <summary>重连成功，且已重新认证、重放订阅。</summary>
    public event Action? Reconnected;

    public bool IsReconnecting => Volatile.Read(ref _reconnecting) == 1;

    public int ReconnectAttempt => Volatile.Read(ref _reconnectAttempt);

    /// <summary>连接 + auth 握手。失败时连接已释放，不会泄漏半开状态。</summary>
    public static Task<PartyUpConnection> ConnectAsync(Uri baseUri, string token, CancellationToken ct = default) =>
        ConnectAsync(baseUri, token, screenWatchCapable: false, ct: ct);

    /// <summary>
    /// 连接 + auth，并显式声明 <c>screen.watch.v1</c> 能力。
    /// 只有声明后服务端才会走「未 watch 不绑定视频」的新路径；v1 UI 不传这个重载，
    /// 保持旧行为。声明前应确认 DLL 有 pu_screen_watch_ex / pu_engine_caps（见 MediaEngine）。
    /// </summary>
    public static async Task<PartyUpConnection> ConnectAsync(
        Uri baseUri, string token, bool screenWatchCapable, CancellationToken ct = default) =>
        await ConnectAsync(baseUri, token, screenWatchCapable, screenWatchIsolated: false, ct)
            .ConfigureAwait(false);

    /// <summary>
    /// 连接 + auth，并声明 <c>screen.watch.isolated.v1</c>（B2 严格接收传输）。
    /// 调用方必须先确认媒体核心有 <see cref="MediaEngine.HasIsolatedReceiveSupport"/>；
    /// 服务端未广告该能力时 <see cref="ServerSupportsIsolatedScreenWatch"/> 为 false，
    /// 严格 watch 必须报不支持而不是退回旧路径。
    /// </summary>
    public static async Task<PartyUpConnection> ConnectAsync(
        Uri baseUri, string token, bool screenWatchCapable, bool screenWatchIsolated,
        CancellationToken ct = default) =>
        await ConnectAsync(baseUri, token, screenWatchCapable, screenWatchIsolated,
            screenPublishSessions: false, ct).ConfigureAwait(false);

    /// <summary>
    /// 连接 + auth，并声明 <c>screen.publish.sessions.v1</c>（B2b direct 共享者侧：
    /// 为单个 viewer 建立独立发送传输/会话）。调用方必须确认
    /// <see cref="MediaEngine.HasDirectPublishSupport"/>；服务端只在共享者声明它时
    /// 才允许别人对该共享做 direct 严格 watch，否则明确 NOT_IMPLEMENTED。
    /// </summary>
    public static async Task<PartyUpConnection> ConnectAsync(
        Uri baseUri, string token, bool screenWatchCapable, bool screenWatchIsolated,
        bool screenPublishSessions, CancellationToken ct = default)
    {
        var connection = new PartyUpConnection
        {
            _screenWatchCapable = screenWatchCapable || screenWatchIsolated || screenPublishSessions,
            _screenWatchIsolated = screenWatchIsolated,
            _screenPublishSessions = screenPublishSessions,
        };
        try
        {
            await connection.OpenAsync(baseUri, token, ct);
            // server.info 必须在任何 PeerConnection 之前拿到：STUN 地址要写进 ICE 配置。
            // 失败不算致命（老服务端没有这条消息 / 没开 STUN 时照样能跑）。
            await connection.EnsureServerInfoAsync(ct);
            connection._watchdog = Task.Run(connection.WatchdogLoopAsync, CancellationToken.None);
            return connection;
        }
        catch
        {
            await connection.DisposeAsync();
            throw;
        }
    }

    // ------------------------------------------------------------------
    // 公共操作
    // ------------------------------------------------------------------

    public Task<ChannelListDto> ListChannelsAsync(CancellationToken ct = default) =>
        RequestAsync<ChannelListDto>("channel.list", null, ct);

    /// <summary>
    /// 全服务器名册（含离线），是 GET /api/members 的 WS 等价物（MEM-01）。
    /// 之后的增量走 <see cref="MembersStateChanged"/>；本请求本身只读。
    /// </summary>
    public Task<MemberListDto> ListMembersAsync(CancellationToken ct = default) =>
        RequestAsync<MemberListDto>("member.list", null, ct);

    public async Task<ChannelDto> CreateChannelAsync(string name, CancellationToken ct = default)
    {
        var dto = await RequestAsync<ChannelCreateDto>("channel.create", new { name }, ct);
        return dto.Channel;
    }

    /// <summary>订阅频道；记录到订阅名单，重连后自动重放。</summary>
    public Task<SubscribeAckDto> SubscribeAsync(string channelId, CancellationToken ct = default) =>
        SubscribeCoreAsync(channelId, ct);

    /// <summary>退订频道，同时从重连重放名单里移除。</summary>
    public async Task<SubscribeAckDto> UnsubscribeAsync(string channelId, CancellationToken ct = default)
    {
        var ack = await RequestAsync<SubscribeAckDto>("channel.unsubscribe", new { channel_id = channelId }, ct);
        lock (_subscribedGate)
        {
            _subscribedChannels.Remove(channelId);
        }
        return ack;
    }

    private async Task<SubscribeAckDto> SubscribeCoreAsync(string channelId, CancellationToken ct)
    {
        var ack = await RequestAsync<SubscribeAckDto>("channel.subscribe", new { channel_id = channelId }, ct);
        lock (_subscribedGate)
        {
            _subscribedChannels.Add(channelId);
        }
        return ack;
    }

    public async Task<TextSendAckDto> SendTextAsync(string channelId, string body, CancellationToken ct = default)
    {
        return await RequestAsync<TextSendAckDto>("text.send", new
        {
            channel_id = channelId,
            client_msg_id = Ulid.NewUlid().ToString(),
            body,
        }, ct);
    }

    /// <summary>当前订阅的频道（重连后按这份名单重放 channel.subscribe）。</summary>
    public IReadOnlyList<string> SubscribedChannels
    {
        get
        {
            lock (_subscribedGate)
            {
                return _subscribedChannels.ToArray();
            }
        }
    }

    public async Task<IReadOnlyList<TextMessageDto>> HistoryAsync(string channelId, long beforeSeq = 0, int limit = 50, CancellationToken ct = default)
    {
        var dto = await RequestAsync<TextHistoryDto>("text.history", new
        {
            channel_id = channelId,
            before_seq = beforeSeq,
            limit,
        }, ct);
        return dto.Messages;
    }

    // ---- 屏幕共享 / 媒体信令（docs/PROTOCOL.md §4.2、§5）----

    /// <summary>
    /// 上报一路共享（screen.start）。服务端随后广播 screen.state。
    /// </summary>
    public Task<ScreenAckDto> StartScreenAsync(string channelId, ScreenSourceDto source, string mode = "direct", CancellationToken ct = default) =>
        RequestAsync<ScreenAckDto>("screen.start", new
        {
            channel_id = channelId,
            source,
            mode,
        }, ct);

    /// <summary>停止共享（screen.stop）。</summary>
    public Task<ScreenAckDto> StopScreenAsync(string channelId, CancellationToken ct = default) =>
        RequestAsync<ScreenAckDto>("screen.stop", new { channel_id = channelId }, ct);

    /// <summary>
    /// 显式选择一路共享（screen.watch，v1.1）。调用前必须：
    ///   1) 本连接以 screenWatchCapable=true 建立（auth 已声明 screen.watch.v1）；
    ///   2) 服务端广告了 screen.watch.v1（<see cref="ServerSupportsScreenWatch"/>）；
    ///   3) 媒体核心探测到 watch_ex 能力（<see cref="MediaEngine.HasScreenWatchSupport"/>）。
    /// 旧服务端会返回 INVALID_ARGUMENT/未知消息；本方法不把它包装成成功。
    /// </summary>
    /// <param name="receiveId">
    /// B2 严格接收的客户端会话标识。非空时必须已在原生 receive 会话里打开；
    /// 服务端为它新建独立接收 PC，后续 media.signal 必须带同一 session/nonce。
    /// </param>
    public Task<ScreenWatchDto> WatchScreenAsync(string channelId, string sharerId,
        string? receiveId = null, CancellationToken ct = default) =>
        RequestAsync<ScreenWatchDto>("screen.watch", new
        {
            channel_id = channelId,
            sharer_id = sharerId,
            receive_id = receiveId,
        }, ct);

    /// <summary>停止当前观看目标（screen.unwatch，幂等）。</summary>
    public Task<ScreenUnwatchDto> UnwatchScreenAsync(string channelId, string? sharerId = null, CancellationToken ct = default) =>
        RequestAsync<ScreenUnwatchDto>("screen.unwatch", new
        {
            channel_id = channelId,
            sharer_id = sharerId,
        }, ct);

    /// <summary>
    /// 按会话精确撤销严格接收 lease（B2）：服务端只清「本连接持有 + receive_id 完全
    /// 匹配」的那条接收会话，不按 sharer 目标猜测（同目标的新会话不受旧撤销影响）。
    /// 幂等：会话已关闭/已被新会话取代时返回空 SharerID。
    /// </summary>
    public Task<ScreenUnwatchDto> UnwatchScreenSessionAsync(string channelId, string sessionId, CancellationToken ct = default) =>
        RequestAsync<ScreenUnwatchDto>("screen.unwatch", new
        {
            channel_id = channelId,
            receive_id = sessionId,
        }, ct);

    /// <summary>
    /// 转发一条媒体信令。payload 是媒体核心的原始 JSON 信封，本方法不解析它。
    /// to 为空表示广播（offer）；非空表示只投递给该成员（answer / candidate）。
    /// </summary>
    public Task<ScreenAckDto> SendMediaSignalAsync(
        string channelId,
        string media,
        string kind,
        string payload,
        string? to = null,
        string? target = null,
        string? sessionId = null,
        string? sessionNonce = null,
        ulong generation = 0,
        CancellationToken ct = default) =>
        RequestAsync<ScreenAckDto>("media.signal", new
        {
            channel_id = channelId,
            target,
            to,
            media,
            kind,
            session_id = sessionId,
            session_nonce = sessionNonce,
            generation,
            payload,
        }, ct);

    // ---- 服务端配置（docs/PROTOCOL.md §1.4）----

    /// <summary>服务端下发的 STUN 地址（`stun:host:port`）；空表示服务端没开 STUN。</summary>
    public IReadOnlyList<string> StunUrls { get; private set; } = Array.Empty<string>();

    private bool _serverInfoLoaded;

    /// <summary>请求 server.info 并缓存（幂等）。老服务端没有这条消息时只告警，不影响可用性。</summary>
    public async Task EnsureServerInfoAsync(CancellationToken ct = default)
    {
        if (_serverInfoLoaded) return;
        try
        {
            var info = await ServerInfoAsync(ct);
            StunUrls = info.StunUrls ?? Array.Empty<string>();
            _serverInfoLoaded = true;
            AppLog.Info("server: stun_urls=" + (StunUrls.Count == 0
                ? "(none — 服务端未开 STUN，直连只能靠 host 候选)"
                : string.Join(",", StunUrls)));
        }
        catch (Exception ex)
        {
            AppLog.Warn("server: server.info 不可用（老服务端？），STUN 未配置：" + ex.Message);
        }
    }

    /// <summary>server.info（§1.4）：服务端配置。只回配置，不含 transport 选择。</summary>
    public Task<ServerConfigDto> ServerInfoAsync(CancellationToken ct = default) =>
        RequestAsync<ServerConfigDto>("server.info", null, ct);

    /// <summary>
    /// voice.transport_failed（§4.4）：**只上报事实**（我这边 ICE 失败/超时），
    /// 是否降级为 relay 由服务端决定。客户端不得据此推断自己能选 transport。
    /// </summary>
    public Task<VoiceAckDto> ReportTransportFailedAsync(
        string channelId, string media, string reason, CancellationToken ct = default) =>
        RequestAsync<VoiceAckDto>("voice.transport_failed",
            new { channel_id = channelId, media, reason }, ct);

    // ---- 语音（docs/PROTOCOL.md §4）----

    /// <summary>
    /// 加入语音。传输方式由**服务端策略**决定，客户端不传也不该传 mode；
    /// 响应里的 transport 是服务端实际决定值（relay / direct），客户端据此选媒体路径。
    /// </summary>
    public Task<VoiceAckDto> VoiceJoinAsync(string channelId, CancellationToken ct = default) =>
        RequestAsync<VoiceAckDto>("voice.join", new { channel_id = channelId }, ct);

    public Task<VoiceAckDto> VoiceLeaveAsync(string channelId, CancellationToken ct = default) =>
        RequestAsync<VoiceAckDto>("voice.leave", new { channel_id = channelId }, ct);

    /// <summary>上报本端静音状态（本地 ADM 已静音，但别人要看得见）。</summary>
    public Task<VoiceAckDto> VoiceSetMuteAsync(string channelId, bool muted, CancellationToken ct = default) =>
        RequestAsync<VoiceAckDto>("voice.set_mute", new { channel_id = channelId, muted }, ct);

    public Task<VoiceAckDto> VoiceSetDeafenAsync(string channelId, bool deafened, CancellationToken ct = default) =>
        RequestAsync<VoiceAckDto>("voice.set_deafen", new { channel_id = channelId, deafened }, ct);

    /// <summary>
    /// 等一条满足谓词的 media.signal；此前的匹配消息不会丢。
    /// 超时抛 <see cref="TimeoutException"/>。
    /// </summary>
    public Task<MediaSignalDto> WaitForMediaSignalAsync(Func<MediaSignalDto, bool> match, TimeSpan timeout, CancellationToken ct = default)
    {
        Task<MediaSignalDto> task;
        TaskCompletionSource<MediaSignalDto> tcs;
        lock (_waiterGate)
        {
            foreach (var signal in _receivedSignals)
            {
                if (match(signal))
                {
                    return Task.FromResult(signal);
                }
            }
            tcs = new TaskCompletionSource<MediaSignalDto>(TaskCreationOptions.RunContinuationsAsynchronously);
            _signalWaiters.Add(new MediaSignalWaiter(match, tcs));
            task = tcs.Task;
        }
        return task.WaitAsync(timeout, ct);
    }

    /// <summary>
    /// 等一条满足谓词的 text.new。若连接此前已收到过匹配消息，立即返回（不丢早期消息）。
    /// 超时抛 <see cref="TimeoutException"/>。
    /// </summary>
    public Task<TextMessageDto> WaitForTextAsync(Func<TextMessageDto, bool> match, TimeSpan timeout, CancellationToken ct = default)
    {
        Task<TextMessageDto> task;
        TaskCompletionSource<TextMessageDto> tcs;
        lock (_waiterGate)
        {
            foreach (var message in _receivedText)
            {
                if (match(message))
                {
                    return Task.FromResult(message);
                }
            }
            tcs = new TaskCompletionSource<TextMessageDto>(TaskCreationOptions.RunContinuationsAsynchronously);
            _textWaiters.Add(new TextWaiter(match, tcs));
            task = tcs.Task.WaitAsync(timeout, ct);
        }
        _ = task.ContinueWith(
            _ =>
            {
                lock (_waiterGate)
                {
                    _textWaiters.RemoveAll(w => w.Tcs == tcs);
                }
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
        return task;
    }

    /// <summary>等某频道 presence 里至少出现 minMembers 个成员。</summary>
    public Task<PresenceDto> WaitForPresenceAsync(string channelId, int minMembers, TimeSpan timeout, CancellationToken ct = default)
    {
        Task<PresenceDto> task;
        TaskCompletionSource<PresenceDto> tcs;
        lock (_waiterGate)
        {
            if (_latestPresence.TryGetValue(channelId, out var current) && current.Members.Count >= minMembers)
            {
                return Task.FromResult(current);
            }
            tcs = new TaskCompletionSource<PresenceDto>(TaskCreationOptions.RunContinuationsAsynchronously);
            _presenceWaiters.Add(new PresenceWaiter(channelId, minMembers, tcs));
            task = tcs.Task.WaitAsync(timeout, ct);
        }
        _ = task.ContinueWith(
            _ =>
            {
                lock (_waiterGate)
                {
                    _presenceWaiters.RemoveAll(w => w.Tcs == tcs);
                }
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
        return task;
    }

    public async ValueTask DisposeAsync()
    {
        _userClosed = true;
        await _cts.CancelAsync();
        try
        {
            if (_socket.State == WebSocketState.Open)
            {
                using var timeout = new CancellationTokenSource(CloseTimeout);
                await _socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "client closing", timeout.Token);
            }
        }
        catch (Exception)
        {
            // 关闭握手失败不影响资源释放。
        }
        _socket.Dispose();
        _cts.Dispose();
        _sendGate.Dispose();
        foreach (var task in new[] { _receiveLoop, _watchdog })
        {
            if (task is null) continue;
            try
            {
                await task;
            }
            catch (Exception)
            {
                // 接收循环 / 看门狗在取消时的异常是可预期的。
            }
        }
    }

    // ------------------------------------------------------------------
    // 内部实现
    // ------------------------------------------------------------------

    /// <summary>auth 里声明的客户端能力（重连时同样使用，避免中途换能力集）。</summary>
    private string[]? AuthCapabilities()
    {
        if (!_screenWatchCapable)
        {
            return null;
        }
        var caps = new List<string> { ScreenWatchCapability };
        if (_screenWatchIsolated)
        {
            caps.Add(ScreenWatchIsolatedCapability);
        }
        if (_screenPublishSessions)
        {
            caps.Add(ScreenPublishSessionsCapability);
        }
        return caps.ToArray();
    }

    private async Task OpenAsync(Uri baseUri, string token, CancellationToken ct)
    {
        // base path 规范化后保存：WS 端点必须保留反代前缀，root 部署路径不变。
        _baseUri = ServerEndpoint.NormalizeBase(baseUri);
        _token = token;
        await ConnectSocketAsync(ct);
        _receiveLoop = Task.Run(ReceiveLoopAsync, CancellationToken.None);

        var auth = await RequestAsync<AuthOkDto>("auth", new
        {
            token,
            capabilities = AuthCapabilities(),
        }, ct);
        Member = auth.Member;
        Server = auth.Server;
    }

    /// <summary>建一条新的 WebSocket（首次连接与每次重连都走这里；媒体引擎不重建）。</summary>
    private async Task ConnectSocketAsync(CancellationToken ct)
    {
        if (_baseUri is null) throw new InvalidOperationException("connection base uri is missing");
        _socket = new ClientWebSocket();
        await _socket.ConnectAsync(ServerEndpoint.WebSocketUri(_baseUri), ct);
        Interlocked.Exchange(ref _lastInboundTicks, DateTime.UtcNow.Ticks);
    }

    private async Task<T> RequestAsync<T>(string type, object? payload, CancellationToken ct)
    {
        var id = Ulid.NewUlid().ToString();
        var envelope = new EnvelopeDto(
            ProtocolVersion,
            id,
            type,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            payload is null ? null : JsonSerializer.SerializeToElement(payload, JsonDefaults.Options));

        var tcs = new TaskCompletionSource<EnvelopeDto>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        try
        {
            await SendEnvelopeAsync(envelope, ct);
            var response = await tcs.Task.WaitAsync(RequestTimeout, ct);
            if (response.Type.EndsWith(".err", StringComparison.Ordinal))
            {
                var error = response.Payload?.Deserialize<ErrorDto>(JsonDefaults.Options)
                            ?? new ErrorDto("INTERNAL", "empty error payload");
                throw new PartyUpApiException(error.Code, error.Message);
            }
            if (response.Payload is { } element)
            {
                var value = element.Deserialize<T>(JsonDefaults.Options);
                if (value is not null)
                {
                    return value;
                }
            }
            throw new PartyUpApiException("INTERNAL", $"empty payload for {type}");
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
    }

    private async Task SendEnvelopeAsync(EnvelopeDto envelope, CancellationToken ct)
    {
        var socket = _socket;
        if (socket.State != WebSocketState.Open)
        {
            // 断线期间请求快速失败（上层会把失败翻译成「重连中」），不要挂 20 秒超时。
            throw new WebSocketException("connection is not open (reconnecting)");
        }
        var bytes = JsonSerializer.SerializeToUtf8Bytes(envelope, JsonDefaults.Options);
        await _sendGate.WaitAsync(ct);
        try
        {
            await socket.SendAsync(bytes, WebSocketMessageType.Text, endOfMessage: true, ct);
        }
        finally
        {
            _sendGate.Release();
        }
    }

    private async Task ReceiveLoopAsync()
    {
        var buffer = new byte[16 * 1024];
        using var message = new MemoryStream();
        try
        {
            while (!_cts.IsCancellationRequested && _socket.State == WebSocketState.Open)
            {
                message.SetLength(0);
                WebSocketReceiveResult result;
                do
                {
                    result = await _socket.ReceiveAsync(new ArraySegment<byte>(buffer), _cts.Token);
                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        // 关闭码是 reason 的同义兜底：服务端通常先发 session.replaced（应用层，
                        // 带精确 reason），但连接也可能被直接关掉，所以两处都要认。
                        var closeCode = result.CloseStatus is { } status ? (int)status : 0;
                        var closeText = result.CloseStatusDescription ?? string.Empty;
                        HandleConnectionLost(
                            $"server closed the connection (code={closeCode} {closeText})", closeCode);
                        return;
                    }
                    message.Write(buffer, 0, result.Count);
                }
                while (!result.EndOfMessage);

                EnvelopeDto? envelope;
                try
                {
                    envelope = JsonSerializer.Deserialize<EnvelopeDto>(
                        message.GetBuffer().AsSpan(0, (int)message.Length),
                        JsonDefaults.Options);
                }
                catch (JsonException ex)
                {
                    AppLog.Warn($"ws: dropping malformed frame: {ex.Message}");
                    continue;
                }
                if (envelope is null)
                {
                    continue;
                }
                await DispatchAsync(envelope);
            }
        }
        catch (OperationCanceledException)
        {
            // 取消：只有非主动关闭才算断线（比如上层把连接掐了）。
            if (!_userClosed) HandleConnectionLost("receive loop canceled");
        }
        catch (WebSocketException ex)
        {
            HandleConnectionLost(ex.Message);
        }
        catch (Exception ex)
        {
            HandleConnectionLost(ex.Message);
        }
    }

    // ------------------------------------------------------------------
    // 断线重连
    // ------------------------------------------------------------------

    /// <summary>连接丢失：非主动关闭就进入自动重连（同一时刻只允许一个重连循环）。</summary>
    /// <param name="closeCode">
    /// 服务端给的 WS 关闭码；4001/4002/4003 表示会话已被撤销（顶号/踢/封禁），
    /// 属于**终态**——token 已经失效，重连不会成功，只会把用户卡在"重连中"。
    /// </param>
    private void HandleConnectionLost(string reason, int closeCode = 0)
    {
        var terminal = _sessionTerminated || closeCode is 4001 or 4002 or 4003;
        if (_userClosed || terminal)
        {
            Closed?.Invoke(reason);
            return;
        }
        if (Interlocked.CompareExchange(ref _reconnecting, 1, 0) != 0)
        {
            return; // 已经在重连（或刚被别的路径触发），让现有循环继续。
        }
        AppLog.Warn($"ws: connection lost: {reason}");
        ConnectionLost?.Invoke(reason);
        _watchdog ??= Task.Run(WatchdogLoopAsync, CancellationToken.None);
        _ = Task.Run(ReconnectLoopAsync, CancellationToken.None);
    }

    /// <summary>指数退避重连：0.5s→1s→2s→4s→5s 封顶，无限重试直到成功或用户关闭。</summary>
    private async Task ReconnectLoopAsync()
    {
        try
        {
            for (var attempt = 1; !_userClosed && !_cts.IsCancellationRequested; attempt++)
            {
                Volatile.Write(ref _reconnectAttempt, attempt);
                var delayMs = (int)Math.Min(
                    ReconnectMaxDelay.TotalMilliseconds,
                    ReconnectBaseDelay.TotalMilliseconds * Math.Pow(2, Math.Min(attempt - 1, 4)));
                Reconnecting?.Invoke(attempt, delayMs);
                AppLog.Info($"ws: reconnect attempt {attempt} in {delayMs}ms");
                try
                {
                    await Task.Delay(delayMs, _cts.Token);
                }
                catch (OperationCanceledException)
                {
                    return;
                }

                try
                {
                    await ConnectSocketAsync(_cts.Token);
                    _receiveLoop = Task.Run(ReceiveLoopAsync, CancellationToken.None);

                    var auth = await RequestAsync<AuthOkDto>("auth", new
                    {
                        token = _token,
                        capabilities = AuthCapabilities(),
                    }, _cts.Token);
                    Member = auth.Member;
                    Server = auth.Server;

                    // 重放订阅：频道 presence / voice.state 由服务端在 subscribe 时补推。
                    foreach (var channelId in SubscribedChannels)
                    {
                        try
                        {
                            await SubscribeCoreAsync(channelId, _cts.Token);
                            AppLog.Info($"ws: resubscribed channel={channelId}");
                        }
                        catch (Exception ex)
                        {
                            AppLog.Warn($"ws: resubscribe {channelId} failed: {ex.Message}");
                        }
                    }

                    Volatile.Write(ref _reconnectAttempt, 0);
                    Interlocked.Exchange(ref _reconnecting, 0);
                    AppLog.Pass($"ws: reconnected (attempt {attempt})");
                    Reconnected?.Invoke();
                    return;
                }
                catch (Exception ex)
                {
                    AppLog.Warn($"ws: reconnect attempt {attempt} failed: {ex.Message}");
                    try { _socket.Dispose(); } catch { /* ignore */ }
                }
            }
        }
        finally
        {
            if (!_userClosed)
            {
                Interlocked.Exchange(ref _reconnecting, 0);
            }
        }
    }

    /// <summary>
    /// 心跳看门狗：拔网线时 TCP 读会静默阻塞、ReceiveAsync 不会返回，
    /// 所以用「45 秒没收到任何帧」作为判死条件，主动 Abort 让接收循环抛错进入重连。
    /// </summary>
    private async Task WatchdogLoopAsync()
    {
        while (!_userClosed && !_cts.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(WatchdogInterval, _cts.Token);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            if (_socket.State != WebSocketState.Open) continue;
            var last = new DateTime(Interlocked.Read(ref _lastInboundTicks), DateTimeKind.Utc);
            if (DateTime.UtcNow - last <= HeartbeatTimeout) continue;

            AppLog.Warn($"ws: no frame for {(DateTime.UtcNow - last).TotalSeconds:F0}s, treating connection as dead");
            try
            {
                _socket.Abort();
            }
            catch (Exception)
            {
                // Abort 失败也没有别的补救手段，交给接收循环的异常路径。
            }
        }
    }

    private async Task DispatchAsync(EnvelopeDto envelope)
    {
        // 任何入站帧（含服务端每 20s 的 ping）都算活着的证据，喂给心跳看门狗。
        Interlocked.Exchange(ref _lastInboundTicks, DateTime.UtcNow.Ticks);
        switch (envelope.Type)
        {
            case "ping":
                await SendEnvelopeAsync(
                    new EnvelopeDto(ProtocolVersion, Ulid.NewUlid().ToString(), "pong", DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), null),
                    CancellationToken.None);
                return;
            case "pong":
                return;
            case "text.new":
            {
                var message = envelope.Payload?.Deserialize<TextMessageDto>(JsonDefaults.Options);
                if (message is not null)
                {
                    OnTextReceived(message);
                }
                return;
            }
            case "presence":
            {
                var presence = envelope.Payload?.Deserialize<PresenceDto>(JsonDefaults.Options);
                if (presence is not null)
                {
                    OnPresenceReceived(presence);
                }
                return;
            }
            case "media.signal":
            {
                var signal = envelope.Payload?.Deserialize<MediaSignalDto>(JsonDefaults.Options);
                if (signal is not null)
                {
                    OnMediaSignalReceived(signal);
                }
                return;
            }
            case "screen.state":
            {
                var state = envelope.Payload?.Deserialize<ScreenStateDto>(JsonDefaults.Options);
                if (state is not null)
                {
                    ScreenStateChanged?.Invoke(state);
                }
                return;
            }
            case "voice.state":
            {
                var state = envelope.Payload?.Deserialize<VoiceStateDto>(JsonDefaults.Options);
                if (state is not null)
                {
                    VoiceStateChanged?.Invoke(state);
                }
                return;
            }
            case "members.state":
            {
                var roster = envelope.Payload?.Deserialize<MemberListDto>(JsonDefaults.Options);
                if (roster is not null)
                {
                    MembersStateChanged?.Invoke(roster);
                }
                return;
            }
            case "session.replaced":
            {
                var replaced = envelope.Payload?.Deserialize<SessionReplacedDto>(JsonDefaults.Options);
                var reason = replaced?.Reason ?? string.Empty;
                // 先立终态标志，再通知上层：这样紧随其后的 Close 帧不会触发重连。
                _sessionTerminated = true;
                AppLog.Warn($"ws: session terminated by server reason={reason}");
                SessionTerminated?.Invoke(reason);
                return;
            }
        }

        // 其余都是请求的响应：按 id 唤醒等待者。
        if (_pending.TryRemove(envelope.Id, out var waiter))
        {
            waiter.TrySetResult(envelope);
        }
    }

    private void OnTextReceived(TextMessageDto message)
    {
        List<TextWaiter> satisfied = new();
        lock (_waiterGate)
        {
            _receivedText.Add(message);
            if (_receivedText.Count > 1000)
            {
                _receivedText.RemoveRange(0, _receivedText.Count - 1000);
            }
            for (var i = _textWaiters.Count - 1; i >= 0; i--)
            {
                var waiter = _textWaiters[i];
                if (!waiter.Match(message))
                {
                    continue;
                }
                _textWaiters.RemoveAt(i);
                satisfied.Add(waiter);
            }
        }
        foreach (var waiter in satisfied)
        {
            waiter.Tcs.TrySetResult(message);
        }
        TextReceived?.Invoke(message);
    }

    private void OnPresenceReceived(PresenceDto presence)
    {
        List<PresenceWaiter> satisfied = new();
        lock (_waiterGate)
        {
            _latestPresence[presence.ChannelId] = presence;
            for (var i = _presenceWaiters.Count - 1; i >= 0; i--)
            {
                var waiter = _presenceWaiters[i];
                if (waiter.ChannelId != presence.ChannelId || presence.Members.Count < waiter.MinMembers)
                {
                    continue;
                }
                _presenceWaiters.RemoveAt(i);
                satisfied.Add(waiter);
            }
        }
        foreach (var waiter in satisfied)
        {
            waiter.Tcs.TrySetResult(presence);
        }
        PresenceChanged?.Invoke(presence);
    }

    private void OnMediaSignalReceived(MediaSignalDto signal)
    {
        List<MediaSignalWaiter> satisfied = new();
        lock (_waiterGate)
        {
            _receivedSignals.Add(signal);
            if (_receivedSignals.Count > 500)
            {
                _receivedSignals.RemoveRange(0, _receivedSignals.Count - 500);
            }
            for (var i = _signalWaiters.Count - 1; i >= 0; i--)
            {
                var waiter = _signalWaiters[i];
                if (!waiter.Match(signal))
                {
                    continue;
                }
                _signalWaiters.RemoveAt(i);
                satisfied.Add(waiter);
            }
        }
        foreach (var waiter in satisfied)
        {
            waiter.Tcs.TrySetResult(signal);
        }
        MediaSignalReceived?.Invoke(signal);
    }

    private sealed class TextWaiter(Func<TextMessageDto, bool> match, TaskCompletionSource<TextMessageDto> tcs)
    {
        public Func<TextMessageDto, bool> Match { get; } = match;

        public TaskCompletionSource<TextMessageDto> Tcs { get; } = tcs;
    }

    private sealed class MediaSignalWaiter(Func<MediaSignalDto, bool> match, TaskCompletionSource<MediaSignalDto> tcs)
    {
        public Func<MediaSignalDto, bool> Match { get; } = match;

        public TaskCompletionSource<MediaSignalDto> Tcs { get; } = tcs;
    }

    private sealed class PresenceWaiter(string channelId, int minMembers, TaskCompletionSource<PresenceDto> tcs)
    {
        public string ChannelId { get; } = channelId;

        public int MinMembers { get; } = minMembers;

        public TaskCompletionSource<PresenceDto> Tcs { get; } = tcs;
    }
}
