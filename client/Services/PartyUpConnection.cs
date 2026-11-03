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

    private readonly ClientWebSocket _socket = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
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

    public event Action<string>? Closed;

    /// <summary>连接 + auth 握手。失败时连接已释放，不会泄漏半开状态。</summary>
    public static async Task<PartyUpConnection> ConnectAsync(Uri baseUri, string token, CancellationToken ct = default)
    {
        var connection = new PartyUpConnection();
        try
        {
            await connection.OpenAsync(baseUri, token, ct);
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

    public Task SubscribeAsync(string channelId, CancellationToken ct = default) =>
        RequestAsync<SubscribeAckDto>("channel.subscribe", new { channel_id = channelId }, ct);

    public Task UnsubscribeAsync(string channelId, CancellationToken ct = default) =>
        RequestAsync<SubscribeAckDto>("channel.unsubscribe", new { channel_id = channelId }, ct);

    public async Task<TextSendAckDto> SendTextAsync(string channelId, string body, CancellationToken ct = default)
    {
        return await RequestAsync<TextSendAckDto>("text.send", new
        {
            channel_id = channelId,
            client_msg_id = Ulid.NewUlid().ToString(),
            body,
        }, ct);
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
        CancellationToken ct = default) =>
        RequestAsync<ScreenAckDto>("media.signal", new
        {
            channel_id = channelId,
            to,
            media,
            kind,
            payload,
        }, ct);

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
        if (_receiveLoop is not null)
        {
            try
            {
                await _receiveLoop;
            }
            catch (Exception)
            {
                // 接收循环在取消时的异常是可预期的。
            }
        }
    }

    // ------------------------------------------------------------------
    // 内部实现
    // ------------------------------------------------------------------

    private async Task OpenAsync(Uri baseUri, string token, CancellationToken ct)
    {
        var builder = new UriBuilder(baseUri)
        {
            Scheme = baseUri.Scheme == Uri.UriSchemeHttps ? "wss" : "ws",
            Path = "/ws",
        };
        await _socket.ConnectAsync(builder.Uri, ct);
        _receiveLoop = Task.Run(ReceiveLoopAsync, CancellationToken.None);

        var auth = await RequestAsync<AuthOkDto>("auth", new { token }, ct);
        Member = auth.Member;
        Server = auth.Server;
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
        var bytes = JsonSerializer.SerializeToUtf8Bytes(envelope, JsonDefaults.Options);
        await _sendGate.WaitAsync(ct);
        try
        {
            await _socket.SendAsync(bytes, WebSocketMessageType.Text, endOfMessage: true, ct);
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
                        Closed?.Invoke("server closed the connection");
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
            // 正常关闭路径。
        }
        catch (WebSocketException ex)
        {
            Closed?.Invoke(ex.Message);
        }
        catch (Exception ex)
        {
            Closed?.Invoke(ex.Message);
        }
    }

    private async Task DispatchAsync(EnvelopeDto envelope)
    {
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
