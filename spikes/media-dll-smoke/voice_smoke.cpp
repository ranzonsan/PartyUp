// 语音冒烟（C ABI 层）：两个 engine 同进程，互相喂 SIGNAL_OUT，验证：
//   1. pu_voice_join(direct) 建轨 + 发 offer，双端能进 connected
//   2. B 端持续解出远端音频 PCM（pu_peer_stats.frames_received 在语音模式下 = PCM 帧数）
//   3. pu_voice_set_mute 后 B 端解出的 PCM 峰值掉到 0（引擎日志 "voice stats: ... peak=" 观察）
//   4. pu_voice_set_mode(RELAY) 返回 PU_NOT_IMPLEMENTED 且错误可读
//   5. pu_voice_leave 干净拆除
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include "partyup_media.h"

namespace {

int PumpEngine(pu_engine* from, const char* name, pu_engine* to) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(from, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: [%s] pu_poll_event rc=%d\n", name, rc);
      return -1;
    }
    if (event.type == PU_EVENT_SIGNAL_OUT && to != nullptr) {
      const int in_rc = pu_signal_in(to, event.data.signal.payload, event.data.signal.payload_len);
      if (in_rc != PU_OK) {
        std::printf("FAIL: [%s] pu_signal_in rc=%d (%s)\n", name, in_rc, pu_last_error(to));
        return -1;
      }
    }
  }
}

pu_engine* CreateEngine(const char* client_name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = client_name;
  config.log_level = 3;
  config.event_queue_capacity = 1024;
  return pu_engine_create(&config);
}

uint32_t ReceivedFrames(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return 0;
  return stats.frames_received;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media voice smoke (two engines, real ADM, 1:1 direct)\n");

  pu_engine* a = CreateEngine("engine-A-voice");
  pu_engine* b = CreateEngine("engine-B-voice");
  if (a == nullptr || b == nullptr) {
    std::printf("FAIL: pu_engine_create returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }

  // ---- 0. relay 必须明确拒绝 ----
  const int relay_rc = pu_voice_set_mode(a, PU_MODE_RELAY);
  if (relay_rc != PU_NOT_IMPLEMENTED) {
    std::printf("FAIL: pu_voice_set_mode(RELAY) rc=%d (want %d)\n", relay_rc, PU_NOT_IMPLEMENTED);
    return 1;
  }
  std::printf("  relay mode -> PU_NOT_IMPLEMENTED: \"%s\"\n", pu_last_error(a));

  // ---- 1. 两端 join（direct）----
  pu_voice_params params;
  std::memset(&params, 0, sizeof(params));
  params.struct_size = sizeof(params);
  params.channel_id = "smoke-channel";
  params.mode = PU_MODE_DIRECT;

  for (pu_engine* e : {a, b}) {
    // A 是 offerer；B 用 flags bit0 = 被动加入（只回 answer），否则双方都发 offer 会 glare。
    pu_voice_params join = params;
    join.flags = (e == b) ? 1u : 0u;
    const int rc = pu_voice_join(e, &join);
    if (rc != PU_OK) {
      std::printf("FAIL: pu_voice_join rc=%d (%s)\n", rc, pu_last_error(e));
      return 1;
    }
  }
  std::printf("  both engines joined (direct)\n");

  // ---- 2. 跑 10 秒：双向喂信令，每秒打一次 B 的 PCM 帧数 ----
  const auto start = std::chrono::steady_clock::now();
  uint32_t b_frames_before_mute = 0;
  bool muted = false;
  bool unmuted = false;
  for (int tick = 1; tick <= 10; ++tick) {
    const auto until = start + std::chrono::seconds(tick);
    while (std::chrono::steady_clock::now() < until) {
      if (PumpEngine(a, "A", b) < 0 || PumpEngine(b, "B", a) < 0) return 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const uint32_t frames = ReceivedFrames(b);
    std::printf("  t+%ds: B pcm frames=%u\n", tick, frames);
    if (tick == 4) {
      b_frames_before_mute = frames;
      if (pu_voice_set_mute(a, 1) != PU_OK) {
        std::printf("FAIL: pu_voice_set_mute(1) failed: %s\n", pu_last_error(a));
        return 1;
      }
      muted = true;
      std::printf("  t+4s: A muted (B's peak in logs should drop to 0)\n");
    }
    if (tick == 7) {
      if (pu_voice_set_mute(a, 0) != PU_OK) {
        std::printf("FAIL: pu_voice_set_mute(0) failed: %s\n", pu_last_error(a));
        return 1;
      }
      unmuted = true;
      std::printf("  t+7s: A unmuted\n");
    }
  }

  // ---- 2b. 离开再重新加入：计数不能翻倍（sink 必须摘干净）----
  for (pu_engine* e : {a, b}) pu_voice_leave(e);
  for (auto* e : {a, b}) {
    pu_voice_params join = params;
    join.flags = (e == b) ? 1u : 0u;
    if (pu_voice_join(e, &join) != PU_OK) {
      std::printf("FAIL: re-join rc=%d (%s)\n", pu_last_error(e));
      return 1;
    }
  }
  const auto rejoin_start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < rejoin_start + std::chrono::seconds(4)) {
    if (PumpEngine(a, "A", b) < 0 || PumpEngine(b, "B", a) < 0) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const uint32_t b_frames_rejoin = ReceivedFrames(b);
  const double rejoin_rate = static_cast<double>(b_frames_rejoin) / 4.0;
  std::printf("  re-join: B pcm frames=%u over 4 s (%.1f/s, want ~100 not ~200)\n",
              b_frames_rejoin, rejoin_rate);
  if (rejoin_rate > 150.0) {
    std::printf("FAIL: re-join doubled the count (%.1f/s) — sink was not detached\n", rejoin_rate);
    return 1;
  }
  const uint32_t b_frames_end = ReceivedFrames(b);

  // ---- 2c. 按轨查询（v2.7）：先问条数再取明细 ----
  {
    const int track_count = pu_voice_tracks(b, nullptr, 0);
    std::printf("  pu_voice_tracks(NULL) -> %d track(s)\n", track_count);
    if (track_count != 1) {
      std::printf("FAIL: expected 1 active remote audio track, got %d\n", track_count);
      return 1;
    }
    pu_voice_track item;
    std::memset(&item, 0, sizeof(item));
    item.struct_size = sizeof(item);
    const int got = pu_voice_tracks(b, &item, 1);
    std::printf("  pu_voice_tracks(1) -> %d: id=%.8s... pcmFrames=%u peak=%u last_frame_ms=%llu\n",
                got, item.id, item.pcm_frames, item.peak,
                static_cast<unsigned long long>(item.last_frame_ms));
    if (got != 1 || item.pcm_frames == 0 || item.last_frame_ms == 0) {
      std::printf("FAIL: per-track snapshot looks wrong (got=%d frames=%u last=%llu)\n", got,
                  item.pcm_frames, static_cast<unsigned long long>(item.last_frame_ms));
      return 1;
    }
  }

  // ---- 3. 拆除 ----
  for (pu_engine* e : {a, b}) {
    const int rc = pu_voice_leave(e);
    if (rc != PU_OK) {
      std::printf("FAIL: pu_voice_leave rc=%d (%s)\n", rc, pu_last_error(e));
      return 1;
    }
  }
  pu_engine_destroy(a);
  pu_engine_destroy(b);

  // ---- 4. 判据 ----
  std::printf("\n==== RESULT ====\n");
  std::printf("  B pcm frames: t+4s=%u t+10s=%u\n", b_frames_before_mute, b_frames_end);
  if (b_frames_end < 300) {
    std::printf("RESULT: FAIL (B decoded %u pcm frames, want >= 300 in 10 s)\n", b_frames_end);
    return 1;
  }
  if (!muted || !unmuted) {
    std::printf("RESULT: FAIL (mute/unmute sequence did not complete)\n");
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
