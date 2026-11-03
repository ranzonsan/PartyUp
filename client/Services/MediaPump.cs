namespace PartyUp.Client.Services;

/// <summary>
/// 媒体事件泵：一条专用后台线程用阻塞式 <see cref="MediaEngine.Poll"/> 等事件，逐条交给订阅者。
///
/// 为什么必须有它：Spike 1 的硬结论是「呈现必须由解码回调驱动」——用 UI 定时器轮询
/// 会把显示帧率压到轮询频率（100ms 定时器 = 最高 10fps），且抖动。这里把轮询从 UI 线程
/// 挪到后台线程并阻塞等待，事件到达即处理，帧率只受采集/编码/渲染能力限制。
///
/// 线程约定：<c>onEvent</c> 在泵线程上回调，**不得在其中直接碰 UI 控件**；
/// UI 更新由上层用 <c>Dispatcher.UIThread.Post/Invoke</c> 派发。
/// </summary>
public sealed class MediaPump : IDisposable
{
    private readonly MediaEngine _engine;
    private readonly Action<MediaEvent> _onEvent;
    private Thread? _thread;
    private volatile bool _stop;
    private long _events;

    public MediaPump(MediaEngine engine, Action<MediaEvent> onEvent)
    {
        _engine = engine;
        _onEvent = onEvent;
    }

    /// <summary>已泵出的事件条数（诊断/取证用）。</summary>
    public long Events => Interlocked.Read(ref _events);

    public bool IsRunning => _thread is { IsAlive: true };

    public string ThreadName => _thread?.Name ?? "(not started)";

    public void Start()
    {
        if (_thread is not null)
        {
            return;
        }
        _stop = false;
        _thread = new Thread(Loop)
        {
            IsBackground = true,
            Name = "partyup-media-pump",
            Priority = ThreadPriority.AboveNormal,
        };
        _thread.Start();
        AppLog.Pass("media pump: 后台事件泵已启动（阻塞 poll，非定时器）");
    }

    public void Stop()
    {
        _stop = true;
        var thread = _thread;
        _thread = null;
        if (thread is not null && thread.IsAlive)
        {
            // Poll 最多阻塞 timeoutMs，Join 上限留足；超时就放弃（线程是后台线程，不阻止进程退出）。
            thread.Join(TimeSpan.FromSeconds(2));
        }
    }

    private void Loop()
    {
        AppLog.Info($"media pump: thread started ({Thread.CurrentThread.Name})");
        while (!_stop)
        {
            MediaEvent? ev;
            try
            {
                // 100ms：既能及时出帧，又保证 Stop 后线程能尽快退出。
                ev = _engine.Poll(100);
            }
            catch (ObjectDisposedException)
            {
                return; // 引擎先被销毁：正常退出，不报错
            }
            catch (MediaEngineException ex)
            {
                AppLog.Fail("media pump: poll 失败，线程退出：" + ex.Message);
                return;
            }

            if (ev is null)
            {
                continue; // 超时无事件
            }

            Interlocked.Increment(ref _events);
            try
            {
                _onEvent(ev);
            }
            catch (Exception ex)
            {
                // 单个事件的消费异常不能打死泵，否则后续帧/信令全丢。
                AppLog.Fail("media pump: 处理事件抛异常（已忽略并继续）：" + ex);
            }
        }
        AppLog.Info("media pump: thread stopped");
    }

    public void Dispose() => Stop();
}
