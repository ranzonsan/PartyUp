// v2.14 验收冒烟：屏幕共享编码尺寸上限 1080p（产品默认）。
//
// 为什么这不是「省带宽」而是「画质」：同一个 3 Mbps 上限下，1440p 的每像素比特数
// 只有 1080p 的 56%，文字边缘会糊。判据因此分两半：
//   * 客观：编码/解码尺寸、码率是否仍在上限内、编码器有没有尺寸不符；
//   * 主观：抓**解码后**的帧（pu_frame_acquire 给的是 BGRA 像素）落盘成图，
//     由人眼比对文字边缘。注意 PARTYUP_CAPTURE_DUMP_PNG 抓的是采集侧（缩放前），
//     两张图会一模一样，**不能用它做这个对比** —— 所以这里自己抓远端的解码帧。
//
// 三个相位（各建一对引擎、共享同一块 2560x1440 真屏、同一套 3 Mbps 上限）：
//   1. uncapped   ：PARTYUP_MAX_CAPTURE_HEIGHT=0 → 关掉上限，应回到 2560x1440
//   2. capped1080 ：默认（1080）→ 应变成 1920x1080
//   3. capped+200k：capped1080 + PARTYUP_MEDIA_TEST_MAX_BITRATE=200000
//                  → 静态降采样与 MAINTAIN_RESOLUTION 不打架：尺寸仍是 1920x1080，
//                    只是掉帧（而不是继续往下缩分辨率）
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
constexpr int kWarmupTimeoutMs = 15000;
constexpr int kSettleMs = 3000;   // 等 GCC 稳定，再抓码率/图
constexpr int kMeasureMs = 5000;

struct Counters {
  int remote_frames = 0;
  int frame_events = 0;
  int errors = 0;
  uint32_t capture_width = 0;
  uint32_t capture_height = 0;
  uint32_t bitrate_kbps = 0;
  uint32_t remote_width = 0;
  uint32_t remote_height = 0;
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
        counters->remote_width = event.data.remote.width;
        counters->remote_height = event.data.remote.height;
        break;
      case PU_EVENT_FRAME:
        ++counters->frame_events;
        counters->capture_width = event.data.frame.width;
        counters->capture_height = event.data.frame.height;
        counters->bitrate_kbps = event.data.frame.bitrate_kbps;
        break;
      case PU_EVENT_ERROR:
        ++counters->errors;
        std::printf("  [%s] ERROR code=%d msg=%s\n", name, event.data.error.code,
                    event.data.error.message);
        break;
      case PU_EVENT_SIGNAL_OUT: {
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

// 抓一张「解码后」的帧落盘成裸 BGRA（行紧密排列，width*4 字节/行）。
// 用裸格式是为了不依赖 PNG 编码器；转 PNG 由调用方用 zlib 做。
bool DumpRemoteFrame(pu_engine* viewer, const std::string& dir, const char* tag) {
  pu_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.struct_size = sizeof(frame);
  int rc = PU_ERR_TIMEOUT;
  for (int attempt = 0; attempt < 300; ++attempt) {
    rc = pu_frame_acquire(viewer, "remote", &frame);
    if (rc == PU_OK) break;
    if (rc != PU_ERR_TIMEOUT) break;  // 真错误（比如 stream_id 不认识）就别再等
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (rc != PU_OK) {
    std::printf("  dump: pu_frame_acquire gave up (rc=%d)\n", rc);
    return false;
  }
  const uint32_t row_bytes = frame.width * 4u;
  const std::string path = dir + "\\remote-" + tag + "-" + std::to_string(frame.width) + "x" +
                           std::to_string(frame.height) + ".bgra";
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    std::printf("  dump: cannot open %s\n", path.c_str());
    pu_frame_release(viewer, &frame);
    return false;
  }
  const auto* base = static_cast<const uint8_t*>(frame.pixels);
  bool ok = true;
  for (uint32_t y = 0; y < frame.height; ++y) {
    const uint8_t* row = base + static_cast<size_t>(y) * frame.stride;
    if (std::fwrite(row, 1, row_bytes, file) != row_bytes) {
      ok = false;
      break;
    }
  }
  std::fclose(file);
  pu_frame_release(viewer, &frame);
  if (!ok) return false;
  std::printf("  dump: %s (%ux%u stride=%u)\n", path.c_str(), frame.width, frame.height,
              frame.stride);
  std::printf("  DUMPFILE %s\n", path.c_str());
  return true;
}

struct PhaseResult {
  bool ok = false;
  uint32_t capture_w = 0;
  uint32_t capture_h = 0;
  uint32_t remote_w = 0;
  uint32_t remote_h = 0;
  int window_frames = 0;
  double window_fps = 0.0;
  uint32_t mean_kbps = 0;
  uint32_t peak_kbps = 0;
  bool dumped = false;
};

PhaseResult RunPhase(const char* tag, const char* max_capture_height, const char* max_bitrate,
                     const std::string& dump_dir) {
  PhaseResult result;
  std::printf("\n==== phase [%s] PARTYUP_MAX_CAPTURE_HEIGHT=%s TEST_MAX_BITRATE=%s ====\n", tag,
              max_capture_height[0] ? max_capture_height : "(unset=no scaling)",
              max_bitrate[0] ? max_bitrate : "(unset=3Mbps)");

  _putenv_s("PARTYUP_MAX_CAPTURE_HEIGHT", max_capture_height);
  _putenv_s("PARTYUP_MEDIA_TEST_MAX_BITRATE", max_bitrate);

  pu_engine* sharer = CreateEngine("res-sharer");
  pu_engine* viewer = CreateEngine("res-viewer");
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
    std::printf("FAIL: [%s] media never came up\n", tag);
    pu_screen_stop(sharer);
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return result;
  }

  // 等画面/GCC 稳定，再抓图 + 量码率
  const auto settle_until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kSettleMs);
  while (std::chrono::steady_clock::now() < settle_until) {
    if (!PumpOnce(sharer, viewer, &ca, &cb)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!dump_dir.empty()) {
    // 抓 4 帧、间隔 700 ms：桌面内容在相位之间会变（别的窗口在动），靠多抓几帧 +
    // 事后按内容相关性挑选配对，才能做「同一内容、不同分辨率」的公平比较。
    char frame_tag[96];
    for (int index = 0; index < 4; ++index) {
      std::snprintf(frame_tag, sizeof(frame_tag), "%s-f%d", tag, index);
      result.dumped = DumpRemoteFrame(viewer, dump_dir, frame_tag) || result.dumped;
      const auto gap_until =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
      while (std::chrono::steady_clock::now() < gap_until) {
        if (!PumpOnce(sharer, viewer, &ca, &cb)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  const auto measure_until = t0 + std::chrono::milliseconds(kMeasureMs);
  const int frames_at_start = cb.remote_frames;
  std::vector<std::pair<double, uint32_t>> series;
  uint32_t last = 0xFFFFFFFFu;
  while (std::chrono::steady_clock::now() < measure_until) {
    if (!PumpOnce(sharer, viewer, &ca, &cb)) break;
    if (ca.bitrate_kbps != last) {
      last = ca.bitrate_kbps;
      series.emplace_back(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
          ca.bitrate_kbps);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  result.capture_w = ca.capture_width;
  result.capture_h = ca.capture_height;
  result.remote_w = cb.remote_width;
  result.remote_h = cb.remote_height;
  result.window_frames = cb.remote_frames - frames_at_start;
  result.window_fps = result.window_frames * 1000.0 / static_cast<double>(kMeasureMs);

  double area = 0.0;
  for (size_t index = 0; index < series.size(); ++index) {
    const double end_t = index + 1 < series.size() ? series[index + 1].first
                                                   : static_cast<double>(kMeasureMs) / 1000.0;
    const double dt = end_t - series[index].first;
    if (dt > 0.0) area += static_cast<double>(series[index].second) * dt;
    if (series[index].second > result.peak_kbps) result.peak_kbps = series[index].second;
  }
  if (!series.empty()) {
    const double span = static_cast<double>(kMeasureMs) / 1000.0 - series.front().first;
    if (span > 0.0) result.mean_kbps = static_cast<uint32_t>(area / span);
  }

  std::printf("  capture %ux%u -> remote %ux%u | %.1f fps | mean=%u peak=%u kbps\n",
              result.capture_w, result.capture_h, result.remote_w, result.remote_h,
              result.window_fps, result.mean_kbps, result.peak_kbps);

  pu_screen_stop(sharer);
  pu_engine_destroy(sharer);
  pu_engine_destroy(viewer);
  result.ok = true;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃时也要看得到最后一句
  std::printf("=== resolution_cap_smoke (v2.14: 1080p encode cap) ===\n");

  // 落盘目录：优先命令行参数，其次 PARTYUP_REMOTE_DUMP_DIR
  std::string dump_dir;
  if (argc > 1) {
    dump_dir = argv[1];
  } else {
    char text[512] = {0};
    const DWORD len = ::GetEnvironmentVariableA("PARTYUP_REMOTE_DUMP_DIR", text,
                                                static_cast<DWORD>(sizeof(text)));
    if (len > 0) dump_dir = text;
  }
  if (!dump_dir.empty()) {
    ::CreateDirectoryA(dump_dir.c_str(), nullptr);
    std::printf("dump dir: %s\n", dump_dir.c_str());
  }

  // 产品默认 = 不缩放（2026-11-04 实测后定）；"1080" 才是显式开启压缩的场景。
  const PhaseResult default_phase = RunPhase("default", "", "", dump_dir);
  const PhaseResult capped = RunPhase("cap1080", "1080", "", dump_dir);
  const PhaseResult cappedTiny = RunPhase("cap1080+200k", "1080", "200000", dump_dir);

  // 码率对齐对比：同样 800 kbps 上限下，1440p vs 1080p 谁的文字更清楚。
  // 这是 Lead 命题的正面检验 —— 只有当码率成为瓶颈时「像素少 = 每像素比特多」
  // 才成立；3 Mbps 下静态桌面根本喂不饱编码器，那时 1080p 反而因为多一次上下采样
  // 更软（见 dumps3 的对比图）。两种情形都测，结论才说得清。
  const PhaseResult uncapped800 = RunPhase("uncapped-800k", "0", "800000", dump_dir);
  const PhaseResult capped800 = RunPhase("capped-800k", "1080", "800000", dump_dir);
  // 真正让码率成为瓶颈的对照：200 kbps。只有这时「像素少 = 每像素比特多」才该兑现成
  // 「1080p 更清楚」；如果连这里都是 1440p 更锐，那 1080p 上限就是纯画质损失。
  const PhaseResult uncapped200 = RunPhase("uncapped-200k", "0", "200000", dump_dir);

  std::printf("  uncapped800: capture %ux%u -> remote %ux%u | %.1f fps | mean %u kbps\n",
              uncapped800.capture_w, uncapped800.capture_h, uncapped800.remote_w,
              uncapped800.remote_h, uncapped800.window_fps, uncapped800.mean_kbps);
  std::printf("  capped800  : capture %ux%u -> remote %ux%u | %.1f fps | mean %u kbps\n",
              capped800.capture_w, capped800.capture_h, capped800.remote_w,
              capped800.remote_h, capped800.window_fps, capped800.mean_kbps);
  std::printf("  uncapped200: capture %ux%u -> remote %ux%u | %.1f fps | mean %u kbps\n",
              uncapped200.capture_w, uncapped200.capture_h, uncapped200.remote_w,
              uncapped200.remote_h, uncapped200.window_fps, uncapped200.mean_kbps);

  int failures = 0;
  // 1) 产品默认必须等于采集原尺寸（不缩放）—— 2026-11-04 实测后的决定
  if (!default_phase.ok || default_phase.capture_h == 0 ||
      default_phase.remote_h != default_phase.capture_h ||
      default_phase.remote_w != default_phase.capture_w) {
    std::printf("FAIL: default (no env) should pass through %ux%u, got %ux%u\n",
                default_phase.capture_w, default_phase.capture_h, default_phase.remote_w,
                default_phase.remote_h);
    ++failures;
  } else {
    std::printf("PASS: product default -> remote %ux%u (= capture, no downscale)\n",
                default_phase.remote_w, default_phase.remote_h);
  }
  // 2) 默认必须缩到 1920x1080。
  // 注：解码端可能比编码尺寸高 8 行（1920x1088）——H.264 的宏块/解码对齐会把 1080
  // 补到 1088，编码器本身日志是 1920x1080，两者都算对。
  const bool capped_size_ok =
      capped.remote_w == 1920 && (capped.remote_h == 1080 || capped.remote_h == 1088);
  if (!capped.ok || !capped_size_ok) {
    std::printf("FAIL: expected 1920x1080, got %ux%u\n", capped.remote_w, capped.remote_h);
    ++failures;
  } else {
    std::printf("PASS: PARTYUP_MAX_CAPTURE_HEIGHT=1080 -> remote %ux%u (from %ux%u)\n",
                capped.remote_w, capped.remote_h, capped.capture_w, capped.capture_h);
  }
  // 3) 码率仍在上限内
  if (!capped.ok || capped.mean_kbps > 3200) {
    std::printf("FAIL: mean bitrate %u kbps exceeds the 3 Mbps cap\n", capped.mean_kbps);
    ++failures;
  } else {
    std::printf("PASS: 1080p mean %u kbps stays under the 3 Mbps cap\n", capped.mean_kbps);
  }
  // 4) 静态降采样与 MAINTAIN_RESOLUTION 不打架：极小码率下尺寸不变、只掉帧
  if (!cappedTiny.ok || cappedTiny.remote_w != capped.remote_w ||
      cappedTiny.remote_h != capped.remote_h) {
    std::printf("FAIL: under 200 kbps the size drifted to %ux%u (static scale and "
                "degradation_preference are fighting)\n",
                cappedTiny.remote_w, cappedTiny.remote_h);
    ++failures;
  } else if (cappedTiny.window_fps >= capped.window_fps * 0.95) {
    std::printf("FAIL: framerate did not drop under 200 kbps (%.1f -> %.1f)\n",
                capped.window_fps, cappedTiny.window_fps);
    ++failures;
  } else {
    std::printf("PASS: 200 kbps -> still %ux%u, fps %.1f -> %.1f (掉帧不降分辨率)\n",
                cappedTiny.remote_w, cappedTiny.remote_h, capped.window_fps,
                cappedTiny.window_fps);
  }

  std::printf(failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n", failures);
  return failures == 0 ? 0 : 1;
}
