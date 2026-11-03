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
    /// <summary>一条待投递给媒体核心的信令：media 决定进 voice 还是 screen 会话。</summary>
    private readonly record struct InboundSignal(string Media, string Payload);

    private readonly Channel<InboundSignal> _inbound = Channel.CreateUnbounded<InboundSignal>(
        new UnboundedChannelOptions { SingleReader = true, SingleWriter = false });
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _inboundLoop;

    private string? _remoteSharerId;
    private string _remoteSharerName = string.Empty;

    // 语音（docs/PROTOCOL.md §4；relay 的信令对象是服务端，direct 的对象是频道成员）。
    private bool _voiceJoined;
    private bool _voiceMuted;
    private bool _voiceDeafened;
    // 频道传输模式：由**服务端策略**决定（客户端无权选择），来自 voice.state / voice.join.ok。
    // relay = 服务端 SFU（服务端 offerer，本端永远 answerer）；direct = 1:1 直连。
    private string _voiceMode = "relay";
    // 服务端 voice.state 的最新快照：member_id → 静音/闭音状态（比本地猜测可靠）。
    private readonly Dictionary<string, VoiceMemberDto> _voiceMembers = new(StringComparer.Ordinal);
    // relay 槽位 → 成员（服务端下发；direct 为空）。native 远端音轨 id 形如 "partyup-slot-00"。
    private readonly Dictionary<int, VoiceSlotDto> _voiceSlots = new();
    private string _lastSlotSignature = string.Empty;

    /// <summary>
    /// 屏幕共享的传输策略（服务端 --screen-mode，从 voice.state / screen.state 学习）。
    /// 与语音 _voiceMode 完全独立：语音可中继、共享可直连。
    /// </summary>
    private string _screenMode = "direct";

    /// <summary>当前本端共享的源（断线重连后按原状恢复）。</summary>
    private MediaSource? _sharingSource;

    /// <summary>最近一条 offer/answer 属于哪个会话（candidate 没有媒体类型，跟它走）。</summary>
    private string? _lastSignalMedia;

    // 路径变化历史（v1 验收第 7 条）：最近 20 条，新的在前。
    private const int PathHistoryCapacity = 20;
    private readonly List<PathHistoryEntry> _pathHistory = new(capacity: PathHistoryCapacity);
    private string? _lastTransport;
    public event Action? PathHistoryChanged;
    public IReadOnlyList<PathHistoryEntry> PathHistory => _pathHistory;

    // 「谁在说话」：轮询 pu_voice_tracks，peak 超阈值即认为在说话（带 600ms 保持，避免闪烁）。
    private static readonly TimeSpan SpeakPollInterval = TimeSpan.FromMilliseconds(200);
    private static readonly TimeSpan SpeakHoldTime = TimeSpan.FromMilliseconds(600);
    private readonly Dictionary<string, DateTime> _speakingSince = new(StringComparer.Ordinal);
    private CancellationTokenSource? _speakCts;
    private uint _speakThreshold = 500;
    private CancellationTokenSource? _voiceStatsCts;
    /// <summary>direct 模式下强制被动（仅测试/调试用；relay 本来就恒被动）。</summary>
    private readonly bool _forcePassive;

    /// <summary>最近一次收到 offer 的对方成员 id：answer/candidate 都要定向回到这个人。</summary>
    private string? _signalPeerId;

    public ShareSession(MediaEngine engine, PartyUpConnection connection, string channelId,
        bool forcePassive = false)
    {
        _forcePassive = forcePassive;
        _speakThreshold = Program.Options.SpeakThreshold;
        _engine = engine;
        _connection = connection;
        _channelId = channelId;
        _thisMemberId = connection.Member.Id;
        _thisMemberName = connection.Member.Name;

        connection.MediaSignalReceived += OnMediaSignal;
        connection.ScreenStateChanged += OnScreenState;
        connection.VoiceStateChanged += OnVoiceState;
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

    /// <summary>当前传输模式："relay"（服务端 SFU）或 "direct"（P2P 直连）。</summary>
    public string VoiceMode => _voiceMode;

    public bool IsRelayMode => _voiceMode == "relay";

    /// <summary>屏幕共享传输策略（服务端 --screen-mode）。</summary>
    public string ScreenMode => _screenMode;

    /// <summary>屏幕共享策略展示文本。</summary>
    public string ScreenModeText => _screenMode == "relay"
        ? "中继（服务端 SFU 视频通道，暂未实现）"
        : "直连（P2P）";

    /// <summary>语音模式展示文本（服务端策略值；客户端不参与选择）。</summary>
    public string VoiceModeText => _voiceMode == "relay"
        ? "中继（服务端 SFU）"
        : "直连（P2P）";

    /// <summary>频道里正在语音的成员（服务端 voice.state 快照，含静音/闭音），含自己。</summary>
    public IReadOnlyDictionary<string, VoiceMemberDto> VoiceMembers => _voiceMembers;

    /// <summary>relay 槽位映射（槽位序号 → 成员）；direct 模式下为空。</summary>
    public IReadOnlyDictionary<int, VoiceSlotDto> VoiceSlots => _voiceSlots;

    public int VoiceMemberCount => _voiceMembers.Count;

    /// <summary>除自己以外是否已有人在语音（direct 模式下决定谁发 offer；relay 恒为 answerer）。</summary>
    public bool HasOtherVoiceMembers => _voiceMembers.Keys.Any(id => id != _thisMemberId);

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

        // 兜底：屏幕共享中继通道（SFU 视频）还没实现。这里在**碰本地采集和信令之前**就干净失败，
        // 避免「点了个共享按钮，语音就没了」。服务端也会拒（权威），这里是快速失败 + 明确文案。
        if (_screenMode == "relay")
        {
            LastError = "当前服务器为纯 SFU 中继模式，暂不支持屏幕共享（视频通道开发中）";
            AppLog.Fail("share: " + LastError);
            Changed?.Invoke();
            throw new InvalidOperationException(LastError);
        }

        // 顺序很重要：**先让服务端批准，再开本地采集**。
        // 纯 SFU 中继模式（视频通道未就绪）下服务端会回 NOT_IMPLEMENTED；如果先 StartScreen，
        // 原生会给已有语音 PC 挂视频轨并发 offer，把语音一起拖坏（实测 role mismatch → PCM 冻结）。
        try
        {
            await _connection.StartScreenAsync(
                _channelId,
                new ScreenSourceDto(source.Id, source.Title, (int)source.Width, (int)source.Height, KindName(source.Kind)),
                "direct",
                ct);
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            _sharingSource = null;
            IsSharing = false;
            SharingSourceTitle = string.Empty;
            AppLog.Fail("share: server rejected — " + ex.Message);
            Changed?.Invoke();
            throw new InvalidOperationException(ex.Message, ex);
        }

        try
        {
            _engine.StartScreen(source.Id);
        }
        catch (Exception ex)
        {
            // 本地采集起不来：把服务端登记回滚掉，避免别人看到一个永远不会有的共享。
            try { await _connection.StopScreenAsync(_channelId, ct); } catch { /* 尽力而为 */ }
            LastError = "本地采集启动失败：" + ex.Message;
            _sharingSource = null;
            IsSharing = false;
            AppLog.Fail("share: " + LastError);
            Changed?.Invoke();
            throw new InvalidOperationException(LastError, ex);
        }

        _sharingSource = source;
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
            _sharingSource = null;
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
                // media 必须显式标出：语音与屏幕共享各用独立 PeerConnection，服务端/原生侧都靠它
                // 把 offer/answer/candidate 精确投递到对应会话（candidate 信封里没有媒体类型，
                // 靠嗅探只能「两边都试」，属于静默失败）。
                //
                // 现阶段媒体核心的 signal 事件还没带 media 字段，所以这里分类：
                //   - offer/answer 看 SDP：带 m=video 的一定是屏幕会话（语音会话只有 audio）；
                //   - candidate 没有媒体类型，沿用同一轮协商的 media（_lastSignalMedia）。
                // 媒体核心补上事件级 media 后，这里应改为直接读事件字段。
                var hasVideo = ev.SignalPayload.Contains("m=video", StringComparison.Ordinal);
                var media = kind == PuSignalKind.Candidate
                    ? (_lastSignalMedia ?? (hasVideo ? "screen" : "voice"))
                    : (hasVideo ? "screen" : "voice");
                if (kind != PuSignalKind.Candidate) _lastSignalMedia = media;
                // target 由**各自策略**决定：voice 看 _voiceMode，screen 看 _screenMode。
                var sfu = media == "screen" ? _screenMode == "relay" : _voiceMode == "relay";
                var target = sfu ? "sfu" : null;
                // direct：offer 广播；answer/candidate 定向回最后一个给我们发 offer 的人。
                var to = sfu ? null : (kind == PuSignalKind.Offer ? null : _signalPeerId);
                ForwardAsync(media, kindName, ev.SignalPayload, to, target);
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

    private void ForwardAsync(string media, string kind, string payload, string? to, string? target = null)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await _connection.SendMediaSignalAsync(_channelId, media, kind, payload, to, target);
                var where = target ?? to ?? "(broadcast)";
                AppLog.Info($"media: signal out media={media} kind={kind} to={where} bytes={payload.Length}");
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
        // relay 模式：服务端 SFU 是 offer 的对方。原样喂媒体核心（此时原生是 answerer），
        // answer 会由核心经 SignalOut 事件回给我们，再由 ForwardAsync 带 target=sfu 发回去。
        if (signal.Target == "sfu")
        {
            SignalsIn++;
            AppLog.Info($"media: sfu signal in kind={signal.Kind} bytes={signal.Payload.Length}");
            _inbound.Writer.TryWrite(new InboundSignal(signal.Media, signal.Payload));
            Changed?.Invoke();
            return;
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
        _inbound.Writer.TryWrite(new InboundSignal(signal.Media, signal.Payload));
        Changed?.Invoke();
    }

    // ------------------------------------------------------------------
    // 语音
    // ------------------------------------------------------------------

    /// <summary>
    /// 加入语音：先向服务端 voice.join 拿传输模式，再按模式决定本端角色。
    /// relay 模式服务端是 offerer，**本端必须被动加入（answerer）**；direct 模式沿用老逻辑
    /// （频道里已有人就被动加入，避免双方并发 offer 的 glare）。
    /// </summary>
    public async Task JoinVoiceAsync(bool afterReconnect = false, CancellationToken ct = default)
    {
        if (_voiceJoined) return;
        LastError = null;

        // 顺序很重要：先让原生把（answerer 角色的）PeerConnection 建好，再向服务端 voice.join。
        // relay 模式下服务端收到 join 会**立刻**下发 offer；如果原生还没 join，
        // 这条 offer 就找不到可应答的 PC（实测报：CreateAnswer failed ... 不是 have-remote-offer）。
        var passive = ShouldBePassive();
        // 原生侧只实现 P2P；对媒体核心来说 relay 就是「对面只有一个 peer」，所以 mode 恒传 1。
        var rc = _engine.VoiceJoin(_channelId, mode: 1, passive);
        if (rc != 0)
        {
            LastError = $"pu_voice_join 返回 {rc}：{_engine.LastErrorText()}";
            AppLog.Fail("voice: " + LastError);
            Changed?.Invoke();
            return;
        }

        try
        {
            // 传输方式由服务端策略决定：请求里不带 mode，只读取应答。
            var ack = await _connection.VoiceJoinAsync(_channelId, ct);
            var transport = IsDirectTransport(ack.Transport) ? "direct" : "relay";
            if (transport != _voiceMode)
            {
                // 本地缓存的服务端策略与应答不一致（例如订阅快照还没到）：按应答重建一次会话。
                AppLog.Info($"voice: server transport={transport} (cached {_voiceMode}), rebuilding local session");
                _engine.VoiceLeave();
                _voiceMode = transport;
                passive = ShouldBePassive();
                rc = _engine.VoiceJoin(_channelId, mode: 1, passive);
                if (rc != 0)
                {
                    LastError = $"pu_voice_join 返回 {rc}：{_engine.LastErrorText()}";
                    AppLog.Fail("voice: " + LastError);
                    Changed?.Invoke();
                    return;
                }
            }
            else
            {
                _voiceMode = transport;
            }
        }
        catch (Exception ex)
        {
            try { _engine.VoiceLeave(); } catch { /* 收尾尽力而为 */ }
            LastError = "voice.join 失败：" + ex.Message;
            AppLog.Fail("voice: " + LastError);
            Changed?.Invoke();
            return;
        }

        _voiceJoined = true;
        _voiceMuted = false;
        _voiceDeafened = false;
        LastError = null;
        AppLog.Pass($"voice: joined channel={_channelId} mode={_voiceMode} passive={passive}");

        // 路径变化历史：重连重新协商一定记一条（即使两端都是 relay，也要能看到「重连后重新协商」）；
        // 其余情况只在传输方式真的变了时记（目前只有服务端策略变更会触发）。
        if (afterReconnect)
        {
            RecordPathChange(_lastTransport ?? _voiceMode, _voiceMode, "重连后重新协商");
        }
        else if (_lastTransport is not null && _lastTransport != _voiceMode)
        {
            RecordPathChange(_lastTransport, _voiceMode, "服务端策略变更");
        }
        _lastTransport = _voiceMode;

        StartVoiceStatsLoop();
        StartSpeakingLoop();
        Changed?.Invoke();
    }

    /// <summary>
    /// 每 2 秒打一条语音统计 + 每条活跃轨的明细（v2.7 按轨统计，relay 多路各算各的）。
    /// 静音帧已被原生侧门控掉，所以这里的帧数能反映「真的收到声音」。
    /// </summary>
    private void StartVoiceStatsLoop()
    {
        StopVoiceStatsLoop();
        var cts = new CancellationTokenSource();
        _voiceStatsCts = cts;
        _ = Task.Run(async () =>
        {
            while (!cts.IsCancellationRequested)
            {
                try
                {
                    var stats = _engine.GetStats();
                    AppLog.Info($"voice: stats pcmRx={stats.FramesReceived} peerState={stats.PeerState} mode={_voiceMode}");
                    foreach (var track in _engine.GetVoiceTracks())
                    {
                        AppLog.Info($"voice: track id={track.Id} pcmFrames={track.PcmFrames} peak={track.Peak} slotOwner={SlotOwnerOf(track.Id)}");
                    }
                }
                catch (Exception ex)
                {
                    AppLog.Warn("voice: stats failed: " + ex.Message);
                    return;
                }
                try { await Task.Delay(2000, cts.Token); }
                catch { return; }
            }
        }, cts.Token);
    }

    private void StopVoiceStatsLoop()
    {
        _voiceStatsCts?.Cancel();
        _voiceStatsCts = null;
    }

    // ------------------------------------------------------------------
    // 路径变化历史（v1 验收第 7 条）
    // ------------------------------------------------------------------

    /// <summary>记一条路径变化（新的在前，最多保留 20 条）。</summary>
    public void RecordPathChange(string from, string to, string reason)
    {
        var entry = new PathHistoryEntry(DateTimeOffset.Now, TransportLabel(from), TransportLabel(to), reason);
        _pathHistory.Insert(0, entry);
        if (_pathHistory.Count > PathHistoryCapacity)
        {
            _pathHistory.RemoveRange(PathHistoryCapacity, _pathHistory.Count - PathHistoryCapacity);
        }
        AppLog.Pass($"voice: path {entry.From} → {entry.To}（{reason}）");
        PathHistoryChanged?.Invoke();
    }

    /// <summary>传输方式的中文展示名。</summary>
    public static string TransportLabel(string? transport) =>
        string.Equals(transport, "direct", StringComparison.OrdinalIgnoreCase) ? "直连" : "中继";

    // ------------------------------------------------------------------
    // 谁在说话（v2.7：pu_voice_tracks 按轨统计 + voice.state.slots 槽位映射）
    // ------------------------------------------------------------------

    /// <summary>当前正在说话的成员 id 集合（UI 用来点亮成员行）。</summary>
    public IReadOnlyCollection<string> SpeakingMemberIds => _speakingSince.Keys;

    public bool IsSpeaking(string memberId) => _speakingSince.ContainsKey(memberId);

    /// <summary>把 "partyup-slot-<n>" 翻译成成员 id；不是槽位轨或没有映射时返回 null。</summary>
    public string? SlotOwnerOf(string trackId)
    {
        if (!trackId.StartsWith("partyup-slot-", StringComparison.Ordinal)) return null;
        if (!int.TryParse(trackId.AsSpan("partyup-slot-".Length), out var index)) return null;
        return _voiceSlots.TryGetValue(index, out var slot) ? slot.MemberId : null;
    }

    private void StartSpeakingLoop()
    {
        StopSpeakingLoop();
        var cts = new CancellationTokenSource();
        _speakCts = cts;
        _ = Task.Run(async () =>
        {
            while (!cts.IsCancellationRequested)
            {
                try
                {
                    PollSpeaking();
                }
                catch (Exception ex)
                {
                    AppLog.Warn("voice: speaking poll failed: " + ex.Message);
                    return;
                }
                try { await Task.Delay(SpeakPollInterval, cts.Token); }
                catch { return; }
            }
        }, cts.Token);
    }

    private void StopSpeakingLoop()
    {
        _speakCts?.Cancel();
        _speakCts = null;
    }

    /// <summary>
    /// 轮询远端音频轨：peak 超阈值（默认 500，可用 --speak-threshold 或
    /// PARTYUP_SPEAK_THRESHOLD 调）就认为该轨在说话；保持 600ms 避免闪烁。
    /// relay 下轨道 id 是 "partyup-slot-&lt;n&gt;"，查 voice.state.slots 得到成员；
    /// direct 下只有一路对端，直接算到「唯一的其他成员」。
    /// </summary>
    private void PollSpeaking()
    {
        if (!_voiceJoined) return;
        var tracks = _engine.GetVoiceTracks();
        var now = DateTime.UtcNow;
        var active = new HashSet<string>(StringComparer.Ordinal);
        foreach (var track in tracks)
        {
            if (track.Peak < _speakThreshold) continue;
            var member = SlotOwnerOf(track.Id);
            if (member is null && _voiceMode != "relay")
            {
                member = _voiceMembers.Keys.FirstOrDefault(id => id != _thisMemberId);
            }
            // 静音成员（服务端 voice.state.muted）一律不点亮：即使静音后仍有低电平噪声帧穿过原生门控，
            // 也不该让用户看到「已静音的人还在说话」。
            if (member is not null && _voiceMembers.TryGetValue(member, out var state) && state.Muted) continue;
            if (member is not null) active.Add(member);
        }

        var changed = false;
        foreach (var member in active)
        {
            if (!_speakingSince.ContainsKey(member))
            {
                _speakingSince[member] = now;
                changed = true;
                AppLog.Info($"voice: speaking {member} peak>={_speakThreshold}");
            }
            else
            {
                _speakingSince[member] = now;
            }
        }
        // 保持窗口：最后一次「超阈值」超过 SpeakHoldTime 才熄灭。
        foreach (var member in _speakingSince.Keys.ToList())
        {
            if (now - _speakingSince[member] > SpeakHoldTime)
            {
                _speakingSince.Remove(member);
                changed = true;
            }
        }
        if (changed)
        {
            Changed?.Invoke();
        }
    }

    /// <summary>
    /// 控制连接恢复后的媒体全量重建。语义上「断线重连 = 新会话」：
    /// 服务端 PeerConnection 已随进程销毁，旧 PC 的 DTLS/ICE 凭据全部作废，
    /// 所以不能只重 join 语音——共享那条 PC 同样是死的，必须一起重建。
    /// 顺序按契约来：停共享（释放 PC）→ 语音 leave → 重新 voice.join → 恢复共享原 source。
    /// 媒体引擎本身不重建（pu_engine 仍复用，只是 PC 由 VoiceLeave 拆掉后由 VoiceJoin 重建）。
    /// </summary>
    public async Task HandleConnectionRestoredAsync(CancellationToken ct = default)
    {
        var wasInVoice = _voiceJoined;
        var wasSharing = IsSharing;
        var sharedSource = _sharingSource;
        if (!wasInVoice && !wasSharing) return;

        AppLog.Info($"media: connection restored, rebuilding session " +
                    $"(voice={wasInVoice} sharing={wasSharing} source=\"{sharedSource?.Title}\")");
        StopVoiceStatsLoop();
        StopSpeakingLoop();
        _speakingSince.Clear();

        try
        {
            // 1) 共享先停：只停本地采集与 PC，不再向旧服务端上报（那边会话已经没了）。
            if (wasSharing)
            {
                try { _engine.StopScreen(); } catch (Exception ex) { AppLog.Warn("share: stop before rebuild failed: " + ex.Message); }
                IsSharing = false;
                SharingSourceTitle = string.Empty;
            }

            // 2) 语音 leave：原生会移除 mic 轨；配合 encoder-dev 的「非共享时拆 PC」，下一次 join 会拿到全新 PC。
            if (wasInVoice)
            {
                try { _engine.VoiceLeave(); } catch (Exception ex) { AppLog.Warn("voice: leave before rejoin failed: " + ex.Message); }
                _voiceJoined = false;
            }

            // 3) 重新 voice.join（先 pu_voice_join 建好 passive PC，再向服务端发 voice.join 等 offer）。
            if (wasInVoice)
            {
                await JoinVoiceAsync(afterReconnect: true, ct);
            }

            // 4) 重新共享原 source（用户不该因为网抖一下就要手动重新点共享）。
            if (wasSharing && sharedSource is not null)
            {
                await StartSharingAsync(sharedSource, ct);
                AppLog.Pass($"share: restored after reconnect source=\"{sharedSource.Title}\"");
            }
        }
        catch (Exception ex)
        {
            LastError = "重连后媒体重建失败：" + ex.Message;
            AppLog.Fail("media: " + LastError);
            Changed?.Invoke();
        }
    }

    /// <summary>
    /// direct 模式下谁发 offer：成员 id 最小的当 offerer，其余被动等待（确定性，不靠抢）。
    /// relay 模式恒为被动（服务端 SFU 才是 offerer）。
    /// </summary>
    private bool ShouldBePassive()
    {
        if (_voiceMode == "relay") return true; // 服务端 SFU 才是 offerer
        if (_forcePassive) return true;
        if (!HasOtherVoiceMembers) return false;
        var first = _voiceMembers.Keys.OrderBy(id => id, StringComparer.Ordinal).FirstOrDefault();
        return first is not null && first != _thisMemberId;
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
            StopVoiceStatsLoop();
            StopSpeakingLoop();
            _speakingSince.Clear();
            _voiceJoined = false;
            _voiceMuted = false;
            _voiceDeafened = false;
            AppLog.Info("voice: left");
            try { await _connection.VoiceLeaveAsync(_channelId, ct); }
            catch (Exception ex) { AppLog.Warn("voice: leave report failed: " + ex.Message); }
            Changed?.Invoke();
        }
    }

    /// <summary>切换麦克风静音，返回 pu_voice_set_mute 原始返回码（验收要看 OK=0）。</summary>
    public int ToggleVoiceMute()
    {
        if (!_voiceJoined) return (int)PuStatus.State;
        var next = !_voiceMuted;
        var rc = _engine.VoiceSetMute(next);
        if (rc == 0)
        {
            _voiceMuted = next;
            ReportVoiceState(() => _connection.VoiceSetMuteAsync(_channelId, next));
        }
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
        if (rc == 0)
        {
            _voiceDeafened = next;
            ReportVoiceState(() => _connection.VoiceSetDeafenAsync(_channelId, next));
        }
        AppLog.Pass($"voice: set_deafen({(next ? 1 : 0)}) rc={rc}");
        Changed?.Invoke();
        return rc;
    }

    /// <summary>把静音/闭音上报给服务端，让 voice.state 能告诉别人（失败只记日志，不影响本地）。</summary>
    private void ReportVoiceState(Func<Task> send)
    {
        _ = Task.Run(async () =>
        {
            try { await send(); }
            catch (Exception ex) { AppLog.Warn("voice: report state failed: " + ex.Message); }
        }, CancellationToken.None);
    }

    /// <summary>
    /// voice.state 推送：更新成员快照（谁在语音、谁静音）与**服务端传输策略**。
    /// 模式是只读的：客户端无权修改，所以这里不做任何「按模式重建会话」的动作。
    /// </summary>
    private void OnVoiceState(VoiceStateDto state)
    {
        if (state.ChannelId != _channelId) return;
        _voiceMembers.Clear();
        foreach (var m in state.Members) _voiceMembers[m.MemberId] = m;
        _voiceSlots.Clear();
        if (state.Slots is { Count: > 0 } slots)
        {
            foreach (var slot in slots)
            {
                if (slot.Index < 0) continue;
                _voiceSlots[slot.Index] = slot;
            }
            // 只在映射变化时打一行：这是「谁在说话」把 track id 翻译成名字的依据。
            var desc = string.Join(", ", _voiceSlots.OrderBy(kv => kv.Key)
                .Select(kv => $"slot{kv.Key}->{kv.Value.MemberId}" + (kv.Value.Muted ? "(muted)" : "")));
            if (desc != _lastSlotSignature)
            {
                _lastSlotSignature = desc;
                AppLog.Info($"voice: slots {_voiceSlots.Count} [{desc}]");
            }
        }
        var serverMode = IsDirectTransport(state.VoiceMode) ? "direct" : "relay";
        if (!_voiceJoined && serverMode != _voiceMode)
        {
            _voiceMode = serverMode;
            AppLog.Info($"voice: server policy voice_mode={_voiceMode}");
        }
        // 屏幕共享策略独立：无论是否在语音都要学，UI 才能在点共享前就显示/拦截。
        var screenMode = IsDirectTransport(state.ScreenMode) ? "direct" : "relay";
        if (screenMode != _screenMode)
        {
            _screenMode = screenMode;
            AppLog.Info($"share: server policy screen_mode={_screenMode}");
        }
        Changed?.Invoke();
    }

    private static bool IsDirectTransport(string? transport) =>
        string.Equals(transport, "direct", StringComparison.OrdinalIgnoreCase);


    private void OnScreenState(ScreenStateDto state)
    {
        if (!string.IsNullOrEmpty(state.Mode))
        {
            var screenMode = IsDirectTransport(state.Mode) ? "direct" : "relay";
            if (screenMode != _screenMode)
            {
                _screenMode = screenMode;
                AppLog.Info($"share: server policy screen_mode={_screenMode}");
            }
        }
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
            await foreach (var signal in _inbound.Reader.ReadAllAsync(_cts.Token))
            {
                // pu_signal_in_media 同步阻塞：这里已经是后台线程，UI 与 WS 接收循环都不受影响。
                // media 决定进 voice 还是 screen 会话：语音/共享是两条独立 PeerConnection。
                var bytes = Encoding.UTF8.GetBytes(signal.Payload);
                int status;
                try
                {
                    status = _engine.SignalIn(signal.Media, bytes);
                }
                catch (InvalidOperationException ex)
                {
                    // 能力缺失（DLL 还没有 pu_signal_in_media）：明确报错、不进静默降级。
                    LastError = ex.Message;
                    AppLog.Fail("media: " + ex.Message);
                    Changed?.Invoke();
                    _inbound.Writer.TryComplete();
                    return;
                }
                if (status != 0)
                {
                    var err = _engine.LastErrorText();
                    // 切模式/重建会话时，旧会话在飞的 answer 会落到刚建好的 PC 上，
                    // 报 "Called in wrong state: stable"。这是预期内的陈旧信令，不该显示成红色错误。
                    if (err.Contains("Called in wrong state", StringComparison.Ordinal))
                    {
                        AppLog.Info($"share: stale signal ignored ({bytes.Length} bytes): {err}");
                    }
                    else
                    {
                        LastError = $"pu_signal_in 返回 {status}：{err}";
                        AppLog.Fail("share: " + LastError);
                        Changed?.Invoke();
                    }
                }
                else
                {
                    AppLog.Info($"media: signal in applied media={signal.Media} ({bytes.Length} bytes)");
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
            StopVoiceStatsLoop();
            try { _engine.VoiceLeave(); } catch { /* 收尾尽力而为 */ }
            _voiceJoined = false;
        }
        _connection.MediaSignalReceived -= OnMediaSignal;
        _connection.ScreenStateChanged -= OnScreenState;
        _connection.VoiceStateChanged -= OnVoiceState;
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


/// <summary>一条路径变化记录（v1 验收第 7 条：时间 / 从 / 到 / 原因）。</summary>
public sealed record PathHistoryEntry(DateTimeOffset At, string From, string To, string Reason)
{
    public override string ToString() => $"{At:HH:mm:ss} {From} → {To}（{Reason}）";
}
