using System.Text;
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

    public ShareSession(MediaEngine engine, PartyUpConnection connection, string channelId)
    {
        _engine = engine;
        _connection = connection;
        _channelId = channelId;
        _thisMemberId = connection.Member.Id;

        connection.MediaSignalReceived += OnMediaSignal;
        connection.ScreenStateChanged += OnScreenState;
        _inboundLoop = Task.Run(InboundLoopAsync);
    }

    private readonly string _thisMemberId;

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
                // offer 广播给频道（观看方据此应答）；answer/candidate 只回给共享方。
                var to = kind == PuSignalKind.Offer ? null : _remoteSharerId;
                ForwardAsync(kindName, ev.SignalPayload, to);
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

    private void ForwardAsync(string kind, string payload, string? to)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await _connection.SendMediaSignalAsync(_channelId, "screen", kind, payload, to);
                AppLog.Info($"share: signal out kind={kind} to={to ?? "(broadcast)"} bytes={payload.Length}");
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
        if (signal.Kind == "offer")
        {
            // 谁是共享方由服务端信封里的 from 决定；应答要回到这个人。
            _remoteSharerId = signal.From;
            _remoteSharerName = string.IsNullOrEmpty(signal.FromName) ? signal.From ?? "?" : signal.FromName;
            AppLog.Info($"share: offer from {_remoteSharerName} ({signal.From}), hand to media core");
        }
        SignalsIn++;
        _inbound.Writer.TryWrite(signal.Payload);
        Changed?.Invoke();
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
