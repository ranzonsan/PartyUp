/* PartyUp 媒体核心 · 窄 C ABI（v2）
 *
 * 设计约束（见 docs/ARCHITECTURE.md §2.1 与 .docs/spikes/2026-11-01-c-abi.md、
 * .docs/spikes/2026-11-01-signaling-abi.md）：
 *   * 纯 C 接口，只暴露 C 类型：不透明句柄 + 定长 POD 结构 + 返回码；
 *   * 不跨边界传 std::string / std::vector，字符串一律 const char*（UTF-8，0 结尾），
 *     内存由调用方或库持有，所有权在函数注释里写死；
 *   * 任何 C++ 异常都不会穿越 ABI：失败一律用负返回码 + pu_last_error()；
 *   * 头文件可被 C 与 C++ 同时包含；C++ 侧编译 DLL 时定义 PU_BUILD_SHARED。
 *
 * 版本历史：
 *   v1（第一切片）6 个函数：engine_create / engine_destroy / screen_start /
 *       screen_stop / poll_event / last_error。只有合成采集源，没有网络。
 *   v2.3（本版）把屏幕采集换成真实 WGC（Windows Graphics Capture），但 ABI 仍是
 *       v2：pu_screen_sources 的返回格式没有变（用的是 v2 已有的 pu_source_info
 *       的 id/title/width/height/kind 字段），只是终于实现了；pu_screen_start 的
 *       参数语义扩展为「接受 pu_screen_sources 返回的 id」。结构体布局、函数签名
 *       一个都没动，v2 客户端无需重编译。
 *   v2 补上信令通道并把真 PeerConnection 接进来：
 *       - 新函数 pu_signal_in（UI 把服务端下发的信令 JSON 原样灌进来）、
 *         pu_peer_stats（读回发/收帧计数，测试用来断言）；
 *       - 新事件 PU_EVENT_SIGNAL_OUT（待发往服务端的信令 JSON）、
 *         PU_EVENT_REMOTE_FRAME（解出的远端帧）；
 *       - pu_screen_start 现在会建 PeerConnection、AddTrack、生成 offer；
 *       - v1 的 6 个函数语义**不变**，v1 的调用方无需改代码。
 *   v2.10（加法式扩张，ABI 仍 v2）：语音与屏幕共享拆成两条独立 PeerConnection，
 *       信令必须能明确指定投给哪条会话（candidate 信封里没有媒体类型，SDP 嗅探只能
 *       猜）。新增两个导出、共 21 个：
 *       - pu_signal_in_media(e, media, payload, len)：显式指定 "voice" / "screen"；
 *       - pu_signal_media(event, out, capacity)：从 SIGNAL_OUT 事件读回它的 media；
 *       - pu_signal_in 保留为兼容入口（media==nullptr，退回 SDP 嗅探）。
 *   v2.11（加法式扩张，ABI 仍 v2）：屏幕共享支持「被动方」（relay 里服务端 offer、
 *       客户端 answer），并允许观看者预建屏幕会话。导出 21 → 23：
 *       - pu_screen_start_ex(e, source_id, flags)：flags bit0 = passive
 *         （采屏但不发 offer，等对端 offer 后挂轨回 answer）；flags==0 等价旧入口；
 *       - pu_screen_prepare(e)：观看者预建 answerer 会话，幂等、不采屏不建轨；
 *       - pu_screen_start 行为不变。
 *   v2.12（加法式扩张，ABI 仍 v2）：直连（direct）需要 srflx 候选才能穿 NAT，而
 *       此前所有 PeerConnection 都是 host-only。新增一个导出、共 24 个：
 *       - pu_engine_set_ice_servers(e, urls, len)：引擎级 ICE servers，
 *         urls 换行分隔的 "stun:host:port"；建 PC 前调用，之后新建的每条会话
 *         （屏幕 + 语音）都会带上；空串/NULL 清空。
 *       - 未调用或清空时行为与 v2.11 完全一致（host-only）。
 *   v2.13（无 ABI 变化，导出仍 24 个；只改屏幕编码策略与内部实现）：兑现
 *       PRODUCT.md「弱网降帧不降分辨率」，并让起播码率跟分辨率走。
 *       - 屏幕 video sender 固定 degradation_preference = MAINTAIN_RESOLUTION
 *         （弱网优先丢帧、不降分辨率，文字不糊）；content_hint = kText。
 *       - 码率上限 3 Mbps / 下限 300 kbps（sender 的 RtpEncodingParameters，
 *         编码器侧再夹一道）；上限是**平均**码率，关键帧突发的瞬时峰值会短暂超过它。
 *       - 起播码率不再用 libwebrtc 的固定 300 kbps，改为
 *         clamp(width*height*0.75, 800 kbps, 2.5 Mbps)（1440p -> 2.5 Mbps）。
 *         仍是 UnconstrainedVBR，不是 CBR。
 *       - 屏幕共享**默认不缩放**，按采集原生分辨率编码（2026-11-04 实测：常见带宽下
 *         原生分辨率明显更锐；强制降 1080p 是画质回退）。若部署方确实要限制编码高度，
 *         设环境变量 PARTYUP_MAX_CAPTURE_HEIGHT=<像素高>（<=0 或未设 = 不缩放）。
 *       - 以上都是内部行为，调用方代码与签名无需任何改动。
 *   v2.15（无 ABI 变化，导出仍 30 个、结构布局与旧导出不变；语义增强）：
 *       - pu_audio_set_device 在通话中执行真实设备热切（Stop->SetDevice->Init->
 *         Start），PU_OK 只在新设备真正启动后返回；失败回滚/回落默认/明确报错。
 *       - 选择按 ADM 的稳定端点 ID（guid）保存，设备重排不会指向别人；index=-1
 *         始终走 WindowsDeviceType::kDefaultDevice，绝不映射枚举 0。
 *       - pu_audio_status 会推进真实重新选择/回退：拔掉选中设备 → 真正切回默认；
 *         系统默认端点变化（数量/名称不变也可发现，用 CoreAudio eConsole 端点 ID）
 *         → 真正重新选择并重启；reserved 的低两位报告「该方向已停止运行」故障。
 *       - pu_audio_devices 的 is_default 改为按系统默认端点 ID 匹配，不再假设
 *         index==0（Windows ADM 的枚举根本没有默认伪设备项）。
 *       - pu_engine_caps 增加 bit4，供托管层区分「真正热切」与旧 DLL 的
 *         「只登记下次 join」。
 *       - 浏览器/UI 侧仍须轮询 pu_audio_status 才能及时发现热插拔与默认变化；
 *         本版没有主动事件通道（不新增导出，旧客户端不受影响）。
 *   v2.16（B2，加法式扩张，ABI 仍 v2、导出 30 → 35）：观看方的**严格接收会话**。
 *       - pu_screen_receive_open/close：独立 receive-only PeerConnection（新
 *         ICE/DTLS/SSRC），与发布 PC、语音、采集源完全分离；
 *       - pu_signal_in_session：按会话灌入 offer/candidate；
 *       - pu_screen_receive_stats：只读会话帧计数；
 *       - pu_screen_pause_send：legacy direct 的单 PC 暂停/恢复（保留采集）。
 *       - pu_engine_caps bit5 广告本组能力。
 *   v2.17（B2b direct，加法式扩张，ABI 仍 v2、导出 35 → 40）：共享方的
 *       **per-viewer 发送会话**与真实 RTP 计数。direct 严格观看要求每个 viewer
 *       一条独立发送传输，旧 pause_send（同一条 PC 上换轨）不能证明传输隔离。
 *       - pu_screen_publish_open：用**已存在的采集源/视频轨**建一条新的
 *         send-only PeerConnection（新 ICE/DTLS/SSRC），不重启采集、不改
 *         screen.state；每 viewer 一条，停看只关该会话；
 *       - pu_screen_publish_signal_in / pu_screen_publish_close：按会话灌 answer/
 *         candidate、只关闭该会话；
 *       - pu_screen_publish_stats / pu_screen_receive_rtp_stats：标准 GetStats 的
 *         outbound/inbound RTP 包数/字节数，用来证明「停看后该会话 RTP 停止」而
 *         不是只看到 ICE 保活；
 *       - pu_engine_caps bit6 广告本组能力。旧 DLL 没有这些导出 → 客户端必须明确
 *         报不支持，不能退回 pause-only 冒充严格隔离。
 *   v2.18（AUD02，加法式扩张，ABI 仍 v2、导出 40 → 43）：独立本端输入电平。
 *       - pu_audio_monitor_start / pu_audio_monitor_stop / pu_audio_monitor_status：
 *         在**同一条通话 ADM** 上显式启停输入采集，用官方
 *         CreateAudioDeviceWithDataObserver 包住生产 ADM；observer 的真实 16 bit
 *         PCM 只用于 peak/RMS/帧数/新鲜时间，不建第二条录音栈、不进语音区、不建
 *         PeerConnection、不发送媒体或信令。
 *       - 默认不采集；只有显式 Start 成功才进入 Active。语音会话存在时拒绝 Start；
 *         进入语音前会先停掉 monitor，确认不了停止就拒绝入语音。
 *       - 新结构 pu_audio_monitor_info（40 字节）区分请求状态与实测 running，
 *         Stop/读回失败进入 Faulted 并如实报告仍运行/未知，不能只清 active 镜像。
 *       - pu_engine_caps bit7 广告本组能力；旧 DLL 没有导出 → 明确报不支持。
 *
 * 线程约定：所有 pu_* 调用必须来自同一个「控制线程」（通常是 UI 线程），
 * 且不得在库触发的任何回调里重入——本库没有任何回调，通知一律走 pu_poll_event。
 */
#ifndef PARTYUP_MEDIA_H
#define PARTYUP_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI 版本。结构体或函数签名有不兼容改动时 +1。
 * v1 → v2：只**新增**（事件类型、结构、函数），没有改动任何 v1 已有字段的
 * 偏移与语义，所以 v1 编译出来的客户端仍能跑（除非它把 pu_event 的 type 当作
 * 枚举全集做了穷举断言）。 */
#define PU_ABI_VERSION 2

#if defined(_WIN32)
#  if defined(PU_BUILD_SHARED)
#    define PU_API __declspec(dllexport)
#  elif defined(PU_STATIC)
#    define PU_API
#  else
#    define PU_API __declspec(dllimport)
#  endif
#else
#  define PU_API __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------ */
/* 返回码                                                             */
/* ------------------------------------------------------------------ */
typedef enum pu_status {
  PU_OK = 0,
  PU_ERR_INVALID_ARG = -1, /* 参数非法：空指针、struct_size 不匹配、未知枚举值 */
  PU_ERR_STATE = -2,       /* 当前状态不允许该操作（如重复 start） */
  PU_ERR_TIMEOUT = -3,     /* pu_poll_event：超时没有事件 */
  PU_ERR_NO_MEMORY = -4,
  PU_ERR_MEDIA = -5,       /* libwebrtc / 编码器初始化或运行失败 */
  PU_ERR_INTERNAL = -6,    /* 其他内部错误（含被拦截的 C++ 异常） */
  PU_NOT_IMPLEMENTED = -100 /* 本版未实现的函数 */
} pu_status;

/* ------------------------------------------------------------------ */
/* 基础类型                                                           */
/* ------------------------------------------------------------------ */

/* 引擎句柄。只能通过 pu_engine_create 获得，pu_engine_destroy 释放。 */
typedef struct pu_engine pu_engine;

/* 屏幕共享源标识：UTF-8、0 结尾的字符串。
 *   "synthetic" / NULL     → 内置合成帧源（640x360 渐变 @30fps），测试/兜底用；
 *   pu_screen_sources 的 id → 真实屏幕采集（Windows Graphics Capture）。
 * 屏幕 id 形如 "display:0x0000000000050001"，是 HMONITOR 的十六进制值：
 *   * 它是「这块屏」的稳定标识，按它就能重新找回同一块屏；
 *   * 不要用 GenericCaptureItem 的 DisplayName（"显示 2"）做匹配，它和 DXGI 的
 *     \\.\DISPLAYn 编号不一致，多屏时会张冠李戴；
 *   * 另外接受 "\\.\DISPLAY1" 这种设备名别名（方便命令行手工传入）。
 * 字符串由调用方持有，只在本次调用期间读取。 */
typedef const char* pu_screen_id;

/* 引擎配置。调用方填 struct_size = sizeof(pu_config)，其余字段可选。 */
typedef struct pu_config {
  uint32_t struct_size;        /* 必须等于 sizeof(pu_config) */
  const char* client_name;     /* 可选，UTF-8，调用方持有；仅 create 期间读取 */
  uint32_t log_level;          /* 预留：0=off 1=error 2=warn 3=info；本版只用 stderr */
  uint32_t event_queue_capacity; /* 事件队列容量；0 = 默认 256 */
} pu_config;

/* 路径模式（ARCHITECTURE §4）。 */
typedef enum pu_mode {
  PU_MODE_RELAY = 0,
  PU_MODE_DIRECT = 1
} pu_mode;

/* 屏幕共享流状态。事件 PU_EVENT_STATE 的 state / previous_state 取值。 */
typedef enum pu_stream_state {
  PU_STREAM_IDLE = 0,
  PU_STREAM_STARTING = 1,
  PU_STREAM_RUNNING = 2,
  PU_STREAM_STOPPING = 3,
  PU_STREAM_FAILED = 4
} pu_stream_state;

/* 状态迁移原因。 */
typedef enum pu_state_reason {
  PU_REASON_NONE = 0,
  PU_REASON_REQUESTED = 1, /* 调用方发起（start/stop） */
  PU_REASON_MEDIA_FAILURE = 2
} pu_state_reason;

/* 事件类型。 */
typedef enum pu_event_type {
  PU_EVENT_NONE = 0,
  PU_EVENT_STATE = 1,        /* 状态迁移：data.state */
  PU_EVENT_FRAME = 2,        /* 一帧本地编码完成：data.frame */
  PU_EVENT_ERROR = 3,        /* 错误：data.error */
  /* ---- v2 新增 ---- */
  PU_EVENT_SIGNAL_OUT = 4,   /* 待发往服务端的信令 JSON：data.signal */
  PU_EVENT_REMOTE_FRAME = 5  /* 解出一帧远端视频：data.remote */
} pu_event_type;

/* 信令种类（pu_signal_event.kind）。与信封里的 "type" 字段一一对应，
 * 方便 UI 不解析 JSON 就能分类（日志/统计/选通道）。 */
typedef enum pu_signal_kind {
  PU_SIGNAL_OFFER = 1,
  PU_SIGNAL_ANSWER = 2,
  PU_SIGNAL_CANDIDATE = 3
} pu_signal_kind;

/* 待发往服务端的信令（PU_EVENT_SIGNAL_OUT）。
 *
 * payload 是**库持有**的 UTF-8 JSON，不是 0 结尾（按 payload_len 读；实现上额外
 * 保证末尾有 0，方便 C 侧直接当字符串用，但不要依赖它）。指针有效期：
 *   * 到 pu_engine_destroy 为止，或
 *   * 到该引擎累计又有 64 条信令被取走、最旧这条被回收为止
 *     （即「取走后再用一下」永远安全，长期持有指针不安全）。
 * 因为 SDP 有几 KB，装不进定长事件结构，只能给指针——这与 v1 的「事件按值复制」
 * 不同，是 v2 唯一的「库持有、调用方只读」输出。
 *
 * 信封格式见 .docs/spikes/2026-11-01-signaling-abi.md：
 *   {"type":"offer","sdp":"..."}
 *   {"type":"answer","sdp":"..."}
 *   {"type":"candidate","mid":"0","mline":0,"candidate":"candidate:..."}
 */
typedef struct pu_signal_event {
  const char* payload;    /* UTF-8 JSON，库持有，见上；可能为 NULL（不应发生） */
  uint32_t payload_len;   /* payload 的字节数，不含结尾 0 */
  uint32_t kind;          /* pu_signal_kind */
  uint32_t reserved;      /* 0 */
} pu_signal_event;

/* 解出的一帧远端视频（PU_EVENT_REMOTE_FRAME）。
 * 这里只给「有一帧到了」的信息与尺寸，像素数据要等后续版本的 pu_frame_acquire；
 * 真正权威的计数看 pu_peer_stats。 */
typedef struct pu_remote_frame_event {
  uint64_t sequence;       /* 该引擎收到的第 N 帧，0 起 */
  int64_t timestamp_us;    /* 解码器重建的采样时间戳：不是 system_clock 纪元，
                            * 只用来算帧间隔/抖动；对时用 received_at_us */
  int64_t received_at_us;  /* 本端收到的时刻，system_clock 纪元微秒 */
  uint32_t width;
  uint32_t height;
  uint32_t frames_received; /* 累计收到帧数 = sequence + 1 */
  uint32_t reserved;
} pu_remote_frame_event;

/* 一帧编码完成。每送出一帧采集帧就产生一条。布局与语义对调用方不变；v2.1 起
 * 编码只在 PC 的 VideoStreamEncoder 里做一次（以前同一帧还会被本地统计编码器
 * 再编一次，采集源挂了两个 sink）。
 *
 * 字段来源分两类：
 *   * sequence / capture_ts_us / width / height 由采集侧直接给出；
 *   * bytes / bitrate_kbps / keyframe 从 PC 的 GetStats 派生（outbound-rtp 的
 *     bytesSent / framesEncoded / keyFramesEncoded 增量），按 250 ms 采样：
 *       bytes        = 最近一个窗口的「bytesSent 增量 / framesEncoded 增量」，
 *                      即该窗口的平均每帧编码字节数（不是本帧的精确字节数）；
 *       bitrate_kbps = 最近一个窗口的编码码率（kbit/s）；
 *       keyframe     = 该窗口内 keyFramesEncoded 有增长时为 1。
 *     VideoFrame 本身没有关键帧标记，只能从统计增量推。
 */
typedef struct pu_frame_event {
  uint64_t sequence;      /* 该路流内的帧序号，0 起 */
  int64_t capture_ts_us;  /* 源帧时间戳，system_clock 纪元微秒 */
  uint32_t width;
  uint32_t height;
  uint32_t bytes;         /* 最近采样窗口的平均每帧编码字节数（见上） */
  uint32_t bitrate_kbps;  /* 最近采样窗口的编码码率（kbit/s） */
  uint32_t keyframe;      /* 1 = 该采样窗口内出现过关键帧 */
  uint32_t reserved;      /* 0，保证对齐与后续扩展 */
} pu_frame_event;

/* 状态迁移。 */
typedef struct pu_state_event {
  int32_t state;          /* pu_stream_state */
  int32_t previous_state; /* pu_stream_state */
  int32_t reason;         /* pu_state_reason */
  int32_t reserved;
} pu_state_event;

/* 错误。message 为 UTF-8、0 结尾，超长截断。 */
typedef struct pu_error_event {
  int32_t code;           /* pu_status 负值，或 PU_ERR_MEDIA 等 */
  int32_t reserved;
  char message[232];
} pu_error_event;

/* 事件。size 固定 256 字节、8 字节对齐；调用方先填 struct_size 再交给
 * pu_poll_event，库只写这一版认识的字段。
 * 布局（v2 只在 union 里追加成员，v1 的四个偏移一个没动）：
 *   0   struct_size   4   type
 *   8   timestamp_us
 *   16  data（union，240 字节）：
 *         frame:   sequence@16 capture_ts_us@24 width@32 height@36
 *                  bytes@40 bitrate_kbps@44 keyframe@48 reserved@52
 *         state:   state@16 previous_state@20 reason@24 reserved@28
 *         error:   code@16 reserved@20 message[232]@24
 *         signal:  payload@16 payload_len@24 kind@28 reserved@32   （v2，24 字节）
 *         remote:  sequence@16 timestamp_us@24 received_at_us@32 width@40
 *                  height@44 frames_received@48 reserved@52          （v2，40 字节）
 */
typedef struct pu_event {
  uint32_t struct_size;      /* 调用方填 sizeof(pu_event) */
  uint32_t type;             /* pu_event_type */
  int64_t timestamp_us;      /* 事件产生时间，system_clock 纪元微秒 */
  union {
    pu_frame_event frame;
    pu_state_event state;
    pu_error_event error;
    pu_signal_event signal;  /* v2: PU_EVENT_SIGNAL_OUT */
    pu_remote_frame_event remote; /* v2: PU_EVENT_REMOTE_FRAME */
  } data;
} pu_event;

/* PeerConnection 状态（pu_stats.peer_state）。取值与 libwebrtc 的
 * PeerConnectionInterface::PeerConnectionState 一致，直接映射，不重新编号。 */
typedef enum pu_peer_state {
  PU_PEER_NONE = -1,        /* 引擎里还没有 PeerConnection（还没 start，也没收到 offer） */
  PU_PEER_NEW = 0,
  PU_PEER_CONNECTING = 1,
  PU_PEER_CONNECTED = 2,    /* ICE 连通 + DTLS 握手完成，媒体可通 */
  PU_PEER_DISCONNECTED = 3,
  PU_PEER_FAILED = 4,
  PU_PEER_CLOSED = 5
} pu_peer_state;

/* ICE 状态（pu_stats.ice_state）。同样与 libwebrtc 的 IceConnectionState 对齐。 */
typedef enum pu_ice_state {
  PU_ICE_NEW = 0,
  PU_ICE_CHECKING = 1,
  PU_ICE_CONNECTED = 2,
  PU_ICE_COMPLETED = 3,
  PU_ICE_FAILED = 4,
  PU_ICE_DISCONNECTED = 5,
  PU_ICE_CLOSED = 6
} pu_ice_state;

/* 引擎实时统计（pu_peer_stats 输出）。全字段由引擎在内部原子量上快照，不加锁、
 * 不发起异步 GetStats，因此这个调用是即时的（微秒级）。
 *
 * 「丢了多少帧」的算法：发送端 frames_sent（推进采集源的帧数）减去接收端
 * frames_received（解码后回调到的帧数）。两个引擎各读一次就能算出来；同一引擎
 * 自环时 frames_sent - frames_received 也成立（发送与接收在同一个引擎上时）。 */
typedef struct pu_stats {
  uint32_t struct_size;        /* 调用方填 sizeof(pu_stats) */
  int32_t  peer_state;         /* pu_peer_state；无 PC 时是 PU_PEER_NONE */
  int32_t  ice_state;          /* pu_ice_state；无 PC 时是 0 */
  uint32_t has_peer;           /* 1 = 引擎内已建 PeerConnection */
  uint32_t frames_sent;        /* 推进采集源的帧数（本地发送侧） */
  uint32_t frames_encoded;     /* PC 侧编码完成的帧数（outbound-rtp.framesEncoded，
                                * 250 ms 采样一次，因此最多陈旧一个窗口）。1:1 编码
                                * 时它等于 frames_sent 与 PU_EVENT_FRAME 的条数。 */
  uint32_t frames_dropped;     /* v2.1 起恒 0：本地编码器已删除，PC 侧的丢帧不在
                                * outbound-rtp 统计里（保留字段，ABI 不变） */
  uint32_t frames_received;    /* 远端解码后回调到引擎的帧数（接收侧硬判据） */
  uint32_t signal_out_count;   /* 已产生的信令事件数（offer/answer/candidate 合计） */
  uint32_t event_drops;        /* 事件队列满导致的丢事件数 */
  uint32_t remote_width;       /* 最近一帧远端视频的宽，0 = 还没收到 */
  uint32_t remote_height;      /* 最近一帧远端视频的高 */
  uint32_t reserved;           /* v2.4 起复用为「实际使用的视频编码器」：
                                *   0 = 还没有编码器（未 start / 未协商）
                                *   1 = 硬件 H.264 MFT（NVENC / QSV / VCE）
                                *   2 = 软件 H.264 MFT（系统 H264 Encoder MFT）
                                *   3 = 软件 VP8（H.264 不可用时的回落）
                                * 类型与偏移不变，ABI 仍是 v2。 */
  uint64_t last_remote_frame_us; /* 最近一帧远端帧的接收时刻，0 = 还没收到 */
} pu_stats;

/* 后续函数用到的结构（本版只声明，字段在实现对应功能时定稿）。 */

/* 语音参数（pu_voice_join）。 */
typedef struct pu_voice_params {
  uint32_t struct_size;   /* sizeof(pu_voice_params) */
  const char* channel_id; /* UTF-8，调用方持有 */
  pu_mode mode;
  uint32_t flags;         /* 预留，0 */
} pu_voice_params;

/* 一个可共享的屏幕/窗口（pu_screen_sources）。 */
typedef struct pu_source_info {
  uint32_t struct_size;   /* sizeof(pu_source_info) */
  const char* id;         /* UTF-8，库持有，有效期同 pu_source_list */
  const char* title;      /* UTF-8，库持有 */
  uint32_t width;         /* 物理像素（真实屏幕）/ 合成尺寸（synthetic） */
  uint32_t height;
  uint32_t kind;          /* 0=屏幕 1=窗口（预留） 2=合成源 */
  uint32_t reserved;
} pu_source_info;

/* 源列表（pu_screen_sources 输出）。items 由库持有，有效期到下一次
 * pu_screen_sources 调用或 pu_engine_destroy。 */
typedef struct pu_source_list {
  uint32_t struct_size;   /* sizeof(pu_source_list) */
  uint32_t count;
  const pu_source_info* items;
} pu_source_list;

/* 一帧待渲染数据（pu_frame_acquire）。
 *
 * 语义（v2.5 起是真实现，结构体布局与 v2 完全一致、未改动）：
 *   * pixels 由引擎持有，**在 pu_frame_release 之前一直有效**且内容稳定
 *     （不会撕裂，也不会被下一帧覆盖）。
 *   * acquire / release 必须**配对**；漏掉 release 时该槽位不会回收，
 *     引擎会记日志 + 计数（不会泄漏内存）。
 *   * 没有比上次更新过的帧时 `pu_frame_acquire` 返回 **PU_ERR_TIMEOUT**，
 *     表示「还没好，继续等」，不是失败 —— **调用方必须忽略它并稍后重试**，
 *     不得把它当成错误去重建引擎或停止拉帧（源 55fps、UI 60Hz 时本来就会周期性出现）。
 *   * acquire 只会返回比上一次更新过的帧；同一帧不会被重复交付两次。
 *   * stride 是**如实**的行字节数，客户端必须按 stride 逐行拷贝，
 *     不能假设 stride == width * 4。
 *   * stream_id 是不透明流标识。pu_frame_acquire 的入参 stream_id 兼容约定：
 *       - "" 或 "remote"：表示**当前选台**（旧客户端 / C# 一直传 "remote"）；
 *       - 其它字符串：按显式 watch 的 target member id 理解，必须与当前 arm 一致，
 *         未 arm 或不是当前目标时返回 PU_ERR_INVALID_ARG。
 *     输出字段 stream_id 在 armed 时为当前原生 epoch（从未 watch 的 legacy 为 1）。
 */
typedef struct pu_frame {
  uint32_t struct_size;   /* sizeof(pu_frame) */
  uint32_t width;
  uint32_t height;
  uint32_t stride;        /* 每行字节数 */
  int64_t timestamp_us;
  const void* pixels;     /* BGRA8，引擎持有 */
  uint64_t stream_id;     /* 不透明流标识 */
} pu_frame;

/* ------------------------------------------------------------------ */
/* 函数                                                               */
/* ------------------------------------------------------------------ */

/* 创建引擎。cfg 可为 NULL（等价于全默认）。成功返回非空句柄，失败返回 NULL 并
 * 设置进程级错误串（用 pu_last_error(NULL) 读取）。
 * 生命周期：内部会启动 libwebrtc 的 network/worker/signaling 三条线程与媒体工厂，
 * 因此本调用有一次性开销（约几十毫秒）。 */
PU_API pu_engine* pu_engine_create(const pu_config* cfg);

/* 销毁引擎。会先停止屏幕共享，再按 Close → track → factory → signaling → worker →
 * network 的顺序收尾。e 为 NULL 时安全无操作。之后句柄不可再用。 */
PU_API void pu_engine_destroy(pu_engine* e);

/* v2.12（第 24 个导出）：设置本引擎所有 PeerConnection 的 ICE servers。
 *
 * 用途：direct（P2P 直连）模式下两端需要交换 srflx 候选才可能穿 NAT；不设置时
 *       引擎只收集 host（局域网）候选，跨 NAT 必然失败。
 *
 * urls 格式：换行分隔的 STUN URL 列表（容忍 \r 与首尾空白，跳过空行）：
 *     "stun:stun.example.com:3478\nstun:1.2.3.4:3478"
 *   只支持 "stun:" scheme，且**不带凭证**；"turn:" / "turns:" / "stuns:" 需要
 *   username/credential，本版 ABI 没有承载它们的字段，会返回 PU_ERR_INVALID_ARG
 *   （不会静默忽略——静默忽略会让调用方以为 TURN 已生效）。
 *   len 是 urls 的字节数（不含结尾 NUL 也可以，len 就是有效长度）。
 *   urls == NULL 或 len == 0 表示**清空**，回到 host-only（与 v2.11 行为一致）。
 *
 * 作用域与时机：
 *   - 配置是**引擎级**的，屏幕会话与语音会话共用同一份；
 *   - 只影响**此后新建**的 PeerConnection（pu_screen_start* / pu_screen_prepare /
 *     pu_voice_join 那一刻读配置）。已经建立的会话不会被追溯修改；要让新配置生效，
 *     必须先 pu_screen_stop / pu_voice_leave 再重新开始；
 *   - 幂等：重复调用整体覆盖旧值。
 *
 * 线程：与其它 pu_* 一样在控制线程调用。内部对配置加锁，因此即使与建 PC 并发
 *      也只会读到「旧的完整配置」或「新的完整配置」，不会读到半个。
 *
 * 返回：PU_OK / PU_ERR_INVALID_ARG（e 为 NULL、urls 为 NULL 但 len != 0、未知
 *       scheme、格式不是 host:port）。 */
PU_API int pu_engine_set_ice_servers(pu_engine* e, const char* urls, size_t len);

/* 开始一路屏幕共享。screen_id 为 NULL 或 "synthetic" 时使用内置合成源。
 * 成功返回 PU_OK；已在运行返回 PU_ERR_STATE；媒体初始化失败返回 PU_ERR_MEDIA，
 * 细节见 pu_last_error()。
 *
 * v2 起本调用还会：建 PeerConnection → AddTrack(视频轨) → CreateOffer →
 * SetLocalDescription → 等 ICE 收集完成后推一个 PU_EVENT_SIGNAL_OUT（offer）。
 * 也就是说 start 返回时 offer 可能还没生成，UI 要照常 poll 事件。 */
PU_API int pu_screen_start(pu_engine* e, pu_screen_id screen_id);

/* v2.11（第 22 个导出）：带**角色**标志的屏幕共享入口。
 *
 * 为什么需要：pu_screen_start 只会「本端当 offerer、自己发 offer」。relay 模式下
 * 服务端是 offerer、客户端是 answerer——共享方也要按 answerer 的方式把视频挂到
 * 服务端 offer 里那条视频段上，不能自己发 offer（会和服务端的 offer 撞车/glare）。
 *
 * flags 位定义（只定义 bit0，其余位**保留**）：
 *   flags == 0        → 与 pu_screen_start(e, source_id) 完全等价（本端 offerer）。
 *   0x1 (PU_SCREEN_START_PASSIVE)
 *                     → 被动共享方：照常采屏并建视频轨，但**不生成 offer**；
 *                       等对端 offer 通过 pu_signal_in_media(e, "screen", ...) 灌进来，
 *                       引擎把视频轨挂到 offer 里那条视频段上并回 answer。
 *   其它位非 0        → PU_ERR_INVALID_ARG（不猜、不静默当 0，否则调用方以为被动
 *                       模式生效、实际却在发 offer，多人同时共享时极难排查）。
 *
 * 语义与 pu_screen_start 相同的部分：source_id 解析规则、采屏、PU_EVENT_FRAME、
 * PU_STREAM_RUNNING 状态事件、pu_screen_stop 停止、pu_peer_stats 统计。
 * 不同点：passive 下**不会**出现 offer 的 SIGNAL_OUT；answer 由对端 offer 触发。
 *
 * 调用顺序（passive）：pu_screen_start_ex(..., 1) → 把 screen.start 发给服务端 →
 * 服务端下发 offer → pu_signal_in_media("screen", offer) → 引擎回 answer。
 * 若 answer 已经发出之后才调本函数，返回 PU_ERR_STATE（那需要重协商，本版不支持；
 * 先 start 再收 offer 即可）。
 *
 * 返回：PU_OK / PU_ERR_INVALID_ARG（未知 flags 位、未知 source id）/ PU_ERR_STATE
 * （已在共享）/ PU_ERR_MEDIA（采集或 PC 初始化失败）。细节见 pu_last_error()。 */
PU_API int pu_screen_start_ex(pu_engine* e, pu_screen_id screen_id, uint32_t flags);

/* pu_screen_start_ex 的 flags bit0：被动共享方（answerer 发视频）。 */
#define PU_SCREEN_START_PASSIVE 0x1u

/* v2.11（第 23 个导出）：观看者**预建**屏幕会话（answerer），不采屏、不建轨。
 *
 * 用途：在服务端 offer 到达之前先把屏幕 PC 建好，offer 一到就能直接
 * SetRemoteDescription + CreateAnswer（省掉建 PC 的延迟，也让信令路径更确定）。
 * 本函数只创建 PeerConnection（role=answerer）；视频轨永远不由它创建。
 *
 * 幂等：PC 已存在（无论之前是 prepare 建的、还是被动 start 建的）直接返回 PU_OK，
 * 不改变角色、不重建会话。重复调用安全。
 *
 * 返回：PU_OK / PU_ERR_MEDIA（建 PC 失败）/ PU_ERR_INVALID_ARG（e 为 NULL）。
 * 线程：与其它 pu_* 相同，只能从控制线程调用。
 * 收尾：pu_screen_stop 会把它关掉（此时没有采集线程，走 stop 的空闲路径）。 */
PU_API int pu_screen_prepare(pu_engine* e);

/* 停止屏幕共享。未在运行时返回 PU_OK（幂等）。
 * v2 起还会 Close 掉 PeerConnection、摘掉远端 sink，并释放 track/pc。 */
PU_API int pu_screen_stop(pu_engine* e);

/* 取一个事件，事件被复制到 out 指向的调用方内存。
 *   timeout_ms > 0：最多等这么多毫秒；
 *   timeout_ms = 0：非阻塞；
 *   timeout_ms < 0：一直等到有事件。
 * 返回 PU_OK 表示取到事件；PU_ERR_TIMEOUT 表示超时；其他为错误。
 * 单消费者：同一引擎不要从多个线程同时调用。
 *
 * 例外：PU_EVENT_SIGNAL_OUT 里的 payload 不复制，是指向库内存的指针，
 * 有效期见 pu_signal_event 的注释（取走后 64 条信令内仍有效，或到 destroy）。 */
PU_API int pu_poll_event(pu_engine* e, pu_event* out, int timeout_ms);

/* 最近一次错误描述（UTF-8，0 结尾）。指针由引擎持有，有效期到下一次调用本函数
 * 或其他 pu_* 函数。e 为 NULL 时返回最近一次 pu_engine_create 失败的原因。 */
PU_API const char* pu_last_error(pu_engine* e);

/* ---- v2 新增 ---- */

/* 把服务端下发的一条信令原样灌进引擎（同步执行，返回时该做的都做了）。
 *
 * payload 指向 UTF-8 JSON，长度 len（不必 0 结尾，中间可以有 0）；字符串由
 * **调用方持有**，只在本次调用期间读取。
 *
 * 接受的信封（与 docs/PROTOCOL.md 对齐，见 .docs/spikes/2026-11-01-signaling-abi.md）：
 *   {"type":"offer","sdp":"..."}                          → 建/复用接收侧 PC，
 *                                                           SetRemoteDescription，
 *                                                           生成并推回 SIGNAL_OUT answer
 *   {"type":"answer","sdp":"..."}                         → SetRemoteDescription(answer)
 *   {"type":"candidate","mid":"0","mline":0,"candidate":"..."} → AddIceCandidate
 * 另外也接受 PROTOCOL.md 的服务端形状，引擎会剥一层 payload：
 *   {"type":"media.offer","payload":{"sdp":"..."}}
 *   {"type":"p2p.signal","payload":{"kind":"ice","candidate":"..."}}
 *
 * 返回：PU_OK / PU_ERR_INVALID_ARG（不是合法 JSON、缺字段、candidate 解析失败）/
 * PU_ERR_STATE（例如正在作为共享方却收到 offer：v2 不做 glare 处理）/
 * PU_ERR_MEDIA（SetRemote/CreateAnswer 等 webrtc 操作失败）/ PU_ERR_INTERNAL。
 * 详细原因在 pu_last_error()。
 *
 * 线程：与其它 pu_* 一样，只能从控制线程调用；内部会阻塞最多几秒（等 SDP 往返），
 * 不要在 UI 线程的渲染路径上直接调——C# 侧应放到后台队列。
 *
 * v2.10 起本函数是**兼容入口**：内部等价于 pu_signal_in_media(e, NULL, payload, len)，
 * 会话归属靠 SDP 嗅探（m=audio / m=video）+ candidate 启发式推断。语音与共享同时在线
 * 时这条推断会歧义（candidate 信封里没有媒体类型），新代码请用 pu_signal_in_media。 */
PU_API int pu_signal_in(pu_engine* e, const char* payload, size_t len);

/* v2.10（第 20 个导出）：把一条信令**明确**投给指定媒体会话。
 *
 *   media = "voice"  → 语音会话（voice_pc_，只有 m=audio）
 *   media = "screen" → 屏幕共享会话（pc_，m=video）
 *   其余值           → PU_ERR_INVALID_ARG（不猜、不静默降级）
 *
 * 为什么必须显式：语音与共享各用一条独立 PeerConnection，而 candidate 信令信封里
 * 只有 mid/mline、没有媒体类型，靠 SDP 嗅探无法判定（会「两边都试」= 静默失败）。
 * 客户端从服务端信令的 media 字段拿到归属后原样转发即可。
 *
 * 语义与 pu_signal_in 完全一致（同步执行、信封格式、返回码、线程要求都相同），
 * 只是去掉了会话归属推断这一步。payload 由**调用方持有**，只在本次调用期间读取。
 * 返回：PU_OK / PU_ERR_INVALID_ARG（media 未知、envelope 不合法、candidate 解析失败）/
 * PU_ERR_STATE（例如把 answer 投给 answerer 角色：role mismatch）/
 * PU_ERR_MEDIA（SetRemote/CreateAnswer 等 webrtc 操作失败）/ PU_ERR_INTERNAL。
 * 详细原因在 pu_last_error()。 */
PU_API int pu_signal_in_media(pu_engine* e, const char* media, const char* payload, size_t len);

/* v2.10（第 21 个导出）：读回一条 PU_EVENT_SIGNAL_OUT 事件属于哪条媒体会话。
 *
 * 为什么需要：UI 把 SIGNAL_OUT 转发给服务端/对端时必须带上 media（服务端按 media
 * 路由；对端也要知道投给哪条会话）。事件结构 pu_event 是 ABI、没有 media 字段，
 * 所以本函数用事件里的 payload 指针向引擎内部查询（media 是内部登记，不跨 ABI）。
 *
 * 参数：
 *   event    由 pu_poll_event 取到的 pu_event（必须原样传，不要自己伪造）
 *   media_out 输出缓冲区，写成 UTF-8、0 结尾的 "voice" / "screen"
 *   capacity  media_out 的字节数，必须 >= 媒体名长度 + 1（当前最大 7）
 *
 * 返回：
 *   PU_OK            成功，media_out 已写入并以 0 结尾
 *   PU_ERR_INVALID_ARG event 为 NULL / struct_size 小于 sizeof(pu_event) /
 *                     event->type != PU_EVENT_SIGNAL_OUT（不是信令事件）/
 *                     media_out 为 NULL / capacity <= 0 / capacity 太小装不下
 *   PU_ERR_STATE      该事件没有 media：不是本引擎本轮投递的信令事件（伪造的、
 *                     或 payload 已被回收——保留期是「该引擎又取走 64 条信令」）
 *
 * 线程：无状态查询，可以从任意线程调用；不设置 pu_last_error（本函数没有引擎参数）。
 * 指针有效期与 pu_signal_event.payload 一致。 */
PU_API int pu_signal_media(const pu_event* event, char* media_out, int capacity);

/* 读引擎统计（发/收帧数、PC/ICE 状态、信令条数、事件丢弃数）。
 * out->struct_size 必须 ≥ sizeof(pu_stats)（库只写这一版认识的字段），
 * 小于则 PU_ERR_INVALID_ARG。
 * 不加锁、不发异步 GetStats，微秒级返回，可以从任意线程周期性调用。 */
PU_API int pu_peer_stats(pu_engine* e, pu_stats* out);

/* v2.7：单条远端音频轨的实时统计（SFU 中继下一个 PC 上会挂多条远端音频轨）。
 *
 * 语义约定：
 *   - 一条 track 对应一个远端音频轨。**注意 relay 会预协商多条 sendonly 槽**，
 *     所以「轨对象数」不等于人数；只有**非静音**的轨才计入 pcm_frames/活跃判定。
 *   - **阈值定位：这是「非零门控」，不是 VAD / 人声检测。** sink 里先算峰值，峰值低于
 *     阈值的帧不计帧、不刷新时间戳；阈值默认 8（可用环境变量
 *     PARTYUP_VOICE_PEAK_THRESHOLD 覆盖，1~32767），每次挂轨时读取。
 *     它解决的问题只有一个：**SFU 预协商空槽送全零静音帧**（peak=0），8 足够把它们
 *     挡掉，让 pu_voice_tracks() 不再返回 12 条空槽。
 *     **不要指望调这个阈值来判「谁在说话」**：真实麦克风的环境噪声峰值得看设备增益
 *     （实测某笔记本麦克风噪声峰值可达 ~300）。「谁在说话」的判定由客户端基于
 *     peak / last_frame_ms 做自适应（噪声底估计 + 迟滞），媒体核心只提供原始量。
 *   - tracks=N（pu_voice_tracks 的返回条数）是「当前活跃（非静音）的远端音频轨数」，
 *     **不是**频道人数（本端自己不算、还没出声的人也不算）。轨对象总数见日志的 tracks=。
 *   - last_frame_ms 是单调时钟毫秒（不随系统时间跳变），0 表示这条轨从未收到过非静音帧。
 *     客户端可据此判定「刚刚有声音」：now - last_frame_ms 越小越新；是否算「正在说话」
 *     由客户端自己的阈值决定（见上一条）。
 *   - pu_voice_tracks() 只返回**活跃**轨（超过 1000 ms 没有新帧的轨会被过滤掉），
 *     所以对端离开后留下的冻结记录不会出现。
 */
typedef struct pu_voice_track {
  uint32_t struct_size;
  char id[64];            /* 远端音频轨 id（webrtc track id，UTF-8，保证 NUL 结尾） */
  uint32_t pcm_frames;    /* 累计**有声** PCM 帧数（每帧 10 ms；静音帧不计） */
  uint32_t peak;          /* 最近一帧峰值振幅（16 bit 有符号采样绝对值），可用于说话指示灯 */
  uint64_t last_frame_ms; /* 最近一帧的单调时钟毫秒；0 = 从未收到 */
} pu_voice_track;

/* ---- 以下 9 个为下一版实现，签名冻结、当前统一返回 PU_NOT_IMPLEMENTED ---- */

PU_API int pu_voice_join(pu_engine* e, const pu_voice_params* p);
PU_API int pu_voice_leave(pu_engine* e);
PU_API int pu_voice_set_mode(pu_engine* e, pu_mode mode);
PU_API int pu_voice_set_mute(pu_engine* e, int muted);
PU_API int pu_voice_set_deafen(pu_engine* e, int deafened);

/* pu_voice_set_mute / pu_voice_set_deafen 的 v2.15 安全语义：
 *   * 调用只在 ADM 真正接受并**读回确认**新状态后返回 PU_OK；返回错误时客户端
 *     不得把本地 UI 改成新状态（镜像只在成功时更新）。
 *   * 静音/闭音为 true 的方向如果设备后来被热切/默认跟随/回退重启，控制层会在
 *     新设备 **Start 之前**重新 SetMuted 并读回；失败则停掉该方向并置 fault，
 *     不会出现「界面仍说 muted、设备却在采集/播放」的窗口。
 *   * 取消静音/闭音失败时该方向会被停下；为了不再放出未静音音频，控制层会保守地
 *     记住这个方向仍应为 muted，直到下一次明确成功取消。
 *   * 一次调用只对请求的方向执行真实写+读回；另一个方向的镜像会同步，但不会因为
 *     它的 ADM 状态导致本次操作误报失败。 */

/* v2.7：按轨读取远端的音频统计（谁在说话 / 按人排查质量）。
 *
 *   out == nullptr            → 返回当前活跃轨数（调用方先问条数再分配缓冲区）
 *   out != nullptr, capacity  → 最多写 capacity 条，返回实际写入条数
 * 每条 out[i] 的 struct_size 由调用方设置；函数按 min(struct_size, sizeof) 填充。
 * 只返回活跃轨（last_frame_ms 在 1000 ms 内），冻结记录被过滤。
 * 未加入语音时返回 0。 */
PU_API int pu_voice_tracks(pu_engine* e, pu_voice_track* out, uint32_t capacity);

/* v2.3 起实现：枚举可共享的源。out->struct_size 必须 >= sizeof(pu_source_list)。
 * 返回 PU_OK 时 out->items 指向库内存，有效期到下一次调用本函数或 pu_engine_destroy；
 * 列表 = 本机所有显示器（kind=0，主屏在前，id 用 HMONITOR）+ 末尾一个 "synthetic"
 * （kind=2）。items 里 id/title 是 UTF-8、库持有。显示器被拔掉/枚举失败时仍返回
 * PU_OK，只是没有 kind=0 的项（合成源永远在）。 */
PU_API int pu_screen_sources(pu_engine* e, pu_source_list* out);

/* v1.1（v2.14）：显式单流观看。
 *
 * pu_screen_watch 是**兼容入口**：只指定目标，不指定槽位/代次；原生会接受当前
 * 屏幕会话上所有远端视频轨（一次一路仍由上层保证）。新客户端应使用
 * pu_screen_watch_ex，把服务端 screen.watch.ok 的 slot/generation 带进来：
 *   - slot >= 0：只接受 track id 为 "partyup-video-slot-<slot>" 的那一路（relay）；
 *   - slot == -1：不按 track 过滤（direct：P2P 只有一条远端视频轨）；
 *   - generation：服务端单调代次。原生拒绝比当前更旧的 generation（乱序到达的
 *     watch 应答不能把已切到新目标的画面又切回去）。
 * 每次成功 arm 都会推进原生 epoch；帧回调捕获 arm 时的 (target, slot, epoch)，
 * 旧代次的迟到帧被丢弃，绝不能读取「当前全局 epoch」重新贴标。
 *
 * 未点 watch 时（且没有 legacy 的自动观看）不解码/不交付远端屏幕帧。
 * 返回 PU_ERR_STATE 表示没有可用的屏幕会话；PU_ERR_INVALID_ARG 表示参数非法。 */
PU_API int pu_screen_watch(pu_engine* e, const char* sharer_id);
PU_API int pu_screen_watch_ex(pu_engine* e, const char* sharer_id, int32_t slot,
                             uint64_t generation);
/* 停止观看：若 sharer_id 非空且与当前目标不一致，则视为已经停止（幂等，不清新目标）。
 * 立即解绑远端视频 sink、清空帧缓冲并推进 epoch；之后到达的旧帧一律丢弃。
 *
 * 新客户端可在建任何屏幕 PC **之前**先用空 sharer_id 调一次：这会把引擎切到
 * 「显式模式」——未 watch 时即使收到远端视频轨也不挂 sink、不解码、不交付帧。
 * v1 客户端从不调用本函数，保持自动观看；Stop 也不会把 v1 会话永久切到显式模式。 */
PU_API int pu_screen_unwatch(pu_engine* e, const char* sharer_id);

/* v2.14：显式能力探测（加法式）。
 * 旧 DLL 没有本导出 → 客户端不得用「pu_screen_watch 导出存在」判断功能可用
 * （旧实现里这个导出存在但返回 PU_NOT_IMPLEMENTED）。caps 位：
 *   bit0 = 目标级 watch/unwatch（帧按目标/epoch 归属）
 *   bit1 = watch_ex 的 slot/generation 过滤
 *   bit2 = 音频设备枚举/选择/状态（pu_audio_*）
 *   bit3 = 本端电平（pu_voice_level）
 *   bit4 = v2.15：通话中真实设备热切 + 稳定 ID 选择 + 系统默认跟随/热插拔回退
 *          （pu_audio_set_device 在通话中执行 Stop->Set->Init->Start，PU_OK 表示
 *          已经生效；pu_audio_status 会推进真实重新选择/回退）。旧 DLL 没有该位，
 *          托管层必须据此把设备切换当作「只登记下次 join」，不能宣称热切可用。
 *   bit5 = v2.16：严格接收会话（receive-only PC + 按会话信令/统计 + pause_send）
 *   bit6 = v2.17：per-viewer 发送会话（publish_open/signal/close/stats）+
 *          接收侧真实 RTP 计数（receive_rtp_stats）。direct 严格观看要求共享者
 *          侧具备 bit6；只有 bit5 的旧 DLL 必须明确报不支持。
 *   bit7 = v2.18（AUD02）：独立本端输入电平（pu_audio_monitor_* + 同一 ADM 的
 *          官方 observer）。旧 DLL 没有该位也没有三个导出，托管层必须明确报
 *          unsupported，不能静默造电平值。
 * 本实现返回 PU_OK 并写 caps；e 为 NULL 返回 PU_ERR_INVALID_ARG。 */
PU_API int pu_engine_caps(pu_engine* e, uint32_t* caps);

/* v2.14：音频设备枚举/选择（复用语音通话的同一个 ADM，不另建录音栈）。
 *
 * kind：0 = 输入（录音），1 = 输出（播放）。
 * out==NULL：返回设备数量；out!=NULL：最多写 capacity 条并返回写入条数。
 * 每条 struct_size 由调用方填；函数按 min(struct_size, sizeof) 填充。
 * index 就是 ADM 当前枚举里的位置（Windows ADM 的枚举**没有**默认伪设备项，
 * 0 只是设备集合里的第一个具体设备）；is_default 不再表示 index==0，而是
 * 「该设备的稳定端点 ID 等于当前系统默认端点」——查不到默认时全部为 0。
 * 设备顺序可能变化，调用方应把 guid 当作稳定身份；不要跨枚举长期保存 index。 */
typedef struct pu_audio_device {
  uint32_t struct_size;
  int32_t index;
  int32_t is_default;
  char name[128];
  char guid[64];
} pu_audio_device;

/* pu_audio_set_device：选择输入/输出设备（index=-1 跟随系统默认）。
 *
 * 语义（v2.15 起）：
 *   * index=-1 走 ADM 的 WindowsDeviceType::kDefaultDevice（Windows 下解析为
 *     GetDefaultAudioEndpoint(..., eConsole)），**不会**映射成枚举索引 0；
 *   * 未在通话中：ADM 接受后立即生效，返回 PU_OK；
 *   * 通话中：执行 Stop->SetDevice->Init->Start 事务（短暂停启），**PU_OK 只表示
 *     新设备已经真正启动**；失败会尝试恢复原设备，恢复不了则回落系统默认、
 *     fallback 置位并在 pu_last_error() 说明；两种都失败时返回错误、该方向音频
 *     停止、pu_audio_status 的 reserved bit 置位。不会假成功。
 *   * 安全启动门禁（v2.15 返修）：被请求静音/闭音的方向，新设备在 Start **之前**
 *     必须成功 SetMuted 并读回（ADM 的 MicrophoneMute/SpeakerMute）。写入失败、
 *     读回失败/读回仍为未静音时，该方向不会 Start，会先停掉再恢复旧设备/回退默认；
 *     每个候选设备都要重新过门禁。恢复/回退成功也遵守同一顺序；全部失败则
 *     fault 置位、方向停止，绝不以「Start 成功但静音没确认」返回 PU_OK。
 *   * 选择按 ADM 给出的稳定端点 ID 保存：设备顺序变化不会把旧选择指向别人；
 *     原设备消失时回落到系统默认。
 * 返回：PU_OK / PU_ERR_INVALID_ARG（kind/index 非法）/ PU_ERR_STATE（ADM 未创建、
 *      设备索引不存在）/ PU_ERR_MEDIA（停/启/初始化失败，详见 pu_last_error()）。 */
PU_API int pu_audio_set_device(pu_engine* e, int32_t kind, int32_t index);

/* pu_audio_status：当前选择 + 设备数量 + 热插拔/默认端点代次。
 *
 * 类型名带 _info 后缀：C++ 里 struct 名与函数名共享普通标识符空间，
 * `pu_audio_status` 同时作为函数名会让 `pu_audio_status*` 参数解析成函数。
 *
 * **本调用会推进真实重新选择/回退**（不是纯只读快照）：
 *   * 显式选择的设备从列表消失 -> 通话中真正切回系统默认（kDefaultDevice），
 *     fallback bit 置位；不在通话中则只更新选择，下次 join 生效；
 *   * 选择为 -1（跟随默认）且系统默认端点变化 -> 真正重新 Set(kDefaultDevice)+
 *     Init+Start；判据是 Windows CoreAudio 默认端点的稳定 ID（eConsole 角色），
 *     设备数量/名称都没变也能发现；
 *   * 设备顺序变化 -> 按稳定 ID 重解析序号，不把选择指向别的设备。
 * generation 在上述列表/顺序、默认端点或回退变化时递增；changed 由调用方把上一次
 * 的 generation 放进 out->generation 带进来计算（首次调用为 0 → changed=0）。
 * fallback：bit0 输入发生过回退，bit1 输出（自上次成功 SetDevice 起保持，直到用户
 * 重新选择成功）。
 * reserved（旧字段，旧客户端按 0 忽略）：bit0 = 输入因切换/回退失败而未在运行，
 * bit1 = 输出同理；0 表示没有这种故障。不能把 fault 当成「设备已静音」。 */
typedef struct pu_audio_status_info {
  uint32_t struct_size;
  int32_t input_index;   /* -1 = 跟随系统默认，否则当前解析出的 ADM 索引 */
  int32_t output_index;
  uint32_t input_count;
  uint32_t output_count;
  uint32_t generation;
  uint32_t changed;
  uint32_t fallback;     /* bit0 输入回落，bit1 输出回落 */
  uint32_t reserved;     /* bit0 输入故障，bit1 输出故障（见上） */
  char input_name[128];
  char output_name[128];
} pu_audio_status_info;

/* pu_voice_level：本端麦克风电平（同一条通话 ADM，不是独立录音栈）。
 *
 * source 字段同时是「哪两个量真的被测过」的可用性标志：
 *   * source=2：**通话 ADM 采集 observer** 的实测值（v2.19 起）。peak 与 rms 都是
 *     真实测量。注意：它不再来自「通话音频轨的 sink」——m152 的本地 audio track
 *     从不回调 AddSink 注册的 sink（真机实测该 sink 调用次数恒 0，远端音轨 sink 正常），
 *     所以电平改从同一条 ADM 的采集回调计算，MUST NOT 再假设「有轨就有 sink 数据」。
 *   * source=1：voice_track_->GetSignalLevel() 返回了电平。它只有一个 level，
 *     因此 peak=level，而 **rms=0 表示未测量 RMS**，不是「RMS 为零」；
 *     调用方不得把 source==1 的 rms 当测量值显示。
 * 有新鲜 PCM（last_frame_ms 在 1000ms 内）时优先用 source=2，以便同时给出真实 RMS。
 * valid=0 表示「没有有效电平」——调用方**不得**把 0 当成静音电平显示。
 * frames 是语音电平表**实际测量过的 PCM 帧数**（10ms/帧，进入语音后从 0 重新计），
 * 用于判断电平数据是否在更新；它与尾部追加的 capture_frames（与内容无关的采集
 * liveness 计数、累计不清零）来源相同但语义不同，见下文 v2.19 说明。
 *
 * v2.19（尾部追加，向后兼容）：以下 7 个字段**与音量无关、静音也增长**，是
 * 「语音采集链路是否活着」的可计数判据（不要拿它们当电平/音量使用）：
 *   * capture_frames：语音采集回调次数（每收到一次采集回调 +1，全零 PCM 也计）；
 *   * capture_bytes：这些回调累计交付的 PCM 字节数；
 *   * last_cb_ms：最近一次采集回调的单调时钟毫秒（epoch 未定义，仅比较差值）；
 *   * adm_initialized / adm_recording / adm_recording_initialized：共享 ADM 自报状态；
 *   * adm_instance：共享 ADM 实例代次（引擎内每新建一块 ADM +1）。多次 join/leave 后
 *     该值不变即证明是**复用同一块 ADM**而不是重建。
 * 老调用方按 32 字节 struct_size 调用时这些字段不填充（原生只写 min(struct_size, sizeof)）。
 */
typedef struct pu_voice_level_info {
  uint32_t struct_size;
  int32_t valid;
  uint32_t peak;        /* 0..32767；valid==0 时无意义 */
  uint32_t rms;         /* 0..32767；source==2 时为实测 RMS，source==1 时 0=未测量 */
  uint32_t frames;
  uint32_t source;      /* 0=none, 1=AudioTrackInterface::GetSignalLevel, 2=通话音频轨 PCM sink */
  uint64_t last_frame_ms;
  /* ---- v2.19 尾部追加：与音量无关的采集liveness计数（静音也增长） ---- */
  uint32_t capture_frames;  /* 语音采集回调次数（每次采集回调 +1，全零 PCM 也计） */
  uint32_t reserved;        /* 对齐保留，恒 0 */
  uint64_t capture_bytes;   /* 这些回调累计交付的 PCM 字节数 */
  uint64_t last_cb_ms;      /* 最近一次采集回调的单调毫秒（只比较差值） */
  uint32_t adm_initialized; /* 共享 ADM 自报 Initialized() */
  uint32_t adm_recording;   /* 共享 ADM 自报 Recording() */
  uint32_t adm_recording_initialized; /* 共享 ADM 自报 RecordingIsInitialized() */
  uint32_t adm_instance;    /* 共享 ADM 实例代次（多次 join 不变 = 复用） */
} pu_voice_level_info;

PU_API int pu_audio_devices(pu_engine* e, int32_t kind, pu_audio_device* out,
                            uint32_t capacity);
PU_API int pu_audio_status(pu_engine* e, pu_audio_status_info* out);
PU_API int pu_voice_level(pu_engine* e, pu_voice_level_info* out);

/* v2.18（AUD02）：独立本端输入电平监测（同一条通话 ADM，不另建录音栈）。
 *
 * 显式启停：默认不采集；只有 pu_audio_monitor_start 成功才进入 ACTIVE。它只启动
 * **输入**方向，绝不启动扬声器，也不解除用户/系统的静音：静音门禁与设备选择仍由
 * 生产 AudioDeviceController 执行（复用通话 ADM 的同一个端口）。
 *
 * 与语音的互斥：语音正在加入/已加入、或存在残留语音 PC/发送轨时 Start 返回
 * PU_ERR_STATE；pu_voice_join 在启动语音前会先停掉 monitor，无法确认停止则拒绝
 * join。pu_audio_monitor_stop 幂等，且不会停掉已由 voice 接管的录音。
 *
 * 数据来源：官方 CreateAudioDeviceWithDataObserver 的 OnCaptureData，**平台 ADM 交付
 * 的全部 PCM（含全零帧）**都计入统计——实测 m152 Windows ADM 交付 16 bit int 立体声
 * （ADM 透传的 bytes-per-sample 实为每帧字节数，实现内部按声道数还原；也兼容 32 bit
 * 格式并换算到 16 bit 幅度尺）。peak 是样本绝对值最大值，rms 是全部样本的均方根
 * （0..32767）；frames 是累计收到的 PCM 回调帧数（不是采样点/字节数）。
 * last_frame_ms 是单调时钟毫秒。valid=1 表示「在 ACTIVE 且最近 1000 ms 内收到过
 * 合法 PCM」——全零 PCM 也是有效测量（peak=/rms=0），不能用非零阈值过滤。
 * valid=0 表示未启用/无新鲜数据/已停止/故障，调用方**不得**把 0 当成静音电平。
 * 停止/重启/切设备会清空旧电平并推进采集代次，旧回调的迟到 PCM 被丢弃。
 */
typedef enum pu_audio_monitor_state {
  PU_AUDIO_MONITOR_IDLE = 0,    /* 未采集（默认） */
  PU_AUDIO_MONITOR_ACTIVE = 1,  /* 显式 Start 成功且输入在跑 */
  PU_AUDIO_MONITOR_FAULTED = 2  /* 启停/读回失败；是否仍在运行看 running */
} pu_audio_monitor_state;

/* struct_size 必须由调用方填 sizeof(pu_audio_monitor_info)（当前 40 字节）：
 *   0 struct_size  4 state  8 running  12 valid  16 peak
 *   20 rms         24 frames  28 reserved  32 last_frame_ms
 * state 是**请求状态**；running 是**实测状态**（-1 未知 / 0 否 / 1 是），两者
 * 必须分开读。Stop 失败时 state=FAULTED，running 如实报告仍运行或未知，不能因为
 * 调用方传了无效 status 缓冲就跳过停止，也不会自动重启采集。
 */
typedef struct pu_audio_monitor_info {
  uint32_t struct_size;
  int32_t state;          /* pu_audio_monitor_state */
  int32_t running;        /* -1 未知 / 0 否 / 1 是 */
  uint32_t valid;         /* 1 = 下面的电平是新鲜实测值 */
  uint32_t peak;          /* 0..32767 */
  uint32_t rms;           /* 0..32767；valid=0 时无意义 */
  uint32_t frames;        /* 累计 PCM 回调帧数；停止/重启后归零 */
  uint32_t reserved;      /* 0（后续扩展） */
  uint64_t last_frame_ms; /* 最近一次合法 PCM 的单调时钟毫秒；0 = 从未收到 */
} pu_audio_monitor_info;

/* 开始/停止独立输入监测。Stop 没有输出参数、幂等，且不依赖 status 缓冲；
 * 失败时返回 PU_ERR_MEDIA 并在 pu_last_error() 说明实际 running/未知。
 * 返回：PU_OK / PU_ERR_STATE（语音占用、未绑定 ADM）/
 *       PU_ERR_INVALID_ARG（e 为 NULL）/ PU_ERR_MEDIA（设备启动或读回失败）。 */
PU_API int pu_audio_monitor_start(pu_engine* e);
PU_API int pu_audio_monitor_stop(pu_engine* e);

/* 读取监测状态。out->struct_size 必须 >= sizeof(pu_audio_monitor_info)。
 * 只读快照：不会启动/停止设备，也不会推进设备事务。返回 PU_OK /
 * PU_ERR_INVALID_ARG / PU_ERR_STATE（引擎没有 monitor）。 */
PU_API int pu_audio_monitor_status(pu_engine* e, pu_audio_monitor_info* out);

/* v2.16（B2）：严格接收会话（隔离）。cap bit5 = 本组能力可用。
 *
 * 为什么需要：v2.14 的 watch/unwatch 作用在**同一条屏幕 PC** 上——那条 PC 可能同时
 * 持有本端发布轨。停看只能解绑 sink（旧 RTP 仍在同一条 DTLS/ICE 上）或关整条 PC
 * （误停本端发布/采集）。这里给观看者一条**独立的 receive-only PeerConnection**：
 *   * pu_screen_receive_open 在发 watch 之前调用，用客户端生成的 session_id 建接收
 *     会话；服务端随后把只含一路 video 的 offer 经 pu_signal_in_session 灌进来；
 *   * 每个会话有自己的 PC/ICE/DTLS/远端轨与帧计数；关闭只销毁这条接收 PC，
 *     不碰 pc_/track_/video_sender_/source_/sharing_，也不碰语音；
 *   * 旧会话 id 的 offer/answer/candidate 在 open/close 之后一律拒绝（PU_ERR_STATE），
 *     不会进入新会话；帧按 session token 归属，旧 sink 的迟到帧被丢弃。
 * 一个引擎同时只保留一路接收会话（一个 viewer 恰好一路）；open 新 id 会先关旧会话。
 * 旧 DLL 没有这些导出 → 客户端必须明确报不支持，不能退回旧 PC 上的 watch。 */

typedef struct pu_receive_stats_info {
  uint32_t struct_size;
  int32_t active;              /* 1 = 会话存在且未关闭 */
  uint32_t token;              /* open 返回的会话 token（SIGNAL_OUT.reserved 同值） */
  uint32_t video_tracks;       /* 已挂上的远端视频轨数（0..1） */
  uint32_t frames_delivered;   /* 本会话被接受并交付的帧数（旧会话计数不再增长） */
  uint32_t reserved;
  uint64_t last_frame_ms;      /* 最近一帧交付时刻（steady ms），0 = 尚无 */
} pu_receive_stats_info;

/* 建立/复用一条接收会话。session_id 非空、1..96 字节；同一 id 重复调用幂等并
 * 回同一个 token；不同 id 且已有活动会话时先关闭旧会话（一次一路）。
 * 返回 PU_OK / PU_ERR_INVALID_ARG（id 非法）/ PU_ERR_STATE（引擎未初始化/MPC 建失败）。 */
PU_API int pu_screen_receive_open(pu_engine* e, const char* session_id, uint32_t* out_token);

/* 关闭接收会话：只销毁该会话的接收 PC、sink、远端轨；session_id 与当前会话不匹配
 * 时是幂等空操作（旧会话的 close 不会误关新会话）。返回 PU_OK。 */
PU_API int pu_screen_receive_close(pu_engine* e, const char* session_id);

/* 按会话 id 灌入屏幕信令（offer/candidate）。session_id 为空或是 "voice" 时退回
 * pu_signal_in_media 的旧行为。会话不存在/已关闭 → PU_ERR_STATE；payload 非法 →
 * PU_ERR_INVALID_ARG。answer 只能由本端生成，这里不接受客户端 answer。 */
PU_API int pu_signal_in_session(pu_engine* e, const char* media, const char* session_id,
                                const char* payload, size_t len);

/* 读取接收会话统计。out->struct_size 由调用方填；会话不存在时 active=0、其余为 0。
 * 只读快照，不加锁等待网络。 */
PU_API int pu_screen_receive_stats(pu_engine* e, const char* session_id, pu_receive_stats_info* out);

/* v2.16（B2 direct）：暂停/恢复本端屏幕发送轨，**保留采集源、视频轨、sender 与
 * 发布状态**（不是 screen.stop，也不是 ReleaseStreamObjects）。paused=1 时
 * SetTrack(nullptr)，paused=0 时把 track_ 重新挂回同一个 sender；不重协商 SDP。
 * 用于 direct 1v1：观看者停看时共享者只停止对该 viewer 的发送，重看时恢复。
 * 没有屏幕会话/没有 sender 时返回 PU_ERR_STATE；成功 PU_OK。 */
PU_API int pu_screen_pause_send(pu_engine* e, int32_t paused);

/* v2.17（B2b direct）：per-viewer 发送会话（共享者侧）。cap bit6 = 本组可用。
 *
 * 为什么需要：direct 严格观看要求共享者为**每个 viewer** 建一条独立发送传输
 * （新 ICE/DTLS/SSRC），复用已存在的采集源与视频轨；停看只关该 viewer 的发送
 * PC，不重启采集、不全局 screen.stop。pu_screen_pause_send 只是在同一条 PC 上
 * 换轨，旧 RTP 仍走同一条 DTLS/ICE，不能作为严格隔离的证据。
 *
 * 生命周期：
 *   * 只能在 pu_screen_start* 之后调用（采集源/视频轨已存在）；否则 PU_ERR_STATE；
 *   * 同一 session_id 重复 open 幂等并回同一个 token；
 *   * 每次 open 产生一条新 PC 并**主动发 offer**（SIGNAL_OUT 的 reserved = 会话
 *     token，payload 是 {"type":"offer","sdp":...}），answer/candidate 由对端经
 *     pu_screen_publish_signal_in 灌回；
 *   * close 只关闭该会话的 PC/sender，不碰采集、track_、发布状态或其它会话。
 * 多个 viewer 各自一个会话；direct 的 1v1 上限由服务端裁决（本 API 不做容量判断）。
 */

typedef struct pu_publish_stats_info {
  uint32_t struct_size;
  int32_t active;              /* 1 = 会话存在且未关闭 */
  uint32_t token;              /* open 返回的会话 token（SIGNAL_OUT.reserved 同值） */
  uint32_t packets_sent;       /* outbound-rtp.packetsSent（真实 RTP，不含 ICE 保活） */
  uint64_t bytes_sent;         /* outbound-rtp.bytesSent */
  uint64_t frames_encoded;     /* outbound-rtp.framesEncoded */
  uint32_t has_remote;         /* 1 = 已应用对端 answer（可发送 RTP 的前提之一） */
  uint32_t reserved;
} pu_publish_stats_info;

/* 建立/复用一条 per-viewer 发送会话。session_id 非空、1..96 字节。
 * 返回 PU_OK / PU_ERR_INVALID_ARG（id 非法）/ PU_ERR_STATE（未在共享/引擎未初始化/
 * 建 PC 失败）/ PU_ERR_MEDIA（CreateOffer/SetLocalDescription 失败）。 */
PU_API int pu_screen_publish_open(pu_engine* e, const char* session_id, uint32_t* out_token);

/* 按会话灌入对端信令（answer/candidate）。会话不存在/已关闭 → PU_ERR_STATE；
 * payload 非法 → PU_ERR_INVALID_ARG；offer 不被接受（本端是 offerer）。 */
PU_API int pu_screen_publish_signal_in(pu_engine* e, const char* session_id,
                                       const char* payload, size_t len);

/* 关闭该会话（幂等；session_id 不匹配时不误关别的会话）。只销毁这条发送 PC。 */
PU_API int pu_screen_publish_close(pu_engine* e, const char* session_id);

/* 读取发送会话的 RTP 计数（标准 GetStats）。out->struct_size 由调用方填；
 * 会话不存在时 active=0、其余为 0。最多等一个很短的采样窗口（<=300 ms）。 */
PU_API int pu_screen_publish_stats(pu_engine* e, const char* session_id,
                                   pu_publish_stats_info* out);

/* v2.17：接收会话的**真实 RTP** 计数（inbound-rtp）。与
 * pu_screen_receive_stats.frames_delivered 的区别：后者只统计进入解码/交付的帧，
 * 前者是网络层收到的包/字节，可区分「RTP 停了」与「ICE/DTLS 保活仍在」。
 * 会话不存在时 active=0、其余为 0。 */
typedef struct pu_rtp_stats_info {
  uint32_t struct_size;
  int32_t active;
  uint32_t packets_received;
  uint32_t reserved;
  uint64_t bytes_received;
  uint64_t frames_decoded;
} pu_rtp_stats_info;

PU_API int pu_screen_receive_rtp_stats(pu_engine* e, const char* session_id,
                                       pu_rtp_stats_info* out);

PU_API int pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out);
PU_API void pu_frame_release(pu_engine* e, pu_frame* f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PARTYUP_MEDIA_H */
