// Spike 2 第三步：同一进程内建两个 PeerConnection，交换 offer/answer，确认 ICE + DTLS 真的建起来。
//
// 前两步只证明了「静态库能链接」「工厂和连接对象能创建」；这一步才真正跑媒体链路：
// ICE 连通性检查（UDP 回环）+ DTLS 握手 + SCTP 数据通道。
//
// 线程模型（这一步最容易踩坑的地方）：
//   * PeerConnection 的方法要在它所属的 signaling 线程上调用，所以主线程把每一步
//     用 signaling->BlockingCall(...) 推进过去；
//   * 回调（CreateSessionDescriptionObserver 等）只写标志位 + notify，绝不在回调里
//     再阻塞等待 signaling 线程上的另一个任务 —— 那样会死锁；
//   * 主线程只做条件变量等待、轮询和打印，不持任何 webrtc 锁。
//
// M152 要点：
//   * rtc:: 命名空间已并入 webrtc::
//   * 建连接用 CreatePeerConnectionOrError（返回 RTCErrorOr），error().message() 是 const char*
//   * 连接状态读 peer_connection_state()（旧版本叫 connection_state()）
//   * OnIceCandidate 的参数是 webrtc::IceCandidate（不再是 IceCandidateInterface）
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/webrtc_sdp.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/thread.h"

namespace {

using PeerConnection = webrtc::PeerConnectionInterface;

constexpr int kSdpTimeoutMs = 10000;      // CreateOffer / CreateAnswer
constexpr int kSetSdpTimeoutMs = 10000;   // SetLocal / SetRemote
constexpr int kGatherTimeoutMs = 10000;   // ICE 收集
constexpr int kConnectTimeoutMs = 15000;  // ICE 连通 + DTLS

// 一次异步操作的结果。回调只往里写，主线程只从里读。
struct Event {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string sdp;
  std::string error;
};

// 等一个 Event，最多 timeout_ms 毫秒；返回是否在超时前完成。
bool WaitEvent(Event* event, int timeout_ms) {
  std::unique_lock<std::mutex> lock(event->mu);
  return event->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [event] { return event->done; });
}

void Finish(Event* event, bool ok, std::string sdp, std::string error) {
  {
    std::lock_guard<std::mutex> lock(event->mu);
    event->ok = ok;
    event->sdp = std::move(sdp);
    event->error = std::move(error);
    event->done = true;
  }
  event->cv.notify_all();
}

// CreateOffer / CreateAnswer 的回调：把 SDP 序列化成字符串就完事。
// 回调转移 desc 的所有权，序列化完直接 delete，避免对象跨线程传递。
class CreateObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateObserver(Event* event) : event_(event) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::string sdp = webrtc::SdpSerialize(*desc);
    delete desc;
    Finish(event_, true, std::move(sdp), {});
  }

  void OnFailure(webrtc::RTCError error) override {
    Finish(event_, false, {}, error.message());
  }

 private:
  Event* const event_;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit SetLocalObserver(Event* event) : event_(event) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    Finish(event_, error.ok(), {},
           error.ok() ? std::string() : std::string(error.message()));
  }

 private:
  Event* const event_;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit SetRemoteObserver(Event* event) : event_(event) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    Finish(event_, error.ok(), {},
           error.ok() ? std::string() : std::string(error.message()));
  }

 private:
  Event* const event_;
};

std::string StateName(PeerConnection::SignalingState state) {
  return std::string(PeerConnection::AsString(state));
}
std::string StateName(PeerConnection::IceConnectionState state) {
  return std::string(PeerConnection::AsString(state));
}
std::string StateName(PeerConnection::IceGatheringState state) {
  return std::string(PeerConnection::AsString(state));
}
std::string StateName(PeerConnection::PeerConnectionState state) {
  return std::string(PeerConnection::AsString(state));
}

// 收到的 ICE 候选。只存 mid / mline / 候选行文本（IceCandidate 不可拷贝，
// 需要时再 Create 一份），这样跨线程传递最省事。
struct CandidateCopy {
  std::string mid;
  int mline_index = 0;
  std::string sdp;
};

class PcObserver final : public webrtc::PeerConnectionObserver {
 public:
  explicit PcObserver(std::string name) : name_(std::move(name)) {}

  void OnSignalingChange(PeerConnection::SignalingState state) override {
    std::printf("[%s] signaling_state=%s\n", name_.c_str(), StateName(state).c_str());
  }

  void OnIceConnectionChange(PeerConnection::IceConnectionState state) override {
    std::printf("[%s] ice_connection_state=%s\n", name_.c_str(), StateName(state).c_str());
  }

  void OnConnectionChange(PeerConnection::PeerConnectionState state) override {
    std::printf("[%s] connection_state=%s\n", name_.c_str(), StateName(state).c_str());
  }

  void OnIceGatheringChange(PeerConnection::IceGatheringState state) override {
    std::printf("[%s] ice_gathering_state=%s\n", name_.c_str(), StateName(state).c_str());
    if (state == PeerConnection::kIceGatheringComplete) {
      gathered_.store(true);
    }
  }

  void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
    if (candidate == nullptr) return;
    CandidateCopy copy;
    copy.mid = candidate->sdp_mid();
    copy.mline_index = candidate->sdp_mline_index();
    copy.sdp = candidate->ToString();
    {
      std::lock_guard<std::mutex> lock(mu_);
      candidates_.push_back(std::move(copy));
    }
    std::printf("[%s] ice_candidate: %s\n", name_.c_str(), candidate->ToString().c_str());
  }

  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
    std::printf("[%s] remote data channel: %s\n", name_.c_str(),
                channel != nullptr ? channel->label().c_str() : "(null)");
  }

  void OnRenegotiationNeeded() override {}

  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface>,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {}

  bool gathered() const { return gathered_.load(); }

  std::vector<CandidateCopy> Candidates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return candidates_;
  }

 private:
  const std::string name_;
  std::atomic<bool> gathered_{false};
  mutable std::mutex mu_;
  std::vector<CandidateCopy> candidates_;
};

// 把候选行补进 SDP。libwebrtc 的 local_description() 会不会自动带上已收集的候选
// 取决于版本；这里先序列化，再把「SDP 里还没有的」补进一个 Clone，两种情况都正确。
std::string SdpWithCandidates(PeerConnection* pc, const PcObserver& observer, const char* tag) {
  const webrtc::SessionDescriptionInterface* local = pc->local_description();
  if (local == nullptr) return {};
  const std::string base = webrtc::SdpSerialize(*local);

  auto desc = local->Clone();
  int added = 0;
  int already = 0;
  for (const CandidateCopy& candidate : observer.Candidates()) {
    if (base.find(candidate.sdp) != std::string::npos) {
      ++already;
      continue;
    }
    auto parsed =
        webrtc::IceCandidate::Create(candidate.mid, candidate.mline_index, candidate.sdp, nullptr);
    if (parsed != nullptr && desc->AddCandidate(parsed.get())) {
      ++added;
    }
  }

  std::string out = webrtc::SdpSerialize(*desc);
  int candidate_lines = 0;
  for (size_t pos = 0; (pos = out.find("a=candidate:", pos)) != std::string::npos; ++pos) {
    ++candidate_lines;
  }
  std::printf(
      "[%s] sdp ready: %zu bytes, %d a=candidate lines (%d attached here, %d already present)\n",
      tag, out.size(), candidate_lines, added, already);
  return out;
}

bool WaitGathered(const PcObserver& observer, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (observer.gathered()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return observer.gathered();
}

struct Snapshot {
  PeerConnection::IceConnectionState ice = PeerConnection::kIceConnectionNew;
  PeerConnection::PeerConnectionState connection = PeerConnection::PeerConnectionState::kNew;
};

}  // namespace

int main() {
  // 静态链接时，libwebrtc 里那个负责 WSAStartup 的全局 WinsockInitializer 会随
  // 「没人引用的目标文件」一起被链接器丢掉，于是所有 socket() 都失败：
  // 症状是只收到 TCP active 候选、一条 UDP 候选都没有，ICE 永远停在 checking。
  // 自己初始化一次，绕开静态库的全局初始化丢弃问题。
  webrtc::WinsockInitializer winsock_init;
  if (winsock_init.error() != 0) {
    std::printf("WSAStartup FAILED: %d\n", winsock_init.error());
    return 1;
  }
  std::printf("winsock OK\n");

  // ---- 0. 三条线程 + 工厂 ----
  auto network = webrtc::Thread::CreateWithSocketServer();
  auto worker = webrtc::Thread::Create();
  auto signaling = webrtc::Thread::Create();
  if (!network->Start() || !worker->Start() || !signaling->Start()) {
    std::printf("thread start FAILED\n");
    return 1;
  }
  std::printf("threads started\n");

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network.get();
  deps.worker_thread = worker.get();
  deps.signaling_thread = signaling.get();

  auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (factory == nullptr) {
    std::printf("factory NULL\n");
    return 1;
  }
  std::printf("factory OK\n");

  // ---- 1. 两个 PeerConnection，各带 observer ----
  PcObserver obs1("pc1");
  PcObserver obs2("pc2");
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  // 同进程回环，不需要 STUN/TURN；默认只会收集 host 候选。

  webrtc::scoped_refptr<PeerConnection> pc1;
  webrtc::scoped_refptr<PeerConnection> pc2;
  std::string error;

  signaling->BlockingCall([&] {
    webrtc::PeerConnectionDependencies pc1_deps(&obs1);
    auto created1 = factory->CreatePeerConnectionOrError(config, std::move(pc1_deps));
    if (!created1.ok()) {
      error = std::string("pc1: ") + created1.error().message();
      return;
    }
    pc1 = created1.value();

    webrtc::PeerConnectionDependencies pc2_deps(&obs2);
    auto created2 = factory->CreatePeerConnectionOrError(config, std::move(pc2_deps));
    if (!created2.ok()) {
      error = std::string("pc2: ") + created2.error().message();
      return;
    }
    pc2 = created2.value();
  });

  if (pc1 == nullptr || pc2 == nullptr) {
    std::printf("peer connection FAILED: %s\n", error.c_str());
    return 1;
  }
  std::printf("peer connection OK (pc1, pc2)\n");

  // ---- 2. PC1 建 DataChannel（保证 SCTP/DTLS 有 m= 段落），然后 CreateOffer ----
  webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel;
  signaling->BlockingCall([&] {
    auto created = pc1->CreateDataChannelOrError("smoke", nullptr);
    if (!created.ok()) {
      error = std::string("data channel: ") + created.error().message();
      return;
    }
    data_channel = created.value();
  });
  if (data_channel == nullptr) {
    std::printf("data channel FAILED: %s\n", error.c_str());
    return 1;
  }
  std::printf("data channel created: %s\n", data_channel->label().c_str());

  Event offer_event;
  auto offer_observer = webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>(
      new webrtc::RefCountedObject<CreateObserver>(&offer_event));
  signaling->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    pc1->CreateOffer(offer_observer.get(), options);
  });
  if (!WaitEvent(&offer_event, kSdpTimeoutMs) || !offer_event.ok) {
    std::printf("offer FAILED: %s\n", offer_event.done ? offer_event.error.c_str() : "timeout");
    return 1;
  }
  const std::string offer_sdp = offer_event.sdp;
  std::printf("offer generated: %zu bytes\n", offer_sdp.size());

  // ---- 3. PC1 SetLocalDescription，等 ICE 收集完成 ----
  Event pc1_local_event;
  auto pc1_local_observer = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
      new webrtc::RefCountedObject<SetLocalObserver>(&pc1_local_event));
  std::string parse_error;
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_sdp, &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc1->SetLocalDescription(std::move(desc), pc1_local_observer);
  });
  if (!parse_error.empty()) {
    std::printf("parse offer FAILED: %s\n", parse_error.c_str());
    return 1;
  }
  if (!WaitEvent(&pc1_local_event, kSetSdpTimeoutMs) || !pc1_local_event.ok) {
    std::printf("pc1 SetLocalDescription FAILED: %s\n",
                pc1_local_event.done ? pc1_local_event.error.c_str() : "timeout");
    return 1;
  }
  std::printf("pc1 local description set\n");

  if (!WaitGathered(obs1, kGatherTimeoutMs)) {
    std::printf("pc1 ICE gathering TIMEOUT\n");
  }
  std::printf("pc1 ICE gathering %s\n", obs1.gathered() ? "complete" : "INCOMPLETE");

  const std::string offer_with_candidates =
      signaling->BlockingCall([&] { return SdpWithCandidates(pc1.get(), obs1, "pc1"); });
  if (offer_with_candidates.empty()) {
    std::printf("pc1 local_description NULL\n");
    return 1;
  }

  // ---- 4. PC2 SetRemoteDescription(offer) → CreateAnswer → SetLocalDescription ----
  Event pc2_remote_event;
  auto pc2_remote_observer = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
      new webrtc::RefCountedObject<SetRemoteObserver>(&pc2_remote_event));
  parse_error.clear();
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    auto desc =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_with_candidates, &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc2->SetRemoteDescription(std::move(desc), pc2_remote_observer);
  });
  if (!parse_error.empty()) {
    std::printf("pc2 parse offer FAILED: %s\n", parse_error.c_str());
    return 1;
  }
  if (!WaitEvent(&pc2_remote_event, kSetSdpTimeoutMs) || !pc2_remote_event.ok) {
    std::printf("pc2 SetRemoteDescription FAILED: %s\n",
                pc2_remote_event.done ? pc2_remote_event.error.c_str() : "timeout");
    return 1;
  }
  std::printf("pc2 remote description set\n");

  Event answer_event;
  auto answer_observer = webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>(
      new webrtc::RefCountedObject<CreateObserver>(&answer_event));
  signaling->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    pc2->CreateAnswer(answer_observer.get(), options);
  });
  if (!WaitEvent(&answer_event, kSdpTimeoutMs) || !answer_event.ok) {
    std::printf("answer FAILED: %s\n", answer_event.done ? answer_event.error.c_str() : "timeout");
    return 1;
  }
  const std::string answer_sdp = answer_event.sdp;
  std::printf("answer generated: %zu bytes\n", answer_sdp.size());

  Event pc2_local_event;
  auto pc2_local_observer = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
      new webrtc::RefCountedObject<SetLocalObserver>(&pc2_local_event));
  parse_error.clear();
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, answer_sdp, &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc2->SetLocalDescription(std::move(desc), pc2_local_observer);
  });
  if (!parse_error.empty()) {
    std::printf("parse answer FAILED: %s\n", parse_error.c_str());
    return 1;
  }
  if (!WaitEvent(&pc2_local_event, kSetSdpTimeoutMs) || !pc2_local_event.ok) {
    std::printf("pc2 SetLocalDescription FAILED: %s\n",
                pc2_local_event.done ? pc2_local_event.error.c_str() : "timeout");
    return 1;
  }
  std::printf("pc2 local description set\n");

  if (!WaitGathered(obs2, kGatherTimeoutMs)) {
    std::printf("pc2 ICE gathering TIMEOUT\n");
  }
  std::printf("pc2 ICE gathering %s\n", obs2.gathered() ? "complete" : "INCOMPLETE");

  const std::string answer_with_candidates =
      signaling->BlockingCall([&] { return SdpWithCandidates(pc2.get(), obs2, "pc2"); });
  if (answer_with_candidates.empty()) {
    std::printf("pc2 local_description NULL\n");
    return 1;
  }

  // ---- 5. answer 回填给 PC1 ----
  Event pc1_remote_event;
  auto pc1_remote_observer = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
      new webrtc::RefCountedObject<SetRemoteObserver>(&pc1_remote_event));
  parse_error.clear();
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, answer_with_candidates,
                                                 &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc1->SetRemoteDescription(std::move(desc), pc1_remote_observer);
  });
  if (!parse_error.empty()) {
    std::printf("pc1 parse answer FAILED: %s\n", parse_error.c_str());
    return 1;
  }
  if (!WaitEvent(&pc1_remote_event, kSetSdpTimeoutMs) || !pc1_remote_event.ok) {
    std::printf("pc1 SetRemoteDescription FAILED: %s\n",
                pc1_remote_event.done ? pc1_remote_event.error.c_str() : "timeout");
    return 1;
  }
  std::printf("pc1 remote description set\n");

  // ---- 6. 轮询两边的 ICE / 连接状态，等 DTLS 建起来 ----
  // peer_connection_state() == kConnected 意味着 ICE 已连通且 DTLS 握手完成。
  bool dtls_connected = false;
  bool failed = false;
  Snapshot last1;
  Snapshot last2;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    Snapshot s1;
    Snapshot s2;
    signaling->BlockingCall([&] {
      s1.ice = pc1->ice_connection_state();
      s1.connection = pc1->peer_connection_state();
      s2.ice = pc2->ice_connection_state();
      s2.connection = pc2->peer_connection_state();
    });

    if (s1.ice != last1.ice || s2.ice != last2.ice) {
      std::printf("poll: pc1 ice=%s  pc2 ice=%s\n", StateName(s1.ice).c_str(),
                  StateName(s2.ice).c_str());
      last1.ice = s1.ice;
      last2.ice = s2.ice;
    }

    dtls_connected = s1.connection == PeerConnection::PeerConnectionState::kConnected &&
                     s2.connection == PeerConnection::PeerConnectionState::kConnected;
    failed = s1.connection == PeerConnection::PeerConnectionState::kFailed ||
             s2.connection == PeerConnection::PeerConnectionState::kFailed;

    if (dtls_connected || failed) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // 数据通道打开是 SCTP over DTLS 已经跑通的直接证据，最多再等 3 秒。
  if (dtls_connected && data_channel->state() != webrtc::DataChannelInterface::kOpen) {
    const auto dc_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < dc_deadline &&
           data_channel->state() != webrtc::DataChannelInterface::kOpen) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  Snapshot final1;
  Snapshot final2;
  signaling->BlockingCall([&] {
    final1.ice = pc1->ice_connection_state();
    final1.connection = pc1->peer_connection_state();
    final2.ice = pc2->ice_connection_state();
    final2.connection = pc2->peer_connection_state();
  });

  std::printf("final state:\n");
  std::printf("  pc1 ice=%s connection=%s\n", StateName(final1.ice).c_str(),
              StateName(final1.connection).c_str());
  std::printf("  pc2 ice=%s connection=%s\n", StateName(final2.ice).c_str(),
              StateName(final2.connection).c_str());
  std::printf("  data channel state=%s\n",
              webrtc::DataChannelInterface::DataStateString(data_channel->state()));

  const bool success =
      final1.connection == PeerConnection::PeerConnectionState::kConnected &&
      final2.connection == PeerConnection::PeerConnectionState::kConnected;

  // ---- 7. 收尾：先 Close 再释放，最后按 signaling → worker → network 停线程 ----
  signaling->BlockingCall([&] {
    if (pc1 != nullptr) pc1->Close();
    if (pc2 != nullptr) pc2->Close();
  });
  data_channel = nullptr;
  pc1 = nullptr;
  pc2 = nullptr;
  factory = nullptr;

  signaling->Stop();
  worker->Stop();
  network->Stop();

  if (success) {
    std::printf("DTLS connected\n");
    return 0;
  }
  std::printf("DTLS NOT connected (timeout or failure), see states above\n");
  return 1;
}
