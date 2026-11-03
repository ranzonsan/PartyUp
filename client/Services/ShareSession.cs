using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using PartyUp.Client.Models;

namespace PartyUp.Client.Services;

/// <summary>
/// 把「媒体核心的信令事件」与「服务端的 media.signal」接起来（docs/PROTOCOL.md §4.2 / §5）。
///
/// 分工：
///   * 共享方：<see cref="StartSharingAsync"/> → `pu_screen_start(id)` 让媒体核心建
///     PeerConnection 并产出 offer；<see cref="HandleMediaEvent"/> 收到 SIGNAL_OUT
///     就经服务端转发（offer 广播、answer/candidate 定向回共享方）。
///   * 观看方：收到 offer 就记住发送者，然后把 payload 原样喂给 `pu_signal_in`（引擎
///     自己生成 answer），后续 answer/candidate 都回给同一个成员。
///   * 远端帧只计数（PU_EVENT_REMOTE_FRAME）：v1 不渲染像素，验收要求是「帧真的到了」。
///
/// 线程模型：`pu_signal_in` 会阻塞（等 SDP 往返，最多几秒），所以入站信令先进
/// Channel，由单独的后台任务串行喂给引擎；绝不在 UI 线程或接收循环里直接调用。
/// </summary>
public sealed class ShareSession : IAsyncDisposable
{
    private readonly MediaEngine _engine;
    private readonly PartyUpConnection _connection;
    private readonly string _channelId;
    private readonly Channel<string> _inbound = Channel.CreateUnbounded<string>(
        new UnboundedChannelOptions { SingleReader = true, SingleWriter = false });
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _inboundLoop;

    private string? _remoteSharerId;
    private string _remoteSharerName = string.Empty;

    // 语音（docs/PROTOCOL.md §4.1；信令复用同一条 media.signal，media=voice）。
    private bool _voiceJoined;
    private bool _voiceMuted;
    private bool _voiceDeafened;
    private readonly Dictionary<string, string> _voiceMembers = new(StringComparer.Ordinal);

    /// <summary>最近一次收到 offer 的对方成员 id：answer/candidate 都要定向回到这个人。</summary>
    private string? _signalPeerId;

    public ShareSession(MediaEngine engine, PartyUpConnection connection, string channelId)
    {
        _engine = engine;
        _connection = connection;
        _channelId = channelId;
        _thisMemberId = connection.Member.Id;
        _thisMemberName = connection.Member.Name;

        connection.MediaSignalReceived += OnMediaSignal;
        connection.ScreenStateChanged += OnScreenState;
        _inboundLoop = Task.Run(InboundLoopAsync);
    }

    private readonly string _thisMemberId;
    private readonly string _thisMemberName;

    /// <summary>本端是否正在共享。</summary>
    public bool IsSharing { get; private set; }

    /// <summary>本端共享的源标题（UI 展示用）。</summary>
    public string SharingSourceTitle { get; private set; } = string.Empty;

    /// <summary>最近一次共享/信令错误（人类可读），成功路径为 null。</summary>
    public string? LastError { get; private set; }

    /// <summary>本端收到的远端帧累计数（验收硬指标）。</summary>
    public int RemoteFrames { get; private set; }

    public uint RemoteWidth { get; private set; }

    public uint RemoteHeight { get; private set; }

    /// <summary>正在观看的共享方名字；空串表示没在观看。</summary>
    public string WatchingName => _remoteSharerName;

    public bool IsWatching => _remoteSharerId is not null;

    // ------------------------------------------------------------------
    // 语音状态（UI 绑定；信令走 media.signal 的 voice 通道）
    // ------------------------------------------------------------------

    /// <summary>本端是否已加入语音。</summary>
    public bool IsInVoice => _voiceJoined;

    public bool IsVoiceMuted => _voiceMuted;

    public bool IsVoiceDeafened => _voiceDeafened;

    /// <summary>语音模式展示文本：v1 只实现了 direct（relay 需要 SFU）。</summary>
    public string VoiceModeText => _voiceJoined ? "direct（P2P 直连）" : "模式：—";

    /// <summary>频道里正在语音的成员（成员 id → 名字），含自己。</summary>
    public IReadOnlyDictionary<string, string> VoiceMembers => _voiceMembers;

    /// <summary>除自己以外是否已有人在语音——后加入者据此改走被动加入，避免双方并发 offer 的 glare。</summary>
    public bool HasOtherVoiceMembers => _voiceMembers.Keys.Any(id => id != _thisMemberId);

    /// <summary>语音成员状态捎带包的类型标记（服务端 v1 没有 voice.state，先走 media.signal 的 voice 通道）。</summary>
    public const string VoicePresenceType = "partyup.voice.presence";

    /// <summary>频道内当前共享列表（screen.state 的最新快照，UI 直接绑定）。</summary>
    public IReadOnlyList<ShareDto> Shares { get; private set; } = Array.Empty<ShareDto>();

    /// <summary>收发到的信令计数（诊断用）。</summary>
    public int SignalsOut { get; private set; }

    public int SignalsIn { get; private set; }

    /// <summary>状态变化（UI 侧注意切回 UI 线程再改控件）。</summary>
    public event Action? Changed;

    // ------------------------------------------------------------------
    // 共享方
    // ------------------------------------------------------------------

    /// <summary>开始共享一个源：先让媒体核心开机，再向服务端登记 screen.start。</summary>
    public async Task StartSharingAsync(MediaSource source, CancellationToken ct = default)
    {
        // 记录真正传给 native 的值：空 id 会被 native 当成合成源，绝不允许静默降级。
        AppLog.Info($"share: start id=\"{source.Id}\" title=\"{source.Title}\" " +
                    $"size={source.Width}x{source.Height} kind={source.Kind}");
        if (string.IsNullOrWhiteSpace(source.Id))
        {
            throw new InvalidOperationException(
                "共享源 id 为空：native 会把空 id 当成合成源，已拒绝启动（请重新选择显示器）");
        }
        LastError = null;
        _engine.StartScreen(source.Id);
        await _connection.StartScreenAsync(
            _channelId,
            new ScreenSourceDto(source.Id, source.Title, (int)source.Width, (int)source.Height, KindName(source.Kind)),
            "direct",
            ct);
        IsSharing = true;
        SharingSourceTitle = source.Title;
        AppLog.Pass($"share: started source=\"{source.Title}\" id={source.Id} size={source.Width}x{source.Height}");
        Changed?.Invoke();
    }

    public async Task StopSharingAsync(CancellationToken ct = default)
    {
        try
        {
            _engine.StopScreen();
        }
        finally
        {
            IsSharing = false;
            SharingSourceTitle = string.Empty;
            await _connection.StopScreenAsync(_channelId, ct);
            AppLog.Info("share: stopped");
            Changed?.Invoke();
        }
    }

    // ------------------------------------------------------------------
    // 事件泵
    // ------------------------------------------------------------------

    /// <summary>由 UI 定时器 / 无头循环调用：处理一条媒体核心事件。</summary>
    public void HandleMediaEvent(MediaEvent ev)
    {
        switch (ev.Kind)
        {
            case MediaEventKind.SignalOut:
            {
                var kind = ev.SignalKind;
                var kindName = kind switch
                {
                    PuSignalKind.Offer => "offer",
                    PuSignalKind.Answer => "answer",
                    PuSignalKind.Candidate => "candidate",
                    _ => null,
                };
                if (kindName is null || string.IsNullOrEmpty(ev.SignalPayload))
                {
                    return;
                }
                SignalsOut++;
                // 语音与屏幕共享共用同一条 media.signal、也共用同一个 PeerConnection，
                // 所以不能用「当前是否在语音」来猜 media：语音时开始共享，第一个 offer 里已经带 video。
                // 直接看 SDP：带 m=video 的 offer/answer 就是屏幕共享的协商，纯音频才是语音。
                var hasVideo = ev.SignalPayload.Contains("m=video", StringComparison.Ordinal);
                var media = hasVideo ? "screen" : (_voiceJoined || IsSharing ? "voice" : "screen");
                // offer 广播给频道；answer/candidate 定向回最后一个给我们发 offer 的人。
                var to = kind == PuSignalKind.Offer ? null : _signalPeerId;
                ForwardAsync(media, kindName, ev.SignalPayload, to);
                break;
            }
            case MediaEventKind.RemoteFrame:
                RemoteFrames++;
                RemoteWidth = ev.RemoteWidth;
                RemoteHeight = ev.RemoteHeight;
                if (RemoteFrames == 1 || RemoteFrames % 30 == 0)
                {
                    AppLog.Pass($"share: remote frame #{ev.RemoteFramesReceived} {ev.RemoteWidth}x{ev.RemoteHeight}");
                    Changed?.Invoke();
                }
                break;
            case MediaEventKind.Error:
                LastError = ev.ErrorMessage;
                AppLog.Fail("share: media error: " + ev.ErrorMessage);
                Changed?.Invoke();
                break;
        }
    }

    private void ForwardAsync(string media, string kind, string payload, string? to)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await _connection.SendMediaSignalAsync(_channelId, media, kind, payload, to);
                AppLog.Info($"media: signal out media={media} kind={kind} to={to ?? "(broadcast)"} bytes={payload.Length}");
            }
            catch (Exception ex)
            {
                LastError = "转发信令失败：" + ex.Message;
                AppLog.Fail("share: send signal failed: " + ex.Message);
                Changed?.Invoke();
            }
        }, CancellationToken.None);
    }

    private void OnMediaSignal(MediaSignalDto signal)
    {
        // 语音成员状态捎带包：本地处理，不喂媒体核心（服务端 v1 没有 voice.state）。
        if (signal.Media == "voice" && TryHandleVoicePresence(signal))
        {
            Changed?.Invoke();
            return;
        }
        if (signal.Media == "voice")
        {
            MarkVoiceMember(signal);
        }
        if (signal.Kind == "offer")
        {
            // 应答要回到「给我们发 offer 的人」——语音与共享共用这一条。
            _signalPeerId = signal.From;
            // SDP 里带 m=video 才是屏幕共享的协商（语音 offer 只有 audio）；不能靠 media 标签，
            // 因为语音中再开始共享时，同一 PC 的重协商 offer 也会被标成 voice。
            var isVideoOffer = signal.Payload.Contains("m=video", StringComparison.Ordinal);
            if (isVideoOffer)
            {
                _remoteSharerId = signal.From;
                _remoteSharerName = string.IsNullOrEmpty(signal.FromName) ? signal.From ?? "?" : signal.FromName;
                RemoteFrames = 0;
            }
            AppLog.Info($"media: offer from {signal.FromName} ({signal.From}) media={signal.Media} " +
                        $"video={isVideoOffer}, hand to media core");
        }
        SignalsIn++;
        _inbound.Writer.TryWrite(signal.Payload);
        Changed?.Invoke();
    }

    // ------------------------------------------------------------------
    // 语音
    // ------------------------------------------------------------------

    /// <summary>
    /// 加入语音。<paramref name="passive"/>=true 时只加音频轨、等对方 offer（后加入的一方必须这样，
    /// 见 <see cref="HasOtherVoiceMembers"/>）。
    /// </summary>
    public async Task JoinVoiceAsync(bool passive, CancellationToken ct = default)
    {
        if (_voiceJoined) return;
        LastError = null;
        var rc = _engine.VoiceJoin(_channelId, mode: 1 /* direct */, passive);
        if (rc != 0)
        {
            LastError = $"pu_voice_join 返回 {rc}：{_engine.LastErrorText()}";
            AppLog.Fail("voice: " + LastError);
            Changed?.Invoke();
            return;
        }
        _voiceJoined = true;
        _voiceMuted = false;
        _voiceDeafened = false;
        _voiceMembers[_thisMemberId] = _thisMemberName;
        AppLog.Pass($"voice: joined channel={_channelId} mode=direct passive={passive}");
        ForwardAsync("voice", "candidate", BuildVoicePresence("join", passive), null);
        Changed?.Invoke();
        await Task.CompletedTask;
    }

    public async Task LeaveVoiceAsync(CancellationToken ct = default)
    {
        if (!_voiceJoined) return;
        try
        {
            _engine.VoiceLeave();
        }
        finally
        {
            _voiceJoined = false;
            _voiceMuted = false;
            _voiceDeafened = false;
            _voiceMembers.Remove(_thisMemberId);
            AppLog.Info("voice: left");
            ForwardAsync("voice", "candidate", BuildVoicePresence("leave", false), null);
            Changed?.Invoke();
        }
        await Task.CompletedTask;
    }

    /// <summary>切换麦克风静音，返回 pu_voice_set_mute 原始返回码（验收要看 OK=0）。</summary>
    public int ToggleVoiceMute()
    {
        if (!_voiceJoined) return (int)PuStatus.State;
        var next = !_voiceMuted;
        var rc = _engine.VoiceSetMute(next);
        if (rc == 0) _voiceMuted = next;
        AppLog.Pass($"voice: set_mute({(next ? 1 : 0)}) rc={rc}");
        Changed?.Invoke();
        return rc;
    }

    /// <summary>切换扬声器静音（deafen），返回 pu_voice_set_deafen 原始返回码。</summary>
    public int ToggleVoiceDeafen()
    {
        if (!_voiceJoined) return (int)PuStatus.State;
        var next = !_voiceDeafened;
        var rc = _engine.VoiceSetDeafen(next);
        if (rc == 0) _voiceDeafened = next;
        AppLog.Pass($"voice: set_deafen({(next ? 1 : 0)}) rc={rc}");
        Changed?.Invoke();
        return rc;
    }

    /// <summary>
    /// 语音成员状态捎带包。服务端 v1 的 presence 只有 online、也没有 voice.state，
    /// 所以先复用 media.signal（media=voice, kind=candidate）传一小段 JSON；
    /// 收端识别到 <see cref="VoicePresenceType"/> 就本地处理、不喂媒体核心。
    /// 服务端实现 voice.state 后应删掉这条路径。
    /// </summary>
    private static string BuildVoicePresence(string action, bool passive) =>
        JsonSerializer.Serialize(new
        {
            type = VoicePresenceType,
            action,
            passive,
        });

    /// <summary>收到语音成员状态包时更新成员表；返回 true 表示这条 payload 不该再喂媒体核心。</summary>
    private bool TryHandleVoicePresence(MediaSignalDto signal)
    {
        if (string.IsNullOrEmpty(signal.Payload) ||
            !signal.Payload.Contains(VoicePresenceType, StringComparison.Ordinal))
        {
            return false;
        }
        try
        {
            using var doc = JsonDocument.Parse(signal.Payload);
            var root = doc.RootElement;
            if (!root.TryGetProperty("type", out var type) ||
                type.GetString() != VoicePresenceType)
            {
                return false;
            }
            var action = root.TryGetProperty("action", out var a) ? a.GetString() : "join";
            var name = string.IsNullOrEmpty(signal.FromName) ? signal.From ?? "?" : signal.FromName;
            if (string.Equals(action, "leave", StringComparison.Ordinal))
            {
                _voiceMembers.Remove(signal.From!);
            }
            else
            {
                _voiceMembers[signal.From!] = name;
            }
            AppLog.Info($"voice: peer presence {action} member={name} ({signal.From})");
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private void MarkVoiceMember(MediaSignalDto signal)
    {
        if (string.IsNullOrEmpty(signal.From)) return;
        var name = string.IsNullOrEmpty(signal.FromName) ? signal.From : signal.FromName;
        _voiceMembers[signal.From] = name;
    }

    private void OnScreenState(ScreenStateDto state)
    {
        Shares = state.Shares;
        // 共享停止时清掉观看状态，UI 才不会一直显示"正在观看"。
        var stillShared = state.Shares.Any(s => s.SharerId == _remoteSharerId);
        if (!stillShared)
        {
            _remoteSharerId = null;
            _remoteSharerName = string.Empty;
            RemoteFrames = 0;
        }
        Changed?.Invoke();
    }

    private async Task InboundLoopAsync()
    {
        try
        {
            await foreach (var payload in _inbound.Reader.ReadAllAsync(_cts.Token))
            {
                // pu_signal_in 同步阻塞：这里已经是后台线程，UI 与 WS 接收循环都不受影响。
                var bytes = Encoding.UTF8.GetBytes(payload);
                var status = _engine.SignalIn(bytes);
                if (status != 0)
                {
                    LastError = $"pu_signal_in 返回 {status}：{_engine.LastErrorText()}";
                    AppLog.Fail("share: " + LastError);
                    Changed?.Invoke();
                }
                else
                {
                    AppLog.Info($"share: signal in applied ({bytes.Length} bytes)");
                }
            }
        }
        catch (OperationCanceledException)
        {
            // 正常关闭。
        }
    }

    private static string KindName(uint kind) => kind switch
    {
        0 => "screen",
        1 => "window",
        2 => "synthetic",
        _ => "screen",
    };

    public async ValueTask DisposeAsync()
    {
        if (_voiceJoined)
        {
            try { _engine.VoiceLeave(); } catch { /* 收尾尽力而为 */ }
            _voiceJoined = false;
        }
        _connection.MediaSignalReceived -= OnMediaSignal;
        _connection.ScreenStateChanged -= OnScreenState;
        _inbound.Writer.TryComplete();
        _cts.Cancel();
        try
        {
            await _inboundLoop.WaitAsync(TimeSpan.FromSeconds(2));
        }
        catch
        {
            // 收尾不阻塞退出。
        }
        _cts.Dispose();
    }
}
