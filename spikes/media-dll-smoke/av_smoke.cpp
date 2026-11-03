// 音视频交叉冒烟：验证「屏幕共享 + 语音」共存，两种顺序各跑一遍。
//   顺序 1：先 pu_screen_start → 再 pu_voice_join
//   顺序 2：先 pu_voice_join  → 再 pu_screen_start
// 判据：两种顺序下语音 PCM 帧率（~100/s）与视频帧数都不掉。
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

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

  r.ok = (r.pcm_fps >= 70.0) && (r.video_fps >= 15.0);
  return r;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media A/V coexistence smoke (screen + voice, two engines)\n");

  const Result r1 = RunOrder(/*screen_first=*/true, 1);
  const Result r2 = RunOrder(/*screen_first=*/false, 2);

  std::printf("\n==== RESULT ====\n");
  std::printf("  order1 screen->voice: pcm=%.1f fps video=%.1f fps -> %s\n", r1.pcm_fps,
              r1.video_fps, r1.ok ? "OK" : "FAIL");
  std::printf("  order2 voice->screen: pcm=%.1f fps video=%.1f fps -> %s\n", r2.pcm_fps,
              r2.video_fps, r2.ok ? "OK" : "FAIL");
  if (!r1.ok || !r2.ok) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
