// 屏幕共享的「被动方」路由冒烟（v2.11 第 3 步，C ABI 层，双引擎）。
//
// 两个用例，覆盖两个新导出：
//
//   Phase 1（端到端，主判据）：观看者预建会话
//     A pu_screen_start("synthetic")            —— offerer（自己发 offer，画面在这里产生）
//     B pu_screen_prepare() ×2（验证幂等）+ 收 A 的 offer + 回 answer
//     判据：B 通过 pu_frame_acquire("remote") 真的取到 A 的画面帧（>= 30 帧，尺寸/stride 合理），
//           且 B 全程没有发过 offer。
//
//   Phase 2（协议级）：被动共享方（relay 里服务端 offer、客户端 answer）
//     C pu_screen_start("synthetic")            —— 只借它生成一条正常的 video offer
//     D pu_screen_prepare() + pu_screen_start_ex(D, "synthetic", PASSIVE)（采屏但不发 offer）
//     把 C 的 offer 的 video 段改写成 recvonly（模拟 SFU 的「我要收」方向）后灌给 D。
//     判据：D 只回 answer、不回 offer（无重协商）；answer 的 video 段与 offer 同一个 mid、
//           且方向是发送（a=sendonly 或带 a=ssrc:）——这就是「视频轨挂到对端 offer 的
//           m-line 上」的直接证据；D 的采集在跑（frames_sent 增长）。
//
// 为什么 Phase 2 是协议级而不是「对端收到 D 的画面」：本库目前没有「只收不发」的屏幕
// 会话入口，造不出一个真能收 D 视频的对端（那正是 relay 里服务端 SFU 的角色，而 Go 侧
// 的屏幕中继还没实现——sfuVideo=false 时返回 NOT_IMPLEMENTED）。所以这里验证到
// 「D 的 answer 里带上了自己的发送段」这一步，媒体是否抵达对端由服务端用例负责。
//
// 注意：本 exe 只能从 partyup_media.dll 同目录加载，必须放在 Windows 本地盘运行。
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "partyup_media.h"

namespace {

struct Counters {
  int offers = 0;
  int answers = 0;
  int remote_frames = 0;  // PU_EVENT_REMOTE_FRAME
  std::string offer_sdp;
  std::string answer_sdp;
  int stray = 0;  // 没有 media 的信令等异常
};

pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 2;
  config.event_queue_capacity = 2048;
  return pu_engine_create(&config);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t FramesReceived(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(e, &stats) != PU_OK) return 0;
  return stats.frames_received;
}

uint32_t FramesSent(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(e, &stats) != PU_OK) return 0;
  return stats.frames_sent;
}

int PeerState(pu_engine* e) {
  pu_stats stats;
  std::memset(&stats, 0, sizeof(stats));
  stats.struct_size = sizeof(stats);
  if (pu_peer_stats(e, &stats) != PU_OK) return PU_PEER_NONE;
  return stats.peer_state;
}

// 抽干 from 的事件，把信令按 media 转发给 peer 上对应的会话（本冒烟只有 screen）。
// capture_offers/capture_answers 为 true 时把对应 SDP 存下来供断言。
int Pump(pu_engine* from, const char* name, pu_engine* peer, Counters* counters) {
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
      std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", name, event.data.error.code,
                  event.data.error.message);
      return -1;
    }
    if (event.type == PU_EVENT_REMOTE_FRAME) {
      ++counters->remote_frames;
      continue;
    }
    if (event.type != PU_EVENT_SIGNAL_OUT) continue;

    char media_buf[16] = {0};
    const int media_rc = pu_signal_media(&event, media_buf, static_cast<int>(sizeof(media_buf)));
    if (media_rc != PU_OK) {
      std::printf("FAIL: [%s] pu_signal_media rc=%d\n", name, media_rc);
      ++counters->stray;
      continue;
    }
    const std::string payload(event.data.signal.payload, event.data.signal.payload_len);
    if (event.data.signal.kind == PU_SIGNAL_OFFER) {
      ++counters->offers;
      counters->offer_sdp = payload;
    } else if (event.data.signal.kind == PU_SIGNAL_ANSWER) {
      ++counters->answers;
      counters->answer_sdp = payload;
    }
    const int in_rc =
        pu_signal_in_media(peer, media_buf, payload.data(), static_cast<size_t>(payload.size()));
    if (in_rc != PU_OK) {
      std::printf("FAIL: [%s] forward rc=%d (%s)\n", name, in_rc, pu_last_error(peer));
      return -1;
    }
  }
}

int PumpBoth(pu_engine* a, const char* an, pu_engine* b, const char* bn, Counters* ca,
             Counters* cb) {
  if (Pump(a, an, b, ca) < 0) return -1;
  if (Pump(b, bn, a, cb) < 0) return -1;
  return 0;
}

// 轮询直到两台引擎的 PC 都 connected，最多 timeout_ms。
int WaitConnected(pu_engine* a, const char* an, pu_engine* b, const char* bn, Counters* ca,
                  Counters* cb, int timeout_ms, bool* connected_out) {
  const int64_t deadline = NowMs() + timeout_ms;
  while (NowMs() < deadline) {
    if (PumpBoth(a, an, b, bn, ca, cb) < 0) return -1;
    if (PeerState(a) == PU_PEER_CONNECTED && PeerState(b) == PU_PEER_CONNECTED) {
      *connected_out = true;
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  *connected_out = false;
  return 0;
}

// 泵 `ms` 毫秒（顺带抽事件），期间统计 answerer 侧 pu_frame_acquire 的成功次数。
int PumpAndAcquire(pu_engine* acquisition_engine, pu_engine* offerer, const char* offerer_name,
                   pu_engine* answerer, const char* answerer_name, Counters* co, Counters* ca,
                   int ms, int* acquired, uint32_t* last_w, uint32_t* last_h) {
  const int64_t deadline = NowMs() + ms;
  while (NowMs() < deadline) {
    if (PumpBoth(offerer, offerer_name, answerer, answerer_name, co, ca) < 0) return -1;
    if (acquisition_engine != nullptr) {
      pu_frame frame;
      std::memset(&frame, 0, sizeof(frame));
      frame.struct_size = sizeof(frame);
      const int rc = pu_frame_acquire(acquisition_engine, "remote", &frame);
      if (rc == PU_OK) {
        ++*acquired;
        *last_w = frame.width;
        *last_h = frame.height;
        if (frame.pixels == nullptr || frame.stride < frame.width * 4) {
          std::printf("FAIL: acquired frame has bad pixels/stride (%ux%u stride=%u)\n",
                      frame.width, frame.height, frame.stride);
          return -1;
        }
        pu_frame_release(acquisition_engine, &frame);
      } else if (rc != PU_ERR_TIMEOUT) {
        std::printf("FAIL: pu_frame_acquire rc=%d (%s)\n", rc, pu_last_error(acquisition_engine));
        return -1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return 0;
}

// 按行切 SDP。libwebrtc 正常用 CRLF，但接收侧/改写后再序列化时可能只有 LF，
// 所以两种都吃，并去掉行尾的 '\r'。
std::vector<std::string> SplitSdpLines(const std::string& sdp) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < sdp.size()) {
    const size_t end = sdp.find('\n', start);
    std::string line =
        (end == std::string::npos) ? sdp.substr(start) : sdp.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return lines;
}

// ---- SDP 工具：把第一条 video m-line 改写成 recvonly ----
// 模拟 relay 服务端的「我要收你的画面」的 offer。做法：
//   * 视频段里的 a=sendrecv / a=sendonly / a=recvonly 全部去掉；
//   * 视频段里的 a=ssrc:* / a=ssrc-group:* / a=msid:* 去掉（recvonly 段不能带发送 ssrc）；
//   * 在视频段末尾追加 a=recvonly。
// 会话级属性（指纹/ice/group）和 m 段里的 mid/rtpmap/rtcp 都原样保留。
std::string ForceVideoRecvOnly(const std::string& sdp) {
  const std::vector<std::string> lines = SplitSdpLines(sdp);
  std::vector<std::string> out;
  bool in_video = false;
  bool video_seen = false;
  int video_end = -1;  // 视频段最后一个属性在 out 里的下标
  for (const std::string& line : lines) {
    if (line.rfind("m=", 0) == 0) {
      in_video = (line.rfind("m=video", 0) == 0);
      if (in_video) video_seen = true;
      out.push_back(line);
      if (in_video) video_end = static_cast<int>(out.size()) - 1;
      continue;
    }
    if (!in_video) {
      out.push_back(line);
      continue;
    }
    if (line.rfind("a=sendrecv", 0) == 0 || line.rfind("a=sendonly", 0) == 0 ||
        line.rfind("a=recvonly", 0) == 0 || line.rfind("a=ssrc:", 0) == 0 ||
        line.rfind("a=ssrc-group:", 0) == 0 || line.rfind("a=msid:", 0) == 0) {
      continue;
    }
    out.push_back(line);
    video_end = static_cast<int>(out.size()) - 1;
  }
  if (!video_seen) return {};  // 没有视频段：调用方应当报错
  std::vector<std::string> final_lines;
  final_lines.reserve(out.size() + 1);
  for (size_t i = 0; i < out.size(); ++i) {
    final_lines.push_back(out[i]);
    if (static_cast<int>(i) == video_end) final_lines.push_back("a=recvonly");
  }
  std::string result;
  for (const std::string& line : final_lines) {
    result += line;
    result += "\r\n";
  }
  return result;
}

// 只抽一台引擎的事件、把信令丢掉（用于「对端不该收到这条信令」的场景）。
int PumpDiscard(pu_engine* engine, const char* name, Counters* counters) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(engine, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: [%s] pu_poll_event rc=%d (%s)\n", name, rc, pu_last_error(engine));
      return -1;
    }
    if (event.type == PU_EVENT_ERROR) {
      std::printf("FAIL: [%s] PU_EVENT_ERROR code=%d message=%s\n", name, event.data.error.code,
                  event.data.error.message);
      return -1;
    }
    if (event.type != PU_EVENT_SIGNAL_OUT) continue;
    const std::string payload(event.data.signal.payload, event.data.signal.payload_len);
    if (event.data.signal.kind == PU_SIGNAL_OFFER) {
      ++counters->offers;
      counters->offer_sdp = payload;
    } else if (event.data.signal.kind == PU_SIGNAL_ANSWER) {
      ++counters->answers;
      counters->answer_sdp = payload;
    }
  }
}

// 从 SDP 里取视频段的 mid（"a=mid:xxx"）。
std::string VideoMid(const std::string& sdp) {
  const std::vector<std::string> lines = SplitSdpLines(sdp);
  bool in_video = false;
  for (const std::string& line : lines) {
    if (line.rfind("m=", 0) == 0) {
      in_video = (line.rfind("m=video", 0) == 0);
      continue;
    }
    if (in_video && line.rfind("a=mid:", 0) == 0) return line.substr(6);
  }
  return {};
}

// 取 SDP 里视频段的属性（用于判断 answer 那段是不是 sendonly / 带 ssrc）。
std::string VideoSection(const std::string& sdp) {
  const std::vector<std::string> lines = SplitSdpLines(sdp);
  std::string section;
  bool in_video = false;
  for (const std::string& line : lines) {
    if (line.rfind("m=", 0) == 0) {
      in_video = (line.rfind("m=video", 0) == 0);
      if (in_video) section = line + "\r\n";
      continue;
    }
    if (in_video) section += line + "\r\n";
  }
  return section;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media screen passive/viewer smoke (v2.11: pu_screen_start_ex / "
              "pu_screen_prepare)\n");

  int failures = 0;

  // ---- 负例：未知 flags 位必须被拒（不静默当 0）----
  {
    pu_engine* probe = CreateEngine("screen-flags-probe");
    if (probe == nullptr) {
      std::printf("FAIL: pu_engine_create returned null: %s\n", pu_last_error(nullptr));
      return 1;
    }
    const int rc = pu_screen_start_ex(probe, "synthetic", 0x2u);
    std::printf("  flags=0x2 (reserved bit) -> %d %s\n", rc,
                rc == PU_ERR_INVALID_ARG ? "(INVALID_ARG, expected)" : "(WRONG)");
    if (rc != PU_ERR_INVALID_ARG) ++failures;
    const int null_rc = pu_screen_start_ex(nullptr, "synthetic", 0u);
    if (null_rc != PU_ERR_INVALID_ARG) {
      std::printf("FAIL: pu_screen_start_ex(NULL engine) returned %d\n", null_rc);
      ++failures;
    }
    pu_engine_destroy(probe);
  }

  // ================= Phase 1：观看者 prepare + 收画面（端到端）=================
  std::printf("\n---- Phase 1: viewer calls pu_screen_prepare(), offerer shares ----\n");
  {
    pu_engine* a = CreateEngine("screen-A-offerer");
    pu_engine* b = CreateEngine("screen-B-viewer");
    Counters ca, cb;
    if (a == nullptr || b == nullptr) {
      std::printf("FAIL: pu_engine_create returned null\n");
      return 1;
    }
    // 幂等性：连调两次都必须 OK，且只有一条 PC。
    const int prep1 = pu_screen_prepare(b);
    const int prep2 = pu_screen_prepare(b);
    std::printf("  pu_screen_prepare x2 -> %d, %d (want 0,0 — idempotent)\n", prep1, prep2);
    if (prep1 != PU_OK || prep2 != PU_OK) ++failures;
    if (pu_screen_prepare(nullptr) != PU_ERR_INVALID_ARG) {
      std::printf("FAIL: pu_screen_prepare(NULL) not INVALID_ARG\n");
      ++failures;
    }

    const int start_rc = pu_screen_start(a, "synthetic");
    std::printf("  A: pu_screen_start -> %d\n", start_rc);
    if (start_rc != PU_OK) ++failures;

    bool connected = false;
    if (WaitConnected(a, "A", b, "B", &ca, &cb, 15000, &connected) < 0) {
      pu_engine_destroy(a);
      pu_engine_destroy(b);
      return 1;
    }
    std::printf("  connected=%d (A peerState=%d, B peerState=%d)\n", connected ? 1 : 0,
                PeerState(a), PeerState(b));
    if (!connected) ++failures;

    int acquired = 0;
    uint32_t last_w = 0;
    uint32_t last_h = 0;
    if (PumpAndAcquire(b, a, "A", b, "B", &ca, &cb, 6000, &acquired, &last_w, &last_h) < 0) {
      pu_engine_destroy(a);
      pu_engine_destroy(b);
      return 1;
    }
    const uint32_t b_received = FramesReceived(b);
    std::printf("  B decoded: stats.frames_received=%u, pu_frame_acquire ok=%d (%ux%u)\n",
                b_received, acquired, last_w, last_h);
    std::printf("  signals: A offers=%d answers=%d | B offers=%d answers=%d\n", ca.offers,
                ca.answers, cb.offers, cb.answers);
    if (acquired < 30) {
      std::printf("  FAIL: B acquired only %d frame(s) via pu_frame_acquire (want >= 30)\n",
                  acquired);
      ++failures;
    }
    if (last_w == 0 || last_h == 0) {
      std::printf("  FAIL: acquired frames have zero dimensions\n");
      ++failures;
    }
    if (cb.offers != 0) {
      std::printf("  FAIL: viewer B emitted %d offer(s) — prepare/answer path must not offer\n",
                  cb.offers);
      ++failures;
    }
    if (cb.answers == 0) {
      std::printf("  FAIL: viewer B never emitted an answer\n");
      ++failures;
    }
    pu_screen_stop(a);
    pu_screen_stop(b);
    pu_engine_destroy(a);
    pu_engine_destroy(b);
  }

  // ================= Phase 2：被动共享方（协议级）=================
  std::printf("\n---- Phase 2: passive sharer (pu_screen_start_ex flags=1) ----\n");
  {
    pu_engine* c = CreateEngine("screen-C-offer-template");
    pu_engine* d = CreateEngine("screen-D-passive");
    Counters cc, cd;
    if (c == nullptr || d == nullptr) {
      std::printf("FAIL: pu_engine_create returned null\n");
      return 1;
    }
    // C 只用来造一条正常 offer（视频段之后会被改写成 recvonly）。
    if (pu_screen_start(c, "synthetic") != PU_OK) {
      std::printf("FAIL: pu_screen_start(C) failed: %s\n", pu_last_error(c));
      pu_engine_destroy(c);
      pu_engine_destroy(d);
      return 1;
    }
    // 等 C 把 offer 发出来（要等 ICE 收集完成）。
    std::string offer;
    {
      const int64_t deadline = NowMs() + 15000;
      while (NowMs() < deadline && offer.empty()) {
        // 只抽 C 的事件，不转发（这条 offer 我们要改写后再用）。
        pu_event event;
        std::memset(&event, 0, sizeof(event));
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        const int rc = pu_poll_event(c, &event, 20);
        if (rc == PU_ERR_TIMEOUT) continue;
        if (rc != PU_OK) {
          std::printf("FAIL: poll(C) rc=%d\n", rc);
          break;
        }
        if (event.type == PU_EVENT_SIGNAL_OUT) {
          ++cc.offers;
          offer.assign(event.data.signal.payload, event.data.signal.payload_len);
        }
      }
    }
    if (offer.empty()) {
      std::printf("FAIL: C never produced an offer\n");
      pu_screen_stop(c);
      pu_engine_destroy(c);
      pu_engine_destroy(d);
      return 1;
    }
    const std::string rewritten = ForceVideoRecvOnly(offer);
    std::printf("  offer: %zu bytes -> recvonly rewrite: %zu bytes (video mid=%s)\n",
                offer.size(), rewritten.size(), VideoMid(offer).c_str());
    if (rewritten.empty()) {
      std::printf("FAIL: could not rewrite the offer to recvonly (no m=video)\n");
      ++failures;
    } else {
      // D：预建 + 被动共享。
      const int prep_rc = pu_screen_prepare(d);
      const int passive_rc = pu_screen_start_ex(d, "synthetic", PU_SCREEN_START_PASSIVE);
      std::printf("  D: prepare=%d, start_ex(passive)=%d\n", prep_rc, passive_rc);
      if (prep_rc != PU_OK || passive_rc != PU_OK) ++failures;

      const uint32_t d_sent_before = FramesSent(d);
      // 把改写后的 offer 灌给 D 的屏幕会话。
      const int in_rc = pu_signal_in_media(d, "screen", rewritten.data(), rewritten.size());
      std::printf("  D: signal_in_media(screen, recvonly offer) -> %d (%s)\n", in_rc,
                  in_rc == PU_OK ? "ok" : pu_last_error(d));
      if (in_rc != PU_OK) ++failures;

      // 抽 2 秒：D 只应回 answer，不应再发 offer（无重协商）。
      // 只抽 D 的事件：C 的 PC 停在 have-local-offer，而 D 的 answer 是按「recvonly offer」
      // 生成的（sendonly），喂回给 C 会方向冲突——那不是这条用例要验的东西。
      const int64_t deadline = NowMs() + 2000;
      while (NowMs() < deadline) {
        if (PumpDiscard(d, "D", &cd) < 0) {
          pu_engine_destroy(c);
          pu_engine_destroy(d);
          return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      const uint32_t d_sent_after = FramesSent(d);
      const std::string d_video = VideoSection(cd.answer_sdp);
      const bool answer_sendonly =
          d_video.find("a=sendonly") != std::string::npos ||
          d_video.find("a=ssrc:") != std::string::npos;
      const std::string offer_mid = VideoMid(offer);
      const bool mid_matches =
          !offer_mid.empty() && cd.answer_sdp.find("a=mid:" + offer_mid) != std::string::npos;
      std::printf("  D signals: offers=%d answers=%d (want 0/1)\n", cd.offers, cd.answers);
      std::printf("  D answer video section: %s mid=%s (want sendonly/ssrc, mid=%s)\n",
                  answer_sendonly ? "sendonly" : "NOT-sendonly", mid_matches ? "match" : "MISMATCH",
                  offer_mid.c_str());
      std::printf("  D capture: frames_sent %u -> %u over 2 s\n", d_sent_before, d_sent_after);
      if (cd.offers != 0) {
        std::printf("  FAIL: passive sharer emitted %d offer(s) — it must not offer\n", cd.offers);
        ++failures;
      }
      if (cd.answers != 1) {
        std::printf("  FAIL: passive sharer emitted %d answer(s) (want 1)\n", cd.answers);
        ++failures;
      }
      if (!answer_sendonly) {
        std::printf("  FAIL: passive sharer's answer has no sending video section "
                    "(the local track was not attached to the offered m-line)\n");
        ++failures;
      }
      if (!mid_matches) {
        std::printf("  FAIL: passive sharer's answer renegotiated a new m-line (mid mismatch)\n");
        ++failures;
      }
      if (d_sent_after <= d_sent_before) {
        std::printf("  FAIL: passive sharer is not capturing (frames_sent did not grow)\n");
        ++failures;
      }
    }
    pu_screen_stop(d);
    pu_screen_stop(c);
    pu_engine_destroy(c);
    pu_engine_destroy(d);
  }

  // ================= Phase 3：flags=0 与旧入口一致 =================
  std::printf("\n---- Phase 3: pu_screen_start_ex(flags=0) behaves like pu_screen_start ----\n");
  {
    pu_engine* e = CreateEngine("screen-E-flags0");
    if (e == nullptr) {
      std::printf("FAIL: pu_engine_create returned null\n");
      return 1;
    }
    const int rc = pu_screen_start_ex(e, "synthetic", 0u);
    std::printf("  start_ex(flags=0) -> %d\n", rc);
    if (rc != PU_OK) ++failures;
    // 它必须像 offerer 一样发出 offer。
    Counters ce;
    const int64_t deadline = NowMs() + 15000;
    while (NowMs() < deadline && ce.offers == 0) {
      pu_event event;
      std::memset(&event, 0, sizeof(event));
      event.struct_size = static_cast<uint32_t>(sizeof(event));
      const int poll_rc = pu_poll_event(e, &event, 20);
      if (poll_rc == PU_ERR_TIMEOUT) continue;
      if (poll_rc != PU_OK) break;
      if (event.type == PU_EVENT_SIGNAL_OUT && event.data.signal.kind == PU_SIGNAL_OFFER) {
        ++ce.offers;
      }
    }
    std::printf("  start_ex(flags=0) emitted %d offer(s) (want 1)\n", ce.offers);
    if (ce.offers != 1) ++failures;
    // stop 之后可以再 start（老入口的幂等/复用语义不变）。
    if (pu_screen_stop(e) != PU_OK) ++failures;
    if (pu_screen_start_ex(e, "synthetic", 0u) != PU_OK) {
      std::printf("FAIL: restart after stop failed: %s\n", pu_last_error(e));
      ++failures;
    }
    pu_screen_stop(e);
    pu_engine_destroy(e);
  }

  if (failures != 0) {
    std::printf("\nRESULT: FAIL (%d check(s) failed)\n", failures);
    return 1;
  }
  std::printf("\nRESULT: PASS\n");
  return 0;
}
