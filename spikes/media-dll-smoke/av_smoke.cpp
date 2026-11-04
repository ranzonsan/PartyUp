// 音视频交叉冒烟：验证「屏幕共享 + 语音」共存，两种顺序各跑一遍。
//   顺序 1：先 pu_screen_start → 再 pu_voice_join
//   顺序 2：先 pu_voice_join  → 再 pu_screen_start
// 判据：两种顺序下语音 PCM 帧率（~100/s）与视频帧数都不掉。
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <windows.h>

#include "partyup_media.h"

namespace {

struct Counters {
  int video_frames = 0;   // 远端解码帧（PU_EVENT_REMOTE_FRAME）或本端编码帧
  int signals = 0;
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
        ++counters->video_frames;
        break;
      case PU_EVENT_FRAME:
        ++counters->video_frames;
        break;
      case PU_EVENT_SIGNAL_OUT: {
        ++counters->signals;
        if (to == nullptr) break;
        const int in_rc = pu_signal_in(to, event.data.signal.payload, event.data.signal.payload_len);
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
  config.log_level = 3;
  config.event_queue_capacity = 2048;
  return pu_engine_create(&config);
}

uint32_t PcmFrames(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return 0;
  return stats.frames_received;  // 语音模式下 = 远端 PCM 帧数
}

// 跑 duration_ms 毫秒：双向喂事件，返回 B 解出的视频帧数。
int RunFor(pu_engine* a, pu_engine* b, Counters* ca, Counters* cb, int duration_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (PumpEngine(a, "A", b, ca) < 0) return -1;
    if (PumpEngine(b, "B", a, cb) < 0) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cb->video_frames;
}

struct Result {
  bool ok = false;
  int order = 0;
  double video_fps = 0.0;
  double pcm_fps = 0.0;
  uint32_t pcm_before = 0;
  uint32_t pcm_after = 0;
};

// 一种顺序跑一遍。screen_first=true → 先共享后语音。
Result RunOrder(bool screen_first, int order_index) {
  Result r;
  r.order = order_index;
  const char* tag = screen_first ? "screen->voice" : "voice->screen";
  std::printf("\n---- order %d: %s ----\n", order_index, tag);

  pu_engine* a = CreateEngine("engine-A");
  pu_engine* b = CreateEngine("engine-B");
  if (a == nullptr || b == nullptr) return r;
  Counters ca, cb;

  pu_voice_params vp;
  std::memset(&vp, 0, sizeof(vp));
  vp.struct_size = sizeof(vp);
  vp.channel_id = "av-smoke";
  vp.mode = PU_MODE_DIRECT;

  auto join_voice = [&](bool passive) {
    pu_voice_params p = vp;
    p.flags = passive ? 1u : 0u;
    return pu_voice_join(passive ? b : a, &p);
  };

  // ---- 顺序 1 的前半：先共享 ----
  if (screen_first) {
    if (pu_screen_start(a, "synthetic") != PU_OK) {
      std::printf("FAIL: pu_screen_start rc=%d (%s)\n", pu_last_error(a));
      return r;
    }
    RunFor(a, b, &ca, &cb, 2500);
    std::printf("  [screen only] B video frames=%d\n", cb.video_frames);
    if (cb.video_frames < 20) {
      std::printf("FAIL: video did not flow before voice (%d frames)\n", cb.video_frames);
      return r;
    }
  }

  // ---- join 语音 ----
  if (join_voice(/*passive=*/false) != PU_OK) {
    std::printf("FAIL: A voice_join rc=%d (%s)\n", pu_last_error(a));
    return r;
  }
  if (join_voice(/*passive=*/true) != PU_OK) {
    std::printf("FAIL: B voice_join rc=%d (%s)\n", pu_last_error(b));
    return r;
  }
  RunFor(a, b, &ca, &cb, 3000);  // 让语音协商完成
  r.pcm_before = PcmFrames(b);
  const int video_mid = cb.video_frames;
  std::printf("  [voice up] B pcm=%u B video frames=%d\n", r.pcm_before, video_mid);

  // ---- 顺序 2 的后半：语音起来后再共享 ----
  if (!screen_first) {
    if (pu_screen_start(a, "synthetic") != PU_OK) {
      std::printf("FAIL: pu_screen_start(after voice) rc=%d (%s)\n", pu_last_error(a));
      return r;
    }
  }

  // ---- 测量窗口 4 秒 ----
  const int video_before = cb.video_frames;
  const uint32_t pcm_start = PcmFrames(b);
  RunFor(a, b, &ca, &cb, 4000);
  const int video_after = cb.video_frames;
  const uint32_t pcm_end = PcmFrames(b);
  r.pcm_after = pcm_end;
  r.pcm_fps = static_cast<double>(pcm_end - pcm_start) / 4.0;
  r.video_fps = static_cast<double>(video_after - video_before) / 4.0;

  std::printf("  [measure 4s] pcm %.1f fps (%u -> %u), video %.1f fps (%d -> %d)\n",
              r.pcm_fps, pcm_start, pcm_end, r.video_fps, video_before, video_after);

  pu_voice_leave(a);
  pu_voice_leave(b);
  pu_screen_stop(a);
  pu_engine_destroy(a);
  pu_engine_destroy(b);

  // PCM 绝对帧率取决于本机麦克风有没有声音（阈值 1 时，纯数字静音的帧不计），
  // 所以这里只要求「语音链路还活着」，真正的 100/s 判据在 voice_smoke（阈值 1、稳定）。
  r.ok = (r.pcm_fps >= 30.0) && (r.video_fps >= 15.0);
  return r;
}

// ---------------------------------------------------------------------------
// 顺序 3（v2.12 / 第 4 步紧急修复的回归点）：语音先起，然后「开始共享」与
// 「停止共享」两个时刻语音都必须继续走。
//
// 曾经的 bug：屏幕停止路径（ReleaseStreamObjects）末尾调 ReleaseVoiceSession()、
// 并在同一个块里摘掉语音的远端音频 sink，于是共享一开一停就把语音会话拆了
// （真机现象：pcmRx 7989 → 0，日志 release-voice: enter (pc=1 joined=1)）。
// 这里把「共享开始」和「共享停止」都各测一个窗口，别只测一个方向。
// ---------------------------------------------------------------------------
struct SurviveResult {
  bool ok = false;
  double baseline_fps = 0.0;
  double after_start_fps = 0.0;
  double after_stop_fps = 0.0;
  int video_frames = 0;
  bool session_alive_after_stop = false;
};

SurviveResult RunVoiceSurvivesScreen() {
  SurviveResult r;
  std::printf("\n---- order 3: voice -> screen start -> screen stop (voice must survive) ----\n");
  pu_engine* a = CreateEngine("survive-A");
  pu_engine* b = CreateEngine("survive-B");
  if (a == nullptr || b == nullptr) return r;
  Counters ca, cb;

  pu_voice_params vp;
  std::memset(&vp, 0, sizeof(vp));
  vp.struct_size = sizeof(vp);
  vp.channel_id = "av-survive";
  vp.mode = PU_MODE_DIRECT;

  const int join_a_rc = pu_voice_join(a, &vp);
  if (join_a_rc != PU_OK) {
    std::printf("FAIL: A voice_join rc=%d (%s)\n", join_a_rc, pu_last_error(a));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  pu_voice_params passive = vp;
  passive.flags = 1u;  // answerer
  const int join_b_rc = pu_voice_join(b, &passive);
  if (join_b_rc != PU_OK) {
    std::printf("FAIL: B voice_join rc=%d (%s)\n", join_b_rc, pu_last_error(b));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  if (RunFor(a, b, &ca, &cb, 3000) < 0) {
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }

  // 基线：纯语音 1.5 s。
  const uint32_t pcm0 = PcmFrames(b);
  if (RunFor(a, b, &ca, &cb, 1500) < 0) {
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  const uint32_t pcm1 = PcmFrames(b);
  r.baseline_fps = static_cast<double>(pcm1 - pcm0) / 1.5;

  // 开始共享：语音必须不停。窗口给 2.5 s——屏幕会话要等 ICE/SDP/DTLS 建起来才出帧，
  // 1.5 s 里只能数到个位数帧（本机实测 5 帧），会把「视频刚起来」误判成失败。
  if (pu_screen_start(a, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start rc=%d (%s)\n", pu_last_error(a));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  const int video_before = cb.video_frames;
  if (RunFor(a, b, &ca, &cb, 2500) < 0) {
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  const uint32_t pcm2 = PcmFrames(b);
  r.after_start_fps = static_cast<double>(pcm2 - pcm1) / 2.5;
  r.video_frames = cb.video_frames - video_before;

  // 停止共享：语音仍必须不停，且会话对象还在（不只是计数器侥幸没清零）。
  if (pu_screen_stop(a) != PU_OK) {
    std::printf("FAIL: pu_screen_stop rc=%d (%s)\n", pu_last_error(a));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  if (RunFor(a, b, &ca, &cb, 2000) < 0) {
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return r;
  }
  const uint32_t pcm3 = PcmFrames(b);
  r.after_stop_fps = static_cast<double>(pcm3 - pcm2) / 2.0;

  pu_voice_track track;
  std::memset(&track, 0, sizeof(track));
  track.struct_size = sizeof(track);
  const bool track_alive = pu_voice_tracks(b, &track, 1) == 1;
  pu_stats stats_a;
  std::memset(&stats_a, 0, sizeof(stats_a));
  stats_a.struct_size = sizeof(stats_a);
  const bool a_voice_connected =
      pu_peer_stats(a, &stats_a) == PU_OK && stats_a.peer_state == PU_PEER_CONNECTED;
  r.session_alive_after_stop = track_alive && a_voice_connected;

  std::printf("  pcm baseline %.1f/s (%u -> %u) | after screen START %.1f/s (%u -> %u) | "
              "after screen STOP %.1f/s (%u -> %u)\n",
              r.baseline_fps, pcm0, pcm1, r.after_start_fps, pcm1, pcm2, r.after_stop_fps, pcm2,
              pcm3);
  std::printf("  screen video frames=%d | after stop: B track=%d A peerState=%d (want 1/2)\n",
              r.video_frames, track_alive ? 1 : 0, stats_a.peer_state);

  // 注意：这个绝对下限不能用 60。ADM 的周期随音频设备/系统负载在 10 ms（100 帧/s）与
  // 20 ms（50 帧/s）之间切换，20 ms 时 baseline 50/s 是**完全正常**的，用 >=60 判会让
  // 这个用例在没有回归的情况下间歇性 FAIL（2026-11-04 实测：同一二进制 50.0/50.4/89.0
  // 判 FAIL，紧接着 100.0/100.0/91.0 判 PASS）。下限只用来排除"语音根本没出帧"，
  // 真正的判据是相对比值（start/stop 后仍 >= baseline 的 60%）。
  // 相对比值也放宽到 0.45：ADM 在共享启停时会重新协商周期，10 ms(100 帧/s) 与
  // 20 ms(50 帧/s) 之间的切换本身就是"合法的 0.5×"，用 0.6× 会把这种正常切换判成
  // 回归（2026-11-04 实测 100.7/100.0/54.5 被判 FAIL，而 54.5 正是 20 ms 周期的满速）。
  // 真正的失败是"几乎不出帧"（例如 100 → 5），0.45× + 绝对下限 15 仍能抓住。
  const bool rate_ok = r.baseline_fps >= 25.0 &&
                       r.after_start_fps >= 0.45 * r.baseline_fps &&
                       r.after_start_fps >= 15.0 &&
                       r.after_stop_fps >= 0.45 * r.baseline_fps &&
                       r.after_stop_fps >= 15.0;
  r.ok = rate_ok && r.session_alive_after_stop && r.video_frames >= 15;
  if (!rate_ok) {
    std::printf("  FAIL: voice PCM rate dropped across screen start/stop\n");
  }
  if (!r.session_alive_after_stop) {
    std::printf("  FAIL: voice session is gone after pu_screen_stop (track/peerState)\n");
  }
  if (r.video_frames < 15) {
    std::printf("  FAIL: screen video did not flow (%d frames)\n", r.video_frames);
  }

  pu_voice_leave(a);
  pu_voice_leave(b);
  pu_engine_destroy(a);
  pu_engine_destroy(b);
  return r;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media A/V coexistence smoke (screen + voice, two engines)\n");
  // 这台机器的麦克风不是数字静音，默认峰值阈值 8 会让计数时有时无；
  // 本冒烟只验证 A/V 共存链路，把阈值压到 1 让计数确定。
  ::SetEnvironmentVariableA("PARTYUP_VOICE_PEAK_THRESHOLD", "1");

  const Result r1 = RunOrder(/*screen_first=*/true, 1);
  const Result r2 = RunOrder(/*screen_first=*/false, 2);
  const SurviveResult r3 = RunVoiceSurvivesScreen();

  std::printf("\n==== RESULT ====\n");
  std::printf("  order1 screen->voice: pcm=%.1f fps video=%.1f fps -> %s\n", r1.pcm_fps,
              r1.video_fps, r1.ok ? "OK" : "FAIL");
  std::printf("  order2 voice->screen: pcm=%.1f fps video=%.1f fps -> %s\n", r2.pcm_fps,
              r2.video_fps, r2.ok ? "OK" : "FAIL");
  std::printf("  order3 voice across screen start/stop: pcm=%.1f/%.1f/%.1f fps "
              "(baseline/start/stop) session_alive=%d -> %s\n",
              r3.baseline_fps, r3.after_start_fps, r3.after_stop_fps,
              r3.session_alive_after_stop ? 1 : 0, r3.ok ? "OK" : "FAIL");
  if (!r1.ok || !r2.ok || !r3.ok) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
