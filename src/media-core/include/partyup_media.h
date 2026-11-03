/* PartyUp 媒体核心 · 窄 C ABI（v2）
 *
 * 设计约束（见 docs/ARCHITECTURE.md §2.1 与 docs/spikes/2026-11-01-c-abi.md、
 * docs/spikes/2026-11-01-signaling-abi.md）：
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
 * 信封格式见 docs/spikes/2026-11-01-signaling-abi.md：
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
 *   * stream_id 是不透明流标识（当前远端视频流为 1）。
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

/* 开始一路屏幕共享。screen_id 为 NULL 或 "synthetic" 时使用内置合成源。
 * 成功返回 PU_OK；已在运行返回 PU_ERR_STATE；媒体初始化失败返回 PU_ERR_MEDIA，
 * 细节见 pu_last_error()。
 *
 * v2 起本调用还会：建 PeerConnection → AddTrack(视频轨) → CreateOffer →
 * SetLocalDescription → 等 ICE 收集完成后推一个 PU_EVENT_SIGNAL_OUT（offer）。
 * 也就是说 start 返回时 offer 可能还没生成，UI 要照常 poll 事件。 */
PU_API int pu_screen_start(pu_engine* e, pu_screen_id screen_id);

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
 * 接受的信封（与 docs/PROTOCOL.md 对齐，见 docs/spikes/2026-11-01-signaling-abi.md）：
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
PU_API int pu_screen_watch(pu_engine* e, const char* sharer_id);
PU_API int pu_screen_unwatch(pu_engine* e, const char* sharer_id);

PU_API int pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out);
PU_API void pu_frame_release(pu_engine* e, pu_frame* f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PARTYUP_MEDIA_H */
