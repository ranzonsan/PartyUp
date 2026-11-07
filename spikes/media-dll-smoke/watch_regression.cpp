// PartyUp 媒体核心 · 显式单流观看的合成回归（v2.14）。
//
// 目的：在**不采全桌面、不打开麦克风**的前提下，用两个同进程引擎把
// pu_screen_watch_ex / pu_screen_unwatch / pu_frame_acquire 的真实代码路径跑一遍：
//   * B 在收到任何远端轨之前先 arm（watch-before-track），OnTrack 到达后必须挂 sink；
//   * B 的 track 已存在时再 arm（track-before-watch），Rebind 必须重新挂 sink；
//   * pu_frame_acquire("remote") 在 armed 时必须按「当前选台」交付（旧实现会误拒）；
//   * 显式目标 stream_id 与当前 target 一致时可用、不一致时返回 INVALID_ARG；
//   * unwatch 后不再交付帧（无 REMOTE_FRAME 事件、acquire 返回 PU_ERR_TIMEOUT）；
//   * arm 到错误槽位（track id 不匹配）不得挂 sink；
//   * 从未调用 watch/unwatch 的 legacy 引擎仍然自动收帧（v1 兼容）。
//
// 画面只来自 pu_screen_start("synthetic")，不枚举/不采集真实显示器，不会弹屏幕采集。
// 必须与 partyup_media.dll 同目录、从 Windows 本地盘运行（UNC 下 DLL 搜索不可靠）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "partyup_media.h"

namespace {

constexpr int kConnectTimeoutMs = 20000;
constexpr int kAcquireTimeoutMs = 5000;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 2;
  config.event_queue_capacity = 2048;
  return pu_engine_create(&config);
}

int PeerState(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = static_cast<uint32_t>(sizeof(stats));
  if (pu_peer_stats(e, &stats) != PU_OK) return PU_PEER_NONE;
  return stats.peer_state;
}

uint32_t FramesReceived(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = static_cast<uint32_t>(sizeof(stats));
  if (pu_peer_stats(e, &stats) != PU_OK) return 0;
  return stats.frames_received;
}

struct Stats {
  int signals = 0;
  int remote_events = 0;
  int errors = 0;
};

// 抽干 from 的事件；信号原样喂给 peer（与 C# 的 poll → WebSocket → pu_signal_in 等价）。
int Pump(pu_engine* from, const char* name, pu_engine* peer, Stats* stats) {
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
    if (event.type == PU_EVENT_SIGNAL_OUT) {
      ++stats->signals;
      if (peer != nullptr) {
        const pu_signal_event& signal = event.data.signal;
        const int in_rc = pu_signal_in(peer, signal.payload, signal.payload_len);
        if (in_rc != PU_OK) {
          std::printf("FAIL: [%s] pu_signal_in rc=%d (%s)\n", name, in_rc, pu_last_error(peer));
          return -1;
        }
      }
    } else if (event.type == PU_EVENT_REMOTE_FRAME) {
      ++stats->remote_events;
    } else if (event.type == PU_EVENT_ERROR) {
      ++stats->errors;
      std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", name,
                  event.data.error.code, event.data.error.message);
      return -1;
    }
  }
}

int PumpBoth(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn, Stats* sb) {
  if (Pump(a, an, b, sa) < 0) return -1;
  if (Pump(b, bn, a, sb) < 0) return -1;
  return 0;
}

// 轮询泵直到 pred 为真；超时返回 -1。
int WaitFor(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn, Stats* sb,
            const char* what, bool (*pred)(pu_engine*, pu_engine*)) {
  const int64_t deadline = NowMs() + kConnectTimeoutMs;
  while (NowMs() < deadline) {
    if (PumpBoth(a, an, sa, b, bn, sb) < 0) return -1;
    if (pred(a, b)) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::printf("FAIL: timed out waiting for %s (A state=%d frames=%u, B state=%d frames=%u)\n",
              what, PeerState(a), FramesReceived(a), PeerState(b), FramesReceived(b));
  return -1;
}

bool BothConnected(pu_engine* a, pu_engine* b) {
  return PeerState(a) == PU_PEER_CONNECTED && PeerState(b) == PU_PEER_CONNECTED;
}

bool BFramesAtLeast5(pu_engine*, pu_engine* b) { return FramesReceived(b) >= 5; }

int AcquireUntil(pu_engine* e, const char* stream, int want, const char* what) {
  int got = 0;
  const int64_t deadline = NowMs() + kAcquireTimeoutMs;
  while (got < want && NowMs() < deadline) {
    pu_frame frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.struct_size = static_cast<uint32_t>(sizeof(frame));
    const int rc = pu_frame_acquire(e, stream, &frame);
    if (rc == PU_OK) {
      if (frame.pixels == nullptr || frame.width == 0 || frame.height == 0 ||
          frame.stride < frame.width * 4u) {
        std::printf("FAIL: %s acquired an invalid frame (%ux%u stride=%u pixels=%p)\n", what,
                    frame.width, frame.height, frame.stride, frame.pixels);
        pu_frame_release(e, &frame);
        return -1;
      }
      ++got;
      pu_frame_release(e, &frame);
    } else if (rc == PU_ERR_TIMEOUT) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } else {
      std::printf("FAIL: %s pu_frame_acquire(%-8s) rc=%d (%s)\n", what, stream, rc,
                  pu_last_error(e));
      return -1;
    }
  }
  if (got < want) {
    std::printf("FAIL: %s acquired %d/%d frame(s) within %d ms\n", what, got, want,
                kAcquireTimeoutMs);
    return -1;
  }
  std::printf("  %-46s acquired %d frame(s) via %-8s\n", what, got, stream);
  return 0;
}

int ExpectFrameRc(pu_engine* e, const char* stream, int expected, const char* what) {
  pu_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.struct_size = static_cast<uint32_t>(sizeof(frame));
  const int rc = pu_frame_acquire(e, stream, &frame);
  if (rc == PU_OK) pu_frame_release(e, &frame);
  if (rc != expected) {
    std::printf("FAIL: %s: pu_frame_acquire(%s) rc=%d, expected %d (%s)\n", what, stream, rc,
                expected, pu_last_error(e));
    return -1;
  }
  std::printf("  %-46s rc=%d (as expected)\n", what, rc);
  return 0;
}

int WaitNoRemoteFrames(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn,
                       Stats* sb, int window_ms, const char* what) {
  if (PumpBoth(a, an, sa, b, bn, sb) < 0) return -1;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  if (PumpBoth(a, an, sa, b, bn, sb) < 0) return -1;
  sb->remote_events = 0;
  const int64_t deadline = NowMs() + window_ms;
  while (NowMs() < deadline) {
    if (PumpBoth(a, an, sa, b, bn, sb) < 0) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (sb->remote_events != 0) {
    std::printf("FAIL: %s: B still delivered %d REMOTE_FRAME event(s) after unwatch\n", what,
                sb->remote_events);
    return -1;
  }
  std::printf("  %-46s no REMOTE_FRAME in %d ms\n", what, window_ms);
  return 0;
}

// 显式观看：B 先进入显式模式，并在收到任何远端轨之前 arm。
int RunExplicitWatch() {
  std::printf("\n== explicit watch regression (synthetic, two engines)\n");
  int failures = 0;

  pu_engine* a = CreateEngine("watch-sharer");
  pu_engine* b = CreateEngine("watch-viewer");
  if (a == nullptr || b == nullptr) {
    std::printf("FAIL: CreateEngine returned null: %s\n", pu_last_error(nullptr));
    if (a != nullptr) pu_engine_destroy(a);
    if (b != nullptr) pu_engine_destroy(b);
    return 1;
  }
  Stats sa;
  Stats sb;

  // 1) 建任何屏幕 PC 之前先切显式模式，然后 arm 一个目标（watch-before-track）。
  if (pu_screen_unwatch(b, nullptr) != PU_OK) {
    std::printf("FAIL: pu_screen_unwatch(B, nullptr) = %s\n", pu_last_error(b));
    ++failures;
  } else {
    std::printf("  B entered explicit mode before receiving any track\n");
  }
  if (pu_screen_watch_ex(b, "peer-a", -1, 1) != PU_OK) {
    std::printf("FAIL: watch-before-track arm failed: %s\n", pu_last_error(b));
    ++failures;
  }

  if (pu_screen_start(a, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(A, synthetic) = %s\n", pu_last_error(a));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return failures + 1;
  }

  if (WaitFor(a, "A", &sa, b, "B", &sb, "explicit watch-before-track frames", BFramesAtLeast5) <
      0) {
    ++failures;
  } else {
    std::printf("  B received %u frame(s) after watch-before-track (sink attached)\n",
                FramesReceived(b));
  }

  // 2) "remote" 是旧客户端的通配流 id，armed 后必须按当前选台交付。
  if (AcquireUntil(b, "remote", 3, "armed wildcard stream id") < 0) ++failures;
  if (AcquireUntil(b, "peer-a", 1, "explicit current target id") < 0) ++failures;
  if (ExpectFrameRc(b, "peer-b", PU_ERR_INVALID_ARG, "explicit wrong target rejected") < 0) {
    ++failures;
  }
  if (ExpectFrameRc(b, "no-such-stream", PU_ERR_INVALID_ARG, "unknown target rejected") < 0) {
    ++failures;
  }

  // 3) unwatch 后必须停止交付（sink 已解绑，旧帧不能重新冒出来）。
  if (pu_screen_unwatch(b, "peer-a") != PU_OK) {
    std::printf("FAIL: pu_screen_unwatch(B, peer-a) = %s\n", pu_last_error(b));
    ++failures;
  }
  if (WaitNoRemoteFrames(a, "A", &sa, b, "B", &sb, 500, "after unwatch") < 0) ++failures;
  if (ExpectFrameRc(b, "remote", PU_ERR_TIMEOUT, "acquire after unwatch") < 0) ++failures;

  // 4) arm 到错误槽位：远端 track id 是 "screen-peer-a"，不是 partyup-video-slot-00，
  //    因此必须不挂 sink、不交付帧。
  const uint32_t before_slot = FramesReceived(b);
  if (pu_screen_watch_ex(b, "peer-a", 0, 2) != PU_OK) {
    std::printf("FAIL: wrong-slot arm failed: %s\n", pu_last_error(b));
    ++failures;
  }
  if (PumpBoth(a, "A", &sa, b, "B", &sb) < 0) ++failures;
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  if (PumpBoth(a, "A", &sa, b, "B", &sb) < 0) ++failures;
  if (FramesReceived(b) != before_slot) {
    std::printf("FAIL: wrong-slot arm still advanced frames_received %u -> %u\n", before_slot,
                FramesReceived(b));
    ++failures;
  } else {
    std::printf("  wrong-slot arm delivered no frames (track id filter works)\n");
  }
  if (ExpectFrameRc(b, "remote", PU_ERR_TIMEOUT, "acquire with wrong slot") < 0) ++failures;

  // 5) track-before-watch：轨已存在，重新 arm 正确槽位后 Rebind 必须重新挂 sink。
  const uint32_t before_rebind = FramesReceived(b);
  if (pu_screen_watch_ex(b, "peer-a", -1, 3) != PU_OK) {
    std::printf("FAIL: re-arm failed: %s\n", pu_last_error(b));
    ++failures;
  }
  const int64_t deadline = NowMs() + 5000;
  while (NowMs() < deadline && FramesReceived(b) <= before_rebind) {
    if (PumpBoth(a, "A", &sa, b, "B", &sb) < 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (FramesReceived(b) <= before_rebind) {
    std::printf("FAIL: track-before-watch did not resume frames (%u -> %u)\n", before_rebind,
                FramesReceived(b));
    ++failures;
  } else {
    std::printf("  track-before-watch resumed frames (%u -> %u)\n", before_rebind,
                FramesReceived(b));
  }
  if (AcquireUntil(b, "remote", 2, "track-before-watch acquire") < 0) ++failures;

  const int counts = sa.errors + sb.errors;
  if (counts != 0) {
    std::printf("FAIL: %d ERROR event(s) during explicit watch phase\n", counts);
    ++failures;
  }

  pu_engine_destroy(a);
  pu_engine_destroy(b);
  std::printf("== explicit watch phase: %d failure(s)\n", failures);
  return failures;
}

// legacy（v1）兼容：从不调用 watch/unwatch 的观看方必须继续自动收帧。
int RunLegacyAutoWatch() {
  std::printf("\n== legacy auto-view regression (no watch calls)\n");
  int failures = 0;
  pu_engine* a = CreateEngine("legacy-sharer");
  pu_engine* b = CreateEngine("legacy-viewer");
  if (a == nullptr || b == nullptr) {
    std::printf("FAIL: CreateEngine returned null: %s\n", pu_last_error(nullptr));
    if (a != nullptr) pu_engine_destroy(a);
    if (b != nullptr) pu_engine_destroy(b);
    return 1;
  }
  Stats sa;
  Stats sb;

  if (pu_screen_start(a, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(A, synthetic) = %s\n", pu_last_error(a));
    pu_engine_destroy(a);
    pu_engine_destroy(b);
    return 1;
  }
  if (WaitFor(a, "A", &sa, b, "B", &sb, "legacy auto-view frames", BFramesAtLeast5) < 0) {
    ++failures;
  } else {
    std::printf("  legacy B received %u frame(s) without watch()\n", FramesReceived(b));
  }
  if (AcquireUntil(b, "remote", 2, "legacy wildcard acquire") < 0) ++failures;
  if (sa.errors + sb.errors != 0) {
    std::printf("FAIL: legacy phase emitted %d ERROR event(s)\n", sa.errors + sb.errors);
    ++failures;
  }

  pu_engine_destroy(a);
  pu_engine_destroy(b);
  std::printf("== legacy auto-view phase: %d failure(s)\n", failures);
  return failures;
}

// v2.16/B2：严格接收会话（独立 receive-only PC）回归。
//
// 目的：用**真实网络**（两个引擎，loopback ICE/DTLS/SRTP）证明：
//   * 接收会话用自己的 PC/DTLS/SSRC，帧走 pu_screen_receive_stats 的会话计数；
//   * 关闭会话后旧发布者继续发 RTP，但新会话收不到任何帧（旧包进不了新接收器）；
//   * 关闭/停止接收不影响本端发布：采集/发布状态仍是 RUNNING，frames_sent 继续增长；
//   * 旧会话 id 的 answer/candidate 被 PU_ERR_STATE 拒绝（不是静默丢回调）。
// 全程 synthetic 源，不采桌面、不开麦。
struct ReceivePumpStats {
  int errors = 0;
  std::string last_answer;
};

int PumpReceive(pu_engine* from, const char* from_name, const char* from_session,
                pu_engine* peer, const char* peer_session, ReceivePumpStats* stats,
                int* remote_events) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(from, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: [%s] pu_poll_event rc=%d (%s)\n", from_name, rc, pu_last_error(from));
      return -1;
    }
    if (event.type == PU_EVENT_SIGNAL_OUT) {
      const pu_signal_event& signal = event.data.signal;
      int in_rc = PU_OK;
      if (event.data.signal.kind == PU_SIGNAL_ANSWER) {
        stats->last_answer.assign(signal.payload, signal.payload_len);
      }
      if (peer_session != nullptr && peer_session[0] != '\0') {
        in_rc = pu_signal_in_session(peer, "screen", peer_session, signal.payload,
                                     signal.payload_len);
      } else {
        in_rc = pu_signal_in(peer, signal.payload, signal.payload_len);
      }
      if (in_rc != PU_OK) {
        std::printf("FAIL: [%s] route signal rc=%d (%s)\n", from_name, in_rc, pu_last_error(peer));
        return -1;
      }
      static_cast<void>(from_session);
    } else if (event.type == PU_EVENT_REMOTE_FRAME) {
      if (remote_events != nullptr) ++(*remote_events);
    } else if (event.type == PU_EVENT_ERROR) {
      ++stats->errors;
      std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", from_name,
                  event.data.error.code, event.data.error.message);
      return -1;
    }
  }
}

uint32_t ReceiveFrames(pu_engine* e, const char* session) {
  pu_receive_stats_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = static_cast<uint32_t>(sizeof(info));
  if (pu_screen_receive_stats(e, session, &info) != PU_OK) return 0;
  return info.frames_delivered;
}

uint32_t SentFrames(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = static_cast<uint32_t>(sizeof(stats));
  if (pu_peer_stats(e, &stats) != PU_OK) return 0;
  return stats.frames_sent;
}

int RunReceiveIsolation() {
  std::printf("\n== strict receive session regression (synthetic, real loopback RTP)\n");
  int failures = 0;
  pu_engine* sharer = CreateEngine("receive-sharer");
  pu_engine* viewer = CreateEngine("receive-viewer");
  if (sharer == nullptr || viewer == nullptr) {
    std::printf("FAIL: CreateEngine returned null: %s\n", pu_last_error(nullptr));
    if (sharer != nullptr) pu_engine_destroy(sharer);
    if (viewer != nullptr) pu_engine_destroy(viewer);
    return 1;
  }

  uint32_t caps = 0;
  if (pu_engine_caps(viewer, &caps) != PU_OK || (caps & 0x20u) == 0) {
    std::printf("FAIL: engine caps missing isolated receive bit (caps=0x%x)\n", caps);
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return 1;
  }

  // 1) 观看者先建接收会话（身份先于信令/媒体），再让发布者开始。
  const char* session1 = "b2-session-one";
  uint32_t token1 = 0;
  if (pu_screen_receive_open(viewer, session1, &token1) != PU_OK || token1 == 0) {
    std::printf("FAIL: pu_screen_receive_open(session1) = %s\n", pu_last_error(viewer));
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return 1;
  }
  std::printf("  receive session 1 opened: token=%u\n", token1);

  if (pu_screen_start(sharer, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(sharer) = %s\n", pu_last_error(sharer));
    pu_engine_destroy(sharer);
    pu_engine_destroy(viewer);
    return 1;
  }

  ReceivePumpStats ss;
  ReceivePumpStats sv;
  int viewer_remote_events = 0;
  const int64_t connect_deadline = NowMs() + kConnectTimeoutMs;
  while (NowMs() < connect_deadline && ReceiveFrames(viewer, session1) < 5) {
    // sharer → viewer 的信令必须带 session id（严格路由），viewer → sharer 走旧
    // screen PC 的 answer。
    if (PumpReceive(sharer, "sharer", nullptr, viewer, session1, &sv, &viewer_remote_events) < 0) {
      ++failures;
      break;
    }
    if (PumpReceive(viewer, "viewer", session1, sharer, nullptr, &ss, nullptr) < 0) {
      ++failures;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (ReceiveFrames(viewer, session1) < 5) {
    std::printf("FAIL: receive session 1 delivered %u frames (want >=5)\n",
                ReceiveFrames(viewer, session1));
    ++failures;
  } else {
    std::printf("  receive session 1 delivered %u frames over a real DTLS/SRTP path\n",
                ReceiveFrames(viewer, session1));
  }
  if (AcquireUntil(viewer, "remote", 2, "receive session frame acquire") < 0) ++failures;

  // 2) 关闭会话：旧发布者继续发，新的接收会话不得收到任何旧包。
  const uint32_t global_before_close = FramesReceived(viewer);
  if (pu_screen_receive_close(viewer, session1) != PU_OK) {
    std::printf("FAIL: pu_screen_receive_close(session1) = %s\n", pu_last_error(viewer));
    ++failures;
  }
  pu_receive_stats_info closed_info;
  std::memset(&closed_info, 0, sizeof(closed_info));
  closed_info.struct_size = static_cast<uint32_t>(sizeof(closed_info));
  if (pu_screen_receive_stats(viewer, session1, &closed_info) != PU_OK ||
      closed_info.active != 0) {
    std::printf("FAIL: closed session still reports active\n");
    ++failures;
  }
  // 关闭后 stats 明确报 active=0（文档语义：不存在会话的计数为 0）；真正的
  // 「旧传输不再增加媒体」由下面的全局 frames_received 断言证明。

  const char* session2 = "b2-session-two";
  uint32_t token2 = 0;
  if (pu_screen_receive_open(viewer, session2, &token2) != PU_OK || token2 == 0 ||
      token2 == token1) {
    std::printf("FAIL: pu_screen_receive_open(session2) token=%u (old=%u) %s\n", token2, token1,
                pu_last_error(viewer));
    ++failures;
  }
  // 旧会话的 answer 必须被拒绝，且不能把会话 1 的媒体计到会话 2。
  if (!sv.last_answer.empty()) {
    const int stale_rc = pu_signal_in_session(viewer, "screen", session1, sv.last_answer.data(),
                                              sv.last_answer.size());
    if (stale_rc != PU_ERR_STATE) {
      std::printf("FAIL: stale session answer rc=%d, want PU_ERR_STATE\n", stale_rc);
      ++failures;
    } else {
      std::printf("  stale answer for closed session rejected (PU_ERR_STATE)\n");
    }
  }
  // 旧发布者仍在向旧传输发 RTP；新会话没有 offer/answer，绝不该有帧。
  const int64_t drain_deadline = NowMs() + 600;
  while (NowMs() < drain_deadline) {
    if (PumpReceive(sharer, "sharer", nullptr, viewer, session2, &sv, nullptr) < 0) break;
    if (PumpReceive(viewer, "viewer", session2, sharer, nullptr, &ss, nullptr) < 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (ReceiveFrames(viewer, session2) != 0) {
    std::printf("FAIL: new session received %u frame(s) from the old closed transport\n",
                ReceiveFrames(viewer, session2));
    ++failures;
  } else {
    std::printf("  old publisher kept sending; new session counted 0 frames\n");
  }
  if (FramesReceived(viewer) != global_before_close) {
    std::printf("FAIL: global frames_received advanced after close %u -> %u\n",
                global_before_close, FramesReceived(viewer));
    ++failures;
  }

  // 3) 停看不影响本端发布：viewer 自己开始共享（与接收会话并存），随后关闭接收，
  //    发布状态必须仍是 RUNNING，frames_sent 继续增长。
  if (pu_screen_start(viewer, "synthetic") != PU_OK) {
    std::printf("FAIL: viewer pu_screen_start while receiving = %s\n", pu_last_error(viewer));
    ++failures;
  } else {
    std::printf("  viewer publishes while holding a receive session\n");
    pu_stats after_start;
    std::memset(&after_start, 0, sizeof(after_start));
    after_start.struct_size = static_cast<uint32_t>(sizeof(after_start));
    pu_peer_stats(viewer, &after_start);
    std::printf("  viewer publish stats right after start: has_peer=%u state=%d frames_sent=%u\n",
                after_start.has_peer, after_start.peer_state, after_start.frames_sent);
  }
  const uint32_t sent_before_close = SentFrames(viewer);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const uint32_t sent_mid = SentFrames(viewer);
  if (pu_screen_receive_close(viewer, session2) != PU_OK) {
    std::printf("FAIL: pu_screen_receive_close(session2) = %s\n", pu_last_error(viewer));
    ++failures;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const uint32_t sent_after_close = SentFrames(viewer);
  pu_stats viewer_stats;
  std::memset(&viewer_stats, 0, sizeof(viewer_stats));
  viewer_stats.struct_size = static_cast<uint32_t>(sizeof(viewer_stats));
  pu_peer_stats(viewer, &viewer_stats);
  std::printf("  viewer publish stats after receive close: has_peer=%u state=%d frames_sent=%u err=%s\n",
              viewer_stats.has_peer, viewer_stats.peer_state, viewer_stats.frames_sent,
              pu_last_error(viewer));
  if (viewer_stats.has_peer != 1) {
    std::printf("FAIL: viewer publish PC disappeared after closing receive session (has_peer=%u)\n",
                viewer_stats.has_peer);
    ++failures;
  }
  if (sent_mid <= sent_before_close || sent_after_close <= sent_mid) {
    std::printf("FAIL: publish frames_sent stopped after receive close (%u -> %u -> %u)\n",
                sent_before_close, sent_mid, sent_after_close);
    ++failures;
  } else {
    std::printf("  publish kept running across receive close (frames_sent %u -> %u -> %u)\n",
                sent_before_close, sent_mid, sent_after_close);
  }
  // 幂等关闭：重复关旧会话不影响当前发布。
  if (pu_screen_receive_close(viewer, session1) != PU_OK) {
    std::printf("FAIL: repeated close of old session failed\n");
    ++failures;
  }
  if (ss.errors + sv.errors != 0) {
    std::printf("FAIL: %d ERROR event(s) during receive isolation phase\n", ss.errors + sv.errors);
    ++failures;
  }

  pu_screen_stop(viewer);
  pu_engine_destroy(sharer);
  pu_engine_destroy(viewer);
  std::printf("== strict receive phase: %d failure(s)\n", failures);
  return failures;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media watch regression (synthetic-only, no desktop capture)\n");
  std::printf("  ABI version: %d\n", PU_ABI_VERSION);

  const int explicit_failures = RunExplicitWatch();
  const int legacy_failures = RunLegacyAutoWatch();
  const int receive_failures = RunReceiveIsolation();
  const int total = explicit_failures + legacy_failures + receive_failures;
  if (total != 0) {
    std::printf("\nFAIL: watch regression found %d problem(s)\n", total);
    return 1;
  }
  std::printf("\nPASS: explicit watch (before/after track), target filtering, unwatch, legacy auto-view\n");
  return 0;
}
