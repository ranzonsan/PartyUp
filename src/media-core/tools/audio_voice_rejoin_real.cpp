/* PartyUp · 「加入语音 → 离开 → 再次加入」的可计数真机探针（voice rejoin）
 *
 * 为什么需要两端：passive join 只建本地 PC/发送轨，**没有协商就不会启动采集**
 * （真机实测：join 后 adm.recording=0）。要观察采集，必须在同进程里跑两个引擎
 * A(offerer) / B(answerer) 并互相喂 SIGNAL_OUT，把 DTLS/ICE 打通。
 *
 * 只用**可计数**判据，不依赖「听不听得见」（降噪/AEC/DTX 会污染听觉判断）：
 *   * A 采集侧：pu_voice_level 的 capture_frames / capture_bytes（与内容无关，静音也增长）；
 *   * A 采集状态：pu_voice_level 的 adm_initialized / adm_recording /
 *     adm_recording_initialized / adm_instance（instance 跨会话不变 ⇒ 复用同一块 ADM）；
 *   * B 接收侧：pu_peer_stats.frames_received（解出的远端音频 PCM 帧数）作交叉验证。
 *
 * 流程（重复 3 次）：A join(active) + B join(passive) → 泵信令直到双方 connected →
 * 观察 2 秒（每 10ms 泵一次）→ 打印增量 → 双方 leave。
 * 判据：三个会话 A 的 capture_frames 增量都 > 0 且会话 2/3 不低于会话 1 的一半；
 * leave 后 A 的 adm_initialized 仍为 1（ADM 是引擎级对象，不被会话回收）。
 *
 * 本程序会真实打开麦克风；不联网、不连服务端。
 * 必须从本机盘运行且 partyup_media.dll 与 exe 同目录；运行器须隐藏窗口。
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "partyup_media.h"

namespace {

constexpr int kSessions = 3;
constexpr int kObserveMs = 2000;
constexpr int kPumpMs = 10;
constexpr int kConnectTimeoutMs = 8000;
constexpr uint32_t kMinCaptureFrames = 20;
constexpr int32_t kConnected = 2;  // PU_PEER_CONNECTED

const char* ErrorOf(pu_engine* engine) {
  const char* text = pu_last_error(engine);
  return (text == nullptr || text[0] == '\0') ? "-" : text;
}

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 3;
  config.event_queue_capacity = 1024;
  return pu_engine_create(&config);
}

/* 把两端的 SIGNAL_OUT 互相灌进对方；返回 -1 表示致命错误。 */
int PumpBoth(pu_engine* a, pu_engine* b) {
  pu_engine* pairs[2][2] = {{a, b}, {b, a}};
  for (auto& pair : pairs) {
    pu_engine* from = pair[0];
    pu_engine* to = pair[1];
    for (;;) {
      pu_event event;
      std::memset(&event, 0, sizeof(event));
      event.struct_size = sizeof(event);
      const int rc = pu_poll_event(from, &event, 0);
      if (rc == PU_ERR_TIMEOUT) break;
      if (rc != PU_OK) {
        std::printf("FAIL: pu_poll_event rc=%d (%s)\n", rc, ErrorOf(from));
        return -1;
      }
      if (event.type != PU_EVENT_SIGNAL_OUT) continue;
      const int in_rc = pu_signal_in(to, event.data.signal.payload,
                                     event.data.signal.payload_len);
      if (in_rc != PU_OK) {
        std::printf("FAIL: pu_signal_in rc=%d (%s)\n", in_rc, ErrorOf(to));
        return -1;
      }
    }
  }
  return 0;
}

int32_t PeerState(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return PU_PEER_NONE;
  return stats.peer_state;
}

uint32_t ReceivedFrames(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return 0;
  return stats.frames_received;
}

pu_voice_level_info SampleLevel(pu_engine* engine) {
  pu_voice_level_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = static_cast<uint32_t>(sizeof(info));  // v2.19 完整尾部
  (void)pu_voice_level(engine, &info);
  return info;
}

void PrintLevel(const char* tag, int session, const pu_voice_level_info& info) {
  std::printf(
      "%s [session %d]: capture_frames=%u capture_bytes=%llu last_cb_ms=%llu "
      "level_frames=%u valid=%d peak=%u rms=%u source=%u "
      "adm(init=%u rec=%u rec_init=%u instance=%u)\n",
      tag, session, info.capture_frames,
      static_cast<unsigned long long>(info.capture_bytes),
      static_cast<unsigned long long>(info.last_cb_ms), info.frames, info.valid, info.peak,
      info.rms, info.source, info.adm_initialized, info.adm_recording,
      info.adm_recording_initialized, info.adm_instance);
}

struct FailureCounter {
  int failures = 0;
  void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
};

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  FailureCounter fail;
  std::printf("partyup voice rejoin probe (two engines, real ADM, countable capture)\n");

  pu_engine* a = CreateEngine("rejoin-A-offerer");
  pu_engine* b = CreateEngine("rejoin-B-answerer");
  if (a == nullptr || b == nullptr) {
    std::printf("FAIL: pu_engine_create returned null: %s\n", ErrorOf(nullptr));
    return 1;
  }

  pu_voice_params params;
  std::memset(&params, 0, sizeof(params));
  params.struct_size = sizeof(params);
  params.channel_id = "voice-rejoin-probe";
  params.mode = PU_MODE_DIRECT;

  uint32_t capture_deltas[kSessions] = {0};
  uint32_t adm_instances[kSessions] = {0};

  for (int session = 1; session <= kSessions; ++session) {
    pu_voice_params join_a = params;  // A 主动发起 offer
    pu_voice_params join_b = params;
    join_b.flags = 1;  // B 被动加入
    const int join_a_rc = pu_voice_join(a, &join_a);
    const int join_b_rc = pu_voice_join(b, &join_b);
    std::printf("[session %d] voice_join A rc=%d (%s) B rc=%d (%s)\n", session, join_a_rc,
                ErrorOf(a), join_b_rc, ErrorOf(b));
    fail.Check(join_a_rc == PU_OK && join_b_rc == PU_OK, "pu_voice_join failed");
    if (join_a_rc != PU_OK || join_b_rc != PU_OK) break;

    // 泵信令直到两端 connected（或超时）。
    const auto connect_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    bool connected = false;
    while (std::chrono::steady_clock::now() < connect_deadline) {
      if (PumpBoth(a, b) != 0) break;
      if (PeerState(a) == kConnected && PeerState(b) == kConnected) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPumpMs));
    }
    std::printf("[session %d] connected=%d A.state=%d B.state=%d (waited)\n", session,
                connected ? 1 : 0, PeerState(a), PeerState(b));
    fail.Check(connected, "voice session did not reach PU_PEER_CONNECTED");

    const pu_voice_level_info begin = SampleLevel(a);
    const uint32_t begin_received = ReceivedFrames(b);
    PrintLevel("  before-observe", session, begin);

    // 观察 2 秒：持续泵信令，保持媒体流动。
    for (int elapsed = 0; elapsed < kObserveMs; elapsed += kPumpMs) {
      (void)PumpBoth(a, b);
      std::this_thread::sleep_for(std::chrono::milliseconds(kPumpMs));
    }

    const pu_voice_level_info end = SampleLevel(a);
    const uint32_t end_received = ReceivedFrames(b);
    const uint32_t capture_delta = end.capture_frames - begin.capture_frames;
    capture_deltas[session - 1] = capture_delta;
    adm_instances[session - 1] = end.adm_instance;
    PrintLevel("  after-observe ", session, end);
    std::printf(
        "[session %d] capture_frames +%u (total %u), capture_bytes +%llu, "
        "level_frames +%u | B.frames_received +%u | A.state=%d B.state=%d\n",
        session, capture_delta, end.capture_frames,
        static_cast<unsigned long long>(end.capture_bytes - begin.capture_bytes),
        end.frames - begin.frames, end_received - begin_received, PeerState(a), PeerState(b));

    fail.Check(capture_delta >= kMinCaptureFrames,
               "A capture callbacks did not grow in this session (content-independent)");
    fail.Check(end.capture_bytes > begin.capture_bytes,
               "A capture bytes did not grow in this session");
    fail.Check(end_received > begin_received, "B received no audio PCM in this session");
    fail.Check(end.adm_recording == 1, "ADM is not recording while voice session is active");

    // 第 1 会话追加「内容相关性」验证（不靠听）：静音麦克风后 peak 必须掉下来，而
    // capture_frames（与内容无关的 liveness）必须继续增长 —— 两者语义不同的现场证明。
    if (session == 1) {
      const uint32_t before_peak = end.peak;
      const int mute_rc = pu_voice_set_mute(a, 1);
      std::printf("[session %d] mute rc=%d (%s), before_peak=%u\n", session, mute_rc,
                  ErrorOf(a), before_peak);
      fail.Check(mute_rc == PU_OK, "pu_voice_set_mute(1) failed");
      uint32_t muted_peak_max = 0;
      const pu_voice_level_info mute_begin = SampleLevel(a);
      for (int elapsed = 0; elapsed < 1000; elapsed += kPumpMs) {
        (void)PumpBoth(a, b);
        std::this_thread::sleep_for(std::chrono::milliseconds(kPumpMs));
        const pu_voice_level_info now = SampleLevel(a);
        if (now.peak > muted_peak_max) muted_peak_max = now.peak;
      }
      const pu_voice_level_info mute_end = SampleLevel(a);
      std::printf(
          "[session %d] while muted: peak_max=%u rms=%u valid=%d | capture_frames +%u "
          "(liveness keeps growing while muted)\n",
          session, muted_peak_max, mute_end.rms, mute_end.valid,
          mute_end.capture_frames - mute_begin.capture_frames);
      fail.Check(mute_end.capture_frames > mute_begin.capture_frames,
                 "capture_frames stopped growing while muted (liveness must be content-independent)");
      if (before_peak >= 20) {
        fail.Check(muted_peak_max * 4u <= before_peak,
                   "peak did not drop when the microphone was muted (level is not content-dependent)");
      } else {
        std::printf("[session %d] before_peak=%u too quiet to judge mute, skipped drop check\n",
                    session, before_peak);
      }
      const int unmute_rc = pu_voice_set_mute(a, 0);
      std::printf("[session %d] unmute rc=%d (%s)\n", session, unmute_rc, ErrorOf(a));
      fail.Check(unmute_rc == PU_OK, "pu_voice_set_mute(0) failed");
    }

    for (pu_engine* e : {a, b}) {
      const int leave_rc = pu_voice_leave(e);
      std::printf("[session %d] voice_leave rc=%d (%s)\n", session, leave_rc, ErrorOf(e));
      fail.Check(leave_rc == PU_OK, "pu_voice_leave failed");
    }
    // 离开后 ADM 应仍然 initialized（引擎级对象），实例代次跨会话不变 = 复用而非重建。
    const pu_voice_level_info after_leave = SampleLevel(a);
    std::printf(
        "[session %d] after-leave: adm(init=%u rec=%u rec_init=%u instance=%u) "
        "capture_frames=%u (cumulative, resets at next join)\n",
        session, after_leave.adm_initialized, after_leave.adm_recording,
        after_leave.adm_recording_initialized, after_leave.adm_instance,
        after_leave.capture_frames);
    fail.Check(after_leave.adm_initialized == 1,
               "ADM is not initialized after voice leave (engine-level ADM was terminated)");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  std::printf("capture_frames deltas: session1=%u session2=%u session3=%u\n",
              capture_deltas[0], capture_deltas[1], capture_deltas[2]);
  std::printf("adm_instance: session1=%u session2=%u session3=%u\n", adm_instances[0],
              adm_instances[1], adm_instances[2]);

  if (capture_deltas[0] > 0) {
    const uint32_t half = capture_deltas[0] / 2;
    fail.Check(capture_deltas[1] >= half,
               "session 2 capture callbacks far below session 1 (rejoin broken)");
    fail.Check(capture_deltas[2] >= half,
               "session 3 capture callbacks far below session 1 (rejoin broken)");
    fail.Check(adm_instances[0] == adm_instances[1] && adm_instances[1] == adm_instances[2],
               "ADM instance changed across sessions (rebuild instead of reuse)");
  }

  pu_engine_destroy(b);
  pu_engine_destroy(a);
  if (fail.failures == 0) {
    std::printf(
        "PASS: capture callbacks alive and comparable across %d join/leave sessions; "
        "one engine-level ADM reused (instance unchanged)\n",
        kSessions);
    return 0;
  }
  std::printf("FAIL: %d check(s) failed\n", fail.failures);
  return 1;
}
