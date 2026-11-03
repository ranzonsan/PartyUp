// PartyUp 语音 spike（A：1 对 1 直连）——先证明「真 WASAPI ADM + libwebrtc 自带
// Opus 编解码 + 同进程双 PeerConnection」这条链路能通，再动生产代码。
//
// 与 media-link 那个视频 spike 的区别：
//   * deps.adm 注入**真实** Core Audio ADM（CreateAudioDeviceModule + kPlatformDefaultAudio），
//     而不是生产代码里那个 NullAudioDeviceModule —— 这正是「接语音」的全部开关；
//   * 没有视频轨，只有一条音频轨；A 端采集（真实麦克风），B 端解出 PCM；
//   * B 端用 AudioTrackSinkInterface 数帧，并用 absolute_capture_timestamp_ms
//     算端到端延迟（采集时刻 → 本进程拿到解码 PCM 的时刻）。
//
// 判据（PASS 条件）：B 端解码出 PCM 帧 > 20 且端到端延迟 < 150 ms。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/webrtc_sdp.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ref_counted_object.h"
#include "api/scoped_refptr.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "rtc_base/thread.h"
#include "rtc_base/win32_socket_init.h"

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::atomic<int> g_audio_frames{0};
std::atomic<int64_t> g_pcm_bytes{0};
std::string g_sdp_out;                       // CreateOffer/Answer 的序列化结果
webrtc::SdpType g_sdp_type = webrtc::SdpType::kOffer;
bool g_sdp_ok = false;
std::string g_sdp_error;
std::atomic<int64_t> g_latency_sum_ms{0};
std::atomic<int64_t> g_latency_max_ms{0};
std::atomic<int> g_latency_samples{0};

// B 端解码后的 PCM 就落到这里。
class AudioCounterSink : public webrtc::AudioTrackSinkInterface {
 public:
  void OnData(const void* /*audio_data*/,
              int bits_per_sample,
              int sample_rate,
              size_t number_of_channels,
              size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override {
    g_audio_frames.fetch_add(1, std::memory_order_relaxed);
    g_pcm_bytes.fetch_add(static_cast<int64_t>(number_of_frames) *
                              static_cast<int64_t>(number_of_channels) * 2,
                          std::memory_order_relaxed);
    if (g_audio_frames.load(std::memory_order_relaxed) == 1) {
      std::printf("  [B] first decoded PCM: %d Hz, %zu ch, %zu frames, bits=%d, "
                  "capture_ts=%s\n",
                  sample_rate, number_of_channels, number_of_frames, bits_per_sample,
                  absolute_capture_timestamp_ms.has_value() ? "yes" : "no");
    }
    if (absolute_capture_timestamp_ms.has_value()) {
      const int64_t latency = NowMs() - *absolute_capture_timestamp_ms;
      if (latency >= 0 && latency < 5000) {
        g_latency_sum_ms.fetch_add(latency, std::memory_order_relaxed);
        g_latency_samples.fetch_add(1, std::memory_order_relaxed);
        int64_t current = g_latency_max_ms.load(std::memory_order_relaxed);
        while (latency > current &&
               !g_latency_max_ms.compare_exchange_weak(current, latency)) {
        }
      }
    }
  }
};

class Semaphore {
 public:
  void Signal() {
    std::lock_guard<std::mutex> lock(mu_);
    signaled_ = true;
    cv_.notify_all();
  }
  bool Wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return signaled_; });
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool signaled_ = false;
};

class SdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit SdpObserver(Semaphore* done) : done_(done) {}
  void AddRef() const override { refs_.fetch_add(1, std::memory_order_relaxed); }
  webrtc::RefCountReleaseStatus Release() const override {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
      return webrtc::RefCountReleaseStatus::kDroppedLastRef;
    }
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // m152 里 desc->ToString() 不再写 SDP，改用 api/webrtc_sdp.h 的 SdpSerialize；
    // 回调转移 desc 的所有权，序列化完直接 delete。
    // 结果写进程级全局：CreateSessionDescriptionObserver 由 libwebrtc AddRef/Release，
    // 回调返回后本对象可能已被释放，读成员是 UB（实测会读到 0 字节 SDP）。
    g_sdp_out = webrtc::SdpSerialize(*desc);
    g_sdp_type = desc->GetType();
    delete desc;
    std::printf("  [sdp] type=%d len=%zu\n", static_cast<int>(g_sdp_type), g_sdp_out.size());
    g_sdp_ok = true;
    done_->Signal();
  }
  void OnFailure(webrtc::RTCError error) override {
    g_sdp_error = std::string(error.message());
    g_sdp_ok = false;
    done_->Signal();
  }
  bool success() const { return success_; }
  const std::string& sdp() const { return sdp_; }
  const std::string& type() const { return type_; }
  webrtc::SdpType type_enum() const { return type_enum_; }
  const std::string& error() const { return error_; }

 private:
  Semaphore* done_;
  bool success_ = false;
  std::string sdp_;
  std::string type_;
  webrtc::SdpType type_enum_ = webrtc::SdpType::kOffer;
  std::string error_;
  mutable std::atomic<int> refs_{0};
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit SetLocalObserver(Semaphore* done) : done_(done) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    message_ = std::string(error.message());
    done_->Signal();
  }
  bool ok() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  Semaphore* done_;
  bool ok_ = false;
  std::string message_;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit SetRemoteObserver(Semaphore* done) : done_(done) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    message_ = std::string(error.message());
    done_->Signal();
  }
  bool ok() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  Semaphore* done_;
  bool ok_ = false;
  std::string message_;
};

struct PcObserver : public webrtc::PeerConnectionObserver {
  PcObserver(const char* name, AudioCounterSink* sink) : name_(name), sink_(sink) {}

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState /*state*/) override {}
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> /*channel*/) override {}
  void OnRenegotiationNeeded() override {}
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState /*state*/) override {}

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string sdp;
    candidate->ToString(&sdp);
    std::lock_guard<std::mutex> lock(cand_mu_);
    candidates_.push_back(sdp);
    std::printf("  [%s] candidate: %s\n", name_.c_str(), sdp.c_str());
  }

  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    std::printf("  [%s] ICE state -> %d\n", name_.c_str(), static_cast<int>(state));
  }

  void OnTrack(
      webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    if (transceiver == nullptr || transceiver->receiver() == nullptr) return;
    auto track = transceiver->receiver()->track();
    if (track == nullptr) return;
    if (track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) return;
    auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
    std::printf("  [%s] remote audio track arrived: %s\n", name_.c_str(),
                audio_track->id().c_str());
    if (sink_ != nullptr) audio_track->AddSink(sink_);
  }

  std::vector<std::string> TakeCandidates() {
    std::lock_guard<std::mutex> lock(cand_mu_);
    std::vector<std::string> out;
    out.swap(candidates_);
    return out;
  }

  std::string name_;
  AudioCounterSink* sink_ = nullptr;
  std::mutex cand_mu_;
  std::vector<std::string> candidates_;
};

bool AddCandidates(webrtc::PeerConnectionInterface* pc,
                   const std::vector<std::string>& candidates) {
  bool all_ok = true;
  for (const std::string& sdp : candidates) {
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate("0", 0, sdp, nullptr));
    if (candidate == nullptr) {
      std::printf("  candidate parse failed: %s\n", sdp.c_str());
      all_ok = false;
      continue;
    }
    if (!pc->AddIceCandidate(candidate.get())) all_ok = false;
  }
  return all_ok;
}

webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> CreateFactory(
    const webrtc::Environment& env,
    webrtc::Thread* network,
    webrtc::Thread* worker,
    webrtc::Thread* signaling,
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm,
    const char* label) {
  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network;
  deps.worker_thread = worker;
  deps.signaling_thread = signaling;
  deps.env = env;

  deps.adm = adm;
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.audio_mixer = webrtc::AudioMixerImpl::Create();
  deps.audio_processing_builder =
      std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();

  // 媒体默认关闭：必须显式 EnableMedia，否则 media_factory 为 null、音频不出包。
  webrtc::EnableMedia(deps);
  auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  std::printf("  factory[%s]: %s\n", label, factory ? "OK" : "NULL");
  return factory;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup voice spike · 1:1 direct, real WASAPI ADM\n");

  webrtc::WinsockInitializer winsock;
  if (winsock.error() != 0) {
    std::printf("WSAStartup FAILED: %d\n", winsock.error());
    return 1;
  }
  std::printf("  winsock OK\n");

  auto network = webrtc::Thread::CreateWithSocketServer();
  auto worker = webrtc::Thread::Create();
  auto signaling = webrtc::Thread::Create();
  if (!network->Start() || !worker->Start() || !signaling->Start()) {
    std::printf("thread start FAILED\n");
    return 1;
  }

  const webrtc::Environment env = webrtc::CreateEnvironment();

  // ---- 1. 两个真 ADM（A 采、B 放）----
  auto adm_a = webrtc::CreateAudioDeviceModule(
      env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  auto adm_b = webrtc::CreateAudioDeviceModule(
      env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  std::printf("  adm A: %s, adm B: %s\n", adm_a ? "OK" : "NULL", adm_b ? "OK" : "NULL");
  if (!adm_a || !adm_b) {
    std::printf("RESULT: FAIL（真 ADM 创建失败）\n");
    return 2;
  }

  auto factory_a = CreateFactory(env, network.get(), worker.get(), signaling.get(),
                                 adm_a, "A/sender");
  auto factory_b = CreateFactory(env, network.get(), worker.get(), signaling.get(),
                                 adm_b, "B/receiver");
  if (!factory_a || !factory_b) {
    std::printf("RESULT: FAIL（工厂创建失败）\n");
    return 2;
  }

  // ---- 2. 两个 PeerConnection ----
  AudioCounterSink audio_sink;  // B 端：解码后的 PCM
  PcObserver obs1("pc1/A", nullptr);
  PcObserver obs2("pc2/B", &audio_sink);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  // libwebrtc 的 PeerConnection 方法有 signaling 线程亲和性：照 media-link spike
  // 的做法，PC/轨道相关调用全部投到 signaling 线程执行。
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc1;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc2;
  std::string pc_error;
  signaling->BlockingCall([&] {
    webrtc::PeerConnectionDependencies pc1_deps(&obs1);
    auto r1 = factory_a->CreatePeerConnectionOrError(config, std::move(pc1_deps));
    if (!r1.ok()) {
      pc_error = std::string("pc1: ") + r1.error().message();
      return;
    }
    pc1 = r1.value();

    webrtc::PeerConnectionDependencies pc2_deps(&obs2);
    auto r2 = factory_b->CreatePeerConnectionOrError(config, std::move(pc2_deps));
    if (!r2.ok()) {
      pc_error = std::string("pc2: ") + r2.error().message();
      return;
    }
    pc2 = r2.value();
  });
  if (pc1 == nullptr || pc2 == nullptr) {
    std::printf("RESULT: FAIL（PC 创建失败: %s）\n", pc_error.c_str());
    return 2;
  }
  std::printf("  pc1/pc2 created\n");

  // ---- 3. A 端加音频轨（真麦克风）----
  webrtc::AudioOptions audio_options;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track;
  std::string track_error;
  signaling->BlockingCall([&] {
    audio_source = factory_a->CreateAudioSource(audio_options);
    audio_track = factory_a->CreateAudioTrack("audio0", audio_source.get());
    if (audio_track == nullptr) {
      track_error = "CreateAudioTrack returned null";
      return;
    }
    auto add_result = pc1->AddTrack(audio_track, {"stream0"});
    if (!add_result.ok()) track_error = add_result.error().message();
  });
  if (audio_track == nullptr || !track_error.empty()) {
    std::printf("RESULT: FAIL（AudioTrack/AddTrack: %s）\n", track_error.c_str());
    return 2;
  }
  std::printf("  A: audio track added\n");

  // ---- 4. offer / answer ----
  Semaphore offer_done, setlocal1_done, setremote2_done, answer_done, setlocal2_done,
      setremote1_done;

  auto* offer_observer = new SdpObserver(&offer_done);
  pc1->CreateOffer(offer_observer,
                   webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  if (!offer_done.Wait(10000) || !g_sdp_ok) {
    std::printf("RESULT: FAIL（CreateOffer: %s）\n", g_sdp_error.c_str());
    return 2;
  }
  std::printf("  offer created (%zu bytes)\n", g_sdp_out.size());

  auto* setlocal1_obs = new webrtc::RefCountedObject<SetLocalObserver>(&setlocal1_done);
  auto offer = webrtc::CreateSessionDescription(g_sdp_type,
                                                g_sdp_out);
  if (offer == nullptr) {
    std::printf("RESULT: FAIL（offer 解析失败）\n");
    return 2;
  }
  pc1->SetLocalDescription(
      std::move(offer),
      webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(setlocal1_obs));
  if (!setlocal1_done.Wait(10000) || !setlocal1_obs->ok()) {
    std::printf("RESULT: FAIL（pc1 SetLocalDescription: %s）\n",
                setlocal1_obs->message().c_str());
    return 2;
  }

  auto* setremote2_obs = new webrtc::RefCountedObject<SetRemoteObserver>(&setremote2_done);
  auto offer_for_2 = webrtc::CreateSessionDescription(g_sdp_type,
                                                      g_sdp_out);
  pc2->SetRemoteDescription(
      std::move(offer_for_2),
      webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
          setremote2_obs));
  if (!setremote2_done.Wait(10000) || !setremote2_obs->ok()) {
    std::printf("RESULT: FAIL（pc2 SetRemoteDescription: %s）\n",
                setremote2_obs->message().c_str());
    return 2;
  }
  std::printf("  pc2: remote offer applied\n");

  auto* answer_observer = new SdpObserver(&answer_done);
  pc2->CreateAnswer(answer_observer,
                    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  if (!answer_done.Wait(10000) || !g_sdp_ok) {
    std::printf("RESULT: FAIL（CreateAnswer: %s）\n", g_sdp_error.c_str());
    return 2;
  }
  std::printf("  answer created (%zu bytes)\n", g_sdp_out.size());

  auto* setlocal2_obs = new webrtc::RefCountedObject<SetLocalObserver>(&setlocal2_done);
  auto answer = webrtc::CreateSessionDescription(g_sdp_type,
                                                 g_sdp_out);
  pc2->SetLocalDescription(
      std::move(answer),
      webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(setlocal2_obs));
  if (!setlocal2_done.Wait(10000) || !setlocal2_obs->ok()) {
    std::printf("RESULT: FAIL（pc2 SetLocalDescription: %s）\n",
                setlocal2_obs->message().c_str());
    return 2;
  }

  auto* setremote1_obs = new webrtc::RefCountedObject<SetRemoteObserver>(&setremote1_done);
  auto answer_for_1 = webrtc::CreateSessionDescription(g_sdp_type,
                                                       g_sdp_out);
  pc1->SetRemoteDescription(
      std::move(answer_for_1),
      webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
          setremote1_obs));
  if (!setremote1_done.Wait(10000) || !setremote1_obs->ok()) {
    std::printf("RESULT: FAIL（pc1 SetRemoteDescription: %s）\n",
                setremote1_obs->message().c_str());
    return 2;
  }
  std::printf("  signaling done: offer/answer applied on both sides\n");

  // ---- 5. 候选补投（同进程 host 候选，SDP 里已经带了一部分）----
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  AddCandidates(pc1.get(), obs2.TakeCandidates());
  AddCandidates(pc2.get(), obs1.TakeCandidates());

  // ---- 6. 跑 8 秒，每秒打一次统计 ----
  const int64_t run_start = NowMs();
  for (int second = 1; second <= 8; ++second) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const int frames = g_audio_frames.load(std::memory_order_relaxed);
    const int64_t pcm_bytes = g_pcm_bytes.load(std::memory_order_relaxed);
    const double kbps = static_cast<double>(pcm_bytes) * 8.0 / 1000.0 / second;
    const int lat_samples = g_latency_samples.load();
    const int64_t lat_avg =
        lat_samples > 0 ? g_latency_sum_ms.load() / lat_samples : -1;
    // 丢包/解码质量这里用「解码出的 PCM 帧数 vs 理论帧数」估算（10ms/帧、100 帧/秒）：
    // 统计接口（RTCStatsReport）要引 Attribute 模板，当前静态库头文件组合编不过，
    // 而 spike 只需要「有没有出包、出得够不够」这一个量，不必为此绕路。
    const double expected_frames = 100.0 * second;
    const double delivered_pct =
        expected_frames > 0 ? 100.0 * frames / expected_frames : 0.0;
    std::printf("  t+%ds: B decoded frames=%d (%.1f%% of expected) pcm_kbps=%.1f "
                "latency_avg=%lld ms latency_max=%lld ms\n",
                second, frames, delivered_pct, kbps, static_cast<long long>(lat_avg),
                static_cast<long long>(g_latency_max_ms.load()));
  }

  const int64_t run_ms = NowMs() - run_start;
  const int frames = g_audio_frames.load(std::memory_order_relaxed);
  const int lat_samples = g_latency_samples.load();
  const int64_t lat_avg = lat_samples > 0 ? g_latency_sum_ms.load() / lat_samples : -1;
  std::printf("\n==== 结果 ====\n");
  std::printf("  运行时长        : %lld ms\n", static_cast<long long>(run_ms));
  std::printf("  B 解码 PCM 帧数 : %d（%.1f 帧/秒，10ms/帧理论 100）\n", frames,
              run_ms > 0 ? 1000.0 * frames / static_cast<double>(run_ms) : 0.0);
  std::printf("  端到端延迟      : avg=%lld ms max=%lld ms（%d 个样本）\n",
              static_cast<long long>(lat_avg),
              static_cast<long long>(g_latency_max_ms.load()), lat_samples);

  pc1->Close();
  pc2->Close();

  const bool ok = frames > 20 && lat_avg < 150;
  std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 3;
}
