// 语音冒烟（C ABI 层）：两个 engine 同进程，互相喂 SIGNAL_OUT，验证：
//   1. pu_voice_join(direct) 建轨 + 发 offer，双端能进 connected
//   2. B 端持续解出远端音频 PCM（pu_peer_stats.frames_received 在语音模式下 = PCM 帧数）
//   3. pu_voice_set_mute 后 B 端解出的 PCM 峰值掉到 0（引擎日志 "voice stats: ... peak=" 观察）
//   4. pu_voice_set_mode(RELAY) 返回 PU_NOT_IMPLEMENTED 且错误可读
//   5. pu_voice_leave 干净拆除
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
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

// DLL 是 /MT（自带一份 CRT environ），_putenv_s 只改本 exe 的副本；
// 必须用 Win32 的 SetEnvironmentVariableA，DLL 里的 GetEnvironmentVariableA 才读得到。
void SetPeakThreshold(const char* value) {
  _putenv_s("PARTYUP_VOICE_PEAK_THRESHOLD", value);
  ::SetEnvironmentVariableA("PARTYUP_VOICE_PEAK_THRESHOLD", value);
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

  // ---- 0b. 峰值门控（v2.8）：静音输入不应计入 ----
  // relay 预协商 12 条槽，libwebrtc 对空槽也按 10ms 送静音帧、OnData 照常回调；
  // 所以「有回调」不等于「有人说话」，只有峰值过阈值才记帧。
  //
  // v2.10 起改成**自适应阈值**：以前硬编码 1000，但这台机器的环境噪声峰值实测能到 ~300、
  // 偶尔更高，于是「3 秒窗口里 0 帧」等于在赌这一刻没人在说话/敲键盘（实测连跑 4 次
  // 有 3 次在 3 秒里数到 1–13 帧）。现在分两步：
  //   1) 阈值 1 先采 2 秒，密集采样 pu_voice_tracks().peak 得到环境峰值 ambient_max；
  //   2) 门控阈值 = max(1000, ambient_max * 4)（上限 32000），再跑 3 秒必须 0 帧 0 轨。
  // 这样验证的是「低于阈值的帧被过滤」这条产品行为，而不是「1000 这个数字对本机合适」。
  // 若第 2 步仍被环境突发尖峰打断，只重试一次（阈值翻倍），不把环境噪声判成产品 bug。
  {
    auto join_pair = [&](const char* phase) {
      for (pu_engine* e : {a, b}) {
        pu_voice_params join = params;
        join.flags = (e == b) ? 1u : 0u;
        if (pu_voice_join(e, &join) != PU_OK) {
          std::printf("FAIL: join (%s) rc=%d (%s)\n", phase, pu_last_error(e));
          return false;
        }
      }
      return true;
    };
    auto leave_pair = [&]() {
      for (pu_engine* e : {a, b}) pu_voice_leave(e);
    };
    // 泵 engines `seconds` 秒，同时记录远端音轨 peak 的最大值（密集采样，5 ms 一次）。
    auto pump_and_track_peak = [&](int seconds, uint32_t* peak_out) {
      const auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() < start + std::chrono::seconds(seconds)) {
        if (PumpEngine(a, "A", b) < 0 || PumpEngine(b, "B", a) < 0) return false;
        pu_voice_track track;
        std::memset(&track, 0, sizeof(track));
        track.struct_size = sizeof(track);
        if (pu_voice_tracks(b, &track, 1) == 1 && track.peak > *peak_out) {
          *peak_out = track.peak;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return true;
    };

    // 1) 量环境峰值。
    SetPeakThreshold("1");
    if (!join_pair("ambient measure")) return 1;
    uint32_t ambient_max = 0;
    const bool measured = pump_and_track_peak(2, &ambient_max);
    leave_pair();
    if (!measured) return 1;

    // 2) 用 4 倍余量做门控，必须一帧都不计。
    uint32_t gate = ambient_max * 4;
    if (gate < 1000) gate = 1000;
    if (gate > 32000) gate = 32000;
    bool gating_ok = false;
    uint32_t last_frames = 0;
    int last_tracks = 0;
    for (int attempt = 1; attempt <= 2 && !gating_ok; ++attempt) {
      char gate_text[16];
      std::snprintf(gate_text, sizeof(gate_text), "%u", gate);
      SetPeakThreshold(gate_text);
      if (!join_pair("gating verify")) return 1;
      uint32_t gating_peak = 0;
      const bool pumped = pump_and_track_peak(3, &gating_peak);
      last_frames = ReceivedFrames(b);
      last_tracks = pu_voice_tracks(b, nullptr, 0);
      leave_pair();
      if (!pumped) return 1;
      std::printf(
          "  gating (threshold=%u = max(1000, ambient %u x4), attempt %d): "
          "frames=%u tracks=%d (want 0/0)\n",
          gate, ambient_max, attempt, last_frames, last_tracks);
      gating_ok = (last_frames == 0 && last_tracks == 0);
      if (!gating_ok) {
        std::printf("  WARN: gate %u was crossed (peak seen %u). Raising it and retrying once.\n",
                    gate, gating_peak);
        gate = (gate > 16000) ? 32000 : gate * 2;
      }
    }
    if (!gating_ok) {
      std::printf("FAIL: silent input was counted (frames=%u tracks=%d) — either peak gating "
                  "is not working, or the room was loud during both attempts\n",
                  last_frames, last_tracks);
      return 1;
    }
  }
  // 后续链路测试（97.5/s、重进、按轨查询）要把阈值降到 1 才在这台静音机器上数得到帧。
  SetPeakThreshold("1");

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
