// PartyUp 媒体核心 · direct 严格接收（B2b）的双向合成回归（v2.17）。
//
// 目的：在**不采全桌面、不打开麦克风**的前提下，用两个同进程引擎跑真实的
// P2P 媒体路径，验证 direct 严格观看的会话隔离不是「pause/sink-only」：
//   * A、B 同时发布（pu_screen_start synthetic）并互相观看：
//       - A 的发布会话 s-ab → B 的接收会话 s-ab（A→B）
//       - B 的发布会话 s-ba → A 的接收会话 s-ba（B→A）
//     共 4 条独立 PeerConnection，每会话一套 ICE/DTLS/SSRC；
//   * 停 A 看 B：先关 B 对该 viewer 的发送传输（publish_close s-ba），再关
//     A 的接收传输（receive_close s-ba）。断言：
//       - 发送端关闭后，观看者仍活着的接收 PC 先做**有界 quiescence 屏障**：
//         用 RTP packets_received + frames_delivered 记录排空过程，最多等 5s，
//         必须出现连续 500ms 无增长；排空后再单独观察稳定窗口不得继续增长
//         （在途 RTP / decoder 队列的迟到帧是合法交付，不是「没停发」）；
//       - 观看者本地 receive_close 之后：acquire 不得再成功、stats inactive、
//         关闭后产生的 REMOTE_FRAME 必须为 0（与 sender close 的在途排空分开）；
//       - B 的发布会话关闭后 active=0；
//       - **A→B 仍在交付新帧**（B 的接收帧数继续增长、A 的发布 RTP 包继续增长）；
//       - 两端采集源都没有停止/重建（frames_sent 继续增长，screen 仍 RUNNING）；
//   * 重看 B 必须建立**新身份**（新 session/token），旧会话信令被拒绝；
//   * 对称停止 B 看 A，验证反方向同理；
//   * 真实生产顺序：viewer 先关接收（服务端尚未通知 sender），此时 sender 的
//     publish 必须仍 active；随后 sender 收到关闭再 publish_close。两个方向各验
//     一次，且另一方向接收与两端采集都必须继续。
//
// 画面只来自 pu_screen_start("synthetic")，不枚举/不采集真实显示器。
// 必须与 partyup_media.dll 同目录、从 Windows 本地盘运行（UNC 下 DLL 搜索不可靠）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "partyup_media.h"

namespace {

constexpr int kConnectTimeoutMs = 25000;
constexpr uint32_t kCapDirectPublish = 0x40u;  // v2.17 bit6

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t NowUsSystem() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 2;
  config.event_queue_capacity = 4096;
  return pu_engine_create(&config);
}

// 一条信号应该投给哪个引擎的哪条会话。
struct Route {
  pu_engine* peer = nullptr;
  const char* session = nullptr;
  bool publish_in = false;  // true = pu_screen_publish_signal_in（回给共享者）
  // 会话关闭后到达的信令在真实客户端会被丢弃（token 已失效），测试同样只记录、
  // 不再灌进已关闭的会话。
  bool closed = false;
};

// token 是**每个引擎内**唯一的会话标识（v2.17 起接收/发送共用一个计数器）：
// 两个引擎可能各有一个 token=1，所以路由必须以 (引擎, token) 为键。
using Routes = std::map<pu_engine*, std::map<uint32_t, Route>>;

struct Stats {
  int signals = 0;
  int stale_signals = 0;
  int remote_frames = 0;
  // 本地关闭后的「新交付」判定用 received_at_us 与关闭时刻比较：关闭前已入队、
  // 关闭后才被 poll 的旧事件不是新交付，不能误报。
  int remote_frames_after_cutoff = 0;
  int64_t remote_cutoff_us = 0;
  int foreign = 0;
  int errors = 0;
};

int Pump(pu_engine* from, const char* name, const Routes& routes, Stats* stats) {
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
    if (event.type == PU_EVENT_ERROR) {
      std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", name,
                  event.data.error.code, event.data.error.message);
      return -1;
    }
    if (event.type == PU_EVENT_REMOTE_FRAME) {
      ++stats->remote_frames;
      if (event.data.remote.received_at_us > stats->remote_cutoff_us) {
        ++stats->remote_frames_after_cutoff;
      }
      continue;
    }
    if (event.type != PU_EVENT_SIGNAL_OUT) continue;
    ++stats->signals;
    const pu_signal_event& signal = event.data.signal;
    const uint32_t token = signal.reserved;
    if (token == 0) {
      // 不是严格会话（主屏幕 PC 的 legacy offer/broadcast）。严格客户端忽略它。
      continue;
    }
    const auto owner = routes.find(from);
    if (owner == routes.end()) {
      ++stats->foreign;
      std::printf("FAIL: [%s] signal for engine without any route token=%u\n", name, token);
      return -1;
    }
    const auto it = owner->second.find(token);
    if (it == owner->second.end()) {
      ++stats->foreign;
      std::printf("FAIL: [%s] signal for unknown session token=%u kind=%u\n", name, token,
                  signal.kind);
      return -1;
    }
    const Route& route = it->second;
    if (route.closed) {
      // 关闭后仍在途的信令：真实客户端按 token 丢弃，不能灌回旧会话。
      ++stats->stale_signals;
      continue;
    }
    int in_rc;
    if (route.publish_in) {
      in_rc = pu_screen_publish_signal_in(route.peer, route.session, signal.payload,
                                          signal.payload_len);
    } else {
      in_rc = pu_signal_in_session(route.peer, "screen", route.session, signal.payload,
                                   signal.payload_len);
    }
    if (in_rc != PU_OK) {
      std::printf("FAIL: [%s] route token=%u session=%s publish_in=%d rc=%d (%s)\n", name, token,
                  route.session, route.publish_in ? 1 : 0, in_rc, pu_last_error(route.peer));
      return -1;
    }
  }
}

int PumpBoth(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn, Stats* sb,
             const Routes& routes) {
  if (Pump(a, an, routes, sa) < 0) return -1;
  if (Pump(b, bn, routes, sb) < 0) return -1;
  return 0;
}

int WaitFor(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn, Stats* sb,
            const Routes& routes, const char* what,
            bool (*pred)(pu_engine*, pu_engine*)) {
  const int64_t deadline = NowMs() + kConnectTimeoutMs;
  while (NowMs() < deadline) {
    if (PumpBoth(a, an, sa, b, bn, sb, routes) < 0) return -1;
    if (pred(a, b)) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::printf("FAIL: timed out waiting for %s\n", what);
  return -1;
}

pu_receive_stats_info ReceiveStats(pu_engine* e, const char* session) {
  pu_receive_stats_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = static_cast<uint32_t>(sizeof(info));
  if (pu_screen_receive_stats(e, session, &info) != PU_OK) info.active = 0;
  return info;
}

pu_publish_stats_info PublishStats(pu_engine* e, const char* session) {
  pu_publish_stats_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = static_cast<uint32_t>(sizeof(info));
  if (pu_screen_publish_stats(e, session, &info) != PU_OK) info.active = 0;
  return info;
}

pu_rtp_stats_info ReceiveRtp(pu_engine* e, const char* session) {
  pu_rtp_stats_info info;
  std::memset(&info, 0, sizeof(info));
  info.struct_size = static_cast<uint32_t>(sizeof(info));
  if (pu_screen_receive_rtp_stats(e, session, &info) != PU_OK) info.active = 0;
  return info;
}

uint32_t FramesSent(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = static_cast<uint32_t>(sizeof(stats));
  if (pu_peer_stats(e, &stats) != PU_OK) return 0;
  return stats.frames_sent;
}

int ExpectRc(int rc, int expected, const char* what) {
  if (rc == expected) {
    std::printf("  %-58s rc=%d (as expected)\n", what, rc);
    return 0;
  }
  std::printf("FAIL: %s: rc=%d, expected %d\n", what, rc, expected);
  return -1;
}

// 等待一段窗口，期间持续泵事件；返回 false 表示泵出错。
bool PumpFor(pu_engine* a, const char* an, Stats* sa, pu_engine* b, const char* bn, Stats* sb,
             const Routes& routes, int window_ms) {
  const int64_t deadline = NowMs() + window_ms;
  while (NowMs() < deadline) {
    if (PumpBoth(a, an, sa, b, bn, sb, routes) < 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

bool BothWatchingEnough(pu_engine* a, pu_engine* b) {
  // A 收 B（s-ba）≥5 帧且 B 收 A（s-ab）≥5 帧。
  return ReceiveStats(a, "s-ba").frames_delivered >= 5 &&
         ReceiveStats(b, "s-ab").frames_delivered >= 5;
}

bool RewatchDelivers(pu_engine* a, pu_engine*) {
  return ReceiveStats(a, "s-ba2").frames_delivered >= 5;
}

bool AB3Delivers(pu_engine*, pu_engine* b) {
  return ReceiveStats(b, "s-ab3").frames_delivered >= 5;
}

// 本端接收会话关闭后，pull 模型不得再交付任何旧帧：acquire 必须非 OK（没有
// armed 的接收），stats 必须 inactive，事件队列在窗口内不得出现新的 REMOTE_FRAME。
int AcquireStatus(pu_engine* e) {
  pu_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.struct_size = static_cast<uint32_t>(sizeof(frame));
  const int rc = pu_frame_acquire(e, "remote", &frame);
  if (rc == PU_OK) pu_frame_release(e, &frame);
  return rc;
}

// sender close 后，观看者接收 PC 仍活着；RTP/解码器可能有界地在途排空。这里用
// （RTP packets_received, frames_delivered）做有界 quiescence 屏障：最多等
// max_ms，必须出现连续 stable_ms 无增长；随后再单独观察一个稳定窗口。只有持续
// 增长才说明生产关闭路径没停发。
bool WaitReceiverQuiescent(pu_engine* a, const char* an, Stats* sa, pu_engine* b,
                           const char* bn, Stats* sb, const Routes& routes,
                           pu_engine* viewer, const char* viewer_name, const char* session,
                           int max_ms, int stable_ms, uint32_t* rtp_before,
                           uint32_t* rtp_after, uint32_t* frames_before,
                           uint32_t* frames_after) {
  *rtp_before = ReceiveRtp(viewer, session).packets_received;
  *frames_before = ReceiveStats(viewer, session).frames_delivered;
  uint32_t rtp_last = *rtp_before;
  uint32_t frames_last = *frames_before;
  const int64_t start = NowMs();
  int64_t last_change = start;
  while (NowMs() - start < max_ms) {
    if (!PumpFor(a, an, sa, b, bn, sb, routes, 40)) return false;
    const uint32_t rtp = ReceiveRtp(viewer, session).packets_received;
    const uint32_t frames = ReceiveStats(viewer, session).frames_delivered;
    if (rtp != rtp_last || frames != frames_last) {
      rtp_last = rtp;
      frames_last = frames;
      last_change = NowMs();
    }
    if (NowMs() - last_change >= stable_ms) {
      *rtp_after = rtp;
      *frames_after = frames;
      std::printf("  %s %s quiesced after %lldms: rtp %u -> %u, frames %u -> %u "
                  "(in-flight/decoder drain bounded)\n",
                  viewer_name, session, static_cast<long long>(NowMs() - start),
                  *rtp_before, *rtp_after, *frames_before, *frames_after);
      return true;
    }
  }
  *rtp_after = rtp_last;
  *frames_after = frames_last;
  std::printf("FAIL: %s %s did not quiesce within %dms: rtp %u -> %u, frames %u -> %u "
              "(sender close did not stop delivery)\n",
              viewer_name, session, max_ms, *rtp_before, *rtp_after, *frames_before,
              *frames_after);
  return false;
}

bool VerifyReceiverStable(pu_engine* a, const char* an, Stats* sa, pu_engine* b,
                          const char* bn, Stats* sb, const Routes& routes,
                          pu_engine* viewer, const char* viewer_name, const char* session,
                          int window_ms) {
  const uint32_t rtp_before = ReceiveRtp(viewer, session).packets_received;
  const uint32_t frames_before = ReceiveStats(viewer, session).frames_delivered;
  if (!PumpFor(a, an, sa, b, bn, sb, routes, window_ms)) return false;
  const uint32_t rtp_after = ReceiveRtp(viewer, session).packets_received;
  const uint32_t frames_after = ReceiveStats(viewer, session).frames_delivered;
  if (rtp_after != rtp_before || frames_after != frames_before) {
    std::printf("FAIL: %s %s still growing after quiescence window: rtp %u -> %u, "
                "frames %u -> %u\n",
                viewer_name, session, rtp_before, rtp_after, frames_before, frames_after);
    return false;
  }
  std::printf("  %s %s stable for %dms after drain: rtp=%u frames=%u (no growth)\n",
              viewer_name, session, window_ms, rtp_after, frames_after);
  return true;
}

bool VerifyNoDeliveryAfterReceiveClose(pu_engine* a, const char* an, Stats* sa,
                                       pu_engine* b, const char* bn, Stats* sb,
                                       const Routes& routes, pu_engine* viewer,
                                       const char* viewer_name, const char* session,
                                       int window_ms) {
  const int rc = AcquireStatus(viewer);
  if (rc == PU_OK) {
    std::printf("FAIL: %s still acquired a frame after local receive_close (%s)\n",
                viewer_name, session);
    return false;
  }
  const pu_receive_stats_info stats = ReceiveStats(viewer, session);
  if (stats.active != 0) {
    std::printf("FAIL: %s receive stats still active after local receive_close (%s)\n",
                viewer_name, session);
    return false;
  }
  Stats* viewer_stats = (viewer == a) ? sa : sb;
  // 关闭返回之后才产生（received_at_us > cutoff）的帧 = 新交付；关闭前已入队的
  // 旧事件不算。
  viewer_stats->remote_cutoff_us = NowUsSystem();
  viewer_stats->remote_frames_after_cutoff = 0;
  if (!PumpFor(a, an, sa, b, bn, sb, routes, window_ms)) return false;
  const int frames_after = viewer_stats->remote_frames_after_cutoff;
  if (frames_after != 0) {
    std::printf("FAIL: %s received %d REMOTE_FRAME event(s) generated after local receive_close (%s)\n",
                viewer_name, frames_after, session);
    return false;
  }
  std::printf("  %s local receive_close (%s): acquire rc=%d (not OK), stats inactive, "
              "0 REMOTE_FRAME in %dms\n",
              viewer_name, session, rc, window_ms);
  return true;
}

}  // namespace

int main() {
  std::printf("== direct isolated receive regression (synthetic, two engines, 4 PCs)\n");
  int failures = 0;

  pu_engine* a = CreateEngine("direct-a");
  pu_engine* b = CreateEngine("direct-b");
  if (a == nullptr || b == nullptr) {
    std::printf("FAIL: CreateEngine returned null: %s\n", pu_last_error(nullptr));
    if (a != nullptr) pu_engine_destroy(a);
    if (b != nullptr) pu_engine_destroy(b);
    return 1;
  }
  Stats sa;
  Stats sb;
  Routes routes;

  uint32_t caps = 0;
  if (pu_engine_caps(a, &caps) != PU_OK) {
    std::printf("FAIL: pu_engine_caps failed: %s\n", pu_last_error(a));
    ++failures;
  } else if ((caps & kCapDirectPublish) == 0) {
    std::printf("FAIL: engine caps=0x%x does not advertise bit6 (per-viewer publish)\n", caps);
    ++failures;
  } else {
    std::printf("  engine caps=0x%x (bit6 per-viewer publish)\n", caps);
  }
  if (pu_engine_caps(b, &caps) != PU_OK || (caps & kCapDirectPublish) == 0) {
    std::printf("FAIL: second engine does not advertise bit6 (caps=0x%x)\n", caps);
    ++failures;
  }

  // 两端都开始共享（合成源）。
  if (pu_screen_start(a, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(A) = %s\n", pu_last_error(a));
    ++failures;
  }
  if (pu_screen_start(b, "synthetic") != PU_OK) {
    std::printf("FAIL: pu_screen_start(B) = %s\n", pu_last_error(b));
    ++failures;
  }

  // 会话 1（A→B）：B 先建接收会话（identity 先于信令），A 再建发送会话。
  uint32_t recv_b_ab = 0;
  uint32_t pub_a_ab = 0;
  if (ExpectRc(pu_screen_receive_open(b, "s-ab", &recv_b_ab), PU_OK,
               "B receive_open(s-ab)") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_open(a, "s-ab", &pub_a_ab), PU_OK,
               "A publish_open(s-ab)") < 0) {
    ++failures;
  }
  routes[a][pub_a_ab] = Route{b, "s-ab", false};    // A 的 offer → B 的接收会话
  routes[b][recv_b_ab] = Route{a, "s-ab", true};    // B 的 answer → A 的发布会话

  // 会话 2（B→A）。
  uint32_t recv_a_ba = 0;
  uint32_t pub_b_ba = 0;
  if (ExpectRc(pu_screen_receive_open(a, "s-ba", &recv_a_ba), PU_OK,
               "A receive_open(s-ba)") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_open(b, "s-ba", &pub_b_ba), PU_OK,
               "B publish_open(s-ba)") < 0) {
    ++failures;
  }
  routes[b][pub_b_ba] = Route{a, "s-ba", false};
  routes[a][recv_a_ba] = Route{b, "s-ba", true};

  if (WaitFor(a, "A", &sa, b, "B", &sb, routes, "bidirectional strict delivery",
              BothWatchingEnough) < 0) {
    ++failures;
  } else {
    const pu_receive_stats_info ra = ReceiveStats(a, "s-ba");
    const pu_receive_stats_info rb = ReceiveStats(b, "s-ab");
    const pu_publish_stats_info pa = PublishStats(a, "s-ab");
    const pu_publish_stats_info pb = PublishStats(b, "s-ba");
    std::printf("  A receives s-ba: frames=%u rtpPackets=%u | B receives s-ab: frames=%u "
                "rtpPackets=%u\n",
                ra.frames_delivered, ReceiveRtp(a, "s-ba").packets_received,
                rb.frames_delivered, ReceiveRtp(b, "s-ab").packets_received);
    std::printf("  A publishes s-ab: packetsSent=%u framesEncoded=%llu | B publishes s-ba: "
                "packetsSent=%u\n",
                pa.packets_sent, static_cast<unsigned long long>(pa.frames_encoded),
                pb.packets_sent);
    if (pa.packets_sent == 0 || pb.packets_sent == 0) {
      std::printf("FAIL: publish stats show no RTP packets (transport not really sending)\n");
      ++failures;
    }
    if (pub_a_ab == 0 || recv_b_ab == 0 || pub_b_ba == 0 || recv_a_ba == 0) {
      std::printf("FAIL: session tokens must be non-zero\n");
      ++failures;
    }
  }

  // ---- A 停止观看 B ----
  std::printf("\n-- A stops watching B (only B→A transport) --\n");
  const uint32_t frames_b_ab_before = ReceiveStats(b, "s-ab").frames_delivered;
  const uint32_t sent_a_before = PublishStats(a, "s-ab").packets_sent;
  const uint32_t capture_a_before = FramesSent(a);
  const uint32_t capture_b_before = FramesSent(b);

  // 共享者关闭对该 viewer 的发送传输（服务端 viewer_close 的效果）。
  if (ExpectRc(pu_screen_publish_close(b, "s-ba"), PU_OK, "B publish_close(s-ba)") < 0) {
    ++failures;
  }
  routes[b][pub_b_ba].closed = true;  // 关闭后迟到的信令按 token 丢弃
  // 观看者仍持有接收 PC：先让在途 RTP / decoder 队列有界排空，再观察稳定窗口。
  // 不能用「close 后立即采样」要求计数立刻不增（in-flight 帧是合法交付）。
  uint32_t rtp_a_before = 0;
  uint32_t rtp_a_after = 0;
  uint32_t frames_a_before = 0;
  uint32_t frames_a_after = 0;
  if (!WaitReceiverQuiescent(a, "A", &sa, b, "B", &sb, routes, a, "A", "s-ba", 5000, 500,
                             &rtp_a_before, &rtp_a_after, &frames_a_before, &frames_a_after)) {
    ++failures;
  } else if (!VerifyReceiverStable(a, "A", &sa, b, "B", &sb, routes, a, "A", "s-ba", 600)) {
    ++failures;
  }

  // 观看者关闭自己的接收传输（本地 stop）；此后的旧会话信令必须被拒绝。
  if (ExpectRc(pu_screen_receive_close(a, "s-ba"), PU_OK, "A receive_close(s-ba)") < 0) {
    ++failures;
  }
  routes[a][recv_a_ba].closed = true;
  // 本地关闭后不得再交付/acquire 旧接收会话的帧（与在途排空是两回事）。
  if (!VerifyNoDeliveryAfterReceiveClose(a, "A", &sa, b, "B", &sb, routes, a, "A", "s-ba",
                                         450)) {
    ++failures;
  }

  // A→B 不能受影响：B 仍在收新帧，A 仍在发 RTP，两端采集都还在推进。
  if (!PumpFor(a, "A", &sa, b, "B", &sb, routes, 700)) {
    ++failures;
  }
  const uint32_t frames_b_ab_after = ReceiveStats(b, "s-ab").frames_delivered;
  const uint32_t sent_a_after = PublishStats(a, "s-ab").packets_sent;
  const uint32_t capture_a_after = FramesSent(a);
  const uint32_t capture_b_after = FramesSent(b);
  if (frames_b_ab_after <= frames_b_ab_before) {
    std::printf("FAIL: A->B stopped delivering frames after A stopped watching B (%u -> %u)\n",
                frames_b_ab_before, frames_b_ab_after);
    ++failures;
  } else {
    std::printf("  A->B kept delivering: frames %u -> %u\n", frames_b_ab_before,
                frames_b_ab_after);
  }
  if (sent_a_after <= sent_a_before) {
    std::printf("FAIL: A's publish RTP to B stopped growing (%u -> %u)\n", sent_a_before,
                sent_a_after);
    ++failures;
  } else {
    std::printf("  A's publish RTP to B kept growing: %u -> %u packets\n", sent_a_before,
                sent_a_after);
  }
  if (capture_a_after <= capture_a_before || capture_b_after <= capture_b_before) {
    std::printf("FAIL: capture source restarted or stopped (A %u->%u, B %u->%u)\n",
                capture_a_before, capture_a_after, capture_b_before, capture_b_after);
    ++failures;
  } else {
    std::printf("  capture kept running: A %u -> %u, B %u -> %u frames\n", capture_a_before,
                capture_a_after, capture_b_before, capture_b_after);
  }

  // 旧会话身份被拒绝：B 关闭后仍可 signal_in（幂等），A 的旧接收会话已不存在。
  const char* dead_candidate = "{\"type\":\"candidate\",\"candidate\":\"\"}";
  const char* dead_offer = "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n\"}";
  if (ExpectRc(pu_screen_publish_signal_in(b, "s-ba", dead_candidate,
                                           std::strlen(dead_candidate)),
               PU_ERR_STATE, "old publish session s-ba rejects signal") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_signal_in_session(a, "screen", "s-ba", dead_offer,
                                    std::strlen(dead_offer)),
               PU_ERR_STATE, "old receive session s-ba rejects offer") < 0) {
    ++failures;
  }

  // ---- 重看 B：必须是新身份（新 session + 新 token）----
  std::printf("\n-- A watches B again (new identity) --\n");
  uint32_t recv_a_ba2 = 0;
  uint32_t pub_b_ba2 = 0;
  if (ExpectRc(pu_screen_receive_open(a, "s-ba2", &recv_a_ba2), PU_OK,
               "A receive_open(s-ba2)") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_open(b, "s-ba2", &pub_b_ba2), PU_OK,
               "B publish_open(s-ba2)") < 0) {
    ++failures;
  }
  routes[b][pub_b_ba2] = Route{a, "s-ba2", false};
  routes[a][recv_a_ba2] = Route{b, "s-ba2", true};
  if (recv_a_ba2 == recv_a_ba || pub_b_ba2 == pub_b_ba) {
    std::printf("FAIL: rewatch reused session token (old=%u/%u new=%u/%u)\n", recv_a_ba,
                pub_b_ba, recv_a_ba2, pub_b_ba2);
    ++failures;
  }
  if (WaitFor(a, "A", &sa, b, "B", &sb, routes, "rewatch delivery", RewatchDelivers) < 0) {
    ++failures;
  } else {
    std::printf("  rewatch delivered %u frame(s) with new session/token\n",
                ReceiveStats(a, "s-ba2").frames_delivered);
  }
  // 旧会话信令仍必须被拒绝（不能被贴到新会话上）。
  if (ExpectRc(pu_signal_in_session(a, "screen", "s-ba", dead_offer,
                                    std::strlen(dead_offer)),
               PU_ERR_STATE, "old session s-ba still rejected after rewatch") < 0) {
    ++failures;
  }

  // ---- 对称：B 停止观看 A（先 sender close，viewer receiver 仍活着）----
  std::printf("\n-- B stops watching A (symmetric, sender close first) --\n");
  const uint32_t frames_a_ba2_before = ReceiveStats(a, "s-ba2").frames_delivered;
  const uint32_t capture_a_before2 = FramesSent(a);
  const uint32_t capture_b_before2 = FramesSent(b);
  if (ExpectRc(pu_screen_publish_close(a, "s-ab"), PU_OK, "A publish_close(s-ab)") < 0) {
    ++failures;
  }
  routes[a][pub_a_ab].closed = true;
  uint32_t rtp_b_before = 0;
  uint32_t rtp_b_after = 0;
  uint32_t frames_b_before = 0;
  uint32_t frames_b_after = 0;
  if (!WaitReceiverQuiescent(a, "A", &sa, b, "B", &sb, routes, b, "B", "s-ab", 5000, 500,
                             &rtp_b_before, &rtp_b_after, &frames_b_before, &frames_b_after)) {
    ++failures;
  } else if (!VerifyReceiverStable(a, "A", &sa, b, "B", &sb, routes, b, "B", "s-ab", 600)) {
    ++failures;
  }
  if (ExpectRc(pu_screen_receive_close(b, "s-ab"), PU_OK, "B receive_close(s-ab)") < 0) {
    ++failures;
  }
  routes[b][recv_b_ab].closed = true;
  if (!VerifyNoDeliveryAfterReceiveClose(a, "A", &sa, b, "B", &sb, routes, b, "B", "s-ab",
                                         450)) {
    ++failures;
  }

  // 反方向不能受影响：B→A（s-ba2）仍在交付新帧，两端采集都还在推进。
  if (!PumpFor(a, "A", &sa, b, "B", &sb, routes, 700)) {
    ++failures;
  }
  const uint32_t frames_a_ba2_after = ReceiveStats(a, "s-ba2").frames_delivered;
  const uint32_t capture_a_after2 = FramesSent(a);
  const uint32_t capture_b_after2 = FramesSent(b);
  if (frames_a_ba2_after <= frames_a_ba2_before) {
    std::printf("FAIL: B->A (s-ba2) stopped when A stopped its own publish transport (%u -> %u)\n",
                frames_a_ba2_before, frames_a_ba2_after);
    ++failures;
  } else {
    std::printf("  B->A (s-ba2) kept delivering: frames %u -> %u\n", frames_a_ba2_before,
                frames_a_ba2_after);
  }
  if (capture_a_after2 <= capture_a_before2 || capture_b_after2 <= capture_b_before2) {
    std::printf("FAIL: capture source restarted or stopped after symmetric close (A %u->%u, B %u->%u)\n",
                capture_a_before2, capture_a_after2, capture_b_before2, capture_b_after2);
    ++failures;
  } else {
    std::printf("  capture kept running after symmetric close: A %u -> %u, B %u -> %u frames\n",
                capture_a_before2, capture_a_after2, capture_b_before2, capture_b_after2);
  }

  // ---- 生产顺序（一）：viewer(B) 先本地关闭接收，server 再通知 sender(A) 关闭发布 ----
  std::printf("\n-- production order: B closes receive first, then A closes publish (s-ab3) --\n");
  uint32_t recv_b_ab3 = 0;
  uint32_t pub_a_ab3 = 0;
  if (ExpectRc(pu_screen_receive_open(b, "s-ab3", &recv_b_ab3), PU_OK,
               "B receive_open(s-ab3)") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_open(a, "s-ab3", &pub_a_ab3), PU_OK,
               "A publish_open(s-ab3)") < 0) {
    ++failures;
  }
  routes[a][pub_a_ab3] = Route{b, "s-ab3", false};
  routes[b][recv_b_ab3] = Route{a, "s-ab3", true};
  if (recv_b_ab3 == 0 || pub_a_ab3 == 0 || pub_a_ab3 == pub_a_ab) {
    std::printf("FAIL: s-ab3 session tokens must be non-zero and fresh (recv=%u pub=%u oldPub=%u)\n",
                recv_b_ab3, pub_a_ab3, pub_a_ab);
    ++failures;
  }
  if (WaitFor(a, "A", &sa, b, "B", &sb, routes, "s-ab3 delivery", AB3Delivers) < 0) {
    ++failures;
  }

  // 观看者先关：服务端尚未通知共享者，A 的发布必须仍然 active（本地关闭不越权停源）。
  if (ExpectRc(pu_screen_receive_close(b, "s-ab3"), PU_OK,
               "B receive_close(s-ab3) first") < 0) {
    ++failures;
  }
  routes[b][recv_b_ab3].closed = true;
  routes[a][pub_a_ab3].closed = true;  // viewer 已关，迟到的 sender 信令按 token 丢弃
  if (!VerifyNoDeliveryAfterReceiveClose(a, "A", &sa, b, "B", &sb, routes, b, "B", "s-ab3",
                                         450)) {
    ++failures;
  }
  const pu_publish_stats_info pub_a_ab3_after_viewer = PublishStats(a, "s-ab3");
  if (pub_a_ab3_after_viewer.active != 1) {
    std::printf("FAIL: viewer local receive_close stopped sender publish before server notice "
                "(s-ab3 active=%d)\n",
                pub_a_ab3_after_viewer.active);
    ++failures;
  } else {
    std::printf("  sender publish s-ab3 still active after viewer local close: %u packet(s)\n",
                pub_a_ab3_after_viewer.packets_sent);
  }
  // 服务端随后通知 sender：关闭该 per-viewer 发布传输，但采集/源不能停。
  if (ExpectRc(pu_screen_publish_close(a, "s-ab3"), PU_OK,
               "A publish_close(s-ab3) after viewer close") < 0) {
    ++failures;
  }
  if (PublishStats(a, "s-ab3").active != 0) {
    std::printf("FAIL: publish s-ab3 still active after its publish_close\n");
    ++failures;
  }

  // 反方向（B→A s-ba2）必须继续接收新帧，两端采集都不能重启。
  const uint32_t frames_a_ba2_before3 = ReceiveStats(a, "s-ba2").frames_delivered;
  const uint32_t capture_a_before3 = FramesSent(a);
  const uint32_t capture_b_before3 = FramesSent(b);
  if (!PumpFor(a, "A", &sa, b, "B", &sb, routes, 700)) {
    ++failures;
  }
  const uint32_t frames_a_ba2_after3 = ReceiveStats(a, "s-ba2").frames_delivered;
  const uint32_t capture_a_after3 = FramesSent(a);
  const uint32_t capture_b_after3 = FramesSent(b);
  if (frames_a_ba2_after3 <= frames_a_ba2_before3) {
    std::printf("FAIL: reverse B->A (s-ba2) stopped after B's viewer-first close (%u -> %u)\n",
                frames_a_ba2_before3, frames_a_ba2_after3);
    ++failures;
  } else {
    std::printf("  reverse B->A (s-ba2) kept delivering after viewer-first close: frames %u -> %u\n",
                frames_a_ba2_before3, frames_a_ba2_after3);
  }
  if (capture_a_after3 <= capture_a_before3 || capture_b_after3 <= capture_b_before3) {
    std::printf("FAIL: capture source restarted/stopped after viewer-first close (A %u->%u, B %u->%u)\n",
                capture_a_before3, capture_a_after3, capture_b_before3, capture_b_after3);
    ++failures;
  } else {
    std::printf("  capture kept running after viewer-first close: A %u -> %u, B %u -> %u frames\n",
                capture_a_before3, capture_a_after3, capture_b_before3, capture_b_after3);
  }
  if (ExpectRc(pu_signal_in_session(b, "screen", "s-ab3", dead_offer,
                                    std::strlen(dead_offer)),
               PU_ERR_STATE, "closed receive s-ab3 rejects old offer") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_signal_in(a, "s-ab3", dead_candidate,
                                           std::strlen(dead_candidate)),
               PU_ERR_STATE, "closed publish s-ab3 rejects old signal") < 0) {
    ++failures;
  }

  // ---- 生产顺序（二）：viewer(A) 先本地关闭 s-ba2，server 再通知 sender(B) ----
  std::printf("\n-- production order: A closes receive first, then B closes publish (s-ba2) --\n");
  if (ExpectRc(pu_screen_receive_close(a, "s-ba2"), PU_OK,
               "A receive_close(s-ba2) first") < 0) {
    ++failures;
  }
  routes[a][recv_a_ba2].closed = true;
  routes[b][pub_b_ba2].closed = true;
  if (!VerifyNoDeliveryAfterReceiveClose(a, "A", &sa, b, "B", &sb, routes, a, "A", "s-ba2",
                                         450)) {
    ++failures;
  }
  const pu_publish_stats_info pub_b_ba2_after_viewer = PublishStats(b, "s-ba2");
  if (pub_b_ba2_after_viewer.active != 1) {
    std::printf("FAIL: viewer local receive_close stopped sender publish before server notice "
                "(s-ba2 active=%d)\n",
                pub_b_ba2_after_viewer.active);
    ++failures;
  } else {
    std::printf("  sender publish s-ba2 still active after viewer local close: %u packet(s)\n",
                pub_b_ba2_after_viewer.packets_sent);
  }
  if (ExpectRc(pu_screen_publish_close(b, "s-ba2"), PU_OK,
               "B publish_close(s-ba2) after viewer close") < 0) {
    ++failures;
  }
  if (PublishStats(b, "s-ba2").active != 0) {
    std::printf("FAIL: publish s-ba2 still active after its publish_close\n");
    ++failures;
  }
  const uint32_t capture_a_before4 = FramesSent(a);
  const uint32_t capture_b_before4 = FramesSent(b);
  if (!PumpFor(a, "A", &sa, b, "B", &sb, routes, 500)) {
    ++failures;
  }
  const uint32_t capture_a_after4 = FramesSent(a);
  const uint32_t capture_b_after4 = FramesSent(b);
  if (capture_a_after4 <= capture_a_before4 || capture_b_after4 <= capture_b_before4) {
    std::printf("FAIL: capture source restarted/stopped after second viewer-first close "
                "(A %u->%u, B %u->%u)\n",
                capture_a_before4, capture_a_after4, capture_b_before4, capture_b_after4);
    ++failures;
  } else {
    std::printf("  capture kept running after both viewer-first closes: A %u -> %u, B %u -> %u frames\n",
                capture_a_before4, capture_a_after4, capture_b_before4, capture_b_after4);
  }
  if (ExpectRc(pu_signal_in_session(a, "screen", "s-ba2", dead_offer,
                                    std::strlen(dead_offer)),
               PU_ERR_STATE, "closed receive s-ba2 rejects old offer") < 0) {
    ++failures;
  }
  if (ExpectRc(pu_screen_publish_signal_in(b, "s-ba2", dead_candidate,
                                           std::strlen(dead_candidate)),
               PU_ERR_STATE, "closed publish s-ba2 rejects old signal") < 0) {
    ++failures;
  }

  const int errors = sa.errors + sb.errors;
  if (errors != 0) {
    std::printf("FAIL: %d PU_EVENT_ERROR(s) during direct isolation phase\n", errors);
    ++failures;
  }

  pu_engine_destroy(a);
  pu_engine_destroy(b);
  std::printf("\n== direct isolation phase: %d failure(s)\n", failures);
  return failures;
}
