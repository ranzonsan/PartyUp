// v2.12 ICE servers 冒烟：验证 pu_engine_set_ice_servers() 真的让 libwebrtc 去问 STUN。
//
// 判据只看 SDP：本地 offer 里必须出现 `a=candidate:... typ srflx ...`。
// 为什么不用「连上了没有」：srflx 只说明「STUN 回应了、我看到了自己的映射地址」，
// 与对端是否可达无关；后者受 NAT 类型/防火墙影响，会把配置问题和其他问题混在一起。
//
// STUN 服务器是本进程内起的一个最小 RFC 5389 实现（只做 Binding Request →
// XOR-MAPPED-ADDRESS 响应）。这样不依赖外网、也不依赖 Windows/WSL 之间的 UDP 通路。
// 它绑在 INADDR_ANY 上，客户端分别试「本机局域网 IP」和「127.0.0.1」，哪个能出
// srflx 就说明哪条路通（Windows 上候选 socket 绑在网卡地址，通常走前者）。
//
// 覆盖：
//   A. 设置 STUN → 语音会话的 offer 含 srflx
//   B. 设置 STUN → 屏幕会话的 offer 含 srflx（两处建 PC 点都要读到配置）
//   C. 不设置 → offer 不含 srflx（回归：默认仍是 host-only）
//   D. 参数校验：turn: → INVALID_ARG（不静默忽略）、nullptr+len!=0 → INVALID_ARG、
//      格式错 → INVALID_ARG、多行合法列表 → OK、空串/NULL 清零 → OK
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "partyup_media.h"

namespace {

constexpr uint32_t kStunMagic = 0x2112A442u;

// ---------------------------------------------------------------------------
// 最小 STUN 服务器（只支持 Binding）
// ---------------------------------------------------------------------------
struct StunServer {
  SOCKET sock = INVALID_SOCKET;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<int> requests{0};
  uint16_t port = 0;

  ~StunServer() {
    stop.store(true);
    if (thread.joinable()) thread.join();
    if (sock != INVALID_SOCKET) ::closesocket(sock);
  }
};

// 对一条 Binding Request 生成 Binding Success Response（XOR-MAPPED-ADDRESS + FINGERPRINT）。
// 返回响应字节数；不是合法的 Binding Request 时返回 0。
//
// FINGERPRINT 不是可选的：libwebrtc 的 ICE 端口会对 STUN 响应做指纹/完整性校验，
// 一个「只有 XOR-MAPPED-ADDRESS」的响应会被静默丢弃（实测：请求收到了、也回了，
// 但候选表里一个 srflx 都没有，gathering 一直超时）。
uint32_t StunCrc32(const unsigned char* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return ~crc;
}

constexpr uint32_t kFingerprintXor = 0x5354554Eu;  // "STUN"

int BuildStunResponse(const unsigned char* in, int in_len, const sockaddr_in& from,
                      unsigned char* out, int* request_attrs) {
  if (in_len < 20) return 0;
  const uint16_t type = static_cast<uint16_t>((in[0] << 8) | in[1]);
  if (type != 0x0001) return 0;  // 只处理 Binding Request
  uint32_t cookie = (static_cast<uint32_t>(in[4]) << 24) | (static_cast<uint32_t>(in[5]) << 16) |
                    (static_cast<uint32_t>(in[6]) << 8) | static_cast<uint32_t>(in[7]);
  if (cookie != kStunMagic) return 0;

  // 统计请求里的属性类型（诊断用）：看到 0x8028 = FINGERPRINT、0x0006 = USERNAME。
  int attrs = 0;
  if (request_attrs != nullptr) {
    int pos = 20;
    const int total = 20 + static_cast<int>((in[2] << 8) | in[3]);
    while (pos + 4 <= total && pos + 4 <= in_len) {
      const uint16_t attr_type = static_cast<uint16_t>((in[pos] << 8) | in[pos + 1]);
      const uint16_t attr_len = static_cast<uint16_t>((in[pos + 2] << 8) | in[pos + 3]);
      if (attr_type == 0x8028 || attr_type == 0x0008) ++attrs;  // fingerprint
      pos += 4 + ((attr_len + 3) & ~3u);
    }
    *request_attrs = attrs;
  }

  std::memset(out, 0, 40);
  out[0] = 0x01;  // Binding Success Response (0x0101)
  out[1] = 0x01;
  out[2] = 0x00;  // 属性总长 20 = XOR-MAPPED-ADDRESS(4+8) + FINGERPRINT(4+4)
  out[3] = 0x14;
  std::memcpy(out + 4, in + 4, 16);  // magic cookie + transaction id
  out[20] = 0x00;                    // XOR-MAPPED-ADDRESS
  out[21] = 0x20;
  out[22] = 0x00;
  out[23] = 0x08;
  out[24] = 0x00;  // reserved
  out[25] = 0x01;  // family = IPv4
  // 映射端口做 +1 平移：真实的 NAT 会改写端口，而这里 STUN 与本进程在同一台机器上，
  // 若原样回显源端口，映射地址就与本机的 host 候选**完全相同**，libwebrtc 的
  // Port::AddAddress 会把它当重复地址丢掉 —— 于是「STUN 生效了」却一个 srflx 都看不到。
  // 平移 1 正好模拟 NAT 的端口映射，让判据（SDP 里有 typ srflx）真的建立起来。
  uint16_t mapped_port = static_cast<uint16_t>(ntohs(from.sin_port) + 1);
  if (mapped_port == 0) mapped_port = 1;
  const uint16_t xport = static_cast<uint16_t>(mapped_port ^ 0x2112u);
  out[26] = static_cast<unsigned char>((xport >> 8) & 0xFF);
  out[27] = static_cast<unsigned char>(xport & 0xFF);
  const uint32_t xip = ntohl(from.sin_addr.s_addr) ^ kStunMagic;
  out[28] = static_cast<unsigned char>((xip >> 24) & 0xFF);
  out[29] = static_cast<unsigned char>((xip >> 16) & 0xFF);
  out[30] = static_cast<unsigned char>((xip >> 8) & 0xFF);
  out[31] = static_cast<unsigned char>(xip & 0xFF);
  out[32] = 0x80;  // FINGERPRINT
  out[33] = 0x28;
  out[34] = 0x00;
  out[35] = 0x04;
  // RFC 5389 §15.5：CRC 只覆盖「到 FINGERPRINT 属性之前」的字节（含已写好长度的头部），
  // 再与 0x5354554E 异或。多算/少算都会让 libwebrtc 静默丢弃整条响应。
  const uint32_t fingerprint = StunCrc32(out, 32) ^ kFingerprintXor;
  out[36] = static_cast<unsigned char>((fingerprint >> 24) & 0xFF);
  out[37] = static_cast<unsigned char>((fingerprint >> 16) & 0xFF);
  out[38] = static_cast<unsigned char>((fingerprint >> 8) & 0xFF);
  out[39] = static_cast<unsigned char>(fingerprint & 0xFF);
  return 40;
}

bool StartStunServer(uint16_t port, StunServer* server, std::string* error) {
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    *error = "socket() failed: " + std::to_string(WSAGetLastError());
    return false;
  }
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = "bind(0.0.0.0:" + std::to_string(port) + ") failed: " +
             std::to_string(WSAGetLastError());
    ::closesocket(s);
    return false;
  }
  int len = sizeof(addr);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  server->sock = s;
  server->port = ntohs(addr.sin_port);
  server->thread = std::thread([server] {
    unsigned char buf[2048];
    unsigned char out[64];
    while (!server->stop.load()) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(server->sock, &read_set);
      timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 200000;  // 200 ms：让 stop 能及时退出
      if (::select(0, &read_set, nullptr, nullptr, &tv) <= 0) continue;
      sockaddr_in from;
      std::memset(&from, 0, sizeof(from));
      int from_len = sizeof(from);
      const int got = ::recvfrom(server->sock, reinterpret_cast<char*>(buf),
                                 static_cast<int>(sizeof(buf)), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
      if (got <= 0) continue;
      int fingerprint_attrs = 0;
      const int n = BuildStunResponse(buf, got, from, out, &fingerprint_attrs);
      if (n > 0) {
        const int sent = ::sendto(server->sock, reinterpret_cast<const char*>(out), n, 0,
                                  reinterpret_cast<sockaddr*>(&from), from_len);
        const int count = server->requests.fetch_add(1) + 1;
        // 只打印前 8 条，避免刷屏；req fingerprint 属性数说明请求带了指纹。
        if (count <= 8) {
          char ip[INET_ADDRSTRLEN] = {0};
          ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
          std::fprintf(stderr,
                       "[stun] req#%d from %s:%u len=%d fingerprint=%d -> resp %d bytes "
                       "sendto=%d\n",
                       count, ip, ntohs(from.sin_port), got, fingerprint_attrs, n, sent);
        }
      }
    }
  });
  return true;
}

// ---------------------------------------------------------------------------
// 本机非回环 IPv4（候选 socket 通常绑在这类地址上）
// ---------------------------------------------------------------------------
std::vector<std::string> LocalIPv4s() {
  std::vector<std::string> result;
  char host[256];
  if (::gethostname(host, sizeof(host)) != 0) return result;
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* info = nullptr;
  if (::getaddrinfo(host, nullptr, &hints, &info) != 0) return result;
  for (addrinfo* it = info; it != nullptr; it = it->ai_next) {
    char text[INET_ADDRSTRLEN] = {0};
    const auto* sin = reinterpret_cast<sockaddr_in*>(it->ai_addr);
    if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) == nullptr) continue;
    std::string ip(text);
    if (ip.rfind("127.", 0) == 0) continue;
    bool dup = false;
    for (const auto& seen : result) dup = dup || (seen == ip);
    if (!dup) result.push_back(ip);
  }
  ::freeaddrinfo(info);
  return result;
}

// ---------------------------------------------------------------------------
// 引擎与事件
// ---------------------------------------------------------------------------
pu_engine* CreateEngine(const char* name) {
  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = name;
  config.log_level = 3;
  config.event_queue_capacity = 2048;
  return pu_engine_create(&config);
}

// 等到一条 offer，返回它的 payload（JSON 信封）原文。
std::string WaitOffer(pu_engine* engine, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(engine, &event, 200);
    if (rc != PU_OK) continue;
    if (event.type == PU_EVENT_SIGNAL_OUT && event.data.signal.kind == PU_SIGNAL_OFFER &&
        event.data.signal.payload != nullptr) {
      return std::string(event.data.signal.payload, event.data.signal.payload_len);
    }
  }
  return std::string();
}

// payload 是 JSON 信封（SDP 的 CRLF 被转义成两个字符），所以按 "\\r" 截断即可。
std::vector<std::string> CandidateLines(const std::string& payload) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while ((pos = payload.find("a=candidate:", pos)) != std::string::npos) {
    size_t end = payload.find("\\r", pos);
    if (end == std::string::npos) end = payload.size();
    lines.push_back(payload.substr(pos, end - pos));
    pos = end;
  }
  return lines;
}

int CountSrflx(const std::string& payload) {
  int count = 0;
  for (const std::string& line : CandidateLines(payload)) {
    if (line.find("typ srflx") != std::string::npos) ++count;
  }
  return count;
}

void PrintCandidates(const std::string& payload, const char* tag) {
  const std::vector<std::string> lines = CandidateLines(payload);
  std::printf("    [%s] %zu candidate(s):\n", tag, lines.size());
  for (const std::string& line : lines) {
    std::printf("      %s\n", line.c_str());
  }
}

// 建一个引擎 + 设置 ICE servers + 用语音会话发一个 offer，返回 offer payload。
std::string OfferWithStun(const std::string& url, const char* engine_name) {
  pu_engine* engine = CreateEngine(engine_name);
  if (engine == nullptr) return std::string();
  const int rc = pu_engine_set_ice_servers(engine, url.c_str(), url.size());
  if (rc != PU_OK) {
    std::printf("  FAIL: pu_engine_set_ice_servers rc=%d (%s)\n", rc, pu_last_error(engine));
    pu_engine_destroy(engine);
    return std::string();
  }
  pu_voice_params params;
  std::memset(&params, 0, sizeof(params));
  params.struct_size = sizeof(params);
  params.channel_id = "ice-smoke";
  params.mode = PU_MODE_DIRECT;
  if (pu_voice_join(engine, &params) != PU_OK) {
    std::printf("  FAIL: pu_voice_join rc=%s\n", pu_last_error(engine));
    pu_engine_destroy(engine);
    return std::string();
  }
  std::string offer = WaitOffer(engine, 8000);
  pu_voice_leave(engine);
  pu_engine_destroy(engine);
  return offer;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup_media ICE/STUN smoke (v2.12 pu_engine_set_ice_servers)\n");

  WSADATA wsa;
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("RESULT: FAIL (WSAStartup)\n");
    return 1;
  }

  StunServer stun;
  std::string stun_error;
  if (!StartStunServer(3478, &stun, &stun_error)) {
    // 3478 被占就换临时端口继续，不因为端口冲突判失败。
    std::printf("  note: bind 3478 failed (%s), retrying on an ephemeral port\n",
                stun_error.c_str());
    if (!StartStunServer(0, &stun, &stun_error)) {
      std::printf("RESULT: FAIL (cannot start local STUN: %s)\n", stun_error.c_str());
      return 1;
    }
  }
  std::printf("  local STUN server on 0.0.0.0:%u (in-process, RFC 5389 binding)\n", stun.port);

  std::vector<std::string> urls;
  for (const std::string& ip : LocalIPv4s()) {
    urls.push_back("stun:" + ip + ":" + std::to_string(stun.port));
  }
  urls.push_back("stun:127.0.0.1:" + std::to_string(stun.port));
  for (const std::string& u : urls) std::printf("  candidate stun url: %s\n", u.c_str());

  bool ok = true;

  // ---- A/B：设了 STUN → 语音 + 屏幕两条 PC 的 offer 都要有 srflx ----
  std::string working_url;
  std::string voice_offer;
  for (const std::string& url : urls) {
    std::printf("\n---- A: voice offer with %s ----\n", url.c_str());
    voice_offer = OfferWithStun(url, "ice-voice");
    if (!voice_offer.empty()) {
      PrintCandidates(voice_offer, "voice");
      if (CountSrflx(voice_offer) > 0) {
        working_url = url;
        break;
      }
    }
    std::printf("    no srflx candidate with this url, trying next\n");
  }
  if (working_url.empty()) {
    std::printf("FAIL: no reachable STUN url produced an srflx candidate "
                "(tried %zu url(s), stun requests seen by server=%d)\n",
                urls.size(), stun.requests.load());
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  const int voice_srflx = CountSrflx(voice_offer);
  std::printf("  PASS: voice offer has %d srflx candidate(s) (stun requests=%d)\n",
              voice_srflx, stun.requests.load());

  std::printf("\n---- B: screen offer with the same config (second PC creation site) ----\n");
  {
    pu_engine* engine = CreateEngine("ice-screen");
    std::string offer;
    if (engine == nullptr) {
      std::printf("FAIL: engine create\n");
      ok = false;
    } else if (pu_engine_set_ice_servers(engine, working_url.c_str(), working_url.size()) !=
               PU_OK) {
      std::printf("FAIL: set_ice_servers rc=%s\n", pu_last_error(engine));
      ok = false;
    } else if (pu_screen_start(engine, "synthetic") != PU_OK) {
      std::printf("FAIL: pu_screen_start rc=%s\n", pu_last_error(engine));
      ok = false;
    } else {
      offer = WaitOffer(engine, 10000);
      PrintCandidates(offer, "screen");
      const int srflx = CountSrflx(offer);
      if (offer.empty() || srflx == 0) {
        std::printf("FAIL: screen offer has no srflx candidate (offer=%zu bytes)\n",
                    offer.size());
        ok = false;
      } else {
        std::printf("  PASS: screen offer has %d srflx candidate(s)\n", srflx);
      }
      pu_screen_stop(engine);
    }
    pu_engine_destroy(engine);
  }

  // ---- C：不设置 → 仍然只有 host 候选（v2.11 行为不变） ----
  std::printf("\n---- C: without set_ice_servers the offer must stay host-only ----\n");
  {
    pu_engine* engine = CreateEngine("ice-none");
    std::string offer;
    if (engine != nullptr) {
      pu_voice_params params;
      std::memset(&params, 0, sizeof(params));
      params.struct_size = sizeof(params);
      params.channel_id = "ice-smoke";
      params.mode = PU_MODE_DIRECT;
      if (pu_voice_join(engine, &params) == PU_OK) offer = WaitOffer(engine, 8000);
      pu_voice_leave(engine);
      pu_engine_destroy(engine);
    }
    PrintCandidates(offer, "none");
    const int srflx = CountSrflx(offer);
    if (offer.empty() || srflx != 0) {
      std::printf("FAIL: default offer should have 0 srflx candidates, got %d\n", srflx);
      ok = false;
    } else {
      std::printf("  PASS: default offer has 0 srflx candidate(s) (host-only)\n");
    }
  }

  // ---- D：参数校验 ----
  std::printf("\n---- D: argument validation ----\n");
  {
    pu_engine* engine = CreateEngine("ice-argv");
    struct Case {
      const char* urls;
      size_t len;
      int want;
      const char* label;
    };
    const std::string multi = "stun:1.2.3.4:3478\n\r\n stun:5.6.7.8:3479 \n";
    const Case cases[] = {
        {"turn:1.2.3.4:3478", 16, PU_ERR_INVALID_ARG, "turn: rejected"},
        {"turns:1.2.3.4:5349", 17, PU_ERR_INVALID_ARG, "turns: rejected"},
        {"stun:noport", 11, PU_ERR_INVALID_ARG, "missing port rejected"},
        {"", 0, PU_OK, "empty string clears"},
        {nullptr, 0, PU_OK, "nullptr clears"},
        {multi.c_str(), multi.size(), PU_OK, "multi-line list accepted"},
    };
    for (const Case& c : cases) {
      const int rc = pu_engine_set_ice_servers(engine, c.urls, c.len);
      const bool pass = rc == c.want;
      std::printf("  %-28s rc=%d (want %d) %s\n", c.label, rc, c.want, pass ? "OK" : "FAIL");
      if (!pass) ok = false;
      if (rc != PU_OK) std::printf("      last_error: %s\n", pu_last_error(engine));
    }
    if (pu_engine_set_ice_servers(nullptr, "stun:1.2.3.4:3478", 19) != PU_ERR_INVALID_ARG) {
      std::printf("  FAIL: nullptr engine should be PU_ERR_INVALID_ARG\n");
      ok = false;
    } else {
      std::printf("  %-28s rc=PU_ERR_INVALID_ARG OK\n", "nullptr engine");
    }
    if (pu_engine_set_ice_servers(engine, nullptr, 5) != PU_ERR_INVALID_ARG) {
      std::printf("  FAIL: nullptr urls with len>0 should be PU_ERR_INVALID_ARG\n");
      ok = false;
    } else {
      std::printf("  %-28s rc=PU_ERR_INVALID_ARG OK\n", "nullptr+len>0");
    }
    pu_engine_destroy(engine);
  }

  std::printf("\n==== RESULT ====\n");
  std::printf("  stun url that worked: %s\n", working_url.c_str());
  std::printf("  srflx in voice offer: %d | stun requests served: %d\n", voice_srflx,
              stun.requests.load());
  std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
  ::WSACleanup();
  return ok ? 0 : 1;
}
