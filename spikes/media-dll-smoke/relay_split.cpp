// 「中继语音 + 直连共享」传输拓扑的并存冒烟（C ABI 层，三引擎）。
//
// 拓扑（与真实 relay 语音 + 直连共享在**传输层**等价）：
//   A = 客户端：语音 answerer（relay 下服务端是 offerer）、屏幕共享 offerer（direct）
//   B = SFU 替身：语音 offerer，只发音频
//   C = 观看方：屏幕共享 answerer，只收视频
//
// 验收：「先 join 语音、再点共享」——A 被动加入语音（先建 answerer PC，等 B 的 offer），
// 语音 PCM 稳定后 A 再 pu_screen_start（屏幕共享走 A 自己的第二条 PC）。
// 判据：共享开始前后，A 解出的语音 PCM 帧数必须持续以 ~100/s（每帧 10 ms）增长，
// 长停顿时长 <= 500 ms；同时 C 必须解出视频帧。
//
// 为什么要三引擎：第 1 步之前语音/屏幕共用一个 pc_，A 同时是语音 answerer 和共享
// offerer，点共享就会把语音会话的 signaling state 拖坏（relay 下实测 role mismatch、
// PCM 冻结）。只有「语音对面是 offerer、共享对面是 answerer」这个拓扑能复现它。
//
// 范围说明：本测试**不经过 Go SFU 与 C# 客户端**——那需要 pu_signal_in_media（ABI v2
// 第 20 个导出），而第 1 步的导出预算固定 19。它证明的是原生传输层里两条 PeerConnection
// 的独立性，不替代端到端 GUI 验收。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <windows.h>

#include "partyup_media.h"

namespace {

// 计数器口径：这台机器的麦克风是静音的（peak≈1），AudioCountSink 默认按峰值阈值门控，
// 会把静音帧全部滤掉，数出来只剩噪声尖峰的零散几帧（实测 50/s，会误判成「语音掉了一半」）。
// 冒烟要的是「PCM 有没有在路上」，所以和 voice_smoke 一样把阈值降到 1。
// DLL 是 /MT（自带一份 CRT environ），_putenv_s 只改 exe 的副本；必须用 Win32 的
// SetEnvironmentVariableA，DLL 里的 GetEnvironmentVariableA 才读得到。
void SetPeakThreshold(const char* value) {
  _putenv_s("PARTYUP_VOICE_PEAK_THRESHOLD", value);
  ::SetEnvironmentVariableA("PARTYUP_VOICE_PEAK_THRESHOLD", value);
}

struct Counters {
  int video_frames = 0;  // 远端视频帧（PU_EVENT_REMOTE_FRAME / PU_EVENT_FRAME）
  int voice_signals = 0;
  int video_signals = 0;
  int stray_signals = 0;  // 没有 m= 行的信令（candidate 之类）；本设计里应恒为 0
};

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 3;
  config.event_queue_capacity = 2048;
  return pu_engine_create(&config);
}

uint32_t PcmFrames(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return 0;
  return stats.frames_received;  // 语音模式下 = 远端解出的 PCM 帧数
}

int PeerState(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(engine, &stats) != PU_OK) return PU_PEER_NONE;
  return stats.peer_state;
}

std::string Payload(const pu_event& event) {
  if (event.data.signal.payload == nullptr || event.data.signal.payload_len == 0) return {};
  return std::string(event.data.signal.payload, event.data.signal.payload_len);
}

// 抽干 from 的事件。SIGNAL_OUT 按 SDP 内容路由：
//   含 m=video             → video_to（屏幕会话）
//   含 m=audio 且无 m=video → voice_to（语音会话，SDP 里只有 audio）
//   两者都没有（candidate）→ 只计数不转发：本设计里候选随 SDP 一起发，正常恒为 0；
//                            真出现了说明 trickle 开始生效，需要显式 media 才能正确路由。
int PumpRouted(pu_engine* from, const char* name, pu_engine* voice_to, pu_engine* video_to,
               Counters* counters) {
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
      case PU_EVENT_FRAME:
        ++counters->video_frames;
        break;
      case PU_EVENT_ERROR:
        std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", name,
                    event.data.error.code, event.data.error.message);
        return -1;
      case PU_EVENT_SIGNAL_OUT: {
        const std::string payload = Payload(event);
        const bool has_video = payload.find("m=video") != std::string::npos;
        const bool has_audio = payload.find("m=audio") != std::string::npos;
        pu_engine* target = nullptr;
        if (has_video) {
          target = video_to;
          ++counters->video_signals;
        } else if (has_audio) {
          target = voice_to;
          ++counters->voice_signals;
        } else {
          ++counters->stray_signals;
        }
        if (target == nullptr) break;
        const int in_rc =
            pu_signal_in(target, event.data.signal.payload, event.data.signal.payload_len);
        if (in_rc != PU_OK) {
          std::printf("FAIL: [%s] pu_signal_in rc=%d (%s)\n", name, in_rc, pu_last_error(target));
          return -1;
        }
        break;
      }
      default:
        break;
    }
  }
}

// 双向抽一次事件：A↔B 走语音、A↔C 走屏幕。任何一条失败返回 -1。
int PumpAll(pu_engine* a, pu_engine* b, pu_engine* c, Counters* ca, Counters* cb,
            Counters* cc) {
  if (PumpRouted(a, "A", b, c, ca) < 0) return -1;
  if (PumpRouted(b, "B(sfu)", a, a, cb) < 0) return -1;
  if (PumpRouted(c, "C(viewer)", a, a, cc) < 0) return -1;
  return 0;
}

struct Sample {
  int64_t at_ms = 0;
  uint32_t pcm = 0;
};

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // 先把计数阈值降到 1（见 SetPeakThreshold 的注释），再建引擎。
  SetPeakThreshold("1");
  std::printf(
      "partyup_media relay-topology smoke: voice(answerer, relay-like) + screen(offerer, "
      "direct)\n");

  pu_engine* a = CreateEngine("relay-A-client");
  pu_engine* b = CreateEngine("relay-B-sfu");
  pu_engine* c = CreateEngine("relay-C-viewer");
  if (a == nullptr || b == nullptr || c == nullptr) {
    std::printf("FAIL: pu_engine_create returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }

  const int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  auto now_ms = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() -
           t0;
  };

  // ---- 1. 先 join 语音：A 被动（relay 里客户端永远是 answerer）----
  // 真实客户端在 relay 下也是 mode=1（direct）——对媒体核心来说「对面只有一个 peer」。
  pu_voice_params vp;
  std::memset(&vp, 0, sizeof(vp));
  vp.struct_size = sizeof(vp);
  vp.channel_id = "relay-split";
  vp.mode = PU_MODE_DIRECT;

  pu_voice_params passive = vp;
  passive.flags = 1u;
  if (pu_voice_join(a, &passive) != PU_OK) {
    std::printf("FAIL: pu_voice_join(A passive): %s\n", pu_last_error(a));
    return 1;
  }
  std::printf("  A: voice joined passive (answerer; waits for the SFU-side offer)\n");
  if (pu_voice_join(b, &vp) != PU_OK) {
    std::printf("FAIL: pu_voice_join(B): %s\n", pu_last_error(b));
    return 1;
  }
  std::printf("  B: voice joined active (offerer — stands in for the SFU)\n");

  Counters ca, cb, cc;
  const int64_t voice_deadline = now_ms() + 15000;
  bool voice_connected = false;
  while (now_ms() < voice_deadline) {
    if (PumpAll(a, b, c, &ca, &cb, &cc) < 0) return 1;
    // 此时还没有屏幕 PC，pu_peer_stats 报的就是语音会话的状态。
    if (PeerState(a) == PU_PEER_CONNECTED && PeerState(b) == PU_PEER_CONNECTED) {
      voice_connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!voice_connected) {
    std::printf("FAIL: voice (A<->B) not connected in 15 s (A=%d B=%d)\n", PeerState(a),
                PeerState(b));
    return 1;
  }
  std::printf("  voice connected at t+%lld ms (A voice signals=%d, B signals=%d)\n",
              static_cast<long long>(now_ms()), ca.voice_signals, cb.voice_signals);

  // ---- 2. 语音单独跑 2 s：基线 PCM 帧率（每帧 10 ms，应 ~100/s）----
  const uint32_t voice_only_start = PcmFrames(a);
  const int64_t baseline_until = now_ms() + 2000;
  while (now_ms() < baseline_until) {
    if (PumpAll(a, b, c, &ca, &cb, &cc) < 0) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const uint32_t voice_only_frames = PcmFrames(a) - voice_only_start;
  const double voice_only_rate = static_cast<double>(voice_only_frames) / 2.0;
  std::printf("  voice-only  : %u pcm frames in 2.00 s = %.1f/s\n", voice_only_frames,
              voice_only_rate);

  // ---- 3. 点共享：A 起屏幕共享（走第二条 PC，offer 发给 C）----
  const uint32_t pcm_before_share = PcmFrames(a);
  const int64_t share_started_ms = now_ms();
  if (pu_screen_start(a, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(A): %s\n", pu_last_error(a));
    return 1;
  }
  std::printf("  A: screen share started at t+%lld ms (second PeerConnection, offerer)\n",
              static_cast<long long>(share_started_ms));

  // ---- 4. 边共享边采 PCM：每 250 ms 一次，持续 8 s ----
  Sample samples[64];
  int sample_count = 0;
  const int64_t observe_until = now_ms() + 8000;
  while (now_ms() < observe_until) {
    if (PumpAll(a, b, c, &ca, &cb, &cc) < 0) return 1;
    if (sample_count < 64) {
      samples[sample_count].at_ms = now_ms();
      samples[sample_count].pcm = PcmFrames(a);
      ++sample_count;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  const int64_t observe_elapsed_ms = now_ms() - share_started_ms;
  const uint32_t pcm_after_share = PcmFrames(a) - pcm_before_share;
  const double after_rate =
      observe_elapsed_ms > 0 ? static_cast<double>(pcm_after_share) * 1000.0 /
                                   static_cast<double>(observe_elapsed_ms)
                             : 0.0;

  // ---- 5. 判据 ----
  // 5a. 语音 PCM 不得中断：逐 250 ms 采样看「有没有新帧」，统计连续无帧的最长时长。
  int64_t stall_ms = 0;
  int64_t longest_stall_ms = 0;
  int64_t longest_stall_at_ms = 0;
  int empty_windows = 0;
  uint32_t min_window_delta = 0xFFFFFFFFu;
  for (int i = 1; i < sample_count; ++i) {
    const uint32_t delta = samples[i].pcm - samples[i - 1].pcm;
    const int64_t dt = samples[i].at_ms - samples[i - 1].at_ms;
    if (delta < min_window_delta) min_window_delta = delta;
    if (delta == 0) {
      if (stall_ms == 0) longest_stall_at_ms = samples[i - 1].at_ms;
      stall_ms += dt;
      if (stall_ms > longest_stall_ms) {
        longest_stall_ms = stall_ms;
        longest_stall_at_ms = samples[i - 1].at_ms - (stall_ms - dt);
      }
      ++empty_windows;
    } else {
      stall_ms = 0;
    }
  }

  std::printf("\n==== RESULT ====\n");
  std::printf("  voice-only rate     : %.1f pcm frames/s (want ~100)\n", voice_only_rate);
  std::printf("  after share started : %u pcm frames in %.2f s = %.1f/s (want ~100)\n",
              pcm_after_share, static_cast<double>(observe_elapsed_ms) / 1000.0, after_rate);
  std::printf("  per-250ms windows   : %d sampled, %d empty, min delta=%u\n", sample_count - 1,
              empty_windows, min_window_delta);
  std::printf("  longest PCM stall   : %lld ms (at t+%lld ms; want <= 500)\n",
              static_cast<long long>(longest_stall_ms),
              static_cast<long long>(longest_stall_at_ms));
  std::printf("  viewer video frames : %d (want >= 30)\n", cc.video_frames);
  std::printf("  signals             : A voice=%d video=%d stray=%d | C video=%d\n",
              ca.voice_signals, ca.video_signals, ca.stray_signals, cc.video_signals);

  int failures = 0;
  // 绝对 PCM 帧率受本机麦克风噪声影响：阈值 1 时只有 peak>1 的帧才计数（见 av_smoke 的
  // 同一条注释），所以这里和 av_smoke 一样用 >= 30/s 作为「链路还活着」的下限；
  // 「不中断」由 0 个空窗口 + 最长停顿 <= 500 ms 来判定，那才是本步要证明的东西。
  if (after_rate < 30.0) {
    std::printf("  FAIL: voice PCM rate %.1f/s after starting the share — voice link is dead\n",
                after_rate);
    ++failures;
  }
  if (longest_stall_ms > 500) {
    std::printf("  FAIL: voice PCM stalled for %lld ms after the share started\n",
                static_cast<long long>(longest_stall_ms));
    ++failures;
  }
  if (cc.video_frames < 30) {
    std::printf("  FAIL: viewer decoded only %d video frame(s)\n", cc.video_frames);
    ++failures;
  }
  if (ca.stray_signals != 0) {
    std::printf("  WARN: %d signal(s) had no m= line (candidate trickle); see TODO(step2)\n",
                ca.stray_signals);
  }

  // ---- 6. 附加观测（不参与 PASS/FAIL）：停止共享时语音会不会被一起拆掉 ----
  // 这是 step5「ReleaseStreamObjects() 末尾 ReleaseVoiceSession()」的直接后果：
  // pu_screen_stop → Stop() → ReleaseStreamObjects() → ReleaseVoiceSession()。
  // 数字交给设计裁定（第 2/4 步是否要把语音收尾移出屏幕停止路径），这里是真实结果。
  const uint32_t pcm_before_stop = PcmFrames(a);
  const int64_t stop_at_ms = now_ms();
  pu_screen_stop(a);
  const int64_t stop_until = now_ms() + 1500;
  while (now_ms() < stop_until) {
    if (PumpAll(a, b, c, &ca, &cb, &cc) < 0) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const uint32_t pcm_after_stop = PcmFrames(a);
  // 注意：这个计数是「当前挂着的远端音轨的 PCM 帧数和」。语音会话被拆掉时
  // DetachAllAudioSinks 会把计数清零，所以这里不能用差值，要看绝对值变化。
  std::printf("  [obs] screen stop at t+%lld ms while voice was up: pcm counter %u -> %u\n",
              static_cast<long long>(stop_at_ms), pcm_before_stop, pcm_after_stop);
  if (pcm_after_stop < pcm_before_stop) {
    std::printf(
        "  [obs] ^ COUNTER RESET: pu_screen_stop tore the voice session down "
        "(ReleaseStreamObjects -> ReleaseVoiceSession)\n");
  } else {
    std::printf("  [obs] ^ voice session survived the screen stop\n");
  }

  pu_voice_leave(a);
  pu_voice_leave(b);
  pu_engine_destroy(a);
  pu_engine_destroy(b);
  pu_engine_destroy(c);

  if (failures != 0) {
    std::printf("RESULT: FAIL (%d check(s) failed)\n", failures);
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
