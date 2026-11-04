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

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    public static extern int pu_screen_unwatch(IntPtr engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string sharerId);

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
