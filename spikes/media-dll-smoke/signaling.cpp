// PartyUp 媒体核心 DLL · 双实例信令冒烟（ABI v2）
//
// 目的：证明「信令通道 + 真 PeerConnection」在**同一个进程里的两个 engine 实例**之间
// 跑得通——这正是 C# 侧要做的事：把服务端下发的 JSON 原样灌给 pu_signal_in，把
// PU_EVENT_SIGNAL_OUT 的 payload 原样发回服务端。本测试把这条链路短路成 A ↔ B 直连。
//
// 判据（缺一不可，全部满足才 exit=0）：
//   1. A、B 双方 pu_peer_stats 的 peer_state == PU_PEER_CONNECTED（ICE 连通 + DTLS 完成）；
//   2. B 的 frames_received > 0，且真的收到过 PU_EVENT_REMOTE_FRAME（远端帧数硬判据）；
//   3. 稳定窗口内「A 推帧增量 − B 收帧增量」= 丢帧数，可算出丢了多少帧；
//   3b. v2.1：同一窗口内 PC 的 outbound-rtp.framesEncoded 增量 ≈ A 的推帧增量
//      （1:1），并且不到 1.5 倍（2 倍就意味着同一帧被编码了两次）；
//   4. 全程没有 PU_EVENT_ERROR；
//   5. 负路径返回码正确：坏 JSON / 未知 type → PU_ERR_INVALID_ARG；共享中收到 offer
//      （glare）→ PU_ERR_STATE；结构体过小 → PU_ERR_INVALID_ARG。
//   6. 喂一条语法合法的多余候选给 B 必须 PU_OK（重复候选要幂等容错，不能把连接判死）。
//
// 任何一步失败都打印 pu_last_error 并以 exit=1 退出。
//
// 运行要求：必须和 partyup_media.dll 同目录，且从 Windows 本地盘运行（UNC 下原生
// DLL 搜索路径不可靠）。构建脚本 -Run 会自动复制到 %LOCALAPPDATA%\PartyUp\media-stage。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "partyup_media.h"

namespace {

constexpr int kRunSeconds = 20;       // 整体超时
constexpr int kMeasureWindowMs = 2000; // 丢帧统计窗口
constexpr int kMinReceivedFrames = 10; // 进入测量前至少收到这么多帧
// 编码计数按 250 ms 采样，一次比较涉及两个采样点，30 fps 下最多差 2×8 帧；
// 留到 16 帧的余量，避免把采样抖动当成断言失败。
constexpr int kWindowSampleLagFrames = 16;

const char* PeerStateName(int state) {
  switch (state) {
    case PU_PEER_NONE:
      return "none";
    case PU_PEER_NEW:
      return "new";
    case PU_PEER_CONNECTING:
      return "connecting";
    case PU_PEER_CONNECTED:
      return "connected";
    case PU_PEER_DISCONNECTED:
      return "disconnected";
    case PU_PEER_FAILED:
      return "failed";
    case PU_PEER_CLOSED:
      return "closed";
    default:
      return "unknown";
  }
}

const char* SignalKindName(uint32_t kind) {
  switch (kind) {
    case PU_SIGNAL_OFFER:
      return "offer";
    case PU_SIGNAL_ANSWER:
      return "answer";
    case PU_SIGNAL_CANDIDATE:
      return "candidate";
    default:
      return "unknown";
  }
}

// 打印信封的前 N 个字节：完整 SDP 有几 KB，日志里只留形状。
std::string Preview(const char* payload, uint32_t len, size_t limit = 160) {
  if (payload == nullptr) return "(null)";
  const size_t n = len < limit ? len : limit;
  std::string out(payload, n);
  for (char& c : out) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  if (len > limit) out += "...";
  return out;
}

// 一个引擎的观测计数。
struct Tally {
  int frames = 0;              // PU_EVENT_FRAME（本地编码完成）
  int remote_frames = 0;       // PU_EVENT_REMOTE_FRAME（解出的远端帧）
  int state_events = 0;
  int error_events = 0;
  int signals = 0;
  int offer_signals = 0;
  int answer_signals = 0;
  int candidate_signals = 0;
  int forwarded = 0;           // 成功喂给对端的信令条数
  int first_remote_width = 0;
  int first_remote_height = 0;
  int64_t last_remote_ts_us = 0;
};

// 把 from 的事件抽干。遇到 PU_EVENT_SIGNAL_OUT 就把 payload 原样交给 to —— 这就是
// C# 侧「收到事件 → 发 WebSocket」那一步的等价物。
int PumpEngine(pu_engine* from, const char* name, pu_engine* to, Tally* tally,
               bool verbose_signals) {
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
      case PU_EVENT_SIGNAL_OUT: {
        ++tally->signals;
        const pu_signal_event& signal = event.data.signal;
        switch (signal.kind) {
          case PU_SIGNAL_OFFER:
            ++tally->offer_signals;
            break;
          case PU_SIGNAL_ANSWER:
            ++tally->answer_signals;
            break;
          default:
            ++tally->candidate_signals;
            break;
        }
        if (verbose_signals) {
          std::printf("[%s] SIGNAL_OUT kind=%-9s len=%u  %s\n", name,
                      SignalKindName(signal.kind), signal.payload_len,
                      Preview(signal.payload, signal.payload_len).c_str());
        }
        if (to == nullptr) break;
        const int in_rc = pu_signal_in(to, signal.payload, signal.payload_len);
        if (in_rc != PU_OK) {
          std::printf("FAIL: [%s] pu_signal_in(%s) rc=%d (%s)\n", name,
                      to == nullptr ? "?" : "peer", in_rc, pu_last_error(to));
          return -1;
        }
        ++tally->forwarded;
        if (verbose_signals) {
          std::printf("[%s] -> pu_signal_in() rc=0 (fed to the other engine)\n", name);
        }
        break;
      }
      case PU_EVENT_REMOTE_FRAME: {
        const pu_remote_frame_event& remote = event.data.remote;
        if (tally->remote_frames == 0) {
          tally->first_remote_width = static_cast<int>(remote.width);
          tally->first_remote_height = static_cast<int>(remote.height);
          std::printf("[%s] first PU_EVENT_REMOTE_FRAME: %ux%u ts=%lld received_at=%lld\n",
                      name, remote.width, remote.height,
                      static_cast<long long>(remote.timestamp_us),
                      static_cast<long long>(remote.received_at_us));
        }
        tally->last_remote_ts_us = remote.timestamp_us;
        ++tally->remote_frames;
        break;
      }
      case PU_EVENT_FRAME:
        ++tally->frames;
        break;
      case PU_EVENT_STATE:
        ++tally->state_events;
        break;
      case PU_EVENT_ERROR:
        ++tally->error_events;
        std::printf("[%s] ERROR event code=%d message=%s\n", name, event.data.error.code,
                    event.data.error.message);
        break;
      default:
        std::printf("[%s] unexpected event type=%u\n", name, event.type);
        break;
    }
  }
}

pu_stats ReadStats(pu_engine* engine) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = static_cast<uint32_t>(sizeof(stats));
  if (pu_peer_stats(engine, &stats) != PU_OK) {
    std::printf("FAIL: pu_peer_stats failed (%s)\n", pu_last_error(engine));
  }
  return stats;
}

// -1 = 断言失败（已经打印原因）
int ExpectRc(pu_engine* engine, const char* what, const char* payload, int expected) {
  const int rc = pu_signal_in(engine, payload, std::strlen(payload));
  if (rc != expected) {
    std::printf("FAIL: %s -> rc=%d, expected %d (%s)\n", what, rc, expected,
                pu_last_error(engine));
    return -1;
  }
  std::printf("%-52s -> rc=%-3d OK\n", what, rc);
  return 0;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("partyup_media signaling smoke test (two engines, one process)\n");
  std::printf("  ABI version        : %d\n", PU_ABI_VERSION);
  std::printf("  sizeof(pu_event)   : %zu bytes\n", sizeof(pu_event));
  std::printf("  sizeof(pu_stats)   : %zu bytes\n", sizeof(pu_stats));
  std::printf("  sizeof(pu_signal_event) : %zu bytes\n", sizeof(pu_signal_event));

  pu_config config_a;
  std::memset(&config_a, 0, sizeof(config_a));
  config_a.struct_size = sizeof(config_a);
  config_a.client_name = "engine-A-sharer";
  config_a.log_level = 3;
  config_a.event_queue_capacity = 1024;

  pu_config config_b = config_a;
  config_b.client_name = "engine-B-viewer";

  const auto create_start = std::chrono::steady_clock::now();
  pu_engine* engine_a = pu_engine_create(&config_a);
  if (engine_a == nullptr) {
    std::printf("FAIL: pu_engine_create(A) returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }
  pu_engine* engine_b = pu_engine_create(&config_b);
  if (engine_b == nullptr) {
    std::printf("FAIL: pu_engine_create(B) returned null: %s\n", pu_last_error(nullptr));
    pu_engine_destroy(engine_a);
    return 1;
  }
  const auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - create_start)
                             .count();
  std::printf("two engines created in %lld ms\n", static_cast<long long>(create_ms));

  Tally tally_a;
  Tally tally_b;

  // ---- 1. A 开始共享：建 PC → AddTrack → CreateOffer → SetLocal → 等 ICE → SIGNAL_OUT ----
  const auto start_begin = std::chrono::steady_clock::now();
  const int start_rc = pu_screen_start(engine_a, "synthetic");
  if (start_rc != PU_OK) {
    std::printf("FAIL: pu_screen_start(A) rc=%d (%s)\n", start_rc, pu_last_error(engine_a));
    pu_engine_destroy(engine_a);
    pu_engine_destroy(engine_b);
    return 1;
  }
  std::printf("A: pu_screen_start rc=0 (offer will arrive as a SIGNAL_OUT event)\n");

  // ---- 2. 双向喂信令，直到 B 收够帧 ----
  // 这就是 C# 侧的主循环：poll → 把 SIGNAL_OUT 原样发出去 → 把收到的原样灌回来。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRunSeconds);
  auto connect_deadline = deadline;
  bool connected = false;
  int64_t connect_ms = 0;
  int iterations = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    ++iterations;
    if (PumpEngine(engine_a, "A", engine_b, &tally_a, true) < 0) {
      pu_engine_destroy(engine_a);
      pu_engine_destroy(engine_b);
      return 1;
    }
    if (PumpEngine(engine_b, "B", engine_a, &tally_b, true) < 0) {
      pu_engine_destroy(engine_a);
      pu_engine_destroy(engine_b);
      return 1;
    }

    const pu_stats stats_a = ReadStats(engine_a);
    const pu_stats stats_b = ReadStats(engine_b);
    if (!connected && stats_a.peer_state == PU_PEER_CONNECTED &&
        stats_b.peer_state == PU_PEER_CONNECTED) {
      connected = true;
      connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_begin)
                       .count();
      std::printf("BOTH CONNECTED (DTLS up) after %lld ms\n", static_cast<long long>(connect_ms));
      connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRunSeconds);
      std::printf("waiting for B to decode remote frames...\n");
    }
    if (connected && tally_b.remote_frames >= kMinReceivedFrames) break;

    if (stats_a.peer_state == PU_PEER_FAILED || stats_b.peer_state == PU_PEER_FAILED) {
      std::printf("FAIL: peer connection failed (A=%s B=%s)\n", PeerStateName(stats_a.peer_state),
                  PeerStateName(stats_b.peer_state));
      break;
    }
    if (std::chrono::steady_clock::now() > connect_deadline) {
      std::printf("FAIL: timed out (connected=%d, A.peer=%s B.peer=%s, B frames=%d)\n",
                  connected ? 1 : 0, PeerStateName(stats_a.peer_state),
                  PeerStateName(stats_b.peer_state), tally_b.remote_frames);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // ---- 3. 稳定窗口内统计丢帧 ----
  pu_stats before_a = ReadStats(engine_a);
  pu_stats before_b = ReadStats(engine_b);
  const auto window_start = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - window_start)
             .count() < kMeasureWindowMs) {
    if (PumpEngine(engine_a, "A", engine_b, &tally_a, false) < 0) break;
    if (PumpEngine(engine_b, "B", engine_a, &tally_b, false) < 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pu_stats after_a = ReadStats(engine_a);
  pu_stats after_b = ReadStats(engine_b);
  const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - window_start)
                             .count();

  const long long sent_delta = static_cast<long long>(after_a.frames_sent) - before_a.frames_sent;
  const long long recv_delta =
      static_cast<long long>(after_b.frames_received) - before_b.frames_received;
  const long long lost = sent_delta > recv_delta ? sent_delta - recv_delta : 0;
  // v2.1：A 的 PC 在同一窗口里编码了多少帧。这是「编码到底做了几次」的直接证据：
  // 1:1 时等于 sent_delta；2 倍就意味着同一帧被两个编码器各编码了一次。
  const long long encoded_delta = static_cast<long long>(after_a.frames_encoded) -
                                  static_cast<long long>(before_a.frames_encoded);

  // ---- 4. 负路径：信封/状态错误必须被区分开 ----
  int negative_failures = 0;
  negative_failures +=
      (ExpectRc(engine_b, "pu_signal_in(B, not-json) exp INVALID_ARG", "not json at all",
                PU_ERR_INVALID_ARG) < 0)
          ? 1
          : 0;
  negative_failures +=
      (ExpectRc(engine_b, "pu_signal_in(B, unknown type) exp INVALID_ARG",
                "{\"type\":\"what-is-this\",\"x\":1}", PU_ERR_INVALID_ARG) < 0)
          ? 1
          : 0;
  negative_failures +=
      (ExpectRc(engine_b, "pu_signal_in(B, empty sdp) exp INVALID_ARG",
                "{\"type\":\"offer\",\"sdp\":\"\"}", PU_ERR_INVALID_ARG) < 0)
          ? 1
          : 0;
  // 语法合法、但指向一个不存在的远端：AddIceCandidate 幂等容错，必须 PU_OK。
  negative_failures +=
      (ExpectRc(engine_b,
                "pu_signal_in(B, extra bogus candidate) exp OK",
                "{\"type\":\"candidate\",\"mid\":\"0\",\"mline\":0,\"candidate\":"
                "\"candidate:1 1 UDP 2130706431 192.0.2.1 65000 typ host\"}",
                PU_OK) < 0)
          ? 1
          : 0;
  // A 正在共享（offerer）却又收到 offer = glare，v2 明确拒绝。
  negative_failures +=
      (ExpectRc(engine_a, "pu_signal_in(A, offer while sharing) exp STATE",
                "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 1 IN IP4 127.0.0.1\\r\\n\"}",
                PU_ERR_STATE) < 0)
          ? 1
          : 0;
  {
    pu_stats too_small;
    std::memset(&too_small, 0, sizeof(too_small));
    too_small.struct_size = 8;
    const int rc = pu_peer_stats(engine_a, &too_small);
    if (rc != PU_ERR_INVALID_ARG) {
      std::printf("FAIL: pu_peer_stats(undersized) rc=%d, expected PU_ERR_INVALID_ARG\n", rc);
      ++negative_failures;
    } else {
      std::printf("%-52s -> rc=%-3d OK\n", "pu_peer_stats(undersized) exp INVALID_ARG", rc);
    }
  }

  // ---- 5. 收尾 ----
  const int stop_a = pu_screen_stop(engine_a);
  const int stop_b = pu_screen_stop(engine_b);
  const int stop_a2 = pu_screen_stop(engine_a);  // 幂等
  pu_engine_destroy(engine_a);
  pu_engine_destroy(engine_b);
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - create_start)
                            .count();

  // ---- 6. 汇总 ----
  std::printf("\n=== summary ===\n");
  std::printf("  create + connect time : %lld ms (connected=%d)\n",
              static_cast<long long>(connect_ms), connected ? 1 : 0);
  std::printf("  event loop iterations : %d\n", iterations);
  std::printf("  A: peer=%s ice=%d frames_sent=%u frames_encoded=%u signals=%d (offer=%d)\n",
              PeerStateName(after_a.peer_state), after_a.ice_state, after_a.frames_sent,
              after_a.frames_encoded, tally_a.signals, tally_a.offer_signals);
  std::printf("  B: peer=%s ice=%d frames_received=%u signals=%d (answer=%d)\n",
              PeerStateName(after_b.peer_state), after_b.ice_state, after_b.frames_received,
              tally_b.signals, tally_b.answer_signals);
  std::printf("  B: remote frame events = %d (first %dx%d)\n", tally_b.remote_frames,
              tally_b.first_remote_width, tally_b.first_remote_height);
  std::printf("  feed: A->B %d message(s), B->A %d message(s)\n", tally_a.forwarded,
              tally_b.forwarded);
  std::printf("  loss window           : %lld ms\n", static_cast<long long>(window_ms));
  std::printf("  sent delta            : %lld frames (A)\n", sent_delta);
  std::printf("  encoded delta         : %lld frames (A, PC outbound-rtp.framesEncoded)\n",
              encoded_delta);
  std::printf("  received delta        : %lld frames (B)\n", recv_delta);
  std::printf("  LOST in window        : %lld frame(s) (%.2f%%)\n", lost,
              sent_delta > 0 ? 100.0 * static_cast<double>(lost) / static_cast<double>(sent_delta)
                             : 0.0);
  {
    // 会话级口径：A 从 start 就在推帧，但 DTLS 建起来之前推的帧没有任何对端能收到，
    // 这批帧在「A 推了多少」和「B 收了多少」之间就会体现为丢失。
    const long long session_lost =
        static_cast<long long>(after_a.frames_sent) - static_cast<long long>(after_b.frames_received);
    std::printf("  LOST in session       : %lld frame(s) (A pushed %u, B decoded %u)\n",
                session_lost > 0 ? session_lost : 0, after_a.frames_sent, after_b.frames_received);
  }
  std::printf("  error events          : A=%d B=%d\n", tally_a.error_events, tally_b.error_events);
  std::printf("  stop rc               : A=%d B=%d (second stop A=%d)\n", stop_a, stop_b, stop_a2);
  std::printf("  event queue drops     : A=%u B=%u\n", after_a.event_drops, after_b.event_drops);
  std::printf("  total elapsed         : %lld ms\n", static_cast<long long>(total_ms));

  int failures = 0;
  if (!connected) {
    std::printf("FAIL: engines never reached DTLS connected\n");
    ++failures;
  }
  if (tally_b.remote_frames <= 0) {
    std::printf("FAIL: B decoded no remote frame (PU_EVENT_REMOTE_FRAME count = 0)\n");
    ++failures;
  }
  if (after_b.frames_received <= 0) {
    std::printf("FAIL: pu_peer_stats(B).frames_received = 0\n");
    ++failures;
  }
  if (tally_a.error_events != 0 || tally_b.error_events != 0) {
    std::printf("FAIL: error events were emitted\n");
    ++failures;
  }
  // v2.1：每帧只编码一次。判定沿用「稳定窗口」口径——只有连上之后 A 才真的在编码。
  if (sent_delta > 0 && encoded_delta * 2 >= sent_delta * 3) {
    std::printf("FAIL: encoding looks doubled in the window (%lld encoded for %lld pushed)\n",
                encoded_delta, sent_delta);
    ++failures;
  }
  if (encoded_delta + kWindowSampleLagFrames < sent_delta) {
    std::printf("FAIL: PC encoded only %lld of the %lld frame(s) pushed in the window\n",
                encoded_delta, sent_delta);
    ++failures;
  }
  if (negative_failures != 0) {
    std::printf("FAIL: %d negative-path assertion(s) failed\n", negative_failures);
    ++failures;
  }
  if (stop_a != PU_OK || stop_b != PU_OK || stop_a2 != PU_OK) {
    std::printf("FAIL: stop returned non-zero\n");
    ++failures;
  }
  if (failures != 0) {
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
  }

  std::printf(
      "PASS: DTLS connected on both sides, B decoded %u frame(s), lost %lld frame(s) in the "
      "%lld ms window\n",
      after_b.frames_received, lost, static_cast<long long>(window_ms));
  return 0;
}
