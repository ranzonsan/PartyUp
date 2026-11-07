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

    /// <summary>
    /// v2.16（B2）：这条信令属于哪条严格接收会话（原生 token；0 = 旧 voice/screen 会话）。
    /// 客户端按 token 映射回协议 session_id，answer 才能带对 session/nonce。
    /// </summary>
    public uint SignalSessionToken { get; init; }

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

/// <summary>音频设备（pu_audio_device）。Index 是 ADM 枚举位置；IsDefault 表示该设备是当前系统默认端点。</summary>
public sealed record AudioDeviceInfo(int Index, bool IsDefault, string Name, string Guid);

/// <summary>
/// 独立接收会话统计（pu_screen_receive_stats，v2.16/B2）。
/// Active=false 表示会话已关闭/不存在；关闭会话的计数不再增长。
/// </summary>
public sealed record ReceiveSessionStats(
    bool Active, uint Token, uint VideoTracks, uint FramesDelivered, ulong LastFrameMs);

/// <summary>
/// per-viewer 发送会话统计（pu_screen_publish_stats，v2.17/B2b direct）。
/// <see cref="PacketsSent"/> 是标准 GetStats 的 outbound-rtp.packetsSent：
/// 它增长说明该会话仍在真实发 RTP（不是仅 ICE 保活）；会话关闭后 Active=false。
/// </summary>
public sealed record PublishSessionStats(
    bool Active, uint Token, uint PacketsSent, ulong BytesSent, ulong FramesEncoded,
    bool HasRemote);

/// <summary>
/// 接收会话的真实 RTP 计数（pu_screen_receive_rtp_stats，v2.17）。
/// 与 <see cref="ReceiveSessionStats.FramesDelivered"/> 不同，它在网络层：
/// 停看后包数不再增长，而 ICE/DTLS 保活不增加它。
/// </summary>
public sealed record ReceiveRtpStats(
    bool Active, uint PacketsReceived, ulong BytesReceived, ulong FramesDecoded);

/// <summary>
/// 音频设备状态（pu_audio_status）：真实选择 + 热插拔/默认端点代次 + 回落/故障标记。
/// 注意 <see cref="MediaEngine.GetAudioStatus"/> 会推进原生的真实重新选择/回退，
/// 不是纯只读快照。InputFaulted/OutputFaulted 表示该方向因切换/回退失败而未在运行
/// （不能当成“已静音”）。热切能力用 <see cref="MediaEngine.HasAudioHotSwitch"/> 区分。
/// </summary>
public sealed record AudioDeviceStatus(
    int InputIndex, int OutputIndex, uint InputCount, uint OutputCount,
    uint Generation, bool Changed, bool InputFallback, bool OutputFallback,
    string InputName, string OutputName, bool InputFaulted = false, bool OutputFaulted = false);

/// <summary>
/// 本端电平（pu_voice_level）。Valid=false 时 Peak/Rms 无意义——不能当成 0 电平显示。
/// Source: 0=none, 1=GetSignalLevel, 2=通话音频轨 PCM sink。
/// Rms 只在 Source==2 时是实测值；Source==1（GetSignalLevel 没有 RMS）时 Rms=0 表示
/// 「未测量 RMS」，UI 不得把它当静音/零 RMS 展示。
///
/// <para>
/// CaptureFrames/CaptureBytes/LastCbMs 是**与音量无关**的采集计数（v2.19 追加）：
/// 全零 PCM（例如麦克风降噪把环境声吃掉、或用户没说话）也会增长。
/// 判断「麦克风采集是否活着」要看它们，**不要**看 Frames/Valid（那条走 track sink，
/// 通话中可能恒为 0，见 2026-11-07 的真机证据）。
/// </para>
/// </summary>
public sealed record VoiceLevel(
    bool Valid, uint Peak, uint Rms, uint Frames, uint Source, ulong LastFrameMs,
    uint CaptureFrames = 0, ulong CaptureBytes = 0, ulong LastCbMs = 0,
    uint AdmInitialized = 0, uint AdmRecording = 0, uint AdmRecordingInitialized = 0,
    uint AdmInstance = 0)
{
    /// <summary>采集链路是否在出帧（与音量无关）。旧 DLL 无该字段时恒为 0。</summary>
    public bool CaptureAlive => CaptureFrames > 0;
}

/// <summary>
/// 独立输入电平（pu_audio_monitor_info，v2.18/AUD02）。State 0=Idle / 1=Active /
/// 2=Faulted；Running -1=未知 / 0=否 / 1=是（Stop 失败时必须看它，不能假设已停）。
/// Valid=false 时 Peak/Rms/Frames 无意义，不能当成「有效静音」显示。
/// </summary>
public sealed record AudioMonitorStatus(
    int State, int Running, bool Valid, uint Peak, uint Rms, uint Frames, ulong LastFrameMs);

/// <summary>
/// 媒体核心（partyup_media.dll，ABI v2）的托管包装。
///
/// 设计要点：
///   * 显式从 AppContext.BaseDirectory 加载 DLL（不靠 PATH 搜索），避免被同名旧库抢走——
///     这是 Spike 1 记下的坑；
///   * 加载后校验 18 个必需导出全部可解析，并统计全部已知导出（24 个）的实际命中数——
///     自检数字必须来自真实探测，不能写死（写死的 18/18 在 DLL 扩到 24 个导出后就不再提示版本不匹配）；
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

    /// <summary>
    /// ABI v2 在 18 个必需导出之后以加法方式追加的导出（v2.2–v2.18，共 20 个）。
    /// 只用于统计"实际命中/已知总数"的自检显示；是否可用仍由下方的 has* 标志决定。
    /// v2.17 的 5 个 per-viewer publish 导出不在此表（由 HasDirectPublishExports 单独探测）。
    /// </summary>
    public static readonly IReadOnlyList<string> ExtendedExportNames = new[]
    {
        "pu_engine_set_ice_servers",
        "pu_signal_in_media",
        "pu_signal_media",
        "pu_screen_prepare",
        "pu_screen_start_ex",
        "pu_voice_tracks",
        // v2.14（v1.1 单流观看 / 音频能力）：
        "pu_screen_watch_ex",
        "pu_engine_caps",
        "pu_audio_devices",
        "pu_audio_status",
        "pu_audio_set_device",
        "pu_voice_level",
        // v2.18（AUD02 独立输入电平）：
        "pu_audio_monitor_start",
        "pu_audio_monitor_stop",
        "pu_audio_monitor_status",
        // v2.16（B2 接收隔离）：
        "pu_screen_receive_open",
        "pu_screen_receive_close",
        "pu_signal_in_session",
        "pu_screen_receive_stats",
        "pu_screen_pause_send",
    };

    /// <summary>ABI 迄今声明的全部导出（18 必需 + 20 扩展 = 38；publish 5 个单独探测）。</summary>
    public static readonly IReadOnlyList<string> KnownExportNames =
        ExportNames.Concat(ExtendedExportNames).ToArray();

    /// <summary>已知导出总数（当前 38）；UI 自检显示 "命中数/这个数"。</summary>
    public static int KnownExportCount => KnownExportNames.Count;

    private static readonly object ResolverGate = new();
    private static bool _resolverInstalled;
    private static string? _resolvedPath;

    private IntPtr _engine;
    private IntPtr _handle;
    private bool _disposed;
    private uint _lastAudioGeneration;

    private MediaEngine(string libraryPath, IntPtr handle, IReadOnlyList<string> missingExports,
        int presentExportCount, bool hasMediaSignal, bool hasScreenRelay, bool hasIceServers,
        bool hasWatchEx, bool hasEngineCaps, bool hasAudioDevices, bool hasVoiceLevel,
        bool hasAudioMonitor)
    {
        LibraryPath = libraryPath;
        _handle = handle;
        MissingExports = missingExports;
        PresentExportCount = presentExportCount;
        HasMediaSignalSupport = hasMediaSignal;
        HasScreenRelaySupport = hasScreenRelay;
        HasIceServersSupport = hasIceServers;
        HasScreenWatchExports = hasWatchEx;
        HasEngineCapsExport = hasEngineCaps;
        HasAudioDeviceExports = hasAudioDevices;
        HasVoiceLevelExport = hasVoiceLevel;
        HasAudioMonitorExport = hasAudioMonitor;
    }

    /// <summary>
    /// <c>pu_frame_acquire</c> 的远端流 id：观看方要渲染的那一路解码帧。
    /// native 约定 <c>"remote"</c>（以及空串）表示**当前选台**，而不是某个固定流：
    /// 显式 watch 之后仍用它取当前目标的帧；改名/改语义要两边一起改。
    /// </summary>
    public const string RemoteStreamId = "remote";

    public string LibraryPath { get; }

    public IReadOnlyList<string> MissingExports { get; }

    /// <summary>
    /// 实际探测到的导出数（来自 <see cref="KnownExportNames"/> 的真实 TryGetExport 命中数）。
    /// 旧 DLL 会小于 <see cref="KnownExportCount"/>——这个数才是自检该显示的东西。
    /// </summary>
    public int PresentExportCount { get; }

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
        // 自检数字：全部已知导出里真正能解析到的个数（24 个都命中才是 24/24）。
        var presentExportCount = KnownExportNames.Count(name => NativeLibrary.TryGetExport(handle, name, out _));
        if (presentExportCount < KnownExportCount)
        {
            AppLog.Warn($"media: 导出自检 {presentExportCount}/{KnownExportCount}——DLL 比客户端期望的旧");
        }
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
        // 可选导出（直连回落 3a）：ICE servers 注入。没有它时直连只能拿到 host 候选。
        var hasIceServers = NativeLibrary.TryGetExport(handle, "pu_engine_set_ice_servers", out _);
        if (!hasIceServers)
        {
            AppLog.Warn("media: DLL 缺少 pu_engine_set_ice_servers（ABI 第 24 个导出）——" +
                        "STUN 配置暂不生效（等 split-dev 的 DLL），直连只剩 host 候选");
        }
        // v2.14：旧 DLL 里 pu_screen_watch/unwatch 导出**存在但返回 NOT_IMPLEMENTED**，
        // 所以不能按导出存在判断可用。必须同时有显式能力探测与 watch_ex 才算支持。
        var hasWatchEx = NativeLibrary.TryGetExport(handle, "pu_screen_watch_ex", out _);
        var hasEngineCaps = NativeLibrary.TryGetExport(handle, "pu_engine_caps", out _);
        if (!hasWatchEx || !hasEngineCaps)
        {
            AppLog.Warn("media: DLL 缺少 pu_screen_watch_ex / pu_engine_caps（v2.14）——" +
                        "显式单流观看不可用；旧导出 pu_screen_watch 返回 NOT_IMPLEMENTED，不能当可用");
        }
        var hasAudioDevices = NativeLibrary.TryGetExport(handle, "pu_audio_devices", out _) &&
                              NativeLibrary.TryGetExport(handle, "pu_audio_status", out _) &&
                              NativeLibrary.TryGetExport(handle, "pu_audio_set_device", out _);
        if (!hasAudioDevices)
        {
            AppLog.Warn("media: DLL 缺少 pu_audio_devices/status/set_device（v2.14）——音频设备选择不可用");
        }
        var hasVoiceLevel = NativeLibrary.TryGetExport(handle, "pu_voice_level", out _);
        if (!hasVoiceLevel)
        {
            AppLog.Warn("media: DLL 缺少 pu_voice_level（v2.14）——本端电平不可用");
        }
        // v2.18（AUD02）：三个导出必须同时存在，缺一个就明确报不支持（旧 DLL 不能
        // 静默返回全 0 电平）。能力位仍由 pu_engine_caps 的 bit7 决定。
        var hasAudioMonitor =
            NativeLibrary.TryGetExport(handle, "pu_audio_monitor_start", out _) &&
            NativeLibrary.TryGetExport(handle, "pu_audio_monitor_stop", out _) &&
            NativeLibrary.TryGetExport(handle, "pu_audio_monitor_status", out _);
        if (!hasAudioMonitor)
        {
            AppLog.Warn("media: DLL 缺少 pu_audio_monitor_start/stop/status（v2.18）——独立输入电平不可用");
        }
        return new MediaEngine(path, handle, missing, presentExportCount, hasMediaSignal,
            hasScreenRelay, hasIceServers, hasWatchEx, hasEngineCaps, hasAudioDevices, hasVoiceLevel,
            hasAudioMonitor);
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
        // ICE servers 必须在第一条 PeerConnection 之前写进引擎，所以在这里补应用。
        ApplyIceServers();
        // v2.14：显式能力探测需要已创建的引擎。旧 DLL 没有这个导出 → 不探测、明确不支持。
        ProbeEngineCaps();
    }

    /// <summary>
    /// 调用 pu_engine_caps 读取真实能力位。导出存在但返回失败时按「不支持」处理，
    /// 不按「有 watch/unwatch 导出」推断可用（旧 DLL 那两个导出是 NOT_IMPLEMENTED 桩）。
    /// </summary>
    private void ProbeEngineCaps()
    {
        if (!HasEngineCapsExport || _engine == IntPtr.Zero)
        {
            EngineCapabilities = 0;
            return;
        }
        uint caps = 0;
        var rc = NativeMethods.pu_engine_caps(_engine, ref caps);
        if (rc != 0)
        {
            EngineCapabilities = 0;
            AppLog.Warn($"media: pu_engine_caps 返回 {rc}：{LastErrorText()}");
            return;
        }
        EngineCapabilities = caps;
        // v2.16：五个新导出缺一不可；旧 DLL 不能按「watch 存在」推断严格接收可用。
        HasIsolatedReceiveExports =
            NativeLibrary.TryGetExport(_handle, "pu_screen_receive_open", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_receive_close", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_signal_in_session", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_receive_stats", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_pause_send", out _);
        // v2.17：per-viewer 发送会话 + 真实 RTP 计数，同样要求导出齐全且能力位为 1。
        HasDirectPublishExports =
            NativeLibrary.TryGetExport(_handle, "pu_screen_publish_open", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_publish_signal_in", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_publish_close", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_publish_stats", out _) &&
            NativeLibrary.TryGetExport(_handle, "pu_screen_receive_rtp_stats", out _);
        AppLog.Info($"media: engine caps=0x{caps:x} watch={(caps & 1) != 0} " +
                    $"watch_slots={(caps & 2) != 0} audio_devices={(caps & 4) != 0} " +
                    $"voice_level={(caps & 8) != 0} audio_hot_switch={(caps & 0x10) != 0} " +
                    $"isolated_receive={(HasIsolatedReceiveExports && (caps & 0x20) != 0)} " +
                    $"direct_publish={(HasDirectPublishExports && (caps & 0x40) != 0)} " +
                    $"audio_monitor={(HasAudioMonitorExport && (caps & 0x80) != 0)}");
    }

    /// <summary>v2.17：per-viewer 发送会话与真实 RTP 计数的五个导出是否齐全（不含能力位）。</summary>
    public bool HasDirectPublishExports { get; private set; }

    /// <summary>
    /// v2.17（B2b direct）：共享者能否为单个 viewer 建立独立发送传输。
    /// 只有 bit5 的旧 DLL 只有 pause_send，不能当成 direct 严格隔离可用。
    /// </summary>
    public bool HasDirectPublishSupport =>
        HasDirectPublishExports && (EngineCapabilities & 0x40) != 0;

    /// <summary>v2.16：独立接收会话的五个导出是否齐全（不含能力位）。</summary>
    public bool HasIsolatedReceiveExports { get; private set; }

    /// <summary>
    /// v2.16（B2）：独立 receive-only 接收会话是否真实可用。
    /// 旧 DLL、或只有导出没有能力位，都按不支持处理——不能退回旧混合 PC 的 watch。
    /// </summary>
    public bool HasIsolatedReceiveSupport =>
        HasIsolatedReceiveExports && (EngineCapabilities & 0x20) != 0;

    /// <summary>引擎能力位（pu_engine_caps；0 = 未探测/旧 DLL）。</summary>
    public uint EngineCapabilities { get; private set; }

    /// <summary>显式单流观看（目标 + 槽位/代次）是否真实可用。</summary>
    public bool HasScreenWatchSupport =>
        HasScreenWatchExports && (EngineCapabilities & 0x1) != 0 && (EngineCapabilities & 0x2) != 0;

    /// <summary>音频设备枚举/选择是否可用（同一通话 ADM）。</summary>
    public bool HasAudioDeviceSupport => HasAudioDeviceExports && (EngineCapabilities & 0x4) != 0;

    /// <summary>
    /// 通话中真实设备热切 + 稳定 ID 选择 + 系统默认跟随/热插拔回退（v2.15 caps bit4）。
    /// 旧 DLL 没有该位：此时 <see cref="SetAudioDevice"/> 只是登记到下次 join，UI 不得
    /// 宣称“通话中已切换”。要配合轮询 <see cref="GetAudioStatus"/> 才能发现热插拔/
    /// 默认端点变化，本版没有主动事件通道。
    /// </summary>
    public bool HasAudioHotSwitch => HasAudioDeviceExports && (EngineCapabilities & 0x10) != 0;

    /// <summary>本端电平是否可用（GetSignalLevel 或通话轨 PCM 兜底）。</summary>
    public bool HasVoiceLevelSupport => HasVoiceLevelExport && (EngineCapabilities & 0x8) != 0;

    /// <summary>
    /// v2.18（AUD02）：独立输入电平是否可用（三个导出齐全 + caps bit7）。
    /// 旧 DLL 没有该位时调用必须明确报 unsupported，不能静默返回全 0 电平。
    /// </summary>
    public bool HasAudioMonitorSupport =>
        HasAudioMonitorExport && (EngineCapabilities & 0x80) != 0;

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

    /// <summary>媒体核心是否支持 ICE servers 注入（ABI 第 24 个导出 pu_engine_set_ice_servers）。</summary>
    public bool HasIceServersSupport { get; private set; }

    /// <summary>是否已建过屏幕会话（只用于日志里标注 pu_stats 的状态来源，不参与逻辑）。</summary>
    public bool HasScreenSession { get; private set; }

    private IReadOnlyList<string> _iceServers = Array.Empty<string>();

    /// <summary>
    /// 设置 ICE servers（STUN 地址来自服务端 server.info，见 PROTOCOL.md §1.4）。
    /// 幂等；引擎尚未 Create 时先缓存，Create 后自动应用。DLL 还没有该导出时只告警并缓存。
    /// </summary>
    public void SetIceServers(IReadOnlyList<string> urls)
    {
        ThrowIfDisposed();
        _iceServers = (urls ?? Array.Empty<string>())
            .Where(u => !string.IsNullOrWhiteSpace(u))
            .Select(u => u.Trim())
            .ToArray();
        AppLog.Info("media: ice servers = " + (_iceServers.Count == 0
            ? "(none)"
            : string.Join(",", _iceServers)));
        if (!HasIceServersSupport)
        {
            return; // 已缓存；等 DLL 提供导出后（下次 Load）或 Create 时再试
        }
        if (IsCreated)
        {
            ApplyIceServers();
        }
    }

    /// <summary>把缓存的 ICE servers 写进引擎（建 PC 之前调用才有效）。</summary>
    private void ApplyIceServers()
    {
        if (!HasIceServersSupport || _engine == IntPtr.Zero) return;
        var joined = string.Join("\n", _iceServers);
        var len = (UIntPtr)System.Text.Encoding.UTF8.GetByteCount(joined);
        var rc = _iceServers.Count == 0
            ? NativeMethods.pu_engine_set_ice_servers(_engine, null, UIntPtr.Zero)
            : NativeMethods.pu_engine_set_ice_servers(_engine, joined, len);
        if (rc != 0)
        {
            AppLog.Fail($"media: pu_engine_set_ice_servers 返回 {rc}：{LastErrorText()}");
            return;
        }
        AppLog.Pass($"media: ice servers applied ({_iceServers.Count})");
    }

    /// <summary>开始一路内置合成源共享（不需要真实屏幕，也不改动窗口内容）。</summary>
    public void StartSynthetic()
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_start(_engine, "synthetic"), "pu_screen_start(synthetic)");
        HasScreenSession = true;
    }

    /// <summary>开始共享指定屏幕（id 来自 <see cref="EnumerateSources"/> 或 "\\.\DISPLAY1"）。</summary>
    public void StartScreen(string screenId)
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_start(_engine, screenId), "pu_screen_start");
        HasScreenSession = true;
    }

    /// <summary>媒体核心是否支持被动屏幕会话（SFU 屏幕共享，ABI v2.11）。</summary>
    public bool HasScreenRelaySupport { get; private set; }

    /// <summary>DLL 是否同时导出 pu_screen_watch_ex 与 pu_engine_caps（仅存在性，不代表可用）。</summary>
    public bool HasScreenWatchExports { get; }

    /// <summary>DLL 是否导出 pu_engine_caps（旧 DLL 没有）。</summary>
    public bool HasEngineCapsExport { get; }

    /// <summary>DLL 是否导出 pu_audio_devices / status / set_device。</summary>
    public bool HasAudioDeviceExports { get; }

    /// <summary>DLL 是否导出 pu_voice_level。</summary>
    public bool HasVoiceLevelExport { get; }

    /// <summary>DLL 是否导出 pu_audio_monitor_start/stop/status（v2.18；不含能力位）。</summary>
    public bool HasAudioMonitorExport { get; }

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
        HasScreenSession = true;
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
        HasScreenSession = true;
    }

    /// <summary>停止共享（幂等）。</summary>
    public void StopScreen()
    {
        ThrowIfDisposed();
        EnsureCreated();
        Check(NativeMethods.pu_screen_stop(_engine), "pu_screen_stop");
        HasScreenSession = false;
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

    /// <summary>
    /// 兼容入口：只按目标 arm（slot=-1/generation=0）。新客户端应使用
    /// <see cref="WatchEx"/> 把服务端 slot/generation 带进来，否则 relay 下无法
    /// 保证「新目标落在不同 track」的帧归属。旧 DLL 会返回 NOT_IMPLEMENTED，
    /// 所以这里必须先检查能力，不能只按导出存在就调用。
    /// </summary>
    public int Watch(string sharerId)
    {
        EnsureWatchSupported();
        return NativeMethods.pu_screen_watch(_engine, sharerId);
    }

    /// <summary>
    /// v1.1 显式观看：目标 + relay 槽位 + 服务端代次。原生按 (target, epoch) 过滤帧，
    /// 切源后旧目标的迟到帧不会串到新目标。
    /// </summary>
    public int WatchEx(string sharerId, int slot, ulong generation)
    {
        EnsureWatchSupported();
        return NativeMethods.pu_screen_watch_ex(_engine, sharerId, slot, generation);
    }

    /// <summary>停止观看；<paramref name="sharerId"/> 与当前目标不一致时是幂等 no-op。</summary>
    public int Unwatch(string sharerId)
    {
        EnsureWatchSupported();
        return NativeMethods.pu_screen_unwatch(_engine, sharerId);
    }

    private void EnsureWatchSupported()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasScreenWatchSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持显式单流观看（需要 v2.14 DLL 的 pu_screen_watch_ex + pu_engine_caps；" +
                "旧 DLL 的 pu_screen_watch 导出存在但返回 NOT_IMPLEMENTED）");
        }
    }

    // ------------------------------------------------------------------
    // v2.16（B2）：独立 receive-only 接收会话
    // ------------------------------------------------------------------

    /// <summary>
    /// 建立（或复用）一条独立接收会话。必须在发送 screen.watch 之前调用：
    /// identity/传输先于信令，服务端 offer 才有确定落点。返回原生 token，
    /// 用于把 SIGNAL_OUT 的 answer 映射回协议 session_id。
    /// </summary>
    public uint ReceiveOpen(string sessionId)
    {
        EnsureIsolatedReceiveSupported();
        var rc = NativeMethods.pu_screen_receive_open(_engine, sessionId, out var token);
        if (rc != 0)
        {
            throw new InvalidOperationException(
                $"pu_screen_receive_open 返回 {rc}：{LastErrorText()}");
        }
        return token;
    }

    /// <summary>只关闭这条接收会话；发布 PC/采集/语音完全不动。幂等。</summary>
    public int ReceiveClose(string sessionId)
    {
        EnsureIsolatedReceiveSupported();
        return NativeMethods.pu_screen_receive_close(_engine, sessionId);
    }

    /// <summary>把屏幕信令按 session_id 灌进正确的接收会话；旧会话 id 会被原生拒绝。</summary>
    public int SignalInSession(string media, string sessionId, byte[] payload)
    {
        if (!HasIsolatedReceiveSupport)
        {
            throw new InvalidOperationException("媒体核心不支持按会话信令（需要 v2.16 DLL）");
        }
        var rc = NativeMethods.pu_signal_in_session(_engine, media, sessionId, payload,
            (UIntPtr)(uint)payload.Length);
        return rc;
    }

    /// <summary>接收会话统计（诊断/回归）；会话不存在时 Active=false。</summary>
    public ReceiveSessionStats GetReceiveStats(string sessionId)
    {
        EnsureIsolatedReceiveSupported();
        var info = new PuReceiveStatsInfo
        {
            StructSize = (uint)Marshal.SizeOf<PuReceiveStatsInfo>(),
        };
        Check(NativeMethods.pu_screen_receive_stats(_engine, sessionId, ref info),
            "pu_screen_receive_stats");
        return new ReceiveSessionStats(info.Active != 0, info.Token, info.VideoTracks,
            info.FramesDelivered, info.LastFrameMs);
    }

    /// <summary>
    /// direct 1v1：暂停/恢复本端屏幕发送轨，保留采集源、视频轨、sender 与发布状态。
    /// 不是 screen.stop，也不重协商。
    /// </summary>
    public int PauseScreenSend(bool paused)
    {
        if (!HasIsolatedReceiveSupport)
        {
            throw new InvalidOperationException("媒体核心不支持暂停发送（需要 v2.16 DLL）");
        }
        return NativeMethods.pu_screen_pause_send(_engine, paused ? 1 : 0);
    }

    private void EnsureIsolatedReceiveSupported()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasIsolatedReceiveSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持独立接收会话（需要 v2.16 DLL 的 receive 导出 + caps bit5）；" +
                "不能退回旧混合 PC 的 watch");
        }
    }

    // ------------------------------------------------------------------
    // v2.17（B2b direct）：共享者 per-viewer 发送会话 + 真实 RTP 计数
    // ------------------------------------------------------------------

    /// <summary>
    /// 为单个 viewer 建立一条独立发送会话（新 PC，复用已存在的采集源/视频轨）。
    /// 原生立刻产生 offer（SIGNAL_OUT 带该会话 token）；answer/candidate 经
    /// <see cref="PublishSignalIn"/> 灌回。不会重启采集、不会全局 screen.stop。
    /// </summary>
    public uint PublishOpen(string sessionId)
    {
        EnsureDirectPublishSupported();
        var rc = NativeMethods.pu_screen_publish_open(_engine, sessionId, out var token);
        if (rc != 0)
        {
            throw new InvalidOperationException(
                $"pu_screen_publish_open 返回 {rc}：{LastErrorText()}");
        }
        return token;
    }

    /// <summary>把对端 answer/candidate 灌进该发送会话；旧会话 id 会被原生拒绝。</summary>
    public int PublishSignalIn(string sessionId, byte[] payload)
    {
        if (!HasDirectPublishSupport)
        {
            throw new InvalidOperationException("媒体核心不支持按会话发送信令（需要 v2.17 DLL）");
        }
        return NativeMethods.pu_screen_publish_signal_in(_engine, sessionId, payload,
            (UIntPtr)(uint)payload.Length);
    }

    /// <summary>只关闭该 viewer 的发送会话；采集、视频轨、发布状态、其它会话都不动。幂等。</summary>
    public int PublishClose(string sessionId)
    {
        EnsureDirectPublishSupported();
        return NativeMethods.pu_screen_publish_close(_engine, sessionId);
    }

    /// <summary>发送会话统计（真实 outbound-rtp 包数/字节数）；会话不存在时 Active=false。</summary>
    public PublishSessionStats GetPublishStats(string sessionId)
    {
        EnsureDirectPublishSupported();
        var info = new PuPublishStatsInfo
        {
            StructSize = (uint)Marshal.SizeOf<PuPublishStatsInfo>(),
        };
        Check(NativeMethods.pu_screen_publish_stats(_engine, sessionId, ref info),
            "pu_screen_publish_stats");
        return new PublishSessionStats(info.Active != 0, info.Token, info.PacketsSent,
            info.BytesSent, info.FramesEncoded, info.HasRemote != 0);
    }

    /// <summary>接收会话的真实 inbound RTP 计数（网络层包/字节），用于验证停收。</summary>
    public ReceiveRtpStats GetReceiveRtpStats(string sessionId)
    {
        EnsureIsolatedReceiveSupported();
        var info = new PuRtpStatsInfo
        {
            StructSize = (uint)Marshal.SizeOf<PuRtpStatsInfo>(),
        };
        Check(NativeMethods.pu_screen_receive_rtp_stats(_engine, sessionId, ref info),
            "pu_screen_receive_rtp_stats");
        return new ReceiveRtpStats(info.Active != 0, info.PacketsReceived, info.BytesReceived,
            info.FramesDecoded);
    }

    private void EnsureDirectPublishSupported()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasDirectPublishSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持 per-viewer 发送会话（需要 v2.17 DLL 的 publish 导出 + caps bit6）；" +
                "不能用 pause_send 冒充 direct 严格隔离");
        }
    }

    /// <summary>枚举音频设备（kind 0=输入 1=输出）；结果来自通话同一个 ADM。</summary>
    public IReadOnlyList<AudioDeviceInfo> GetAudioDevices(int kind)
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioDeviceSupport)
        {
            throw new InvalidOperationException("媒体核心不支持音频设备枚举（需要 v2.14 DLL）");
        }
        var count = NativeMethods.pu_audio_devices(_engine, kind, null, 0);
        if (count <= 0) return Array.Empty<AudioDeviceInfo>();
        var buffer = new PuAudioDevice[count];
        for (var i = 0; i < buffer.Length; i++)
        {
            buffer[i].StructSize = (uint)Marshal.SizeOf<PuAudioDevice>();
            buffer[i].Name = new byte[128];
            buffer[i].Guid = new byte[64];
        }
        var written = NativeMethods.pu_audio_devices(_engine, kind, buffer, (uint)buffer.Length);
        if (written <= 0) return Array.Empty<AudioDeviceInfo>();
        var result = new List<AudioDeviceInfo>(written);
        for (var i = 0; i < written && i < buffer.Length; i++)
        {
            var item = buffer[i];
            result.Add(new AudioDeviceInfo(
                item.Index,
                item.IsDefault != 0,
                ReadFixedUtf8(item.Name),
                ReadFixedUtf8(item.Guid)));
        }
        return result;
    }

    /// <summary>
    /// 读取设备状态。changed 由原生相对本引擎上一次调用计算；失败时抛异常而不是返回全 0。
    /// 注意：本调用会推进原生的真实重新选择/回退（拔掉选中设备、系统默认端点变化时
    /// 会停/启对应方向的设备），不是纯只读快照；要发现热插拔/默认变化必须周期性轮询。
    /// </summary>
    public AudioDeviceStatus GetAudioStatus()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioDeviceSupport)
        {
            throw new InvalidOperationException("媒体核心不支持音频设备状态（需要 v2.14 DLL）");
        }
        var status = new PuAudioStatus
        {
            StructSize = (uint)Marshal.SizeOf<PuAudioStatus>(),
            Generation = _lastAudioGeneration,
            InputName = new byte[128],
            OutputName = new byte[128],
        };
        Check(NativeMethods.pu_audio_status(_engine, ref status), "pu_audio_status");
        _lastAudioGeneration = status.Generation;
        return new AudioDeviceStatus(
            status.InputIndex,
            status.OutputIndex,
            status.InputCount,
            status.OutputCount,
            status.Generation,
            status.Changed != 0,
            (status.Fallback & 0x1) != 0,
            (status.Fallback & 0x2) != 0,
            ReadFixedUtf8(status.InputName),
            ReadFixedUtf8(status.OutputName),
            (status.Reserved & 0x1) != 0,
            (status.Reserved & 0x2) != 0);
    }

    /// <summary>
    /// 选择输入/输出设备（kind 0=输入 1=输出；index=-1 跟随系统默认）。
    /// 通话中且 <see cref="HasAudioHotSwitch"/> 为真时执行真实热切：返回成功表示新设备
    /// 已经实际启动（失败会抛异常，原生已尝试恢复原设备/回落默认）。旧 DLL 没有热切
    /// 能力位时，调用只登记到下次 join，不能据此宣称当前通话已切换。
    /// </summary>
    public void SetAudioDevice(int kind, int index)
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioDeviceSupport)
        {
            throw new InvalidOperationException("媒体核心不支持音频设备选择（需要 v2.14 DLL）");
        }
        Check(NativeMethods.pu_audio_set_device(_engine, kind, index), "pu_audio_set_device");
    }

    /// <summary>本端电平。Valid=false 表示没有有效数据（不是 0 电平）。</summary>
    public VoiceLevel GetVoiceLevel()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasVoiceLevelSupport)
        {
            throw new InvalidOperationException("媒体核心不支持本端电平（需要 v2.14 DLL）");
        }
        var level = new PuVoiceLevel { StructSize = (uint)Marshal.SizeOf<PuVoiceLevel>() };
        Check(NativeMethods.pu_voice_level(_engine, ref level), "pu_voice_level");
        return new VoiceLevel(level.Valid != 0, level.Peak, level.Rms, level.Frames,
            level.Source, level.LastFrameMs,
            level.CaptureFrames, level.CaptureBytes, level.LastCbMs,
            level.AdmInitialized, level.AdmRecording, level.AdmRecordingInitialized, level.AdmInstance);
    }

    /// <summary>
    /// v2.18（AUD02）：显式开始独立输入电平监测。默认不采集；语音占用、设备无法
    /// 安全启动或旧 DLL 缺接口时抛异常（不静默造电平）。本方法只打开输入，不启动
    /// 扬声器、不解除静音、不进语音、不建 PeerConnection。
    /// </summary>
    public void StartAudioMonitor()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioMonitorSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持独立输入电平（需要 v2.18 DLL 的 pu_audio_monitor_* 导出 + caps bit7）");
        }
        Check(NativeMethods.pu_audio_monitor_start(_engine), "pu_audio_monitor_start");
    }

    /// <summary>
    /// v2.18：幂等停止输入监测。不依赖状态结构；Stop/读回失败时抛异常（原生会进入
    /// Faulted 并保留真实 running），不会自动重试或自动重新打开采集。
    /// </summary>
    public void StopAudioMonitor()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioMonitorSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持独立输入电平（需要 v2.18 DLL 的 pu_audio_monitor_* 导出 + caps bit7）");
        }
        Check(NativeMethods.pu_audio_monitor_stop(_engine), "pu_audio_monitor_stop");
    }

    /// <summary>
    /// v2.18：读取独立输入电平快照。只读，不推进设备事务；State/Running 分开读
    /// （Running=-1 表示未知，不能当成"已停止"）。旧 DLL 明确抛不支持。
    /// </summary>
    public AudioMonitorStatus GetAudioMonitorStatus()
    {
        ThrowIfDisposed();
        EnsureCreated();
        if (!HasAudioMonitorSupport)
        {
            throw new InvalidOperationException(
                "媒体核心不支持独立输入电平（需要 v2.18 DLL 的 pu_audio_monitor_* 导出 + caps bit7）");
        }
        var info = new PuAudioMonitor
        {
            StructSize = (uint)Marshal.SizeOf<PuAudioMonitor>(),
        };
        Check(NativeMethods.pu_audio_monitor_status(_engine, ref info), "pu_audio_monitor_status");
        return new AudioMonitorStatus(info.State, info.Running, info.Valid != 0, info.Peak,
            info.Rms, info.Frames, info.LastFrameMs);
    }

    private static string ReadFixedUtf8(byte[] bytes)
    {
        var length = Array.IndexOf(bytes, (byte)0);
        if (length < 0) length = bytes.Length;
        return System.Text.Encoding.UTF8.GetString(bytes, 0, length);
    }

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
                    SignalSessionToken = ev.Data.Signal.Reserved,
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
