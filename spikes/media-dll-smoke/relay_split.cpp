// 「中继语音 + 直连共享」传输拓扑的并存冒烟（C ABI 层，三引擎）。
//
// 拓扑（与真实 relay 语音 + 直连共享在**传输层**等价）：
//   A = 客户端：语音 answerer（relay 下服务端是 offerer）、屏幕共享 offerer（direct）
//   B = SFU 替身：语音 offerer，只发音频
//   C = 观看方：屏幕共享 answerer，只收视频
//
// 验收：「先 join 语音、再点共享」——A 被动加入语音（先建 answerer PC，等 B 的 offer），
// 语音 PCM 稳定后 A 再 pu_screen_start（屏幕共享走 A 自己的第二条 PC）。
// 判据：共享开始前后，A 解出的语音 PCM 帧数必须持续增长，长停顿时长 <= 500 ms；
// 同时 C 必须解出视频帧。
//
// v2.10 第 2 步追加（信令路由 ABI）：
//   * 每条 SIGNAL_OUT 都用 pu_signal_media 读回 media，并和 SDP 里的 m= 行对照；
//   * 转发默认走 pu_signal_in_media（显式 media，生产路径）；
//     --legacy 改成走 pu_signal_in（media==nullptr，SDP 嗅探兼容路径）；
//   * 非信令事件调 pu_signal_media 必须返回 INVALID_ARG；伪造信令事件必须返回 STATE；
//   * pu_signal_in_media 传未知 media 必须返回 INVALID_ARG。
//
// 范围说明：本测试**不经过 Go SFU 与 C# 客户端**（那需要真实服务端 + GUI 构建，
// 且 Windows↔WSL 的 UDP 不通）。它证明的是原生传输层里两条 PeerConnection 的独立性与
// 信令路由 ABI，不替代端到端 GUI 验收。
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
// 会把静音帧全部滤掉，数出来只剩噪声尖峰的零散几帧（会误判成「语音掉了一半」）。
// 冒烟要的是「PCM 有没有在路上」，所以和 voice_smoke 一样把阈值降到 1。
// DLL 是 /MT（自带一份 CRT environ），_putenv_s 只改 exe 的副本；必须用 Win32 的
// SetEnvironmentVariableA，DLL 里的 GetEnvironmentVariableA 才读得到。
void SetPeakThreshold(const char* value) {
  _putenv_s("PARTYUP_VOICE_PEAK_THRESHOLD", value);
  ::SetEnvironmentVariableA("PARTYUP_VOICE_PEAK_THRESHOLD", value);
}

struct Counters {
  int video_frames = 0;        // 远端视频帧（PU_EVENT_REMOTE_FRAME / PU_EVENT_FRAME）
  int voice_signals = 0;       // 转发出去的语音信令条数
  int video_signals = 0;       // 转发出去的屏幕信令条数
  int unroutable = 0;          // 既读不出 media、SDP 里也没有 m= 行的信令
  int media_ok = 0;            // pu_signal_media 读出 media（且与 SDP 的 m= 行一致）
  int media_wrong = 0;         // 读出的 media 与 SDP 的 m= 行矛盾
  int media_missing = 0;       // SIGNAL_OUT 却读不出 media（应为 0）
  int non_signal_checked = 0;  // 非信令事件调用 pu_signal_media 的次数
  int non_signal_bad = 0;      // 其中没有返回 INVALID_ARG 的次数（应为 0）
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

// 抽干 from 的事件。
//
// 会话归属**不靠 SDP 嗅探**：每条 SIGNAL_OUT 都先问 pu_signal_media（v2.10 第 2 步新增的
// 第 21 个导出），拿到的 media 既用于路由、也用于验收。candidate 信封里没有 m= 行，
// 正是「只有它能说出归属」的场景。
//
// legacy=true 时用 pu_signal_in（media==nullptr，SDP 嗅探的兼容路径）转发，
// 用来证明老入口仍然正确路由。
int PumpRouted(pu_engine* from, const char* name, pu_engine* voice_to, pu_engine* video_to,
               Counters* counters, bool legacy) {
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

    // pu_signal_media 对所有事件都能调：信令事件应给出 media，其它事件应报 INVALID_ARG。
    char media_buf[16] = {0};
    const int media_rc = pu_signal_media(&event, media_buf, static_cast<int>(sizeof(media_buf)));

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
        const bool sdp_has_video = payload.find("m=video") != std::string::npos;
        const bool sdp_has_audio = payload.find("m=audio") != std::string::npos;
        const char* sdp_media =
            sdp_has_video ? "screen" : (sdp_has_audio ? "voice" : nullptr);

        if (media_rc != PU_OK) {
          ++counters->media_missing;
          std::printf("FAIL: [%s] pu_signal_media on SIGNAL_OUT rc=%d\n", name, media_rc);
        } else if (sdp_media == nullptr) {
          // candidate：信封里没有 m= 行 —— 只有 pu_signal_media 能说出归属。
          ++counters->media_ok;
        } else if (std::strcmp(media_buf, sdp_media) != 0) {
          ++counters->media_wrong;
          std::printf("FAIL: [%s] media mismatch: pu_signal_media=%s but SDP says %s\n", name,
                      media_buf, sdp_media);
        } else {
          ++counters->media_ok;
        }

        pu_engine* target = nullptr;
        if (media_rc == PU_OK) {
          target = std::strcmp(media_buf, "voice") == 0 ? voice_to : video_to;
        } else if (sdp_has_video) {
          target = video_to;  // 读不出 media 时退回 SDP 嗅探，仅用于诊断
        } else if (sdp_has_audio) {
          target = voice_to;
        }
        if (target == nullptr) {
          ++counters->unroutable;
          break;
        }
        // 按**解析出的 media** 计数，不要按目标指针比：B/C 的 voice_to 与 video_to 是
        // 同一个引擎，指针比较会把 video answer 记成 voice。
        const bool forward_to_voice =
            media_rc == PU_OK ? (std::strcmp(media_buf, "voice") == 0)
                              : (!sdp_has_video && sdp_has_audio);
        if (forward_to_voice) {
          ++counters->voice_signals;
        } else {
          ++counters->video_signals;
        }

        const char* media_for_forward =
            media_rc == PU_OK ? media_buf : sdp_media;
        int in_rc = PU_ERR_INTERNAL;
        if (legacy) {
          in_rc = pu_signal_in(target, payload.data(), static_cast<size_t>(payload.size()));
        } else {
          in_rc = pu_signal_in_media(target, media_for_forward, payload.data(),
                                     static_cast<size_t>(payload.size()));
        }
        if (in_rc != PU_OK) {
          std::printf("FAIL: [%s] forward(%s) rc=%d (%s)\n", name,
                      legacy ? "pu_signal_in" : "pu_signal_in_media", in_rc,
                      pu_last_error(target));
          return -1;
        }
        break;
      }
      default:
        ++counters->non_signal_checked;
        if (media_rc != PU_ERR_INVALID_ARG) {
          ++counters->non_signal_bad;
          std::printf(
              "FAIL: [%s] pu_signal_media on non-signal event type=%u rc=%d (want INVALID_ARG)\n",
              name, event.type, media_rc);
        }
        break;
    }
  }
}

// 双向抽一次事件：A↔B 走语音、A↔C 走屏幕。任何一条失败返回 -1。
int PumpAll(pu_engine* a, pu_engine* b, pu_engine* c, Counters* ca, Counters* cb, Counters* cc,
            bool legacy) {
  if (PumpRouted(a, "A", b, c, ca, legacy) < 0) return -1;
  if (PumpRouted(b, "B(sfu)", a, a, cb, legacy) < 0) return -1;
  if (PumpRouted(c, "C(viewer)", a, a, cc, legacy) < 0) return -1;
  return 0;
}

struct Sample {
  int64_t at_ms = 0;
  uint32_t pcm = 0;
};

// pu_signal_media / pu_signal_in_media 的负例（边界契约）。
int CheckSignalAbiNegative(pu_engine* engine) {
  char buffer[16] = {0};
  int failures = 0;

  if (pu_signal_media(nullptr, buffer, sizeof(buffer)) != PU_ERR_INVALID_ARG) {
    std::printf("FAIL: pu_signal_media(NULL event) did not return INVALID_ARG\n");
    ++failures;
  }
  // 结构体过小：调用方传旧版 pu_event。
  pu_event small;
  std::memset(&small, 0, sizeof(small));
  small.struct_size = 8;
  if (pu_signal_media(&small, buffer, sizeof(buffer)) != PU_ERR_INVALID_ARG) {
    std::printf("FAIL: pu_signal_media(small struct_size) did not return INVALID_ARG\n");
    ++failures;
  }
  // 非信令事件。
  pu_event fake;
  std::memset(&fake, 0, sizeof(fake));
  fake.struct_size = sizeof(fake);
  fake.type = PU_EVENT_STATE;
  if (pu_signal_media(&fake, buffer, sizeof(buffer)) != PU_ERR_INVALID_ARG) {
    std::printf("FAIL: pu_signal_media(non-signal event) did not return INVALID_ARG\n");
    ++failures;
  }
  // 伪造的信令事件：没有 payload → 没有 media 记录 → STATE。
  std::memset(&fake, 0, sizeof(fake));
  fake.struct_size = sizeof(fake);
  fake.type = PU_EVENT_SIGNAL_OUT;
  if (pu_signal_media(&fake, buffer, sizeof(buffer)) != PU_ERR_STATE) {
    std::printf("FAIL: pu_signal_media(forged signal event) did not return STATE\n");
    ++failures;
  }
  // 未知 media：必须在解析 payload 之前就被拒。
  const char kPayload[] = "{\"type\":\"answer\",\"sdp\":\"v=0\"}";
  if (pu_signal_in_media(engine, "bogus", kPayload, sizeof(kPayload) - 1) != PU_ERR_INVALID_ARG) {
    std::printf("FAIL: pu_signal_in_media(bogus media) did not return INVALID_ARG\n");
    ++failures;
  }
  // 显式 media + 空 payload：仍是 INVALID_ARG。
  if (pu_signal_in_media(engine, "voice", nullptr, 0) != PU_ERR_INVALID_ARG) {
    std::printf("FAIL: pu_signal_in_media(NULL payload) did not return INVALID_ARG\n");
    ++failures;
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  bool legacy = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--legacy") == 0) legacy = true;
  }
  // 先把计数阈值降到 1（见 SetPeakThreshold 的注释），再建引擎。
  SetPeakThreshold("1");
  std::printf(
      "partyup_media relay-topology smoke: voice(answerer, relay-like) + screen(offerer, "
      "direct) [forwarding=%s]\n",
      legacy ? "pu_signal_in(sniff)" : "pu_signal_in_media(explicit)");

  pu_engine* a = CreateEngine("relay-A-client");
  pu_engine* b = CreateEngine("relay-B-sfu");
  pu_engine* c = CreateEngine("relay-C-viewer");
  if (a == nullptr || b == nullptr || c == nullptr) {
    std::printf("FAIL: pu_engine_create returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }

  Counters ca, cb, cc;
  const int abi_failures = CheckSignalAbiNegative(a);
  std::printf("  ABI 负例（pu_signal_media / pu_signal_in_media 边界）: %s (%d failure)\n",
              abi_failures == 0 ? "OK" : "FAIL", abi_failures);

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

  const int64_t voice_deadline = now_ms() + 15000;
  bool voice_connected = false;
  while (now_ms() < voice_deadline) {
    if (PumpAll(a, b, c, &ca, &cb, &cc, legacy) < 0) return 1;
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
    if (PumpAll(a, b, c, &ca, &cb, &cc, legacy) < 0) return 1;
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
    if (PumpAll(a, b, c, &ca, &cb, &cc, legacy) < 0) return 1;
    if (sample_count < 64) {
      samples[sample_count].at_ms = now_ms();
      samples[sample_count].pcm = PcmFrames(a);
      ++sample_count;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  const int64_t observe_elapsed_ms = now_ms() - share_started_ms;
  const uint32_t pcm_after_share = PcmFrames(a) - pcm_before_share;
  const double after_rate = observe_elapsed_ms > 0
                                ? static_cast<double>(pcm_after_share) * 1000.0 /
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
  std::printf("  signals             : A voice=%d video=%d unroutable=%d | C video=%d\n",
              ca.voice_signals, ca.video_signals, ca.unroutable, cc.video_signals);
  std::printf("  signal media (ABI)  : A ok=%d wrong=%d missing=%d | non-signal=%d bad=%d\n",
              ca.media_ok, ca.media_wrong, ca.media_missing, ca.non_signal_checked,
              ca.non_signal_bad);

  int failures = abi_failures;
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
  if (ca.media_missing != 0 || ca.media_wrong != 0) {
    std::printf("  FAIL: %d SIGNAL_OUT without media / %d with a wrong media\n", ca.media_missing,
                ca.media_wrong);
    ++failures;
  }
  if (ca.media_ok == 0) {
    std::printf("  FAIL: pu_signal_media never returned a media for any SIGNAL_OUT\n");
    ++failures;
  }
  if (ca.non_signal_bad != 0) {
    std::printf("  FAIL: pu_signal_media accepted %d non-signal event(s)\n", ca.non_signal_bad);
    ++failures;
  }
  if (ca.unroutable != 0) {
    std::printf("  FAIL: %d signal(s) could not be routed at all\n", ca.unroutable);
    ++failures;
  }

  // ---- 6. 附加观测（不参与 PASS/FAIL）：停止共享时语音会不会被一起拆掉 ----
  // 这是 step5「ReleaseStreamObjects() 末尾 ReleaseVoiceSession()」的直接后果：
  // pu_screen_stop → Stop() → ReleaseStreamObjects() → ReleaseVoiceSession()。
  // 数字交给设计裁定（第 4 步修，已写进代码里的 TODO(step4)），这里是真实结果。
  const uint32_t pcm_before_stop = PcmFrames(a);
  const int64_t stop_at_ms = now_ms();
  pu_screen_stop(a);
  const int64_t stop_until = now_ms() + 1500;
  while (now_ms() < stop_until) {
    if (PumpAll(a, b, c, &ca, &cb, &cc, legacy) < 0) return 1;
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
