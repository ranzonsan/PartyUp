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
    private long _lastInboundTicks;
    private int _reconnecting; // 0/1，Interlocked 保护
    private volatile bool _userClosed;
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

    public bool IsConnected => _socket.State == WebSocketState.Open;

    public event Action<TextMessageDto>? TextReceived;

    public event Action<PresenceDto>? PresenceChanged;

    /// <summary>媒体信令推送（media.signal，docs/PROTOCOL.md §4.2）。在接收循环线程触发。</summary>
    public event Action<MediaSignalDto>? MediaSignalReceived;

    /// <summary>共享状态推送（screen.state，docs/PROTOCOL.md §5）。在接收循环线程触发。</summary>
    public event Action<ScreenStateDto>? ScreenStateChanged;

    /// <summary>语音状态推送（voice.state，docs/PROTOCOL.md §4）。在接收循环线程触发。</summary>
    public event Action<VoiceStateDto>? VoiceStateChanged;

    /// <summary>连接丢失（非主动关闭）；随后会自动进入重连。</summary>
    public event Action<string>? ConnectionLost;

    /// <summary>主动关闭（Dispose）或重连无法继续。</summary>
    public event Action<string>? Closed;

    /// <summary>重连中：(第几次尝试, 本次等待毫秒)。</summary>
    public event Action<int, int>? Reconnecting;

    /// <summary>重连成功，且已重新认证、重放订阅。</summary>
    public event Action? Reconnected;

    public bool IsReconnecting => Volatile.Read(ref _reconnecting) == 1;

    public int ReconnectAttempt => Volatile.Read(ref _reconnectAttempt);

    /// <summary>连接 + auth 握手。失败时连接已释放，不会泄漏半开状态。</summary>
    public static async Task<PartyUpConnection> ConnectAsync(Uri baseUri, string token, CancellationToken ct = default)
    {
        var connection = new PartyUpConnection();
        try
        {
            await connection.OpenAsync(baseUri, token, ct);
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
        CancellationToken ct = default) =>
        RequestAsync<ScreenAckDto>("media.signal", new
        {
            channel_id = channelId,
            target,
            to,
            media,
            kind,
            payload,
        }, ct);

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

    private async Task OpenAsync(Uri baseUri, string token, CancellationToken ct)
    {
        _baseUri = baseUri;
        _token = token;
        await ConnectSocketAsync(ct);
        _receiveLoop = Task.Run(ReceiveLoopAsync, CancellationToken.None);

        var auth = await RequestAsync<AuthOkDto>("auth", new { token }, ct);
        Member = auth.Member;
        Server = auth.Server;
    }

    /// <summary>建一条新的 WebSocket（首次连接与每次重连都走这里；媒体引擎不重建）。</summary>
    private async Task ConnectSocketAsync(CancellationToken ct)
    {
        if (_baseUri is null) throw new InvalidOperationException("connection base uri is missing");
        var builder = new UriBuilder(_baseUri)
        {
            Scheme = _baseUri.Scheme == Uri.UriSchemeHttps ? "wss" : "ws",
            Path = "/ws",
        };
        _socket = new ClientWebSocket();
        await _socket.ConnectAsync(builder.Uri, ct);
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
                        HandleConnectionLost("server closed the connection");
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
    private void HandleConnectionLost(string reason)
    {
        if (_userClosed)
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

                    var auth = await RequestAsync<AuthOkDto>("auth", new { token = _token }, _cts.Token);
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
