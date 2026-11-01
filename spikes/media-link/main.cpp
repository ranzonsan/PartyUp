// Spike 2：第三步（双连接 ICE/DTLS/SCTP）+ 第四步（一路合成视频轨，PC1 编码、PC2 解出帧）。
//
// 第三步证明了「静态库能链接」「工厂和连接对象能创建」「同进程两条连接能连通」；
// 第四步补上媒体本身：合成 I420 帧 → 视频轨 → AddTrack → 协商 m=video → VP8 编码出包
// → PC2 收包解码 → sink 回调计数。闭环判据有两条，缺一不可：
//   * PC1 GetStats 的 outbound-rtp.packetsSent > 0（RTP 真的出去了）
//   * PC2 的 VideoSinkInterface::OnFrame 计数 > 0（对端真的解出了帧）
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
//   * 媒体默认是关的：必须自己装 video_encoder/decoder_factory + EnableMedia(deps)
//   * VideoTrackInterface 已经没有 SetSink()，挂 sink 走 AddOrUpdateSink()
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
#include <utility>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/media_types.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/webrtc_sdp.h"
#include "pc/video_track.h"
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
  // video_sink 非空时，收到远端视频轨就挂上去（PC2 用；PC1 传 nullptr）。
  PcObserver(std::string name, webrtc::VideoSinkInterface<webrtc::VideoFrame>* video_sink)
      : name_(std::move(name)), video_sink_(video_sink) {}

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

  // Unified Plan 下远端有新的 m= 段落时先调 OnTrack；OnAddTrack 是兼容旧语义的
  // 同义回调，两个都挂一遍最保险（重复挂同一个 sink 对 VideoBroadcaster 是幂等的）。
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    if (transceiver == nullptr) return;
    AttachVideoSink(transceiver->receiver());
  }

  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {
    AttachVideoSink(std::move(receiver));
  }

  bool gathered() const { return gathered_.load(); }
  bool video_track_seen() const { return video_track_seen_.load(); }

  std::vector<CandidateCopy> Candidates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return candidates_;
  }

 private:
  // 把 sink 挂到远端视频轨上。M152 的 VideoTrackInterface 已经没有 SetSink()
  // （RtpReceiverInterface 也没有），能用的是 VideoTrackInterface::AddOrUpdateSink：
  // 基类里它是空实现，真正的实现是 VideoTrack::AddOrUpdateSink，虚分派会走对。
  void AttachVideoSink(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    if (video_sink_ == nullptr || receiver == nullptr) return;
    if (receiver->media_type() != webrtc::MediaType::VIDEO) return;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track = receiver->track();
    if (track == nullptr || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
      return;
    }
    auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
    video_track->AddOrUpdateSink(video_sink_, webrtc::VideoSinkWants());
    video_track_seen_.store(true);
    std::printf("[%s] remote video track -> sink: id=%s kind=%s\n", name_.c_str(),
                track->id().c_str(), track->kind().c_str());
  }

  const std::string name_;
  webrtc::VideoSinkInterface<webrtc::VideoFrame>* const video_sink_;
  std::atomic<bool> video_track_seen_{false};
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

// ---------------------------------------------------------------------------
// 合成视频源
// ---------------------------------------------------------------------------
// 直接实现 VideoTrackSourceInterface（它本身是 VideoSourceInterface<VideoFrame>），
// 帧分发交给 webrtc::VideoBroadcaster：它线程安全，会把每一帧送到所有 sink，
// 而「编码器」就是 VideoTrack 挂上来的那个 sink。
//
// 注意：类不能写 final —— 外面还要套 webrtc::RefCountedObject<T> 才能提供 Ref/Release
// （VideoTrackSourceInterface 继承自 RefCountInterface）。
class SyntheticVideoSource : public webrtc::VideoTrackSourceInterface {
 public:
  void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const webrtc::VideoSinkWants& wants) override {
    broadcaster_.AddOrUpdateSink(sink, wants);
  }
  void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
    broadcaster_.RemoveSink(sink);
  }

  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return true; }
  std::optional<bool> needs_denoising() const override { return false; }
  bool GetStats(Stats* /*stats*/) override { return false; }
  bool SupportsEncodedOutput() const override { return false; }
  void GenerateKeyFrame() override {}
  void AddEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>* /*sink*/) override {}
  void RemoveEncodedSink(
      webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>* /*sink*/) override {}
  void RegisterObserver(webrtc::ObserverInterface* /*observer*/) override {}
  void UnregisterObserver(webrtc::ObserverInterface* /*observer*/) override {}

  // 推一帧。VideoBroadcaster 自带锁，从主线程调用没问题；
  // 但它会把帧直接转给编码器的 sink，所以别在持锁的位置调用。
  void PushFrame(const webrtc::VideoFrame& frame) { broadcaster_.OnFrame(frame); }

 private:
  webrtc::VideoBroadcaster broadcaster_;
};

// PC2 侧的接收 sink：OnFrame 在 worker（解码）线程回调，只做原子计数。
class FrameCounterSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  void OnFrame(const webrtc::VideoFrame& frame) override {
    if (frames_.fetch_add(1, std::memory_order_relaxed) == 0) {
      first_width_.store(frame.width(), std::memory_order_relaxed);
      first_height_.store(frame.height(), std::memory_order_relaxed);
    }
    last_timestamp_us_.store(frame.timestamp_us(), std::memory_order_relaxed);
  }

  int frames() const { return frames_.load(std::memory_order_relaxed); }
  int first_width() const { return first_width_.load(std::memory_order_relaxed); }
  int first_height() const { return first_height_.load(std::memory_order_relaxed); }
  int64_t last_timestamp_us() const { return last_timestamp_us_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int> frames_{0};
  std::atomic<int> first_width_{0};
  std::atomic<int> first_height_{0};
  std::atomic<int64_t> last_timestamp_us_{0};
};

// 640x360 I420 渐变：Y 平面随列 + 行 + 帧号变化，U/V 固定 128（灰色）。
void FillI420(webrtc::I420Buffer* buffer, int frame_index) {
  const int width = buffer->width();
  const int height = buffer->height();
  uint8_t* y = buffer->MutableDataY();
  const int y_stride = buffer->StrideY();
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      y[row * y_stride + col] = static_cast<uint8_t>((col + row + frame_index * 5) & 0xFF);
    }
  }
  const int chroma_height = (height + 1) / 2;
  std::memset(buffer->MutableDataU(), 128,
              static_cast<size_t>(buffer->StrideU()) * chroma_height);
  std::memset(buffer->MutableDataV(), 128,
              static_cast<size_t>(buffer->StrideV()) * chroma_height);
}

// 只摘视频相关的 RTP 统计。PC1 看出站（packetsSent），PC2 看入站（framesDecoded）。
struct VideoStatsSnapshot {
  bool got_report = false;
  uint64_t packets_sent = 0;
  uint64_t bytes_sent = 0;
  uint32_t frames_encoded = 0;
  uint32_t frames_sent = 0;
  uint32_t packets_received = 0;
  uint32_t frames_received = 0;
  uint32_t frames_decoded = 0;
  uint64_t jitter_buffer_emitted = 0;
};

class VideoStatsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  VideoStatsObserver(Event* event, VideoStatsSnapshot* out) : event_(event), out_(out) {}

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    if (report != nullptr) {
      out_->got_report = true;
      for (const webrtc::RTCOutboundRtpStreamStats* stats :
           report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
        if (stats->kind.has_value() && *stats->kind != "video") continue;
        out_->packets_sent += stats->packets_sent.value_or(0);
        out_->bytes_sent += stats->bytes_sent.value_or(0);
        out_->frames_encoded += stats->frames_encoded.value_or(0);
        out_->frames_sent += stats->frames_sent.value_or(0);
      }
      for (const webrtc::RTCInboundRtpStreamStats* stats :
           report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
        if (stats->kind.has_value() && *stats->kind != "video") continue;
        out_->packets_received += stats->packets_received.value_or(0);
        out_->frames_received += stats->frames_received.value_or(0);
        out_->frames_decoded += stats->frames_decoded.value_or(0);
        out_->jitter_buffer_emitted += stats->jitter_buffer_emitted_count.value_or(0);
      }
    }
    Finish(event_, true, {}, {});
  }

 private:
  Event* const event_;
  VideoStatsSnapshot* const out_;
};

}  // namespace

int main() {
  const auto process_start = std::chrono::steady_clock::now();

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

  // CreateModularPeerConnectionFactory 默认 media_factory == nullptr（「媒体默认关闭」），
  // 这时 AddTrack 出来的视频 m= 段落不会挂编码器、offer 里也不会出现 m=video。
  // 打开媒体 + 给出编解码器工厂，两件事都得做：
  //   * EnableMedia(deps) 装 MediaFactory —— 库里只有 EnableMedia，没有 EnableMediaWithDefaults；
  //   * encoder/decoder 工厂用模板 + libvpx VP8 适配器（库里确实有 libvpx VP8/VP9 和 H264/AV1）。
  // 音频不用，但 MediaEngine 建音频引擎时仍会读这两个工厂，给内置默认实现最省事。
  deps.video_encoder_factory = std::make_unique<
      webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter>>();
  deps.video_decoder_factory = std::make_unique<
      webrtc::VideoDecoderFactoryTemplate<webrtc::LibvpxVp8DecoderTemplateAdapter>>();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  webrtc::EnableMedia(deps);

  auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (factory == nullptr) {
    std::printf("factory NULL\n");
    return 1;
  }
  std::printf("factory OK\n");

  // 先把这一版库里实际可用的发送编解码器列出来：视频编码器缺失时，
  // 后面只会看到 packetsSent=0，提前打印一眼就能定位。
  std::vector<std::string> video_codecs;
  signaling->BlockingCall([&] {
    const webrtc::RtpCapabilities caps =
        factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    for (const webrtc::RtpCodecCapability& codec : caps.codecs) {
      video_codecs.push_back(codec.name);
    }
  });
  std::printf("send video codecs:");
  for (const std::string& codec : video_codecs) std::printf(" %s", codec.c_str());
  std::printf("\n");
  if (video_codecs.empty()) {
    std::printf("no video send codec in this build\n");
    return 1;
  }

  // ---- 1. 两个 PeerConnection，各带 observer ----
  // 接收 sink 故意 new 出来不释放：Close()/Stop() 之后仍可能有解码回调在飞，
  // 让它在进程退出前一直活着，比为它安排析构顺序安全（一次性 spike）。
  auto* frame_sink = new FrameCounterSink();
  PcObserver obs1("pc1", nullptr);
  PcObserver obs2("pc2", frame_sink);
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

  // ---- 2b. PC1 挂一路合成视频轨（640x360 @30fps）----
  // 必须在 CreateOffer 之前 AddTrack，否则 offer 里不会有 m=video。
  constexpr int kVideoWidth = 640;
  constexpr int kVideoHeight = 360;
  constexpr int kFrameCount = 100;
  constexpr int64_t kFrameIntervalUs = 33333;  // 30fps

  webrtc::scoped_refptr<SyntheticVideoSource> video_source;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track;
  error.clear();
  signaling->BlockingCall([&] {
    video_source = webrtc::scoped_refptr<SyntheticVideoSource>(
        new webrtc::RefCountedObject<SyntheticVideoSource>());
    // 第三个参数必须是真 worker 线程：VideoTrack 内部会包一层
    // VideoTrackSourceProxy，AddOrUpdateSink / RemoveSink 都投递到它上面执行，
    // 传 nullptr 会在建代理时炸掉。
    webrtc::scoped_refptr<webrtc::VideoTrack> track =
        webrtc::VideoTrack::Create("screen", video_source, worker.get());
    if (track == nullptr) {
      error = "VideoTrack::Create returned null";
      return;
    }
    auto added = pc1->AddTrack(track, {"stream0"});
    if (!added.ok()) {
      error = std::string("AddTrack(video): ") + added.error().message();
      return;
    }
    video_track = track;
  });
  if (video_track == nullptr) {
    std::printf("video track FAILED: %s\n", error.c_str());
    return 1;
  }
  std::printf("video track added: id=%s kind=%s\n", video_track->id().c_str(),
              video_track->kind().c_str());

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

  const bool transport_connected =
      final1.connection == PeerConnection::PeerConnectionState::kConnected &&
      final2.connection == PeerConnection::PeerConnectionState::kConnected;

  // ---- 7. 推 100 帧合成视频，再等 2 秒让编码/传输/解码/统计落定 ----
  int frames_pushed = 0;
  int sink_frames = 0;
  int64_t push_elapsed_ms = 0;
  VideoStatsSnapshot pc1_stats;
  VideoStatsSnapshot pc2_stats;

  if (transport_connected) {
    std::printf("pushing %d synthetic frames (%dx%d @30fps)...\n", kFrameCount, kVideoWidth,
                kVideoHeight);
    const auto push_start = std::chrono::steady_clock::now();
    // 时间戳必须是「像现在」的绝对值：VideoStreamEncoder 会拿帧时间戳和墙上时钟比，
    // 太旧的帧按过期丢弃。从 0 开始数（1970）时只有第一帧能编码，其余 99 帧全被丢。
    int64_t timestamp_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    for (int i = 0; i < kFrameCount; ++i) {
      webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
          webrtc::I420Buffer::Create(kVideoWidth, kVideoHeight);
      FillI420(buffer.get(), i);
      const webrtc::VideoFrame frame =
          webrtc::VideoFrame::Builder()
              .set_video_frame_buffer(buffer)
              .set_timestamp_us(timestamp_us)
              .set_ntp_time_ms(timestamp_us / 1000)
              // RTP 时间戳是 90kHz 时钟，编码器靠它算帧间隔；不设会全是 0。
              .set_rtp_timestamp(static_cast<uint32_t>(timestamp_us * 90 / 1000))
              .build();
      video_source->PushFrame(frame);
      ++frames_pushed;
      timestamp_us += kFrameIntervalUs;
      // 按绝对时间睡，避免 I420 填充耗时累积成掉帧。
      std::this_thread::sleep_until(push_start +
                                    std::chrono::microseconds(kFrameIntervalUs * (i + 1)));
    }
    push_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - push_start)
                          .count();
    std::printf("frames pushed: %d in %lld ms (%.1f fps)\n", frames_pushed,
                static_cast<long long>(push_elapsed_ms),
                frames_pushed * 1000.0 /
                    static_cast<double>(push_elapsed_ms > 0 ? push_elapsed_ms : 1));

    // 再等 2 秒：最后几帧要走过 pacer → 网络 → 抖动缓冲 → 解码器。
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sink_frames = frame_sink->frames();
    std::printf("pc2 sink: frames=%d first=%dx%d\n", sink_frames, frame_sink->first_width(),
                frame_sink->first_height());

    // GetStats 是异步的：在 signalling 线程发起，回调里只写结果 + notify。
    auto collect_stats = [&](const webrtc::scoped_refptr<PeerConnection>& pc,
                             VideoStatsSnapshot* out, const char* tag) {
      Event stats_event;
      auto observer = webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>(
          new webrtc::RefCountedObject<VideoStatsObserver>(&stats_event, out));
      signaling->BlockingCall([&] { pc->GetStats(observer.get()); });
      if (!WaitEvent(&stats_event, 5000)) {
        std::printf("[%s] GetStats TIMEOUT\n", tag);
      }
    };
    collect_stats(pc1, &pc1_stats, "pc1");
    collect_stats(pc2, &pc2_stats, "pc2");
  }

  std::printf("video stats:\n");
  std::printf("  pc1 outbound-rtp: packetsSent=%llu bytesSent=%llu framesEncoded=%u framesSent=%u\n",
              static_cast<unsigned long long>(pc1_stats.packets_sent),
              static_cast<unsigned long long>(pc1_stats.bytes_sent), pc1_stats.frames_encoded,
              pc1_stats.frames_sent);
  std::printf("  pc2 inbound-rtp : packetsReceived=%u framesReceived=%u framesDecoded=%u "
              "jitterBufferEmitted=%llu\n",
              pc2_stats.packets_received, pc2_stats.frames_received, pc2_stats.frames_decoded,
              static_cast<unsigned long long>(pc2_stats.jitter_buffer_emitted));
  std::printf("  pc2 sink frames: %d\n", sink_frames);

  // 闭环判据（缺一不可）：PC1 真的发出了 RTP 包 + PC2 的 sink 真的被喂到解出的帧。
  const bool video_ok = transport_connected && pc1_stats.packets_sent > 0 && sink_frames > 0;

  // ---- 8. 收尾：先 Close 再释放，最后按 signaling → worker → network 停线程 ----
  signaling->BlockingCall([&] {
    if (pc1 != nullptr) pc1->Close();
    if (pc2 != nullptr) pc2->Close();
    // VideoTrack（及其内部 source 代理）的析构要求 signalling 线程。
    video_track = nullptr;
  });
  video_source = nullptr;
  data_channel = nullptr;
  pc1 = nullptr;
  pc2 = nullptr;
  factory = nullptr;

  signaling->Stop();
  worker->Stop();
  network->Stop();

  const int64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - process_start)
                               .count();
  std::printf("total elapsed: %lld ms\n", static_cast<long long>(total_ms));

  if (video_ok) {
    std::printf("VIDEO RTP OK: packetsSent>0 and PC2 decoded frames>0\n");
    return 0;
  }
  if (!transport_connected) {
    std::printf("DTLS NOT connected (timeout or failure), see states above\n");
  } else if (pc1_stats.packets_sent == 0) {
    std::printf("VIDEO FAILED: PC1 sent no RTP packets (encoder/negotiation problem?)\n");
  } else {
    std::printf("VIDEO FAILED: PC2 decoded no frames (packetsSent=%llu sinkFrames=%d)\n",
                static_cast<unsigned long long>(pc1_stats.packets_sent), sink_frames);
  }
  return 1;
}
