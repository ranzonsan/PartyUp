using System.Reflection;
using System.Runtime.InteropServices;

namespace PartyUp.Client.Services;

/// <summary>媒体核心调用失败（DLL 缺失/加载失败/返回负值）。</summary>
public sealed class MediaEngineException : Exception
{
    public MediaEngineException(string message) : base(message) { }
}

/// <summary>
/// 从 pu_event 里摊平出来的一条事件。
///
/// 状态迁移用 State/PreviousState/Reason；错误用 ErrorMessage；信令用
/// SignalKind/SignalPayload（payload 在 Map 里就复制，指针不越过本次 poll）；
/// 远端帧用 Remote* 四个字段。
/// </summary>
public sealed record MediaEvent(
    MediaEventKind Kind,
    uint RawType,
    long TimestampUs,
    int State,
    int PreviousState,
    int Reason,
    string? ErrorMessage)
{
    public PuStreamState StateValue => (PuStreamState)State;

    public PuStreamState PreviousStateValue => (PuStreamState)PreviousState;

    /// <summary>PU_EVENT_SIGNAL_OUT：offer / answer / candidate。</summary>
    public PuSignalKind SignalKind { get; init; }

    /// <summary>PU_EVENT_SIGNAL_OUT：媒体核心产出的原始 JSON 信封（已复制成托管字符串）。</summary>
    public string? SignalPayload { get; init; }

    /// <summary>PU_EVENT_REMOTE_FRAME：该引擎收到的第 N 帧（0 起）。</summary>
    public ulong RemoteSequence { get; init; }

    /// <summary>PU_EVENT_REMOTE_FRAME：累计收到帧数（= RemoteSequence + 1）。</summary>
    public uint RemoteFramesReceived { get; init; }

    public uint RemoteWidth { get; init; }

    public uint RemoteHeight { get; init; }
}

/// <summary>pu_screen_sources 的一项（骨架 UI 只用 id/title 展示，不启动共享）。</summary>
public sealed record MediaSource(string Id, string Title, uint Width, uint Height, uint Kind);

/// <summary>
/// 单条远端音频轨的统计（pu_voice_tracks）。relay 下 Id 形如 "partyup-slot-&lt;n&gt;"，
/// 用 <see cref="ShareSession.VoiceSlots"/> 的映射翻译成成员。
/// </summary>
public sealed record VoiceTrackStats(string Id, uint PcmFrames, uint Peak, ulong LastFrameMs);

/// <summary>
/// 媒体核心（partyup_media.dll，ABI v2）的托管包装。
///
/// 设计要点：
///   * 显式从 AppContext.BaseDirectory 加载 DLL（不靠 PATH 搜索），避免被同名旧库抢走——
///     这是 Spike 1 记下的坑；
///   * 加载后校验 18 个导出全部可解析（ABI 契约的运行时检查）；
///   * 只封装骨架要用的 create/destroy/poll_event/peer_stats/screen_start|stop/sources，
///     其余导出的签名也一并声明（NativeMethods），但未接进 UI——语音/共享不在本轮范围。
/// </summary>
public sealed class MediaEngine : IDisposable
{
    /// <summary>ABI v2 的 18 个导出，顺序与 partyup_media.h 的声明顺序一致。</summary>
    public static readonly IReadOnlyList<string> ExportNames = new[]
    {
        "pu_engine_create",
        "pu_engine_destroy",
        "pu_screen_start",
        "pu_screen_stop",
        "pu_poll_event",
        "pu_last_error",
        "pu_signal_in",
        "pu_peer_stats",
        "pu_voice_join",
        "pu_voice_leave",
        "pu_voice_set_mode",
        "pu_voice_set_mute",
        "pu_voice_set_deafen",
        "pu_screen_sources",
        "pu_screen_watch",
        "pu_screen_unwatch",
        "pu_frame_acquire",
        "pu_frame_release",
    };

    private static readonly object ResolverGate = new();
    private static bool _resolverInstalled;
    private static string? _resolvedPath;

    private IntPtr _engine;
    private IntPtr _handle;
    private bool _disposed;

    private MediaEngine(string libraryPath, IntPtr handle, IReadOnlyList<string> missingExports,
        bool hasMediaSignal, bool hasScreenRelay)
    {
        LibraryPath = libraryPath;
        _handle = handle;
        MissingExports = missingExports;
        HasMediaSignalSupport = hasMediaSignal;
        HasScreenRelaySupport = hasScreenRelay;
    }

    /// <summary>
    /// <c>pu_frame_acquire</c> 的远端流 id：观看方要渲染的那一路解码帧。
    /// 与 native 侧约定（encoder-dev 实现），改名要两边一起改。
    /// </summary>
    public const string RemoteStreamId = "remote";

    public string LibraryPath { get; }

    public IReadOnlyList<string> MissingExports { get; }

    public bool IsCreated => _engine != IntPtr.Zero;

    /// <summary>默认 DLL 位置：<c>&lt;应用目录&gt;/partyup_media.dll</c>（构建时从 src/media-core/out 拷入）。</summary>
    public static string DefaultLibraryPath => Path.Combine(AppContext.BaseDirectory, "partyup_media.dll");

    /// <summary>
    /// 加载媒体核心。显式路径 &gt; 环境变量 PARTYUP_MEDIA_DLL &gt; 应用目录。
    /// 加载成功但缺导出时也返回实例，由调用方决定是否降级（MissingExports 非空）。
    /// </summary>
    public static MediaEngine Load(string? explicitPath = null)
    {
        var path = explicitPath;
        if (string.IsNullOrWhiteSpace(path))
        {
            path = Environment.GetEnvironmentVariable("PARTYUP_MEDIA_DLL");
        }
        if (string.IsNullOrWhiteSpace(path))
        {
            path = DefaultLibraryPath;
        }
        path = Path.GetFullPath(path);

        if (!File.Exists(path))
        {
            throw new MediaEngineException(
                $"找不到媒体核心 DLL：{path}。先运行 scripts/build-media-dll.ps1，或用 PARTYUP_MEDIA_DLL 指定路径。");
        }

        lock (ResolverGate)
        {
            _resolvedPath = path;
            if (!_resolverInstalled)
            {
                NativeLibrary.SetDllImportResolver(typeof(MediaEngine).Assembly, ResolveLibrary);
                _resolverInstalled = true;
            }
        }

        if (!NativeLibrary.TryLoad(path, out var handle))
        {
            throw new MediaEngineException($"LoadLibrary 失败：{path}");
        }

        var missing = ExportNames
            .Where(name => !NativeLibrary.TryGetExport(handle, name, out _))
            .ToArray();
        // 可选导出（加法式 ABI）：有就用，没有就明确报缺，不静默降级。
        var hasMediaSignal = NativeLibrary.TryGetExport(handle, "pu_signal_in_media", out _);
        if (!hasMediaSignal)
        {
            AppLog.Fail("media: DLL 缺少 pu_signal_in_media（ABI v2 第 20 个导出）——" +
                        "语音/屏幕共享信令无法按会话路由，需要新 DLL");
        }
        // 可选导出（SFU 屏幕共享，ABI v2.11）：两个都要有才认为可用。
        var hasScreenRelay = NativeLibrary.TryGetExport(handle, "pu_screen_start_ex", out _) &&
                             NativeLibrary.TryGetExport(handle, "pu_screen_prepare", out _);
        if (!hasScreenRelay)
        {
            AppLog.Warn("media: DLL 缺少 pu_screen_start_ex / pu_screen_prepare（ABI v2.11）——" +
                        "screen_mode=relay 的屏幕共享不可用");
        }
        return new MediaEngine(path, handle, missing, hasMediaSignal, hasScreenRelay);
    }

    /// <summary>DllImport 解析器：只接管 partyup_media，其余交给默认逻辑。</summary>
    private static IntPtr ResolveLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, NativeMethods.Library, StringComparison.Ordinal))
        {
            return IntPtr.Zero;
        }
        var candidate = _resolvedPath ?? DefaultLibraryPath;
        return File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var handle) ? handle : IntPtr.Zero;
    }

    /// <summary>创建引擎。返回时事件队列已就绪；事件要用 <see cref="Poll"/> 主动取。</summary>
    public void Create(string clientName = "partyup-ui")
    {
        ThrowIfDisposed();
        if (_engine != IntPtr.Zero)
        {
            throw new MediaEngineException("引擎已创建，不能重复 pu_engine_create");
        }

        var namePtr = Marshal.StringToCoTaskMemUTF8(clientName);
        try
        {
            var config = new PuConfig
            {
                StructSize = (uint)Marshal.SizeOf<PuConfig>(),
                ClientName = namePtr,
                LogLevel = 3,
                EventQueueCapacity = 256,
            };
            var engine = NativeMethods.pu_engine_create(in config);
            if (engine == IntPtr.Zero)
            {
                throw new MediaEngineException("pu_engine_create 返回 NULL：" + LastErrorText());
            }
            _engine = engine;
        }
        finally
        {
            Marshal.FreeCoTaskMem(namePtr);
        }
    }

    /// <summary>非阻塞/阻塞取一条事件。<paramref name="timeoutMs"/> &lt; 0 表示一直等。无事件返回 null。</summary>
    public MediaEvent? Poll(int timeoutMs = 0)
    {
        ThrowIfDisposed();
        EnsureCreated();

        // struct_size 必须由调用方填好：库用它与 sizeof(pu_event) 比对，判定客户端结构是否够新。
        var ev = new PuEvent { StructSize = (uint)EventStructSize };
        var status = NativeMethods.pu_poll_event(_engine, ref ev, timeoutMs);
        if (status == (int)PuStatus.Timeout)
        {
            return null;
        }
        if (status != (int)PuStatus.Ok)
        {
            throw new MediaEngineException($"pu_poll_event 返回 {status}：{LastErrorText()}");
        }
        return Map(ev);
    }

    /// <summary>pu_event 在 C 侧 sizeof 为 256；这里暴露给自检做布局断言。</summary>
    public static int EventStructSize { get; } = Marshal.SizeOf<PuEvent>();

    /// <summary>pu_stats 在 C 侧 sizeof 为 64。</summary>
    public static int StatsStructSize { get; } = Marshal.SizeOf<PuStats>();

    /// <summary>开始一路内置合成源共享（不需要真实屏幕，也不改动窗口内容）。</summary>
    public void StartSynthetic()
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_start(_engine, "synthetic"), "pu_screen_start(synthetic)");
    }

    /// <summary>开始共享指定屏幕（id 来自 <see cref="EnumerateSources"/> 或 "\\.\DISPLAY1"）。</summary>
    public void StartScreen(string screenId)
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_start(_engine, screenId), "pu_screen_start");
    }

    /// <summary>媒体核心是否支持被动屏幕会话（SFU 屏幕共享，ABI v2.11）。</summary>
    public bool HasScreenRelaySupport { get; private set; }

    /// <summary>
    /// 开始共享，可指定角色。<paramref name="passive"/>=true 时（SFU 中继）：只采屏 + 建视频轨，
    /// **不生成 offer**，等对端 offer 经 <see cref="SignalIn(string, byte[])"/> 灌进来。 
    /// </summary>
    public void StartScreenPassive(string screenId)
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasScreenRelaySupport)
        {
            throw new InvalidOperationException(
                "媒体核心缺少 pu_screen_start_ex：需要 ABI v2.11 的 DLL（SFU 屏幕共享）");
        }
        Check(NativeMethods.pu_screen_start_ex(_engine, screenId, 0x1u), "pu_screen_start_ex(passive)");
    }

    /// <summary>观看者预建屏幕会话（answerer），不采屏、不建轨；幂等。</summary>
    public void PrepareScreenSession()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasScreenRelaySupport)
        {
            throw new InvalidOperationException(
                "媒体核心缺少 pu_screen_prepare：需要 ABI v2.11 的 DLL（SFU 屏幕共享）");
        }
        Check(NativeMethods.pu_screen_prepare(_engine), "pu_screen_prepare");
    }

    /// <summary>停止共享（幂等）。</summary>
    public void StopScreen()
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_stop(_engine), "pu_screen_stop");
    }

    /// <summary>读引擎统计快照（微秒级，不发异步 GetStats）。</summary>
    internal PuStats GetStats()
    {
        ThrowIfDisposed();
        EnsureCreated();
        var stats = new PuStats { StructSize = (uint)Marshal.SizeOf<PuStats>() };
        Check(NativeMethods.pu_peer_stats(_engine, ref stats), "pu_peer_stats");
        return stats;
    }

    /// <summary>
    /// 读远端音频按轨统计（v2.7）。只返回活跃轨（近 1 秒内有过非静音帧），
    /// 每条带 id（relay 下是 "partyup-slot-&lt;n&gt;"）、累计有声帧数、最近一帧峰值。
    /// 用于「谁在说话」与按人排查质量；direct 模式下只有一条。
    /// </summary>
    public IReadOnlyList<VoiceTrackStats> GetVoiceTracks()
    {
        ThrowIfDisposed();
        EnsureCreated();
        var count = NativeMethods.pu_voice_tracks(_engine, null, 0);
        if (count <= 0) return Array.Empty<VoiceTrackStats>();

        var buffer = new PuVoiceTrack[count];
        for (var i = 0; i < buffer.Length; i++)
        {
            buffer[i].StructSize = (uint)Marshal.SizeOf<PuVoiceTrack>();
            buffer[i].Id = new byte[64];
        }
        var written = NativeMethods.pu_voice_tracks(_engine, buffer, (uint)buffer.Length);
        if (written <= 0) return Array.Empty<VoiceTrackStats>();

        var result = new List<VoiceTrackStats>(written);
        for (var i = 0; i < written && i < buffer.Length; i++)
        {
            var raw = buffer[i];
            var len = Array.IndexOf(raw.Id, (byte)0);
            if (len < 0) len = raw.Id.Length;
            result.Add(new VoiceTrackStats(
                System.Text.Encoding.UTF8.GetString(raw.Id, 0, len),
                raw.PcmFrames,
                raw.Peak,
                raw.LastFrameMs));
        }
        return result;
    }

    /// <summary>枚举可共享的屏幕/窗口（v2.3 起返回真实显示器 + 末尾 synthetic 项）。</summary>
    public IReadOnlyList<MediaSource> EnumerateSources()
    {
        ThrowIfDisposed();
        EnsureCreated();

        var list = new PuSourceList { StructSize = (uint)Marshal.SizeOf<PuSourceList>() };
        Check(NativeMethods.pu_screen_sources(_engine, ref list), "pu_screen_sources");

        var stride = Marshal.SizeOf<PuSourceInfo>();
        var sources = new List<MediaSource>((int)list.Count);
        for (var i = 0; i < list.Count; i++)
        {
            var info = Marshal.PtrToStructure<PuSourceInfo>(IntPtr.Add(list.Items, i * stride));
            sources.Add(new MediaSource(
                Marshal.PtrToStringUTF8(info.Id) ?? string.Empty,
                Marshal.PtrToStringUTF8(info.Title) ?? string.Empty,
                info.Width,
                info.Height,
                info.Kind));
        }
        return sources;
    }

    /// <summary>最近一次错误描述（UTF-8）。无引擎时读进程级错误串。</summary>
    public string LastErrorText()
    {
        var ptr = NativeMethods.pu_last_error(_engine);
        return ptr == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;
    }

    // ---- 语音（ABI v2 的 pu_voice_* 五个导出）----

    /// <summary>
    /// 加入语音。<paramref name="mode"/>：0=relay、1=direct。
    /// <paramref name="passive"/>=true 时置 <c>pu_voice_params.flags</c> bit0=1：只加音频轨、
    /// 不主动发 offer，等对方 offer 后回 answer。1:1 里后加入的一方必须走这条，否则双方并发
    /// offer 会 glare（引擎 SignalIn 会直接报角色冲突）。
    /// </summary>
    public int VoiceJoin(string channelId, int mode, bool passive = false)
    {
        ThrowIfDisposed();
        EnsureCreated();
        var idPtr = Marshal.StringToCoTaskMemUTF8(channelId);
        try
        {
            var parameters = new PuVoiceParams
            {
                StructSize = (uint)Marshal.SizeOf<PuVoiceParams>(),
                ChannelId = idPtr,
                Mode = mode,
                Flags = passive ? 1u : 0u,
            };
            return NativeMethods.pu_voice_join(_engine, in parameters);
        }
        finally
        {
            Marshal.FreeCoTaskMem(idPtr);
        }
    }

    public int VoiceLeave() => NativeMethods.pu_voice_leave(_engine);

    public int VoiceSetMode(int mode) => NativeMethods.pu_voice_set_mode(_engine, mode);

    public int VoiceSetMute(bool muted) => NativeMethods.pu_voice_set_mute(_engine, muted ? 1 : 0);

    public int VoiceSetDeafen(bool deafened) => NativeMethods.pu_voice_set_deafen(_engine, deafened ? 1 : 0);

    public int Watch(string sharerId) => NativeMethods.pu_screen_watch(_engine, sharerId);

    public int Unwatch(string sharerId) => NativeMethods.pu_screen_unwatch(_engine, sharerId);

    /// <summary>
    /// 把信令投递给指定媒体会话（"voice" / "screen"）。
    /// 语音与屏幕共享是两条独立 PeerConnection，媒体类型由客户端显式给出，不做 SDP 嗅探——
    /// candidate 信封里根本没有媒体类型，嗅探只能「两边都试」，属于静默失败。
    /// </summary>
    /// <summary>
    /// 媒体核心是否已提供 pu_signal_in_media（ABI v2 第 20 个导出）。
    /// 没有它就无法把信令明确路由到 voice / screen 会话——这是**能力缺失**，
    /// 不静默退回 pu_signal_in（那会让路由问题重新不可诊断）。
    /// </summary>
    public bool HasMediaSignalSupport { get; private set; }

    public int SignalIn(string media, byte[] payload)
    {
        if (!HasMediaSignalSupport)
        {
            throw new InvalidOperationException(
                "媒体核心缺少 pu_signal_in_media：需要带 ABI v2 第 20 个导出的新 DLL（语音/共享信令无法路由）");
        }
        var ptr = Marshal.AllocHGlobal(payload.Length);
        try
        {
            Marshal.Copy(payload, 0, ptr, payload.Length);
            return NativeMethods.pu_signal_in_media(_engine, media, ptr, (UIntPtr)(uint)payload.Length);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    public int SignalIn(byte[] payload)
    {
        var ptr = Marshal.AllocHGlobal(payload.Length);
        try
        {
            Marshal.Copy(payload, 0, ptr, payload.Length);
            return NativeMethods.pu_signal_in(_engine, ptr, (UIntPtr)(uint)payload.Length);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    internal int FrameAcquire(string streamId, out PuFrame frame)
    {
        frame = new PuFrame { StructSize = (uint)Marshal.SizeOf<PuFrame>() };
        return NativeMethods.pu_frame_acquire(_engine, streamId, ref frame);
    }

    internal void FrameRelease(ref PuFrame frame) => NativeMethods.pu_frame_release(_engine, ref frame);

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        if (_engine != IntPtr.Zero)
        {
            NativeMethods.pu_engine_destroy(_engine);
            _engine = IntPtr.Zero;
        }
        if (_handle != IntPtr.Zero)
        {
            NativeLibrary.Free(_handle);
            _handle = IntPtr.Zero;
        }
    }

    private void EnsureCreated()
    {
        if (_engine == IntPtr.Zero)
        {
            throw new MediaEngineException("引擎尚未创建（先调用 Create）");
        }
    }

    private void Check(int status, string operation)
    {
        if (status != (int)PuStatus.Ok)
        {
            throw new MediaEngineException($"{operation} 返回 {status}：{LastErrorText()}");
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    private static unsafe MediaEvent Map(PuEvent ev)
    {
        var kind = (MediaEventKind)ev.Type;
        switch (kind)
        {
            case MediaEventKind.State:
                return new MediaEvent(
                    kind,
                    ev.Type,
                    ev.TimestampUs,
                    ev.Data.State.State,
                    ev.Data.State.PreviousState,
                    ev.Data.State.Reason,
                    null);
            case MediaEventKind.Error:
                return new MediaEvent(
                    kind,
                    ev.Type,
                    ev.TimestampUs,
                    0,
                    0,
                    0,
                    ReadFixedMessage(ev.Data.Error));
            case MediaEventKind.SignalOut:
                return new MediaEvent(kind, ev.Type, ev.TimestampUs, 0, 0, 0, null)
                {
                    SignalKind = (PuSignalKind)ev.Data.Signal.Kind,
                    // 库持有 payload 指针（有效期到 destroy 或再取走 64 条信令），
                    // 这里立刻复制：事件对象会跨线程/跨定时器存活。
                    SignalPayload = Marshal.PtrToStringUTF8(
                        ev.Data.Signal.Payload, (int)ev.Data.Signal.PayloadLen) ?? string.Empty,
                };
            case MediaEventKind.RemoteFrame:
                return new MediaEvent(kind, ev.Type, ev.TimestampUs, 0, 0, 0, null)
                {
                    RemoteSequence = ev.Data.Remote.Sequence,
                    RemoteFramesReceived = ev.Data.Remote.FramesReceived,
                    RemoteWidth = ev.Data.Remote.Width,
                    RemoteHeight = ev.Data.Remote.Height,
                };
            default:
                return new MediaEvent(kind, ev.Type, ev.TimestampUs, 0, 0, 0, null);
        }
    }

    /// <summary>把 pu_error_event.message[232] 这条 C 定长 UTF-8 字段读成字符串。</summary>
    private static unsafe string ReadFixedMessage(PuErrorEvent error)
    {
        // 对 local 结构体的 fixed buffer 成员，在 unsafe 上下文里本身就是已固定指针。
        byte* pointer = error.Message;
        return Marshal.PtrToStringUTF8((IntPtr)pointer) ?? string.Empty;
    }
}
