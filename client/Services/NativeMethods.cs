// P/Invoke 声明：与 src/media-core/include/partyup_media.h（ABI v2，18 个导出）
// 一一对应。结构体布局按头文件注释里的偏移写死（LayoutKind.Sequential / Explicit），
// 注释里标了每个结构在 C 侧的 sizeof，便于对照。
//
// 只在这里出现 DllImport 与结构体定义；面向 UI 的封装在 MediaEngine.cs。
using System.Runtime.InteropServices;

namespace PartyUp.Client.Services;

/// <summary>pu_status。负值错误码，0 成功。</summary>
internal enum PuStatus
{
    Ok = 0,
    InvalidArg = -1,
    State = -2,
    Timeout = -3,
    NoMemory = -4,
    Media = -5,
    Internal = -6,
    NotImplemented = -100,
}

/// <summary>pu_event_type。</summary>
public enum MediaEventKind
{
    None = 0,
    State = 1,
    Frame = 2,
    Error = 3,
    SignalOut = 4,
    RemoteFrame = 5,
}

/// <summary>pu_signal_kind：PU_EVENT_SIGNAL_OUT 的信令种类，与信封里的 type 一一对应。</summary>
public enum PuSignalKind
{
    Offer = 1,
    Answer = 2,
    Candidate = 3,
}

/// <summary>pu_stream_state。</summary>
public enum PuStreamState
{
    Idle = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Failed = 4,
}

internal static class NativeMethods
{
    /// <summary>DLL 名（不带扩展名）；加载路径由 MediaEngine 的 DllImportResolver 决定。</summary>
    public const string Library = "partyup_media";

    /// <summary>pu_engine_create：cfg 可为 NULL，这里总是传合法结构。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr pu_engine_create(in PuConfig cfg);

    /// <summary>pu_engine_destroy：传 NULL 安全。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void pu_engine_destroy(IntPtr engine);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_start(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string? screenId);

    /// <summary>v2.11：开始屏幕共享并可指定角色。flags bit0 = 被动（answerer：只采屏+挂轨，不发 offer）。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_start_ex(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string? screenId, uint flags);

    /// <summary>v2.11：观看者预建屏幕会话（answerer），不采屏、不建轨；幂等。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_prepare(IntPtr engine);

    // v2.16（B2）：独立 receive-only 会话。旧 DLL 没有这些导出，调用方必须先探测。
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_receive_open(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId, out uint token);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_receive_close(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_signal_in_session(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string media,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId, byte[] payload, UIntPtr len);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_receive_stats(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId,
        ref PuReceiveStatsInfo info);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_pause_send(IntPtr engine, int paused);

    // v2.17（B2b direct）：共享者侧 per-viewer 发送会话 + 真实 RTP 计数。
    // 旧 DLL 没有这些导出；调用方必须先探测 caps bit6。
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_publish_open(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId, out uint token);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_publish_signal_in(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId,
        byte[] payload, UIntPtr len);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_publish_close(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_publish_stats(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId,
        ref PuPublishStatsInfo info);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_receive_rtp_stats(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sessionId,
        ref PuRtpStatsInfo info);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_stop(IntPtr engine);

    /// <summary>
    /// pu_poll_event：调用方必须先把 out->struct_size 填成 sizeof(pu_event)，
    /// 所以这里用 ref 而不是 out——out 不会读调用方的初值，库会把 0 当成
    /// "比 ABI 结构小" 拒绝（实测错误：pu_event.struct_size is smaller than the ABI struct）。
    /// </summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_poll_event(IntPtr engine, ref PuEvent ev, int timeoutMs);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern IntPtr pu_last_error(IntPtr engine);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_signal_in(IntPtr engine, IntPtr payload, UIntPtr len);

    /// <summary>
    /// pu_signal_in_media（ABI v2 第 20 个导出，加法式扩张）：把一条信令明确投给
    /// "voice" 或 "screen" 会话。语音/共享各用独立 PeerConnection 后必须显式指定，
    /// 不能靠 SDP 嗅探（candidate 信封里没有媒体类型）。
    /// </summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_signal_in_media(
        IntPtr engine,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string media,
        IntPtr payload,
        UIntPtr len);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_peer_stats(IntPtr engine, ref PuStats stats);

    /// <summary>
    /// pu_engine_set_ice_servers（ABI v2 第 24 个导出，直连回落 3a）：设置本引擎所有
    /// PeerConnection 的 ICE servers。urls 用换行分隔（"stun:1.2.3.4:3478\nstun:..."），
    /// 空/null 表示清空。必须在建 PeerConnection **之前**调用（建好的 PC 不再改）。
    /// </summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_engine_set_ice_servers(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string? urls, UIntPtr len);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_join(IntPtr engine, in PuVoiceParams p);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_leave(IntPtr engine);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_set_mode(IntPtr engine, int mode);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_set_mute(IntPtr engine, int muted);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_set_deafen(IntPtr engine, int deafened);

    /// <summary>pu_voice_tracks(v2.7)：按轨读远端音频统计；out 为 null 时返回活跃轨数。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_tracks(IntPtr engine, [In, Out] PuVoiceTrack[]? outTracks, uint capacity);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_sources(IntPtr engine, ref PuSourceList list);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_watch(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sharerId);

    /// <summary>
    /// v2.14：带服务端槽位/代次的观看入口。relay 下 slot 来自 screen.watch.ok；
    /// generation 是服务端单调代次，原生拒绝更旧的乱序 watch。
    /// </summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_watch_ex(
        IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sharerId, int slot, ulong generation);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_unwatch(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sharerId);

    /// <summary>v2.14：显式能力位（旧 DLL 没有本导出，不能按 watch 导出存在判断可用）。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_engine_caps(IntPtr engine, ref uint caps);

    /// <summary>v2.14：枚举音频设备；kind 0=输入 1=输出；out null 返回数量。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_devices(
        IntPtr engine, int kind, [In, Out] PuAudioDevice[]? devices, uint capacity);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_status(IntPtr engine, ref PuAudioStatus status);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_set_device(IntPtr engine, int kind, int index);

    /// <summary>v2.14：本端电平；valid=0 表示没有有效数据（不能把 0 当静音电平）。</summary>
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_voice_level(IntPtr engine, ref PuVoiceLevel level);

    // v2.18（AUD02）：独立输入电平（同一条通话 ADM；默认不采集）。旧 DLL 没有这些
    // 导出，调用方必须先探测（HasAudioMonitorSupport），不能静默造电平值。
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_monitor_start(IntPtr engine);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_monitor_stop(IntPtr engine);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_audio_monitor_status(IntPtr engine, ref PuAudioMonitor info);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_frame_acquire(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string streamId, ref PuFrame frame);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern void pu_frame_release(IntPtr engine, ref PuFrame frame);
}

/// <summary>pu_config：C sizeof = 24（4 + pad4 + ptr8 + 4 + 4）。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuConfig
{
    public uint StructSize;
    public IntPtr ClientName;
    public uint LogLevel;
    public uint EventQueueCapacity;
}

/// <summary>pu_frame_event：C sizeof = 40。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuFrameEvent
{
    public ulong Sequence;
    public long CaptureTsUs;
    public uint Width;
    public uint Height;
    public uint Bytes;
    public uint BitrateKbps;
    public uint Keyframe;
    public uint Reserved;
}

/// <summary>pu_state_event：C sizeof = 16。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuStateEvent
{
    public int State;
    public int PreviousState;
    public int Reason;
    public int Reserved;
}

/// <summary>pu_error_event：C sizeof = 240（4 + 4 + 232）。</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PuErrorEvent
{
    public int Code;
    public int Reserved;
    public fixed byte Message[232];
}

/// <summary>pu_signal_event：C sizeof = 24（ptr8 + 4 + 4 + 4 + pad4）。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuSignalEvent
{
    public IntPtr Payload;
    public uint PayloadLen;
    public uint Kind;
    public uint Reserved;
}

/// <summary>pu_remote_frame_event：C sizeof = 40。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuRemoteFrameEvent
{
    public ulong Sequence;
    public long TimestampUs;
    public long ReceivedAtUs;
    public uint Width;
    public uint Height;
    public uint FramesReceived;
    public uint Reserved;
}

/// <summary>
/// pu_event.data 的 union：C 侧 240 字节，所有成员都从 offset 0 开始。
/// 用 LayoutKind.Explicit 表达重叠，不做手写偏移读取。
/// </summary>
[StructLayout(LayoutKind.Explicit, Size = 240)]
internal struct PuEventData
{
    [FieldOffset(0)] public PuFrameEvent Frame;
    [FieldOffset(0)] public PuStateEvent State;
    [FieldOffset(0)] public PuErrorEvent Error;
    [FieldOffset(0)] public PuSignalEvent Signal;
    [FieldOffset(0)] public PuRemoteFrameEvent Remote;
}

/// <summary>pu_event：C sizeof = 256（4 + 4 + 8 + 240）。</summary>
[StructLayout(LayoutKind.Sequential, Size = 256)]
internal struct PuEvent
{
    public uint StructSize;
    public uint Type;
    public long TimestampUs;
    public PuEventData Data;
}

/// <summary>pu_stats：C sizeof = 64。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuStats
{
    public uint StructSize;
    public int PeerState;
    public int IceState;
    public uint HasPeer;
    public uint FramesSent;
    public uint FramesEncoded;
    public uint FramesDropped;
    public uint FramesReceived;
    public uint SignalOutCount;
    public uint EventDrops;
    public uint RemoteWidth;
    public uint RemoteHeight;
    public uint Reserved;
    public ulong LastRemoteFrameUs;
}

/// <summary>
/// pu_voice_track（v2.7）：单条远端音频轨的统计。
/// id 形如 "partyup-slot-&lt;n&gt;"（relay 槽位号），配合 voice.state.slots 映射到成员。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuVoiceTrack
{
    public uint StructSize;
    /// <summary>webrtc track id，UTF-8，最多 63 字节 + NUL。</summary>
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
    public byte[] Id;
    /// <summary>累计「有声」PCM 帧数（每帧 10 ms；静音帧不计）。</summary>
    public uint PcmFrames;
    /// <summary>最近一帧峰值振幅（16 bit 有符号采样绝对值）。</summary>
    public uint Peak;
    /// <summary>最近一帧的单调时钟毫秒；0 = 从未收到。</summary>
    public ulong LastFrameMs;
}

/// <summary>pu_voice_params：C sizeof = 24。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuVoiceParams
{
    public uint StructSize;
    public IntPtr ChannelId;
    public int Mode;
    public uint Flags;
}

/// <summary>pu_source_info：C sizeof = 40。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuSourceInfo
{
    public uint StructSize;
    public IntPtr Id;
    public IntPtr Title;
    public uint Width;
    public uint Height;
    public uint Kind;
    public uint Reserved;
}

/// <summary>pu_source_list：C sizeof = 16。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuSourceList
{
    public uint StructSize;
    public uint Count;
    public IntPtr Items;
}

/// <summary>pu_frame：C sizeof = 40。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuFrame
{
    public uint StructSize;
    public uint Width;
    public uint Height;
    public uint Stride;
    public long TimestampUs;
    public IntPtr Pixels;
    public ulong StreamId;
}

/// <summary>
/// pu_audio_device：C sizeof = 204（4+4+4+128+64）。IsDefault 表示该设备是当前系统
/// 默认端点（按端点 ID 匹配），不是 index==0；Index 只在当前枚举快照内有效。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuAudioDevice
{
    public uint StructSize;
    public int Index;
    public int IsDefault;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
    public byte[] Name;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
    public byte[] Guid;
}

/// <summary>pu_audio_status：C sizeof = 292（9×4 + 128 + 128）。</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuAudioStatus
{
    public uint StructSize;
    public int InputIndex;
    public int OutputIndex;
    public uint InputCount;
    public uint OutputCount;
    public uint Generation;
    public uint Changed;
    public uint Fallback;
    /// <summary>旧 ABI 的 reserved 字段：v2.15 起 bit0/bit1 = 输入/输出方向因设备切换/回退失败而未运行。</summary>
    public uint Reserved;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
    public byte[] InputName;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
    public byte[] OutputName;
}

/// <summary>
/// pu_voice_level：v2.19 之后 C sizeof = 72。前 32 字节是老布局（v2.14–v2.18），
/// 后面是 AUD02 追加的「与音量无关的采集 liveness 计数」；原生按
/// <c>min(struct_size, sizeof)</c> 填充，所以传 32 仍只填旧字段（老客户端不受影响）。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuVoiceLevel
{
    public uint StructSize;
    public int Valid;
    public uint Peak;
    public uint Rms;
    public uint Frames;
    public uint Source;
    public ulong LastFrameMs;

    // ---- v2.19 尾部追加：静音/全零 PCM 也会增长，可用来判断"麦克风采集是否还活着" ----
    /// <summary>语音采集回调次数（每次回调 +1，与音量无关）。</summary>
    public uint CaptureFrames;
    public uint Reserved;
    /// <summary>这些回调累计交付的 PCM 字节数。</summary>
    public ulong CaptureBytes;
    /// <summary>最近一次采集回调的单调毫秒数（只比较差值，不是墙钟）。</summary>
    public ulong LastCbMs;
    public uint AdmInitialized;
    public uint AdmRecording;
    public uint AdmRecordingInitialized;
    /// <summary>共享 ADM 实例代次：多次 join 之间不变 = 复用同一块 ADM。</summary>
    public uint AdmInstance;
}

/// <summary>
/// pu_audio_monitor_info：C sizeof = 40（4×8 + 8）。State 是请求状态
/// （0=Idle / 1=Active / 2=Faulted），Running 是实测状态（-1 未知 / 0 否 / 1 是）；
/// 两者必须分开读。Valid=0 时 Peak/Rms/Frames 不能当成「有效静音」。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuAudioMonitor
{
    public uint StructSize;
    public int State;
    public int Running;
    public uint Valid;
    public uint Peak;
    public uint Rms;
    public uint Frames;
    public uint Reserved;
    public ulong LastFrameMs;
}

/// <summary>
/// pu_receive_stats_info（v2.16/B2）：独立接收会话的只读统计。
/// C sizeof = 32（4×5 + 4 + 8）。会话关闭后 active=0，其余字段按 0 报。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuReceiveStatsInfo
{
    public uint StructSize;
    public int Active;
    public uint Token;
    public uint VideoTracks;
    public uint FramesDelivered;
    public uint Reserved;
    public ulong LastFrameMs;
}

/// <summary>
/// pu_publish_stats_info（v2.17/B2b direct）：共享者 per-viewer 发送会话的统计。
/// C sizeof = 40（4×4 + 8 + 8 + 4 + 4）。packets_sent 是标准 GetStats 的
/// outbound-rtp.packetsSent，不含 ICE/DTLS 保活，可证明「该会话 RTP 是否还在发」。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuPublishStatsInfo
{
    public uint StructSize;
    public int Active;
    public uint Token;
    public uint PacketsSent;
    public ulong BytesSent;
    public ulong FramesEncoded;
    public uint HasRemote;
    public uint Reserved;
}

/// <summary>
/// pu_rtp_stats_info（v2.17）：接收会话的真实 inbound RTP 计数。
/// C sizeof = 40（4×4 + 8 + 8）。与 frames_delivered 的区别是它在网络层：
/// 停看后包数不再增长，而 ICE/DTLS 保活不会增加它。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PuRtpStatsInfo
{
    public uint StructSize;
    public int Active;
    public uint PacketsReceived;
    public uint Reserved;
    public ulong BytesReceived;
    public ulong FramesDecoded;
}
