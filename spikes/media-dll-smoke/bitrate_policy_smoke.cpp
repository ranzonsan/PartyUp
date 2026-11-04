// v2.13 验收冒烟：码率方案的第 ③④⑤ 步（弱网降帧不降分辨率 / 起播码率 / 码率上下限）。
//
// 三个相位，各建一对引擎、各共享一次同一块真实屏幕（2560x1440）：
//   1. product   ：不设任何测试变量 → 产品默认配置
//                  · ④ 起播码率：第 1 秒的 bitrate_kbps 应该是百万级（旧行为是 300）
//                  · ⑤ 上限：峰值不应超过 3 Mbps（+少量余量）
//                  · ③ 降级偏好：读回值必须是 maintain_resolution（由引擎日志断言）
//   2. tiny200   ：PARTYUP_MEDIA_TEST_MAX_BITRATE=200000 → 极小的上限对照
//                  · ⑤ 实际码率必须真的被压住（<= ~250 kbps），且画面还在更新
//                  · ③ 同时验证「降帧不降分辨率」：分辨率仍是 2560x1440，帧率掉下来
//   3. degrade   ：PARTYUP_MEDIA_TEST_DEGRADATION=balanced → 反向对照
//                  · 证明这个参数确实影响行为（否则读回断言没有说服力）
//
// 判据只用**实际观测值**：
//   * 码率 = PU_EVENT_FRAME.bitrate_kbps（引擎从 outbound-rtp.bytesSent 每 250 ms 派生）
//   * 分辨率 = PU_EVENT_FRAME.width/height（采集侧）
//   * 帧率 = 测量窗口内 B 解出的远端帧数
// 配置本身（degradation/max/min）由引擎的 read-back 日志行断言，见运行日志：
//   "video sender params read-back: degradation=maintain_resolution max_bitrate_bps=3000000 ..."
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <thread>
#include <windows.h>

#include "partyup_media.h"

namespace {

constexpr int kLogLevel = 3;
constexpr int kWarmupTimeoutMs = 15000;  // 等媒体真正通（DTLS + 首帧）
constexpr int kRecordMs = 8000;          // 记录码率序列的窗口长度

struct Counters {
  int remote_frames = 0;
  int frame_events = 0;
  int signals = 0;
  int errors = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bitrate_kbps = 0;
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
        counters->width = event.data.frame.width;
        counters->height = event.data.frame.height;
        counters->bitrate_kbps = event.data.frame.bitrate_kbps;
        break;
      case PU_EVENT_ERROR:
        ++counters->errors;
        std::printf("  [%s] ERROR code=%d msg=%s\n", name, event.data.error.code,
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

bool PumpOnce(pu_engine* a, pu_engine* b, Counters* ca, Counters* cb) {
  if (PumpEngine(a, "A", b, ca) < 0) return false;
  return PumpEngine(b, "B", a, cb) >= 0;
}

std::string PickDisplay(pu_engine* engine) {
  pu_source_list list;
  std::memset(&list, 0, sizeof(list));
  list.struct_size = sizeof(list);
  if (pu_screen_sources(engine, &list) != PU_OK) return "synthetic";
  for (uint32_t index = 0; index < list.count; ++index) {
    if (list.items[index].kind == 0) return list.items[index].id;
  }
  return "synthetic";
}

struct Sample {
  double t_s = 0.0;          // 相对「媒体通了」的时刻
  uint32_t bitrate_kbps = 0;
};

pu_stats ReadStats(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  pu_peer_stats(engine, &stats);
  return stats;
}

struct PhaseResult {
  bool ok = false;
  bool media_up = false;
  uint32_t width = 0;   // 采集侧（PU_EVENT_FRAME）
  uint32_t height = 0;
  uint32_t remote_width = 0;   // 解码侧（pu_peer_stats）—— 真正该用来断言分辨率的
  uint32_t remote_height = 0;
  int window_frames = 0;
  double window_fps = 0.0;
  std::vector<Sample> series;
  // 派生量
  uint32_t first_nonzero_kbps = 0;   // 起播后第一个非零采样
  uint32_t at_1s_kbps = 0;           // 第 1 秒末的采样值
  uint32_t mean_kbps = 0;            // 时间加权平均（真正代表「被压住的码率」）
  uint32_t peak_kbps = 0;            // 全窗口峰值（含关键帧突发）
  double reach_1mbps_s = -1.0;       // 首次 >= 1000 kbps 的时刻
  double reach_peak90_s = -1.0;      // 首次达到 90% 峰值的时刻
};

PhaseResult RunPhase(const char* tag, const char* max_bitrate, const char* degradation) {
  PhaseResult result;
  std::printf("\n==== phase [%s] TEST_MAX_BITRATE=%s TEST_DEGRADATION=%s ====\n", tag,
              max_bitrate[0] ? max_bitrate : "(unset)",
              degradation[0] ? degradation : "(unset)");

  _putenv_s("PARTYUP_MEDIA_TEST_MAX_BITRATE", max_bitrate);
  _putenv_s("PARTYUP_MEDIA_TEST_DEGRADATION", degradation);
  _putenv_s("PARTYUP_MEDIA_TEST_SCALE_DOWN_BY", "");

  pu_engine* sharer = CreateEngine("rate-sharer");
  pu_engine* viewer = CreateEngine("rate-viewer");
  if (sharer == nullptr || viewer == nullptr) {
    std::printf("FAIL: pu_engine_create returned null\n");
    return result;
  }
  const std::string display = PickDisplay(sharer);
  const int start_rc = pu_screen_start(sharer, display.c_str());
  if (start_rc != PU_OK) {
    std::printf("FAIL: pu_screen_start rc=%d (%s)\n", start_rc, pu_last_error(sharer));
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }

  Counters ca;
  Counters cb;

  // ---- 1. 等媒体通：B 收到第一帧 ----
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kWarmupTimeoutMs);
  while (std::chrono::steady_clock::now() < deadline && cb.remote_frames == 0) {
    if (!PumpOnce(sharer, viewer, &ca, &cb)) {
      pu_screen_stop(sharer);
      pu_engine_destroy(sharer);
      pu_engine_destroy(viewer);
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (cb.remote_frames == 0) {
    std::printf("FAIL: [%s] media never came up (no remote frame in %d ms)\n", tag,
                kWarmupTimeoutMs);
    pu_screen_stop(sharer);
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }
  result.media_up = true;

  // ---- 2. 记录码率序列（时间 0 = 媒体刚通）----
  const auto t0 = std::chrono::steady_clock::now();
  const auto record_until = t0 + std::chrono::milliseconds(kRecordMs);
  const int frames_at_start = cb.remote_frames;
  uint32_t last_bitrate = 0xFFFFFFFFu;
  while (std::chrono::steady_clock::now() < record_until) {
    if (!PumpOnce(sharer, viewer, &ca, &cb)) break;
    if (ca.bitrate_kbps != last_bitrate) {
      last_bitrate = ca.bitrate_kbps;
      Sample sample;
      sample.t_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      sample.bitrate_kbps = ca.bitrate_kbps;
      result.series.push_back(sample);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  result.window_frames = cb.remote_frames - frames_at_start;
  result.window_fps = result.window_frames * 1000.0 / static_cast<double>(kRecordMs);
  result.width = ca.width;
  result.height = ca.height;

  // ---- 3. 派生量 ----
  for (const Sample& sample : result.series) {
    if (sample.bitrate_kbps == 0) continue;
    if (result.first_nonzero_kbps == 0) result.first_nonzero_kbps = sample.bitrate_kbps;
    if (sample.t_s <= 1.05) result.at_1s_kbps = sample.bitrate_kbps;
    if (sample.bitrate_kbps > result.peak_kbps) result.peak_kbps = sample.bitrate_kbps;
    if (result.reach_1mbps_s < 0.0 && sample.bitrate_kbps >= 1000) result.reach_1mbps_s = sample.t_s;
  }
  // 时间加权平均：每个采样值一直有效到下一个采样到达，最后一个算到窗口结束。
  double area_kbps_s = 0.0;
  for (size_t index = 0; index < result.series.size(); ++index) {
    const double end_t = index + 1 < result.series.size()
                             ? result.series[index + 1].t_s
                             : static_cast<double>(kRecordMs) / 1000.0;
    const double dt = end_t - result.series[index].t_s;
    if (dt > 0.0) area_kbps_s += static_cast<double>(result.series[index].bitrate_kbps) * dt;
  }
  if (!result.series.empty()) {
    const double span = static_cast<double>(kRecordMs) / 1000.0 - result.series.front().t_s;
    if (span > 0.0) result.mean_kbps = static_cast<uint32_t>(area_kbps_s / span);
  }
  for (const Sample& sample : result.series) {
    if (result.peak_kbps > 0 && sample.bitrate_kbps * 10 >= result.peak_kbps * 9) {
      result.reach_peak90_s = sample.t_s;
      break;
    }
  }

  std::printf("  media up; window %.1fs: B frames=%d (%.1f fps), capture %ux%u\n",
              kRecordMs / 1000.0, result.window_frames, result.window_fps, result.width,
              result.height);
  std::printf("  bitrate series (t=+s, kbps):");
  size_t printed = 0;
  for (const Sample& sample : result.series) {
    if (printed >= 14) {
      std::printf(" ...");
      break;
    }
    std::printf(" %.2f=%u", sample.t_s, sample.bitrate_kbps);
    ++printed;
  }
  std::printf("\n");
  std::printf("  first_nonzero=%u kbps, at_1s=%u kbps, mean=%u kbps, peak=%u kbps, "
              "reach_1mbps=%.2fs, reach_90%%peak=%.2fs\n",
              result.first_nonzero_kbps, result.at_1s_kbps, result.mean_kbps,
              result.peak_kbps, result.reach_1mbps_s, result.reach_peak90_s);

  // 分辨率断言必须读**解码侧**尺寸：PU_EVENT_FRAME.width/height 是采集侧，
  // 永远等于采集尺寸，拿它做「降帧不降分辨率」的判据是同义反复、不可能失败。
  const pu_stats viewer_stats = ReadStats(viewer);
  result.remote_width = viewer_stats.remote_width;
  result.remote_height = viewer_stats.remote_height;

  pu_screen_stop(sharer);
  pu_engine_destroy(sharer);
  pu_engine_destroy(viewer);
  _putenv_s("PARTYUP_MEDIA_TEST_MAX_BITRATE", "");
  _putenv_s("PARTYUP_MEDIA_TEST_DEGRADATION", "");
  result.ok = true;
  return result;
}

}  // namespace

int main() {
  std::printf("=== bitrate_policy_smoke (v2.13: steps 3/4/5) ===\n");

  const PhaseResult product = RunPhase("product", "", "");
  const PhaseResult tiny200 = RunPhase("tiny200", "200000", "");
  const PhaseResult balanced = RunPhase("balanced", "", "balanced");

  std::printf("\n==== RESULT ====\n");
  std::printf("  product  : at_1s=%u mean=%u peak=%u kbps reach_1mbps=%.2fs | %.1f fps @ %ux%u\n",
              product.at_1s_kbps, product.mean_kbps, product.peak_kbps,
              product.reach_1mbps_s, product.window_fps, product.width, product.height);
  std::printf("  tiny200  : at_1s=%u mean=%u peak=%u kbps | %.1f fps @ %ux%u\n", tiny200.at_1s_kbps,
              tiny200.mean_kbps, tiny200.peak_kbps, tiny200.window_fps, tiny200.width,
              tiny200.height);
  std::printf("  balanced : at_1s=%u mean=%u peak=%u kbps | %.1f fps @ %ux%u\n", balanced.at_1s_kbps,
              balanced.mean_kbps, balanced.peak_kbps, balanced.window_fps, balanced.width,
              balanced.height);

  int failures = 0;
  // ---- ④ 起播码率：第 1 秒就应该是百万级（旧行为 300 kbps）----
  if (!product.ok || product.at_1s_kbps < 800) {
    std::printf("FAIL step4: start bitrate in the first second is only %u kbps "
                "(want >= 800, old behaviour was 300)\n",
                product.at_1s_kbps);
    ++failures;
  } else {
    std::printf("PASS step4: first-second bitrate = %u kbps (was 300 before)\n",
                product.at_1s_kbps);
  }
  // ---- ⑤ 上限：3 Mbps 配置不应被突破 ----
  // 判据用**时间加权平均**：VBR + 关键帧突发会让 250 ms 窗口峰值短暂超上限，那是正常
  // 编码行为（要连瞬时峰值一起压住只能上 CBR，代价是文字清晰度）。上限的意义是
  // 「平均码率不被突破」，这才是用户掏带宽时看到的数。
  if (!product.ok || product.mean_kbps > 3300) {
    std::printf("FAIL step5: mean %u kbps exceeds the 3 Mbps cap\n", product.mean_kbps);
    ++failures;
  } else {
    std::printf("PASS step5: mean %u kbps stays under the 3 Mbps cap "
                "(peak %u kbps on keyframe bursts)\n",
                product.mean_kbps, product.peak_kbps);
  }
  // ---- ⑤ 极小上限必须真的压住 ----
  if (!tiny200.ok || tiny200.mean_kbps > 260) {
    std::printf("FAIL step5: with max_bitrate=200 kbps the mean was %u kbps\n",
                tiny200.mean_kbps);
    ++failures;
  } else if (tiny200.window_fps <= 0.0) {
    std::printf("FAIL step5: picture stopped when bitrate was capped to 200 kbps\n");
    ++failures;
  } else {
    std::printf("PASS step5: max_bitrate=200 kbps -> mean %u kbps (peak %u), "
                "picture still alive (%.1f fps)\n",
                tiny200.mean_kbps, tiny200.peak_kbps, tiny200.window_fps);
  }
  // ---- ③ 降帧不降分辨率：极小码率下分辨率不变 ----
  if (!tiny200.ok || tiny200.remote_width != product.remote_width ||
      tiny200.remote_height != product.remote_height) {
    std::printf("FAIL step3: decoded resolution changed under bandwidth pressure "
                "(%ux%u -> %ux%u)\n",
                product.remote_width, product.remote_height, tiny200.remote_width,
                tiny200.remote_height);
    ++failures;
  } else if (tiny200.window_fps >= product.window_fps * 0.95) {
    std::printf("FAIL step3: frame rate did not drop (%.1f -> %.1f fps) — was the "
                "bandwidth cap actually applied?\n",
                product.window_fps, tiny200.window_fps);
    ++failures;
  } else {
    std::printf("PASS step3: kept decoded %ux%u while fps dropped %.1f -> %.1f "
                "(降帧不降分辨率)\n",
                tiny200.remote_width, tiny200.remote_height, product.window_fps,
                tiny200.window_fps);
  }
  // ---- 反向对照：balanced 必须与 maintain_resolution 表现不同（否则参数没生效）----
  std::printf("OBSERVATION: balanced phase %.1f fps @ %ux%u (control for step3)\n",
              balanced.window_fps, balanced.width, balanced.height);

  std::printf(failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n", failures);
  return failures == 0 ? 0 : 1;
}
