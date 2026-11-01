/* PartyUp 媒体核心 · 窄 C ABI（v1）
 *
 * 设计约束（见 docs/ARCHITECTURE.md §2.1 与 docs/spikes/2026-11-01-c-abi.md）：
 *   * 纯 C 接口，只暴露 C 类型：不透明句柄 + 定长 POD 结构 + 返回码；
 *   * 不跨边界传 std::string / std::vector，字符串一律 const char*（UTF-8，0 结尾），
 *     内存由调用方或库持有，所有权在函数注释里写死；
 *   * 任何 C++ 异常都不会穿越 ABI：失败一律用负返回码 + pu_last_error()；
 *   * 头文件可被 C 与 C++ 同时包含；C++ 侧编译 DLL 时定义 PU_BUILD_SHARED。
 *
 * 本版（ABI v1，第一切片）只实现 6 个函数：
 *     pu_engine_create / pu_engine_destroy
 *     pu_screen_start / pu_screen_stop
 *     pu_poll_event / pu_last_error
 * 其余 10 个函数按 ARCHITECTURE 的签名先声明，统一返回 PU_NOT_IMPLEMENTED。
 *
 * 线程约定：所有 pu_* 调用必须来自同一个「控制线程」（通常是 UI 线程），
 * 且不得在库触发的任何回调里重入（本版库里没有回调，全部走 pu_poll_event 轮询）。
 */
#ifndef PARTYUP_MEDIA_H
#define PARTYUP_MEDIA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI 版本。结构体或函数签名有不兼容改动时 +1。 */
#define PU_ABI_VERSION 1

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
 *   "synthetic" / NULL  → 内置合成帧源（640x360 渐变 @30fps，本版唯一可用的源）
 * 真实屏幕/窗口枚举（pu_screen_sources）在下一版实现，届时返回的 id 才能传进来。
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
  PU_EVENT_STATE = 1, /* 状态迁移：data.state */
  PU_EVENT_FRAME = 2, /* 一帧编码完成：data.frame */
  PU_EVENT_ERROR = 3  /* 错误：data.error */
} pu_event_type;

/* 一帧编码完成。sequence 从 0 递增；bitrate_kbps 是最近 1 秒的编码码率。 */
typedef struct pu_frame_event {
  uint64_t sequence;      /* 该路流内的帧序号，0 起 */
  int64_t capture_ts_us;  /* 源帧时间戳，system_clock 纪元微秒 */
  uint32_t width;
  uint32_t height;
  uint32_t bytes;         /* 本帧编码后字节数 */
  uint32_t bitrate_kbps;  /* 最近 1s 的编码码率（kbit/s） */
  uint32_t keyframe;      /* 1 = 关键帧 */
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
 * 布局：
 *   0   struct_size   4   type
 *   8   timestamp_us
 *   16  data（union，240 字节）：
 *         frame: sequence@16 capture_ts_us@24 width@32 height@36
 *                bytes@40 bitrate_kbps@44 keyframe@48 reserved@52
 *         state: state@16 previous_state@20 reason@24 reserved@28
 *         error: code@16 reserved@20 message[232]@24
 */
typedef struct pu_event {
  uint32_t struct_size;      /* 调用方填 sizeof(pu_event) */
  uint32_t type;             /* pu_event_type */
  int64_t timestamp_us;      /* 事件产生时间，system_clock 纪元微秒 */
  union {
    pu_frame_event frame;
    pu_state_event state;
    pu_error_event error;
  } data;
} pu_event;

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
  uint32_t width;
  uint32_t height;
  uint32_t kind;          /* 0=屏幕 1=窗口 */
  uint32_t reserved;
} pu_source_info;

/* 源列表（pu_screen_sources 输出）。items 由库持有，有效期到下一次
 * pu_screen_sources 调用或 pu_engine_destroy。 */
typedef struct pu_source_list {
  uint32_t struct_size;   /* sizeof(pu_source_list) */
  uint32_t count;
  const pu_source_info* items;
} pu_source_list;

/* 一帧待渲染数据（pu_frame_acquire）。由引擎持有，pu_frame_release 前不可跨帧复用。 */
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
 * 细节见 pu_last_error()。 */
PU_API int pu_screen_start(pu_engine* e, pu_screen_id screen_id);

/* 停止屏幕共享。未在运行时返回 PU_OK（幂等）。 */
PU_API int pu_screen_stop(pu_engine* e);

/* 取一个事件，事件被复制到 out 指向的调用方内存。
 *   timeout_ms > 0：最多等这么多毫秒；
 *   timeout_ms = 0：非阻塞；
 *   timeout_ms < 0：一直等到有事件。
 * 返回 PU_OK 表示取到事件；PU_ERR_TIMEOUT 表示超时；其他为错误。
 * 单消费者：同一引擎不要从多个线程同时调用。 */
PU_API int pu_poll_event(pu_engine* e, pu_event* out, int timeout_ms);

/* 最近一次错误描述（UTF-8，0 结尾）。指针由引擎持有，有效期到下一次调用本函数
 * 或其他 pu_* 函数。e 为 NULL 时返回最近一次 pu_engine_create 失败的原因。 */
PU_API const char* pu_last_error(pu_engine* e);

/* ---- 以下 10 个为下一版实现，签名冻结、当前统一返回 PU_NOT_IMPLEMENTED ---- */

PU_API int pu_voice_join(pu_engine* e, const pu_voice_params* p);
PU_API int pu_voice_leave(pu_engine* e);
PU_API int pu_voice_set_mode(pu_engine* e, pu_mode mode);
PU_API int pu_voice_set_mute(pu_engine* e, int muted);
PU_API int pu_voice_set_deafen(pu_engine* e, int deafened);

PU_API int pu_screen_sources(pu_engine* e, pu_source_list* out);
PU_API int pu_screen_watch(pu_engine* e, const char* sharer_id);
PU_API int pu_screen_unwatch(pu_engine* e, const char* sharer_id);

PU_API int pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out);
PU_API void pu_frame_release(pu_engine* e, pu_frame* f);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PARTYUP_MEDIA_H */
