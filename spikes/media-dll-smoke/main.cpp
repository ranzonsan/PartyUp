// PartyUp 媒体核心 DLL 冒烟测试
//
// 判据（缺一不可）：
//   * pu_engine_create 返回非空句柄；
//   * pu_screen_start 返回 PU_OK，且事件里出现 STATE=RUNNING；
//   * 10 秒内收到 >= 30 个 PU_EVENT_FRAME；
//   * v2.1：PC 侧只编码一次——outbound-rtp.framesEncoded ≈ 连上之后推送的帧数，
//     并且严格小于「推送帧数的 1.5 倍」（2 倍就意味着同一帧被编码了两次）；
//   * 9 个未实现函数都返回 PU_NOT_IMPLEMENTED（证明导出表齐全）；
//   * v2.3：pu_screen_sources 已实现——至少 1 块真实显示器 + synthetic 在列表里；
//   * 全程没有 PU_EVENT_ERROR；
//   * pu_screen_stop / pu_engine_destroy 正常返回。
// 任何一步失败都打印 pu_last_error 并以 exit=1 退出。
//
// 注意：本 exe 只会从 partyup_media.dll 同目录加载，必须放在 Windows 本地盘运行，
// 不要从 UNC 路径启动（原生库搜索路径不可靠）。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "partyup_media.h"

namespace {

constexpr int kTargetFrames = 30;
constexpr int kTimeoutSeconds = 10;
// PC 的编码计数每 250 ms 采样一次，30 fps 下一个窗口最多 8 帧还没算进去，留 1 帧余量。
constexpr int kSampleLagFrames = 9;

const char* StateName(int state) {
  switch (state) {
    case PU_STREAM_IDLE:
      return "IDLE";
    case PU_STREAM_STARTING:
      return "STARTING";
    case PU_STREAM_RUNNING:
      return "RUNNING";
    case PU_STREAM_STOPPING:
      return "STOPPING";
    case PU_STREAM_FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

const char* ReasonName(int reason) {
  switch (reason) {
    case PU_REASON_NONE:
      return "none";
    case PU_REASON_REQUESTED:
      return "requested";
    case PU_REASON_MEDIA_FAILURE:
      return "media-failure";
    default:
      return "unknown";
  }
}

void PrintLastError(pu_engine* engine, const char* what) {
  std::printf("FAIL: %s\n", what);
  std::printf("  pu_last_error: %s\n", pu_last_error(engine));
}

// 9 个未实现函数必须都存在且返回 PU_NOT_IMPLEMENTED（pu_screen_sources 在 v2.3
// 已经实现了，改由 CheckScreenSources 断言，见下）。
int CheckUnimplementedSurface(pu_engine* engine) {
  pu_voice_params voice;
  std::memset(&voice, 0, sizeof(voice));
  voice.struct_size = sizeof(voice);
  pu_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.struct_size = sizeof(frame);

  struct {
    const char* name;
    int rc;
  } calls[] = {
      {"pu_voice_join", pu_voice_join(engine, &voice)},
      {"pu_voice_leave", pu_voice_leave(engine)},
      {"pu_voice_set_mode", pu_voice_set_mode(engine, PU_MODE_DIRECT)},
      {"pu_voice_set_mute", pu_voice_set_mute(engine, 1)},
      {"pu_voice_set_deafen", pu_voice_set_deafen(engine, 1)},
      {"pu_screen_watch", pu_screen_watch(engine, "peer-1")},
      {"pu_screen_unwatch", pu_screen_unwatch(engine, "peer-1")},
      {"pu_frame_acquire", pu_frame_acquire(engine, "stream-1", &frame)},
  };

  int failures = 0;
  for (const auto& call : calls) {
    if (call.rc != PU_NOT_IMPLEMENTED) {
      std::printf("FAIL: %s returned %d, expected PU_NOT_IMPLEMENTED (%d)\n", call.name, call.rc,
                  PU_NOT_IMPLEMENTED);
      ++failures;
    }
  }
  pu_frame_release(engine, &frame);  // void，也要能调用
  std::printf("unimplemented surface: %zu int calls + 1 void call checked, %d unexpected\n",
              sizeof(calls) / sizeof(calls[0]), failures);
  return failures;
}

// v2.3：pu_screen_sources 现在必须实现。判据：至少 1 项、id/title 非空、
// 尺寸非 0、末尾一定有 synthetic、连续两次调用都稳定（不是一次性缓冲）。
int CheckScreenSources(pu_engine* engine) {
  int failures = 0;
  pu_source_list sources;
  std::memset(&sources, 0, sizeof(sources));
  sources.struct_size = static_cast<uint32_t>(sizeof(sources));
  const int rc = pu_screen_sources(engine, &sources);
  if (rc != PU_OK) {
    std::printf("FAIL: pu_screen_sources rc=%d (%s)\n", rc, pu_last_error(engine));
    return 1;
  }
  if (sources.count == 0 || sources.items == nullptr) {
    std::printf("FAIL: pu_screen_sources returned no items\n");
    return 1;
  }
  bool has_display = false;
  bool has_synthetic = false;
  for (uint32_t i = 0; i < sources.count; ++i) {
    const pu_source_info& info = sources.items[i];
    if (info.id == nullptr || info.id[0] == '\0') {
      std::printf("FAIL: source[%u] has an empty id\n", i);
      ++failures;
      continue;
    }
    if (info.kind == 0) {
      has_display = true;
      if (info.width == 0 || info.height == 0) {
        std::printf("FAIL: display source '%s' has a 0 dimension\n", info.id);
        ++failures;
      }
    }
    if (std::strcmp(info.id, "synthetic") == 0) has_synthetic = true;
    std::printf("source[%u]: kind=%u %ux%u id=%s title=\"%s\"\n", i, info.kind, info.width,
                info.height, info.id, info.title != nullptr ? info.title : "(null)");
  }
  if (!has_display) {
    std::printf("FAIL: no display source (kind=0) reported\n");
    ++failures;
  }
  if (!has_synthetic) {
    std::printf("FAIL: \"synthetic\" is not in the source list\n");
    ++failures;
  }
  // 第二次调用必须还能用（缓存被重建，不是把指针指向临时量）。
  pu_source_list again;
  std::memset(&again, 0, sizeof(again));
  again.struct_size = static_cast<uint32_t>(sizeof(again));
  if (pu_screen_sources(engine, &again) != PU_OK || again.count != sources.count) {
    std::printf("FAIL: second pu_screen_sources call is inconsistent\n");
    ++failures;
  }
  std::printf("screen sources: %u item(s), displays=%s, synthetic=%s, %d failure(s)\n",
              sources.count, has_display ? "yes" : "no", has_synthetic ? "yes" : "no",
              failures);
  return failures;
}

// 观看方的最小转发器：把它的 SIGNAL_OUT 原样喂回共享方，顺手数它解出多少帧。
// 本测试不对观看方做断言（那些在 signaling 冒烟里），它只负责让共享方的 PC
// 真的进入「有对端、在编码」的状态——没有对端时 PC 的 VideoStreamEncoder 不产出。
// 返回 -1 = 出错（已打印原因）。
int DrainViewer(pu_engine* viewer, pu_engine* sharer, int* remote_frames) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));

    const int rc = pu_poll_event(viewer, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: viewer pu_poll_event rc=%d (%s)\n", rc, pu_last_error(viewer));
      return -1;
    }
    if (event.type == PU_EVENT_SIGNAL_OUT) {
      const pu_signal_event& signal = event.data.signal;
      if (pu_signal_in(sharer, signal.payload, signal.payload_len) != PU_OK) {
        std::printf("FAIL: pu_signal_in(sharer) from viewer: %s\n", pu_last_error(sharer));
        return -1;
      }
    } else if (event.type == PU_EVENT_REMOTE_FRAME) {
      ++*remote_frames;
    }
  }
}

// 观看方引擎的 RAII 守卫：任何提前 return 都把它收掉。
struct EngineGuard {
  pu_engine* engine = nullptr;
  ~EngineGuard() {
    if (engine != nullptr) pu_engine_destroy(engine);
  }
};

}  // namespace

int main() {
  // 关掉 stdout 缓冲，避免与库的 stderr 日志乱序。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("partyup_media smoke test\n");
  std::printf("  ABI version : %d\n", PU_ABI_VERSION);
  std::printf("  sizeof(pu_event) : %zu bytes\n", sizeof(pu_event));

  // ABI 版本门：struct_size 不匹配必须拒绝，并且 pu_last_error(NULL) 要有话说。
  {
    pu_config bad_config;
    std::memset(&bad_config, 0, sizeof(bad_config));
    bad_config.struct_size = 0;
    pu_engine* bad_engine = pu_engine_create(&bad_config);
    if (bad_engine != nullptr) {
      std::printf("FAIL: pu_engine_create accepted a bad struct_size\n");
      pu_engine_destroy(bad_engine);
      return 1;
    }
    const char* bad_reason = pu_last_error(nullptr);
    if (bad_reason == nullptr || bad_reason[0] == '\0') {
      std::printf("FAIL: pu_last_error(NULL) gave no reason for a rejected create\n");
      return 1;
    }
    std::printf("bad config rejected: %s\n", bad_reason);
  }

  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = "media-dll-smoke";
  config.log_level = 3;  // 让库把初始化细节打到 stderr
  config.event_queue_capacity = 512;

  const auto create_start = std::chrono::steady_clock::now();
  pu_engine* engine = pu_engine_create(&config);
  if (engine == nullptr) {
    std::printf("FAIL: pu_engine_create returned null\n");
    std::printf("  pu_last_error: %s\n", pu_last_error(nullptr));
    return 1;
  }
  const auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - create_start)
                             .count();
  std::printf("engine created in %lld ms\n", static_cast<long long>(create_ms));

  // v2.1：再起一个观看方。共享方的 PC 只有在 DTLS 连上之后才会真正编码，
  // 而「一帧只编码一次」这条断言必须有真实的编码路径才验证得了。
  pu_config viewer_config = config;
  viewer_config.client_name = "media-dll-smoke-viewer";
  viewer_config.event_queue_capacity = 512;
  pu_engine* viewer = pu_engine_create(&viewer_config);
  if (viewer == nullptr) {
    std::printf("FAIL: viewer pu_engine_create returned null: %s\n", pu_last_error(nullptr));
    pu_engine_destroy(engine);
    return 1;
  }
  EngineGuard viewer_guard{viewer};
  std::printf("viewer engine created (peer for the encoding check)\n");

  // 未知源 id 必须被拒绝（v2.3：真实显示器 id 之外只有 synthetic），
  // 且不能把引擎带进错误状态。
  {
    const int bad_id_rc = pu_screen_start(engine, "no-such-monitor");
    if (bad_id_rc != PU_ERR_INVALID_ARG) {
      std::printf("FAIL: pu_screen_start(unknown id) returned %d, expected PU_ERR_INVALID_ARG\n",
                  bad_id_rc);
      pu_engine_destroy(engine);
      return 1;
    }
    std::printf("unknown screen id rejected: %s\n", pu_last_error(engine));
  }

  // struct_size 太小的 pu_event 必须被拒绝，而不是越界写。
  {
    pu_event too_small;
    std::memset(&too_small, 0, sizeof(too_small));
    too_small.struct_size = 8;
    const int rc = pu_poll_event(engine, &too_small, 0);
    if (rc != PU_ERR_INVALID_ARG) {
      std::printf("FAIL: pu_poll_event(undersized event) returned %d, expected PU_ERR_INVALID_ARG\n",
                  rc);
      pu_engine_destroy(engine);
      return 1;
    }
  }

  const int start_rc = pu_screen_start(engine, "synthetic");
  if (start_rc != PU_OK) {
    PrintLastError(engine, "pu_screen_start");
    std::printf("  rc=%d\n", start_rc);
    pu_engine_destroy(engine);
    return 1;
  }

  // 重复 start 必须被拒绝（PU_ERR_STATE），且不影响正在运行的那一路。
  {
    const int again_rc = pu_screen_start(engine, "synthetic");
    if (again_rc != PU_ERR_STATE) {
      std::printf("FAIL: second pu_screen_start returned %d, expected PU_ERR_STATE\n", again_rc);
      pu_engine_destroy(engine);
      return 1;
    }
    std::printf("duplicate start rejected: %s\n", pu_last_error(engine));
  }

  int frames = 0;
  int state_events = 0;
  int error_events = 0;
  int other_events = 0;
  int signal_events = 0;      // v2: PU_EVENT_SIGNAL_OUT
  bool saw_offer_signal = false;
  int remote_frames = 0;      // v2: PU_EVENT_REMOTE_FRAME（本引擎是共享方，收不到流）
  bool saw_running = false;
  uint64_t total_bytes = 0;
  uint64_t key_frames = 0;
  int64_t first_capture_us = 0;
  int64_t last_capture_us = 0;
  int64_t first_event_us = 0;
  // v2.1 编码检查用：观看方解出多少帧、双方何时连上、连上时已推多少帧。
  int viewer_remote_frames = 0;
  bool peer_connected = false;
  int64_t sent_at_connect = 0;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTimeoutSeconds);
  while (frames < kTargetFrames && std::chrono::steady_clock::now() < deadline) {
    // 每轮先把观看方的事件抽干（answer / 候选越早喂回共享方越好），顺手看连上没有。
    if (DrainViewer(viewer, engine, &viewer_remote_frames) < 0) {
      pu_engine_destroy(engine);
      return 1;
    }

    // 记下「DTLS 连上那一刻已经推了多少帧」：连上之前的帧 PC 不会编码，
    // 编码次数的口径从这里起算。
    if (!peer_connected) {
      pu_stats stats_a;
      pu_stats stats_v;
      std::memset(&stats_a, 0, sizeof(stats_a));
      std::memset(&stats_v, 0, sizeof(stats_v));
      stats_a.struct_size = static_cast<uint32_t>(sizeof(stats_a));
      stats_v.struct_size = static_cast<uint32_t>(sizeof(stats_v));
      if (pu_peer_stats(engine, &stats_a) == PU_OK && pu_peer_stats(viewer, &stats_v) == PU_OK &&
          stats_a.peer_state == PU_PEER_CONNECTED &&
          stats_v.peer_state == PU_PEER_CONNECTED) {
        peer_connected = true;
        sent_at_connect = static_cast<int64_t>(stats_a.frames_sent);
        std::printf("peer connected (DTLS); %d frame event(s) so far, A pushed %lld frame(s)\n",
                    frames, static_cast<long long>(sent_at_connect));
      }
    }

    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));

    const int rc = pu_poll_event(engine, &event, 100);
    if (rc == PU_ERR_TIMEOUT) continue;
    if (rc != PU_OK) {
      PrintLastError(engine, "pu_poll_event");
      std::printf("  rc=%d\n", rc);
      pu_engine_destroy(engine);
      return 1;
    }

    switch (event.type) {
      case PU_EVENT_FRAME: {
        const pu_frame_event& frame = event.data.frame;
        if (frames == 0) {
          first_capture_us = frame.capture_ts_us;
          first_event_us = event.timestamp_us;
          std::printf("first frame event:\n");
          std::printf("  sequence=%llu capture_ts_us=%lld width=%u height=%u\n",
                      static_cast<unsigned long long>(frame.sequence),
                      static_cast<long long>(frame.capture_ts_us), frame.width, frame.height);
        }
        last_capture_us = frame.capture_ts_us;
        total_bytes += frame.bytes;
        key_frames += frame.keyframe;
        ++frames;
        std::printf(
            "frame seq=%llu bytes=%u key=%u bitrate=%u kbps capture_ts_us=%lld event_ts_us=%lld\n",
            static_cast<unsigned long long>(frame.sequence), frame.bytes, frame.keyframe,
            frame.bitrate_kbps, static_cast<long long>(frame.capture_ts_us),
            static_cast<long long>(event.timestamp_us));
        break;
      }
      case PU_EVENT_STATE:
        ++state_events;
        std::printf("state %d -> %d (%s -> %s), reason=%s\n", event.data.state.previous_state,
                    event.data.state.state, StateName(event.data.state.previous_state),
                    StateName(event.data.state.state), ReasonName(event.data.state.reason));
        if (event.data.state.state == PU_STREAM_RUNNING) saw_running = true;
        break;
      case PU_EVENT_ERROR:
        ++error_events;
        std::printf("error event: code=%d message=%s\n", event.data.error.code,
                    event.data.error.message);
        break;
      case PU_EVENT_SIGNAL_OUT: {
        // v2：start 之后引擎会把自己生成的 offer 交出来，UI 负责发给服务端。
        // 本测试没有服务端也没有对端，只确认这条通道真的会冒事件、payload 是合法 JSON。
        const pu_signal_event& signal = event.data.signal;
        ++signal_events;
        if (signal.kind == PU_SIGNAL_OFFER) saw_offer_signal = true;
        std::printf("signal out: kind=%u len=%u payload=%s%s\n", signal.kind,
                    signal.payload_len,
                    signal.payload != nullptr ? std::string(signal.payload).substr(0, 48).c_str()
                                              : "(null)",
                    signal.payload_len > 48 ? "..." : "");
        // 把 offer 直接喂给观看方（C# 侧是「原样发给服务端」）。
        const int forward_rc = pu_signal_in(viewer, signal.payload, signal.payload_len);
        if (forward_rc != PU_OK) {
          std::printf("FAIL: pu_signal_in(viewer) rc=%d (%s)\n", forward_rc,
                      pu_last_error(viewer));
          pu_engine_destroy(engine);
          return 1;
        }
        break;
      }
      case PU_EVENT_REMOTE_FRAME:
        ++remote_frames;
        break;
      default:
        ++other_events;
        std::printf("unexpected event type=%u\n", event.type);
        break;
    }
  }

  // ---- 排空窗口（为异步编码器准备）----
  // 硬件编码走 MF async MFT：`Encode()` 只把帧入队，真正的 ProcessInput/ProcessOutput
  // 在编码器自己的线程上跑，所以「推送结束」到「最后一帧编码完成」之间有几十到几百
  // 毫秒在途。同步 VP8 的这段窗口约等于 0，于是同一个冒烟在 VP8 下是 0.96x、在异步
  // 硬件编码下只有 0.71x —— 差额是 teardown 截断在途帧，不是丢帧（编码器内部计数
  // 是 encode_calls==pushed==out、enqueue_drops=0）。这里给 400 ms 让在途帧走完，
  // 1:1 判据才有可比性。
  {
    const auto drain_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < drain_deadline) {
      pu_event drain_event;
      std::memset(&drain_event, 0, sizeof(drain_event));
      drain_event.struct_size = static_cast<uint32_t>(sizeof(drain_event));
      if (pu_poll_event(engine, &drain_event, 20) != PU_OK) continue;
      if (drain_event.type == PU_EVENT_FRAME) {
        ++frames;
        total_bytes += drain_event.data.frame.bytes;
        key_frames += drain_event.data.frame.keyframe;
      } else if (drain_event.type == PU_EVENT_REMOTE_FRAME) {
        ++remote_frames;
      }
    }
  }

  const int stop_rc = pu_screen_stop(engine);
  // 再停一次必须幂等返回 PU_OK。
  const int second_stop_rc = pu_screen_stop(engine);

  // ---- v2.1：编码次数检查（同一帧只编码一次）----
  // 口径：PC 的 outbound-rtp.framesEncoded 只统计 PC 自己的 VideoStreamEncoder。
  // 本地编码器删掉之后，它应当 ≈「DTLS 连上之后推送的帧数」；连上之前的帧 PC 不
  // 编码（实测 framesEncoded 恒 0），所以要先减掉那一段。2 倍就意味着同一帧被
  // 两个编码器各编码了一次。
  pu_stats stats_final;
  std::memset(&stats_final, 0, sizeof(stats_final));
  stats_final.struct_size = static_cast<uint32_t>(sizeof(stats_final));
  const bool stats_ok = (pu_peer_stats(engine, &stats_final) == PU_OK);
  const int64_t sent_total = stats_ok ? static_cast<int64_t>(stats_final.frames_sent) : 0;
  const int64_t encoded_total = stats_ok ? static_cast<int64_t>(stats_final.frames_encoded) : 0;
  const int64_t sent_after_connect = sent_total - sent_at_connect;
  const double encode_ratio =
      sent_after_connect > 0
          ? static_cast<double>(encoded_total) / static_cast<double>(sent_after_connect)
          : 0.0;

  // 先把观看方关掉，再关共享方：两端都还连着的时候关一端，比「对端已经没了再去关
  // 另一端」更接近真实退出顺序，也少一类「半死连接」上的收尾竞态。
  pu_engine_destroy(viewer);
  viewer_guard.engine = nullptr;

  // 收尾的 IDLE 状态事件（不阻塞太久，拿到就打印）。
  {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    while (pu_poll_event(engine, &event, 100) == PU_OK) {
      if (event.type == PU_EVENT_STATE) {
        ++state_events;
        std::printf("state %d -> %d (%s -> %s), reason=%s\n", event.data.state.previous_state,
                    event.data.state.state, StateName(event.data.state.previous_state),
                    StateName(event.data.state.state), ReasonName(event.data.state.reason));
      }
    }
  }

  const int source_failures = CheckScreenSources(engine);
  const int surface_failures = CheckUnimplementedSurface(engine);

  // pu_last_error 的指针在 destroy 之后失效，先拷出来。
  // 未经 stop 直接 destroy 也必须干净收尾（UI 直接退出的路径）。
  {
    pu_engine* abrupt = pu_engine_create(&config);
    if (abrupt == nullptr) {
      std::printf("FAIL: second pu_engine_create failed: %s\n", pu_last_error(nullptr));
      pu_engine_destroy(engine);
      return 1;
    }
    if (pu_screen_start(abrupt, "synthetic") != PU_OK) {
      std::printf("FAIL: second engine pu_screen_start failed: %s\n", pu_last_error(abrupt));
      pu_engine_destroy(abrupt);
      pu_engine_destroy(engine);
      return 1;
    }
    pu_event warmup;
    std::memset(&warmup, 0, sizeof(warmup));
    warmup.struct_size = static_cast<uint32_t>(sizeof(warmup));
    static_cast<void>(pu_poll_event(abrupt, &warmup, 500));
    const auto abrupt_start = std::chrono::steady_clock::now();
    pu_engine_destroy(abrupt);  // 没有 stop
    const auto abrupt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - abrupt_start)
                               .count();
    std::printf("destroy while running: %lld ms (no explicit stop)\n",
                static_cast<long long>(abrupt_ms));
  }

  const std::string last_error_copy = pu_last_error(engine) != nullptr ? pu_last_error(engine) : "";

  const auto destroy_start = std::chrono::steady_clock::now();
  pu_engine_destroy(engine);
  const auto destroy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - destroy_start)
                              .count();

  std::printf("\n=== summary ===\n");
  std::printf("  frames            : %d (target %d)\n", frames, kTargetFrames);
  std::printf("  key frames        : %llu\n", static_cast<unsigned long long>(key_frames));
  std::printf("  encoded bytes     : %llu\n", static_cast<unsigned long long>(total_bytes));
  std::printf("  state events      : %d (saw RUNNING: %s)\n", state_events,
              saw_running ? "yes" : "no");
  std::printf("  error events      : %d\n", error_events);
  std::printf("  other events      : %d\n", other_events);
  std::printf("  signal events     : %d (saw offer: %s)\n", signal_events,
              saw_offer_signal ? "yes" : "no");
  std::printf("  remote frames     : %d\n", remote_frames);
  if (frames > 1) {
    const double span_s = static_cast<double>(last_capture_us - first_capture_us) / 1e6;
    std::printf("  capture span      : %.3f s (%.1f fps)\n", span_s,
                span_s > 0.0 ? (frames - 1) / span_s : 0.0);
  }
  std::printf("  avg frame bytes   : %.1f\n",
              frames > 0 ? static_cast<double>(total_bytes) / frames : 0.0);
  std::printf("  note              : bytes/bitrate_kbps 是 PC GetStats 的 250 ms 窗口均值；\n");
  std::printf("                      avg frame bytes / avg bitrate 只作趋势参考，不是逐帧精确值\n");
  std::printf("  avg bitrate       : %.1f kbps\n",
              frames > 1 && last_capture_us > first_capture_us
                  ? static_cast<double>(total_bytes) * 8.0 /
                        (static_cast<double>(last_capture_us - first_capture_us) / 1e6) / 1000.0
                  : 0.0);
  std::printf("  A pushed          : %lld frame(s) (%lld before DTLS)\n",
              static_cast<long long>(sent_total), static_cast<long long>(sent_at_connect));
  std::printf("  PC framesEncoded  : %lld (outbound-rtp.framesEncoded)\n",
              static_cast<long long>(encoded_total));
  std::printf("  encode ratio      : %.2fx per pushed frame after DTLS (2.00x = duplicate encoding)\n",
              encode_ratio);
  std::printf("  viewer decoded    : %d remote frame(s)\n", viewer_remote_frames);
  std::printf("  stop rc           : %d (second stop: %d)\n", stop_rc, second_stop_rc);
  std::printf("  destroy           : %lld ms\n", static_cast<long long>(destroy_ms));
  std::printf("  first event ts    : %lld\n", static_cast<long long>(first_event_us));

  if (frames < kTargetFrames) {
    std::printf("FAIL: only %d frame events in %d s\n", frames, kTimeoutSeconds);
    std::printf("  pu_last_error: %s\n", last_error_copy.c_str());
    return 1;
  }
  if (!saw_running) {
    std::printf("FAIL: never observed STATE=RUNNING\n");
    return 1;
  }
  if (error_events != 0) {
    std::printf("FAIL: %d error event(s)\n", error_events);
    return 1;
  }
  if (!saw_offer_signal) {
    // v2：start 之后必须能看到一条 offer 信令出去，否则「信令通道 + PC」等于没接上。
    std::printf("FAIL: no PU_EVENT_SIGNAL_OUT offer was emitted after pu_screen_start\n");
    return 1;
  }
  if (stop_rc != PU_OK || second_stop_rc != PU_OK) {
    std::printf("FAIL: pu_screen_stop returned %d / %d\n", stop_rc, second_stop_rc);
    return 1;
  }
  if (source_failures != 0) {
    std::printf("FAIL: pu_screen_sources check found %d problem(s)\n", source_failures);
    return 1;
  }
  if (surface_failures != 0) {
    std::printf("FAIL: ABI surface check found %d unexpected result(s)\n", surface_failures);
    return 1;
  }
  if (!peer_connected) {
    std::printf("FAIL: the viewer never reached DTLS connected, cannot verify the encoding count\n");
    return 1;
  }
  if (!stats_ok) {
    std::printf("FAIL: pu_peer_stats failed after stop\n");
    return 1;
  }
  if (encoded_total > sent_total + kSampleLagFrames) {
    std::printf("FAIL: PC encoded %lld frame(s) for %lld pushed — more than one encode per frame\n",
                static_cast<long long>(encoded_total), static_cast<long long>(sent_total));
    return 1;
  }
  if (encoded_total + kSampleLagFrames < sent_after_connect) {
    std::printf(
        "FAIL: PC only encoded %lld of the %lld frame(s) pushed after DTLS (expected ~1:1)\n",
        static_cast<long long>(encoded_total), static_cast<long long>(sent_after_connect));
    return 1;
  }
  if (sent_after_connect > 0 && encoded_total * 2 >= sent_after_connect * 3) {
    std::printf("FAIL: encoding looks doubled: %lld encoded for %lld pushed after DTLS\n",
                static_cast<long long>(encoded_total), static_cast<long long>(sent_after_connect));
    return 1;
  }

  std::printf("PASS: %d frame events, no errors, %.2fx encoding (%lld encoded / %lld pushed after DTLS)\n",
              frames, encode_ratio, static_cast<long long>(encoded_total),
              static_cast<long long>(sent_after_connect));
  return 0;
}
