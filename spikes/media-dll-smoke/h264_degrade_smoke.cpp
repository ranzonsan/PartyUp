// v2.13 冒烟：验证「降分辨率」这条路在编码器侧到底会不会把画面搞死。
//
// 背景（这是本轮要回答的地雷）：
//   libwebrtc 的 quality scaler / VideoAdapter 一旦决定降分辨率，会用**更小的尺寸
//   再调一次 VideoEncoder::InitEncode**。而 MfH264Encoder 的 MFT 类型协商是静态的：
//   InitEncode 在 worker 已起时直接返回 OK（不重配），随后 Encode 对尺寸不符的帧
//   返回 ERR_PARAMETER。如果 libwebrtc 不先 Release() 就重新 InitEncode，画面会
//   直接卡死 —— 而且不会有明显报错。
//
// 三个相位（测试变量只在建 PC / AddTrack 时读，所以每相位独立建一对引擎）：
//   1. control    ：不设变量。基线：应稳定出帧，且日志里没有 IGNORING re-init。
//   2. scale2     ：PARTYUP_MEDIA_TEST_SCALE_DOWN_BY=2。**确定性**地逼 libwebrtc
//                   把帧在上游缩到一半、并用 1/2 尺寸重配编码器 —— 直接压在地雷上。
//                   不需要真的把带宽打满，也不依赖编码器上报 QP（我们其实没报 QP，
//                   所以 quality scaler 用 QP 那条路根本不会触发）。
//   3. lowbitrate ：MAX_BITRATE=150000 + DEGRADATION=balanced。真实拥塞形态：
//                   看没有 QP 反馈时，libwebrtc 会不会自然去降分辨率。
//
// 判据：测量窗口内 B 收到多少帧（画面是否还在更新）+ A 最后一条 PU_EVENT_FRAME
// 的尺寸 + B 的 remote_width/height + 引擎日志里的三条探针
// （IGNORING re-init / REJECT frame / max frame rate ->）。
//
// 期望（结论在日志里）：
//   * control  ：帧率正常、无 "IGNORING re-init"、无 "REJECT frame"。
//   * scale2   ：若出现 "IGNORING re-init" + "REJECT frame" 且 B 不再出帧 → 地雷坐实。
//   * lowbitrate：若同样出现 → 说明真实拥塞也会踩；否则说明今天这条路上走不到地雷。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <chrono>
#include <thread>
#include <windows.h>

#include "partyup_media.h"

namespace {

constexpr int kLogLevel = 3;

struct Counters {
  int remote_frames = 0;   // B：解出的远端帧
  int frame_events = 0;    // A：编码帧事件
  int signals = 0;
  int errors = 0;
  uint32_t last_width = 0;
  uint32_t last_height = 0;
  uint32_t last_bitrate_kbps = 0;
};

int PumpEngine(pu_engine* from, const char* name, pu_engine* to, Counters* counters) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(from, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: [%s] pu_poll_event rc=%d (%s)\n", name, rc, pu_last_error(from));
      return -1;
    }
    switch (event.type) {
      case PU_EVENT_REMOTE_FRAME:
        ++counters->remote_frames;
        break;
      case PU_EVENT_FRAME:
        ++counters->frame_events;
        counters->last_width = event.data.frame.width;
        counters->last_height = event.data.frame.height;
        counters->last_bitrate_kbps = event.data.frame.bitrate_kbps;
        break;
      case PU_EVENT_ERROR:
        ++counters->errors;
        std::printf("  [%s] ERROR event code=%d msg=%s\n", name, event.data.error.code,
                    event.data.error.message);
        break;
      case PU_EVENT_SIGNAL_OUT: {
        ++counters->signals;
        if (to == nullptr) break;
        const int in_rc =
            pu_signal_in(to, event.data.signal.payload, event.data.signal.payload_len);
        if (in_rc != PU_OK) {
          std::printf("FAIL: [%s] pu_signal_in rc=%d (%s)\n", name, in_rc, pu_last_error(to));
          return -1;
        }
        break;
      }
      default:
        break;
    }
  }
}

pu_engine* CreateEngine(const char* client_name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = client_name;
  config.log_level = kLogLevel;
  config.event_queue_capacity = 4096;
  return pu_engine_create(&config);
}

// 双向喂事件 duration_ms 毫秒；返回 false = 硬失败。
bool RunFor(pu_engine* a, pu_engine* b, Counters* ca, Counters* cb, int duration_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (PumpEngine(a, "A", b, ca) < 0) return false;
    if (PumpEngine(b, "B", a, cb) < 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

std::string PickDisplay(pu_engine* engine) {
  pu_source_list list;
  std::memset(&list, 0, sizeof(list));
  list.struct_size = sizeof(list);
  if (pu_screen_sources(engine, &list) != PU_OK) return "synthetic";
  for (uint32_t index = 0; index < list.count; ++index) {
    if (list.items[index].kind == 0) {
      std::printf("  display: %s %ux%u\n", list.items[index].id, list.items[index].width,
                  list.items[index].height);
      return list.items[index].id;
    }
  }
  std::printf("  no real display found -> synthetic 640x360@30\n");
  return "synthetic";
}

pu_stats ReadStats(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  pu_peer_stats(engine, &stats);
  return stats;
}

struct PhaseResult {
  bool ok = false;
  int warmup_frames = 0;
  int window_frames = 0;
  double window_fps = 0.0;
  uint32_t encoded_width = 0;
  uint32_t encoded_height = 0;
  uint32_t bitrate_kbps = 0;
  uint32_t stats_encoded = 0;
  uint32_t remote_width = 0;
  uint32_t remote_height = 0;
  int errors = 0;
};

// 一个相位：独立建一对引擎、独立共享一块屏，测 warmup 与正式窗口两段。
PhaseResult RunPhase(const char* tag, const char* max_bitrate, const char* degradation,
                     const char* scale_down_by, int warmup_ms, int measure_ms) {
  PhaseResult result;
  std::printf("\n==== phase [%s] MAX_BITRATE=%s DEGRADATION=%s SCALE_DOWN_BY=%s ====\n", tag,
              max_bitrate[0] ? max_bitrate : "(unset)",
              degradation[0] ? degradation : "(unset)",
              scale_down_by[0] ? scale_down_by : "(unset)");

  // 空串 = 删除该环境变量（GetEnvironmentVariableA 之后读不到）。
  _putenv_s("PARTYUP_MEDIA_TEST_MAX_BITRATE", max_bitrate);
  _putenv_s("PARTYUP_MEDIA_TEST_DEGRADATION", degradation);
  _putenv_s("PARTYUP_MEDIA_TEST_SCALE_DOWN_BY", scale_down_by);

  pu_engine* sharer = CreateEngine("degrade-sharer");
  pu_engine* viewer = CreateEngine("degrade-viewer");
  if (sharer == nullptr || viewer == nullptr) {
    std::printf("FAIL: pu_engine_create returned null\n");
    return result;
  }

  const std::string display = PickDisplay(sharer);
  const int start_rc = pu_screen_start(sharer, display.c_str());
  if (start_rc != PU_OK) {
    std::printf("FAIL: pu_screen_start(%s) rc=%d (%s)\n", display.c_str(), start_rc,
                pu_last_error(sharer));
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }

  Counters cs;
  Counters cv;
  if (!RunFor(sharer, viewer, &cs, &cv, warmup_ms)) {
    std::printf("FAIL: [%s] warmup pump failed\n", tag);
    pu_screen_stop(sharer);
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }
  result.warmup_frames = cv.remote_frames;

  const int before = cv.remote_frames;
  if (!RunFor(sharer, viewer, &cs, &cv, measure_ms)) {
    std::printf("FAIL: [%s] measure pump failed\n", tag);
    pu_screen_stop(sharer);
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }
  result.window_frames = cv.remote_frames - before;
  result.window_fps = result.window_frames * 1000.0 / static_cast<double>(measure_ms);
  result.encoded_width = cs.last_width;
  result.encoded_height = cs.last_height;
  result.bitrate_kbps = cs.last_bitrate_kbps;
  result.errors = cs.errors + cv.errors;

  const pu_stats sharer_stats = ReadStats(sharer);
  const pu_stats viewer_stats = ReadStats(viewer);
  result.stats_encoded = sharer_stats.frames_encoded;
  result.remote_width = viewer_stats.remote_width;
  result.remote_height = viewer_stats.remote_height;

  std::printf("  warmup %.1fs -> B frames=%d\n", warmup_ms / 1000.0, result.warmup_frames);
  std::printf("  window %.1fs -> B frames=%d (%.1f fps), A encoded %ux%u, %u kbps, "
              "stats.frames_encoded=%u\n",
              measure_ms / 1000.0, result.window_frames, result.window_fps,
              result.encoded_width, result.encoded_height, result.bitrate_kbps,
              result.stats_encoded);
  std::printf("  B remote %ux%u, errors=%d\n", result.remote_width, result.remote_height,
              result.errors);

  pu_screen_stop(sharer);
  pu_engine_destroy(sharer);
  pu_engine_destroy(viewer);
  _putenv_s("PARTYUP_MEDIA_TEST_MAX_BITRATE", "");
  _putenv_s("PARTYUP_MEDIA_TEST_DEGRADATION", "");
  _putenv_s("PARTYUP_MEDIA_TEST_SCALE_DOWN_BY", "");
  result.ok = true;
  return result;
}

}  // namespace

int main() {
  std::printf("=== h264_degrade_smoke (v2.13) ===\n");
  std::printf("patches are read when the screen PC is created (AddTrack).\n");

  constexpr int kWarmupMs = 4000;
  constexpr int kMeasureMs = 4000;

  const PhaseResult control = RunPhase("control", "", "", "", kWarmupMs, kMeasureMs);
  const PhaseResult scale2 = RunPhase("scale2", "", "", "2.0", kWarmupMs, kMeasureMs);
  const PhaseResult lowbitrate = RunPhase("lowbitrate", "150000", "balanced", "", kWarmupMs,
                                          kMeasureMs);

  std::printf("\n==== RESULT ====\n");
  std::printf("  control    : B %.1f fps, encoded %ux%u, %u kbps\n", control.window_fps,
              control.encoded_width, control.encoded_height, control.bitrate_kbps);
  std::printf("  scale2     : B %.1f fps, encoded %ux%u, %u kbps\n", scale2.window_fps,
              scale2.encoded_width, scale2.encoded_height, scale2.bitrate_kbps);
  std::printf("  lowbitrate : B %.1f fps, encoded %ux%u, %u kbps\n", lowbitrate.window_fps,
              lowbitrate.encoded_width, lowbitrate.encoded_height, lowbitrate.bitrate_kbps);

  // 控制组是回归门：基线本身必须出帧，否则后面的对比没有意义。
  if (!control.ok || control.window_fps < 10.0) {
    std::printf("FAIL: control phase did not carry video (%.1f fps)\n", control.window_fps);
    return 1;
  }
  std::printf("PASS: control phase carries video (%.1f fps)\n", control.window_fps);

  // 两个实验相位都只报事实，不做断言 —— 结论要看引擎日志里的探针行。
  std::printf("OBSERVATION: scale2 %.1f fps, lowbitrate %.1f fps\n", scale2.window_fps,
              lowbitrate.window_fps);
  std::printf("RESULT: PASS (facts reported; see engine log for IGNORING re-init / REJECT frame)\n");
  return 0;
}
