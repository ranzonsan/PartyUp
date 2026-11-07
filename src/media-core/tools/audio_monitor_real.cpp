/* PartyUp AUD02 · 真实硬件输入电平探针（真机诊断 + 验收用）
 *
 * 目的：用**生产 DLL 的 C ABI**（partyup_media.dll，与客户端同一条路径）回答真机问题，
 * 不依赖 GUI / 服务端 / 客户端：
 *   1) pu_audio_monitor_start 之后，官方 observer 是否真的收到 PCM（frames 增长、
 *      valid=1；不说话时应为全零 PCM，但仍是有效测量）；
 *   2) 互斥语义 A：语音已加入时 pu_audio_monitor_start 必须被拒（PU_ERR_STATE）；
 *   3) 互斥语义 B：monitor 先跑、再入语音时，voice join 必须先停掉 monitor 且 join 成功；
 *   4) **语音退出后 monitor 能否再次 Start 并恢复 PCM**（join→leave→monitor 循环 3 次，
 *      同时打印进程 handle 数量，验证共享 ADM 生命周期修复不会累积泄漏）。
 *
 * 本程序会真实打开麦克风（InitRecording/StartRecording），仅用于本机验证；
 * 不联网、不连服务端、不建与远端有关的信令（passive join 只建本地 PC/发送轨）。
 *
 * 运行要求：必须从本机盘运行，且 partyup_media.dll 与 exe 同目录（见
 * scripts/build-media-dll.ps1 的说明）；从 UNC 启动会因为原生库搜索路径失败。
 */
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "partyup_media.h"

namespace {

constexpr int kSampleMs = 100;
constexpr int kMonitorMs = 2500;  // 首次观察窗口：足够覆盖 ADM 采集线程起跑
constexpr int kCycleMs = 1200;    // 每轮 join→leave→monitor 的观察窗口
constexpr int kCycles = 3;

const char* StateName(int32_t state) {
  switch (state) {
    case PU_AUDIO_MONITOR_IDLE:
      return "IDLE";
    case PU_AUDIO_MONITOR_ACTIVE:
      return "ACTIVE";
    case PU_AUDIO_MONITOR_FAULTED:
      return "FAULTED";
    default:
      return "UNKNOWN";
  }
}

const char* ErrorOf(pu_engine* engine) {
  const char* text = pu_last_error(engine);
  return (text == nullptr || text[0] == '\0') ? "-" : text;
}

unsigned ProcessHandles() {
  DWORD count = 0;
  if (!::GetProcessHandleCount(::GetCurrentProcess(), &count)) return 0;
  return static_cast<unsigned>(count);
}

void PrintStatus(const char* tag, pu_engine* engine) {
  pu_audio_monitor_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  const int rc = pu_audio_monitor_status(engine, &info);
  if (rc != PU_OK) {
    std::printf("%s status rc=%d (%s)\n", tag, rc, ErrorOf(engine));
    return;
  }
  std::printf("%s status rc=%d state=%s running=%d valid=%u frames=%u peak=%u rms=%u\n", tag,
              rc, StateName(info.state), info.running, info.valid, info.frames, info.peak,
              info.rms);
}

/* 在 ms 毫秒窗口里每 100 ms 采一次 status，返回窗口内最后一次快照；
 * 同时打印变化点，便于人工判读「frames 是否在涨」。 */
pu_audio_monitor_info Observe(const char* tag, pu_engine* engine, int ms) {
  pu_audio_monitor_info last;
  std::memset(&last, 0, sizeof(last));
  last.struct_size = sizeof(last);
  uint32_t seen_frames = 0;
  for (int elapsed = 0; elapsed < ms; elapsed += kSampleMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
    pu_audio_monitor_info info;
    std::memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (pu_audio_monitor_status(engine, &info) != PU_OK) continue;
    last = info;
    if (info.frames != seen_frames) {
      std::printf("  %s t=%dms frames=%u valid=%u state=%s running=%d peak=%u rms=%u\n", tag,
                  elapsed + kSampleMs, info.frames, info.valid, StateName(info.state),
                  info.running, info.peak, info.rms);
      seen_frames = info.frames;
    }
  }
  return last;
}

struct FailureCounter {
  int failures = 0;
  void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
};

void FillVoiceParams(pu_voice_params* params, const char* channel) {
  std::memset(params, 0, sizeof(*params));
  params->struct_size = sizeof(*params);
  params->channel_id = channel;
  params->mode = PU_MODE_DIRECT;
  params->flags = 1;  // bit0 = 被动加入：只建本地 PC/发送轨，等远端 offer
}

}  // namespace

int main() {
  int exit_code = 0;
  FailureCounter fail;

  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = "audio-monitor-real";
  config.log_level = 3;
  config.event_queue_capacity = 256;

  pu_engine* engine = pu_engine_create(&config);
  if (engine == nullptr) {
    std::printf("FAIL: pu_engine_create returned null: %s\n", ErrorOf(nullptr));
    return 1;
  }

  uint32_t caps = 0;
  if (pu_engine_caps(engine, &caps) == PU_OK) {
    std::printf("engine caps=0x%x audio_monitor_bit7=%u\n", caps, (caps & 0x80u) != 0 ? 1 : 0);
    fail.Check((caps & 0x80u) != 0, "pu_engine_caps bit7 (AUD02) is not advertised");
  } else {
    fail.Check(false, "pu_engine_caps failed");
  }

  const int inputs = pu_audio_devices(engine, 0, nullptr, 0);
  std::printf("input device count=%d (%s)\n", inputs, inputs < 0 ? ErrorOf(engine) : "ok");
  fail.Check(inputs >= 0, "pu_audio_devices(input) returned a negative count");

  // ---- 阶段 1：真实硬件 monitor（首次） ----
  std::printf("== phase 1: monitor start (real hardware) ==\n");
  int rc = pu_audio_monitor_start(engine);
  std::printf("monitor_start rc=%d (%s)\n", rc, ErrorOf(engine));
  fail.Check(rc == PU_OK, "pu_audio_monitor_start failed on real hardware");

  pu_audio_monitor_info first = Observe("monitor", engine, kMonitorMs);
  std::printf("monitor final: state=%s running=%d valid=%u frames=%u peak=%u rms=%u\n",
              StateName(first.state), first.running, first.valid, first.frames, first.peak,
              first.rms);
  fail.Check(first.state == PU_AUDIO_MONITOR_ACTIVE, "monitor state is not ACTIVE");
  fail.Check(first.frames > 0, "monitor received no PCM (frames=0) on real hardware");
  fail.Check(first.valid == 1, "monitor valid=0 (no fresh PCM within 1s)");

  rc = pu_audio_monitor_stop(engine);
  std::printf("monitor_stop rc=%d (%s)\n", rc, ErrorOf(engine));
  fail.Check(rc == PU_OK, "pu_audio_monitor_stop failed");

  // ---- 阶段 2：join → leave → monitor，循环 kCycles 次 ----
  // 本轮验收重点：共享 ADM 在语音引擎 Terminate 之后必须还能重新开始采集。
  std::printf("== phase 2: %d x (voice join -> leave -> monitor start) ==\n", kCycles);
  const unsigned handles_before = ProcessHandles();
  std::printf("handles before cycles=%u\n", handles_before);
  unsigned handles_first_cycle = 0;
  pu_voice_params params;
  FillVoiceParams(&params, "aud02-cycle-probe");
  for (int cycle = 1; cycle <= kCycles; ++cycle) {
    rc = pu_voice_join(engine, &params);
    std::printf("[cycle %d] voice_join rc=%d (%s)\n", cycle, rc, ErrorOf(engine));
    fail.Check(rc == PU_OK, "pu_voice_join failed inside the cycle");
    if (rc != PU_OK) break;

    const int leave_rc = pu_voice_leave(engine);
    std::printf("[cycle %d] voice_leave rc=%d (%s)\n", cycle, leave_rc, ErrorOf(engine));
    fail.Check(leave_rc == PU_OK, "pu_voice_leave failed inside the cycle");

    const int start_rc = pu_audio_monitor_start(engine);
    std::printf("[cycle %d] monitor_start after leave rc=%d (%s)\n", cycle, start_rc,
                ErrorOf(engine));
    fail.Check(start_rc == PU_OK, "monitor start after voice leave failed");
    if (start_rc != PU_OK) {
      // 失败必须是「明确的 FAULTED」，不能假装 ACTIVE。
      pu_audio_monitor_info failed;
      std::memset(&failed, 0, sizeof(failed));
      failed.struct_size = sizeof(failed);
      if (pu_audio_monitor_status(engine, &failed) == PU_OK) {
        std::printf("[cycle %d] failed-start state=%s running=%d\n", cycle,
                    StateName(failed.state), failed.running);
        fail.Check(failed.state == PU_AUDIO_MONITOR_FAULTED,
                   "failed monitor start did not report state=FAULTED");
      }
      break;
    }
    pu_audio_monitor_info cycle_info = Observe("cycle-monitor", engine, kCycleMs);
    std::printf("[cycle %d] monitor state=%s valid=%u frames=%u peak=%u rms=%u\n", cycle,
                StateName(cycle_info.state), cycle_info.valid, cycle_info.frames,
                cycle_info.peak, cycle_info.rms);
    fail.Check(cycle_info.frames > 0, "monitor received no PCM after voice leave");
    fail.Check(cycle_info.valid == 1, "monitor valid=0 after voice leave");
    const int stop_rc = pu_audio_monitor_stop(engine);
    fail.Check(stop_rc == PU_OK, "monitor stop inside the cycle failed");
    std::printf("[cycle %d] handles=%u\n", cycle, ProcessHandles());
    if (cycle == 1) handles_first_cycle = ProcessHandles();
  }
  const unsigned handles_after = ProcessHandles();
  std::printf("handles after cycles=%u (before=%u, after-first-cycle=%u, delta=%d)\n",
              handles_after, handles_before, handles_first_cycle,
              static_cast<int>(handles_after) - static_cast<int>(handles_before));
  // 重建会一次性多开一块 ADM（handle 上涨属预期）；判据是**之后每轮不再增长**。
  fail.Check(handles_first_cycle == 0 || handles_after <= handles_first_cycle + 8u,
             "handle count keeps growing after the first rebuild (possible per-cycle leak)");

  // ---- 阶段 3：monitor 先跑，入语音必须先停 monitor ----
  std::printf("== phase 3: monitor first, voice join must stop it ==\n");
  rc = pu_audio_monitor_start(engine);
  std::printf("monitor_start (pre-voice) rc=%d (%s)\n", rc, ErrorOf(engine));
  fail.Check(rc == PU_OK, "monitor start before voice failed");
  if (rc == PU_OK) {
    rc = pu_voice_join(engine, &params);
    std::printf("voice_join while monitor active rc=%d (%s)\n", rc, ErrorOf(engine));
    fail.Check(rc == PU_OK, "pu_voice_join failed when monitor was active");
    PrintStatus("after voice join", engine);
    pu_audio_monitor_info after_join;
    std::memset(&after_join, 0, sizeof(after_join));
    after_join.struct_size = sizeof(after_join);
    if (pu_audio_monitor_status(engine, &after_join) == PU_OK) {
      fail.Check(after_join.state != PU_AUDIO_MONITOR_ACTIVE,
                 "monitor stayed ACTIVE after voice took over the capture");
    }
    const int leave_rc = pu_voice_leave(engine);
    std::printf("voice_leave (post-monitor) rc=%d (%s)\n", leave_rc, ErrorOf(engine));
    fail.Check(leave_rc == PU_OK, "pu_voice_leave after monitor-then-voice failed");
  }

  // ---- 阶段 4：语音占用时 monitor 必须被拒（不能抢语音的录音） ----
  std::printf("== phase 4: voice first, monitor start must be rejected ==\n");
  rc = pu_voice_join(engine, &params);
  std::printf("voice_join(passive) rc=%d (%s)\n", rc, ErrorOf(engine));
  fail.Check(rc == PU_OK, "pu_voice_join(passive) failed locally");
  if (rc == PU_OK) {
    const int rejected = pu_audio_monitor_start(engine);
    std::printf("monitor_start while in voice rc=%d (%s)\n", rejected, ErrorOf(engine));
    fail.Check(rejected == PU_ERR_STATE,
               "pu_audio_monitor_start was not rejected while voice owns the capture");
    PrintStatus("while in voice", engine);
    const int leave_rc = pu_voice_leave(engine);
    std::printf("voice_leave (phase 4) rc=%d (%s)\n", leave_rc, ErrorOf(engine));
    fail.Check(leave_rc == PU_OK, "pu_voice_leave (phase 4) failed");
  }

  std::printf("final handles=%u\n", ProcessHandles());
  pu_engine_destroy(engine);
  if (fail.failures == 0) {
    std::printf("PASS: real-hardware AUD02 monitor + voice/monitor mutex + post-voice restart\n");
    exit_code = 0;
  } else {
    std::printf("FAIL: %d check(s) failed\n", fail.failures);
    exit_code = 1;
  }
  return exit_code;
}
