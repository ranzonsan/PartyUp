// PartyUp 媒体核心 · C ABI（v2：信令通道 + 真 PeerConnection）
//
// v1 只做「合成采集 → 本地 VP8 编码 → 编码事件」，没有网络。v2 补上信令通道：
//   pu_screen_start  → 建 PC → AddTrack(视频轨) → CreateOffer → SetLocalDescription
//                     → 等 ICE 收集完 → PushEvent(PU_EVENT_SIGNAL_OUT, offer)
//   pu_signal_in     → 解析 {"type":..,"sdp"/"candidate":..} → 喂 PC
//                     （收到 offer 时顺手建接收侧 PC 并回 answer）
//   远端帧           → OnTrack 挂 sink → PushEvent(PU_EVENT_REMOTE_FRAME) + 计数
//
// 初始化顺序完全照抄 spikes/media-link/main.cpp（顺序本身是踩出来的）：
//   1. webrtc::WinsockInitializer（静态链接会丢掉 WSAStartup，只有 TCP 候选）
//   2. network / worker / signaling 三条 webrtc 线程
//   3. PeerConnectionFactoryDependencies + 自装的 VP8 编/解码器工厂
//   4. EnableMedia(deps)      —— 不调用则 media_factory 为 null，媒体静默不出包
//   5. CreateModularPeerConnectionFactory
// 收尾顺序也照抄：关闭协商 → Close() PC → 释放 track/pc（signaling 线程）→ 释放
// factory → signaling → worker → network 依次 Stop，WinsockInitializer 最后析构。
//
// v1 的本地编码链路被完整保留（PU_EVENT_FRAME 的语义不变，v1 冒烟测试仍按原样
// 通过）。代价是采集源上挂了两个 sink：本地统计编码器 + PC 的 VideoStreamEncoder，
// 同一帧被编码两次。这是 v2 有意接受的重复计算——下一版把 PU_EVENT_FRAME 改成
// 从 PC 的 GetStats 派生之后就能删掉本地编码器（见文档 §8「已知代价」）。
#include "partyup_media.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// 第三方 JSON：nlohmann/json v3.11.3 单头文件（vendored，见
// src/media-core/vendor/nlohmann/README.md）。不用仓库自带的 jsoncpp，因为
// third_party/libwebrtc 只带了它的头文件、没有实现，webrtc.lib 里也没有符号。
#include "nlohmann/json.hpp"

// libwebrtc（m152）—— 与 spikes/media-link/main.cpp 同一组头文件
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/media_types.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video/video_codec_type.h"
#include "api/webrtc_sdp.h"
#include "pc/video_track.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"
#include "rtc_base/win32_socket_init.h"

// 事件的内存布局是对外契约，改动必须同步改头文件注释与文档。
static_assert(sizeof(pu_event) == 256, "pu_event 布局变了：这是 ABI 契约");
static_assert(offsetof(pu_event, data) == 16, "pu_event.data 偏移变了");
static_assert(sizeof(pu_frame_event) == 40, "pu_frame_event 布局变了");
static_assert(offsetof(pu_event, data.frame.sequence) == 16, "frame.sequence 偏移变了");
static_assert(offsetof(pu_event, data.frame.capture_ts_us) == 24, "frame.capture_ts_us 偏移变了");
static_assert(sizeof(pu_state_event) == 16, "pu_state_event 布局变了");
static_assert(sizeof(pu_error_event) == 240, "pu_error_event 布局变了");
static_assert(offsetof(pu_event, data.error.message) == 24, "error.message 偏移变了");
// v2 新增的两块：只往 union 里追加，v1 的偏移一个没动。
static_assert(offsetof(pu_event, data.signal.payload) == 16, "signal.payload 偏移变了");
static_assert(offsetof(pu_event, data.signal.payload_len) == 24, "signal.payload_len 偏移变了");
static_assert(offsetof(pu_event, data.signal.kind) == 28, "signal.kind 偏移变了");
static_assert(offsetof(pu_event, data.remote.sequence) == 16, "remote.sequence 偏移变了");
static_assert(offsetof(pu_event, data.remote.width) == 40, "remote.width 偏移变了");
// pu_stats 的状态字段直接透传 libwebrtc 的枚举值，两边必须一一对应。
static_assert(static_cast<int>(webrtc::PeerConnectionInterface::PeerConnectionState::kNew) ==
                  static_cast<int>(PU_PEER_NEW),
              "pu_peer_state 与 webrtc PeerConnectionState 不再对齐");
static_assert(static_cast<int>(webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) ==
                  static_cast<int>(PU_PEER_CONNECTED),
              "pu_peer_state 与 webrtc PeerConnectionState 不再对齐");
static_assert(static_cast<int>(webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionNew) ==
                  static_cast<int>(PU_ICE_NEW),
              "pu_ice_state 与 webrtc IceConnectionState 不再对齐");


namespace partyup {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFps = 30;
constexpr int64_t kFrameIntervalUs = 1000000 / kFps;  // 33333us

// 编码码率的统计窗口
constexpr int64_t kRateWindowUs = 1000000;
constexpr uint32_t kDefaultQueueCapacity = 256;

// ---- 信令 / 协商（v2）----
// ICE 收集超时：本机回环只会有 host 候选，实测 20–80 ms 收完；3 秒是给
// 「将来接 STUN/TURN」留的余量，超时也照发 SDP（里面带上当时已收集的候选）。
constexpr int kGatherTimeoutMs = 3000;
constexpr int kSdpTimeoutMs = 10000;     // CreateOffer / CreateAnswer
constexpr int kSetSdpTimeoutMs = 10000;  // SetLocal / SetRemote
// 已投递的信令 JSON 保留多少条。见头文件 pu_signal_event 注释。
constexpr size_t kMaxRetainedSignals = 64;

int64_t NowUsSystem() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t NowUsSteady() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---- 进程级「创建失败」信息：pu_engine_create 失败时没有句柄可查，用
// pu_last_error(NULL) 读这里。 ----
std::mutex g_create_error_mu;
std::string g_create_error;

void SetCreateError(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_create_error_mu);
  g_create_error = message;
}

}  // namespace

const char* CreateErrorCString() {
  std::lock_guard<std::mutex> lock(g_create_error_mu);
  return g_create_error.c_str();
}

namespace {

// ---------------------------------------------------------------------------
// 合成视频源：直接实现 VideoTrackSourceInterface，帧分发交给 VideoBroadcaster
// （线程安全）。类不能写 final —— 外面要套 webrtc::RefCountedObject<T>。
// ---------------------------------------------------------------------------
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

  // 从采集线程调用：同步分发给所有 sink（本切片只有编码器一个）。
  void PushFrame(const webrtc::VideoFrame& frame) { broadcaster_.OnFrame(frame); }

 private:
  webrtc::VideoBroadcaster broadcaster_;
};

// 640x360 I420 渐变，Y 随列 + 行 + 帧号变化，U/V 固定 128（灰）——照抄 spike。
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
  std::memset(buffer->MutableDataU(), 128, static_cast<size_t>(buffer->StrideU()) * chroma_height);
  std::memset(buffer->MutableDataV(), 128, static_cast<size_t>(buffer->StrideV()) * chroma_height);
}

// VP8 单流 640x360@30 的设置。取值与 webrtc 自己的 test/video_codec_settings.h 一致。
webrtc::VideoCodec MakeVp8CodecSettings() {
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecVP8;
  codec.width = kWidth;
  codec.height = kHeight;
  codec.startBitrate = 1500;  // kbps
  codec.maxBitrate = 2500;
  codec.minBitrate = 300;
  codec.maxFramerate = kFps;
  codec.active = true;
  codec.qpMax = 56;
  codec.numberOfSimulcastStreams = 0;  // 单流
  codec.mode = webrtc::VideoCodecMode::kScreensharing;
  codec.timing_frame_thresholds = {200, 250};
  *codec.VP8() = webrtc::VideoEncoder::GetDefaultVp8Settings();
  return codec;
}

// ---------------------------------------------------------------------------
// 信令用的小工具（v2）
// ---------------------------------------------------------------------------

// 收到的 ICE 候选。IceCandidate 不可拷贝，跨线程只传「mid / mline / 候选行文本」，
// 需要时再 Create 一份——照抄 spike 的做法。
struct CandidateCopy {
  std::string mid;
  int mline_index = 0;
  std::string sdp;
};

// 一次异步 webrtc 操作的结果。回调只往里写，控制线程只从里读；绝不在回调里
// 反过来等待 signaling 线程上的另一个任务（那样会死锁，spike 注释里写死了）。
struct AsyncResult {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string sdp;
  std::string error;
};

bool WaitAsync(AsyncResult* r, int timeout_ms) {
  std::unique_lock<std::mutex> lock(r->mu);
  return r->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [r] { return r->done; });
}

void FinishAsync(AsyncResult* r, bool ok, std::string sdp, std::string error) {
  {
    std::lock_guard<std::mutex> lock(r->mu);
    r->ok = ok;
    r->sdp = std::move(sdp);
    r->error = std::move(error);
    r->done = true;
  }
  r->cv.notify_all();
}

// CreateOffer / CreateAnswer 的回调：把 SDP 序列化成字符串就完事。
// 回调转移 desc 的所有权，序列化完直接 delete，避免对象跨线程传递。
// 注意：类不能加 final —— 外面要套 webrtc::RefCountedObject<T>。
class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateSdpObserver(AsyncResult* result) : result_(result) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::string sdp = webrtc::SdpSerialize(*desc);
    delete desc;
    FinishAsync(result_, true, std::move(sdp), {});
  }

  void OnFailure(webrtc::RTCError error) override {
    FinishAsync(result_, false, {}, error.message());
  }

 private:
  AsyncResult* const result_;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit SetLocalObserver(AsyncResult* result) : result_(result) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    FinishAsync(result_, error.ok(), {},
                error.ok() ? std::string() : std::string(error.message()));
  }

 private:
  AsyncResult* const result_;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit SetRemoteObserver(AsyncResult* result) : result_(result) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    FinishAsync(result_, error.ok(), {},
                error.ok() ? std::string() : std::string(error.message()));
  }

 private:
  AsyncResult* const result_;
};

// ---- 信封 <-> 结构 ----
// 出站信封（与 docs/PROTOCOL.md 对齐，详见 docs/spikes/2026-11-01-signaling-abi.md）
std::string MakeSdpEnvelope(const char* type, const std::string& sdp) {
  nlohmann::json j;
  j["type"] = type;
  j["sdp"] = sdp;
  return j.dump();
}

std::string MakeCandidateEnvelope(const CandidateCopy& candidate) {
  nlohmann::json j;
  j["type"] = "candidate";
  j["mid"] = candidate.mid;
  j["mline"] = candidate.mline_index;
  j["candidate"] = candidate.sdp;
  return j.dump();
}

struct SignalMessage {
  std::string type;       // "offer" | "answer" | "candidate"
  std::string sdp;
  std::string candidate;  // 空字符串 = end-of-candidates
  std::string mid;
  int mline = 0;
};

std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// 归一化 type：剥掉 "media." / "p2p." / "screen." / "rtc." 前缀，把 ice 映射成
// candidate。PROTOCOL.md 用点分命名（media.offer / p2p.signal + kind），媒体核心
// 内部的信封用短名，两种都认。
std::string NormalizeSignalType(const std::string& raw) {
  std::string type = ToLowerAscii(raw);
  for (const char* prefix : {"media.", "p2p.", "screen.", "rtc.", "pu."}) {
    const size_t len = std::strlen(prefix);
    if (type.size() > len && type.compare(0, len, prefix) == 0) {
      type = type.substr(len);
      break;
    }
  }
  if (type == "ice" || type == "ice-candidate" || type == "candidate") return "candidate";
  if (type == "sdp-offer") return "offer";
  if (type == "sdp-answer") return "answer";
  return type;
}

// 在 src 上按多个候选键名找字符串（同时接受数字，按十进制转）。
bool PickString(const nlohmann::json& src, const std::vector<const char*>& keys, std::string* out) {
  for (const char* key : keys) {
    auto it = src.find(key);
    if (it == src.end() || it->is_null()) continue;
    if (it->is_string()) {
      *out = it->get<std::string>();
      return true;
    }
    if (it->is_number_integer()) {
      *out = std::to_string(it->get<long long>());
      return true;
    }
  }
  return false;
}

bool PickInt(const nlohmann::json& src, const std::vector<const char*>& keys, int* out) {
  for (const char* key : keys) {
    auto it = src.find(key);
    if (it == src.end() || it->is_null()) continue;
    if (it->is_number_integer()) {
      *out = it->get<int>();
      return true;
    }
    if (it->is_string()) {
      try {
        *out = std::stoi(it->get<std::string>());
        return true;
      } catch (...) {
        continue;
      }
    }
  }
  return false;
}

// 解析一条入站信令。禁止手写 JSON 解析器：全部交给 nlohmann/json。
bool ParseSignal(const char* payload, size_t len, SignalMessage* out, std::string* error) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(payload, payload + len);
  } catch (const std::exception& ex) {
    *error = std::string("not valid JSON: ") + ex.what();
    return false;
  }
  if (!doc.is_object()) {
    *error = "signal payload is not a JSON object";
    return false;
  }

  std::string raw_type;
  if (!PickString(doc, {"type", "kind", "action"}, &raw_type)) {
    *error = "signal payload has no \"type\" field";
    return false;
  }
  out->type = NormalizeSignalType(raw_type);

  // PROTOCOL.md 的服务端形状把真正的字段放在 payload 里，剥一层。
  const nlohmann::json* src = &doc;
  auto payload_it = doc.find("payload");
  if (payload_it != doc.end()) {
    if (payload_it->is_object()) {
      src = &*payload_it;
      if (out->type == "p2p.signal" || out->type.empty()) {
        std::string inner_kind;
        if (PickString(*src, {"kind"}, &inner_kind)) out->type = NormalizeSignalType(inner_kind);
      }
    } else if (payload_it->is_string()) {
      // payload 是原始字符串载荷：按 kind 解释成 SDP 或候选行。
      const std::string raw = payload_it->get<std::string>();
      if (out->type == "offer" || out->type == "answer") {
        out->sdp = raw;
      } else if (out->type == "candidate") {
        out->candidate = raw;
      }
    }
  }
  if (out->type.empty() && !raw_type.empty()) out->type = NormalizeSignalType(raw_type);

  if (out->type == "offer" || out->type == "answer") {
    if (out->sdp.empty() && !PickString(*src, {"sdp"}, &out->sdp)) {
      *error = "signal type " + out->type + " has no \"sdp\" field";
      return false;
    }
    if (out->sdp.empty()) {
      *error = "signal type " + out->type + " carries an empty \"sdp\"";
      return false;
    }
  } else if (out->type == "candidate") {
    if (!PickString(*src, {"candidate"}, &out->candidate)) {
      *error = "signal type candidate has no \"candidate\" field";
      return false;
    }
    PickString(*src, {"mid", "sdpMid", "sdp_mid"}, &out->mid);
    PickInt(*src, {"mline", "mline_index", "sdpMLineIndex", "sdp_mline_index"}, &out->mline);
  } else {
    *error = "unsupported signal type \"" + raw_type + "\"";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 引擎实现
// ---------------------------------------------------------------------------
class MediaEngine : public webrtc::VideoSinkInterface<webrtc::VideoFrame>,
                    public webrtc::EncodedImageCallback {
 public:
  // 两个辅助类在类外定义（需要 MediaEngine 完整类型），这里只前置声明。
  class PcObserver;       // webrtc::PeerConnectionObserver：把状态写回原子量、挂远端 sink
  class RemoteFrameSink;  // 远端视频轨的 sink：把帧转给引擎

  MediaEngine() = default;
  ~MediaEngine() override;

  MediaEngine(const MediaEngine&) = delete;
  MediaEngine& operator=(const MediaEngine&) = delete;

  bool Init(const pu_config* cfg, std::string* error);
  int Start(const char* screen_id, std::string* error);
  int Stop();
  int Poll(pu_event* out, int timeout_ms);
  const char* LastError();
  void SetLastError(const std::string& message);

  // v2：把一条服务端信令灌进引擎（同步执行）
  int SignalIn(const char* payload, size_t len, std::string* error);
  // v2：读统计快照
  int PeerStats(pu_stats* out);

  // webrtc::VideoSinkInterface<VideoFrame>：采集线程（pump）上被调用
  void OnFrame(const webrtc::VideoFrame& frame) override;
  // v2：远端视频轨的 sink 回调，解码线程（worker）上被调用
  void OnRemoteFrame(const webrtc::VideoFrame& frame);

  // webrtc::EncodedImageCallback：同上，编码器在同一线程回调
  webrtc::EncodedImageCallback::Result OnEncodedImage(
      const webrtc::EncodedImage& encoded_image,
      const webrtc::CodecSpecificInfo* codec_specific_info) override;
  void OnFrameDropped(uint32_t rtp_timestamp,
                      int spatial_id,
                      bool is_end_of_temporal_unit) override;

 private:
  void PumpLoop();
  void SignalStart(bool ok, const std::string& error);
  void PushState(int32_t state, int32_t previous, int32_t reason);
  void PushError(int32_t code, const std::string& message);
  void PushEvent(const pu_event& event);
  void PushEventWithPayload(pu_event event, std::string payload);
  pu_event MakeEvent(uint32_t type) const;
  void Fail(const std::string& message, std::string* error);
  void ReleaseStreamObjects();
  void Log(int level, const char* format, ...);

  // ---- v2：PeerConnection 与信令 ----
  // 建 PC（控制线程调用，内部投递到 signaling 线程）。for_sending 时立刻 AddTrack。
  bool EnsurePeerConnection(bool for_sending, std::string* error);
  bool CreateOfferAndSetLocal(std::string* error);
  bool CreateAnswerAndSetLocal(std::string* error);
  // type 是 webrtc::SdpType::kOffer / kAnswer
  bool SetRemoteDescriptionFromSdp(int type, const std::string& sdp, std::string* error);
  // 启动协商收尾线程：等 ICE 收集完 → 序列化带候选的 SDP → 推 SIGNAL_OUT
  void StartNegotiationEmit(const char* signal_type);
  void NegotiationEmitLoop(std::string signal_type,
                           webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc);
  std::string BuildLocalSdpWithCandidates(webrtc::PeerConnectionInterface* pc);
  void EmitSignal(const char* type, const std::string& payload, uint32_t kind);
  void ApplyBufferedCandidates();
  std::vector<CandidateCopy> SnapshotCandidates() const;
  void ResetSignalingStateLocked();
  // 等一个 ICE 收集完成的时刻（轮询，最多 kGatherTimeoutMs）
  bool WaitIceGatheringComplete(webrtc::PeerConnectionInterface* pc);

  // WinsockInitializer 必须先构造、最后析构（成员逆序析构）。
  webrtc::WinsockInitializer winsock_;

  std::unique_ptr<webrtc::Thread> network_;
  std::unique_ptr<webrtc::Thread> worker_;
  std::unique_ptr<webrtc::Thread> signaling_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
  std::unique_ptr<webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter>>
      encoder_factory_;
  std::optional<webrtc::Environment> env_;

  // 一路共享流的对象
  webrtc::scoped_refptr<SyntheticVideoSource> source_;
  webrtc::scoped_refptr<webrtc::VideoTrack> track_;
  std::thread pump_;
  std::atomic<bool> stop_requested_{false};
  bool running_ = false;      // 只在控制线程读写（control_mu_ 保护）
  int32_t state_ = PU_STREAM_IDLE;

  std::mutex control_mu_;

  // start 握手：pump 线程建好编码器（或失败）后通知控制线程
  std::mutex start_mu_;
  std::condition_variable start_cv_;
  bool start_done_ = false;
  bool start_ok_ = false;
  std::string start_error_;

  // 编码器只在 pump 线程上创建、使用、释放；OnFrame/OnEncodedImage 也在该线程。
  webrtc::VideoEncoder* encoder_ = nullptr;
  uint64_t frame_sequence_ = 0;
  int64_t capture_ts_us_ = 0;
  std::deque<std::pair<int64_t, uint32_t>> rate_window_;

  // ---- v2：PeerConnection ----
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  PcObserver* pc_observer_ = nullptr;   // 必须比 pc_ 活得久（析构里最后删）
  RemoteFrameSink* remote_sink_ = nullptr;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_track_;
  std::thread negotiator_;
  std::atomic<bool> negotiator_stop_{false};
  bool sharing_ = false;               // 本引擎是 offerer（自己发 offer）
  bool has_remote_description_ = false;
  std::atomic<bool> local_sdp_emitted_{false};
  mutable std::mutex cand_mu_;
  std::vector<CandidateCopy> gathered_candidates_;   // OnIceCandidate 写
  std::vector<CandidateCopy> buffered_candidates_;   // SetRemote 之前到的一律先存

  // ---- v2：统计（全是原子量，pu_peer_stats 直接快照，不加锁） ----
  std::atomic<uint32_t> stat_frames_sent_{0};
  std::atomic<uint32_t> stat_frames_encoded_{0};
  std::atomic<uint32_t> stat_frames_dropped_{0};
  std::atomic<uint32_t> stat_frames_received_{0};
  std::atomic<uint32_t> stat_signal_out_{0};
  std::atomic<int32_t> stat_peer_state_{PU_PEER_NONE};
  std::atomic<int32_t> stat_ice_state_{PU_ICE_NEW};
  std::atomic<uint32_t> stat_remote_width_{0};
  std::atomic<uint32_t> stat_remote_height_{0};
  std::atomic<int64_t> stat_last_remote_us_{0};
  std::atomic<uint64_t> stat_remote_sequence_{0};

  // 事件队列（有界 deque）：多生产者（pump / signaling / 协商线程 / 控制线程）、
  // 单消费者。SIGNAL_OUT 的 JSON 跟着队列项一起走，投递时才把指针填进事件。
  struct QueuedEvent {
    pu_event event{};
    std::string payload;  // 只有 PU_EVENT_SIGNAL_OUT 会用到
  };
  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<QueuedEvent> queue_;
  // 已投递出去的信令 JSON：保证调用方手里的指针在「取走之后」还有效。
  std::deque<std::string> delivered_signals_;
  size_t capacity_ = kDefaultQueueCapacity;
  uint64_t dropped_events_ = 0;

  // 错误串
  std::mutex error_mu_;
  std::string last_error_;

  int log_level_ = 0;
};

// 远端视频轨的 sink：OnFrame 在解码线程（worker）回调，只做计数 + 推事件。
// 由引擎 new 出来、在三条 webrtc 线程都 Stop 之后才 delete（见析构函数）。
class MediaEngine::RemoteFrameSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit RemoteFrameSink(MediaEngine* engine) : engine_(engine) {}

  void OnFrame(const webrtc::VideoFrame& frame) override { engine_->OnRemoteFrame(frame); }

 private:
  MediaEngine* const engine_;
};

// PeerConnection 的回调。这里只写原子量 / 记录候选 / 挂 sink，绝不做阻塞等待：
// 这些回调全部在 signaling 线程上，等 signaling 线程上的另一件事会直接死锁。
// 不加 final（不是必须，但和 spike 的写法保持一致，也免得套 RefCountedObject 时再踩）。
class MediaEngine::PcObserver : public webrtc::PeerConnectionObserver {
 public:
  explicit PcObserver(MediaEngine* engine) : engine_(engine) {}

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState /*state*/) override {}

  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    engine_->stat_ice_state_.store(static_cast<int32_t>(state), std::memory_order_relaxed);
    engine_->Log(3, "ice_connection_state=%s",
                 std::string(webrtc::PeerConnectionInterface::AsString(state)).c_str());
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    engine_->stat_peer_state_.store(static_cast<int32_t>(state), std::memory_order_relaxed);
    engine_->Log(3, "peer_connection_state=%s",
                 std::string(webrtc::PeerConnectionInterface::AsString(state)).c_str());
  }

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    if (state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
      gathered_.store(true, std::memory_order_relaxed);
    }
  }

  void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
    if (candidate == nullptr) return;
    CandidateCopy copy;
    copy.mid = candidate->sdp_mid();
    copy.mline_index = candidate->sdp_mline_index();
    copy.sdp = candidate->ToString();
    {
      std::lock_guard<std::mutex> lock(engine_->cand_mu_);
      engine_->gathered_candidates_.push_back(copy);
    }
    engine_->Log(3, "ice_candidate: %s", copy.sdp.c_str());
    // SDP 已经发出去了才来的候选，单独补一条 candidate 信令（trickle）。正常情况下
    // 我们要等收集完成才发 SDP，所以这条基本走不到；留着是为了将来接 TURN/SFU。
    if (engine_->local_sdp_emitted_.load(std::memory_order_relaxed)) {
      engine_->EmitSignal("candidate", MakeCandidateEnvelope(copy), PU_SIGNAL_CANDIDATE);
    }
  }

  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
    engine_->Log(3, "remote data channel: %s",
                 channel != nullptr ? channel->label().c_str() : "(null)");
  }

  void OnRenegotiationNeeded() override {}

  // Unified Plan 下远端有新的 m= 段落时先调 OnTrack；OnAddTrack 是兼容旧语义的同义
  // 回调，两个都挂一遍最保险（重复挂同一个 sink 对 VideoBroadcaster 是幂等的）。
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    if (transceiver == nullptr) return;
    AttachVideoSink(transceiver->receiver());
  }

  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {
    AttachVideoSink(std::move(receiver));
  }

  bool gathered() const { return gathered_.load(std::memory_order_relaxed); }

 private:
  // M152 的 VideoTrackInterface 已经没有 SetSink()，能用的是
  // VideoTrackInterface::AddOrUpdateSink（基类里是空实现，虚分派会走
  // VideoTrack::AddOrUpdateSink）。
  void AttachVideoSink(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    if (engine_->remote_sink_ == nullptr || receiver == nullptr) return;
    if (receiver->media_type() != webrtc::MediaType::VIDEO) return;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track = receiver->track();
    if (track == nullptr || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
      return;
    }
    auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track.get());
    video_track->AddOrUpdateSink(engine_->remote_sink_, webrtc::VideoSinkWants());
    engine_->remote_track_ = video_track;
    engine_->Log(3, "remote video track -> sink: id=%s kind=%s", track->id().c_str(),
                 track->kind().c_str());
  }

  MediaEngine* const engine_;
  std::atomic<bool> gathered_{false};
};

MediaEngine::~MediaEngine() {
  Stop();

  // 顺序照抄 spike：Close()/track/pc（signaling 线程）→ source → factory → 三条线程 Stop。
  ReleaseStreamObjects();

  pc_factory_ = nullptr;
  encoder_factory_.reset();
  env_.reset();

  if (signaling_ != nullptr) signaling_->Stop();
  if (worker_ != nullptr) worker_->Stop();
  if (network_ != nullptr) network_->Stop();

  signaling_.reset();
  worker_.reset();
  network_.reset();

  // 三条 webrtc 线程都停了，才删「可能被它们回调」的对象：远端 sink（解码线程）
  // 与 PC 观察者（signaling 线程）。这样即使有回调在飞也没有线程能再碰它们。
  delete remote_sink_;
  remote_sink_ = nullptr;
  delete pc_observer_;
  pc_observer_ = nullptr;
}

void MediaEngine::Log(int level, const char* format, ...) {
  if (level > log_level_) return;
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  std::fprintf(stderr, "[partyup-media] %s\n", buffer);
  std::fflush(stderr);
}

void MediaEngine::SetLastError(const std::string& message) {
  std::lock_guard<std::mutex> lock(error_mu_);
  last_error_ = message;
}

const char* MediaEngine::LastError() {
  std::lock_guard<std::mutex> lock(error_mu_);
  return last_error_.c_str();
}

bool MediaEngine::Init(const pu_config* cfg, std::string* error) {
  if (cfg != nullptr) {
    log_level_ = static_cast<int>(std::min<uint32_t>(cfg->log_level, 4));
    if (cfg->event_queue_capacity != 0) {
      capacity_ = std::min<size_t>(std::max<size_t>(cfg->event_queue_capacity, 8), 4096);
    }
    if (cfg->client_name != nullptr) {
      Log(3, "client_name=%s", cfg->client_name);
    }
  }

  // 静态链接会把 libwebrtc 里做 WSAStartup 的那个全局对象连同它的 .obj 一起丢掉，
  // 症状是只有 TCP 候选、UDP 一条都没有、ICE 永远 checking。这里显式初始化。
  if (winsock_.error() != 0) {
    *error = "WSAStartup failed, error=" + std::to_string(winsock_.error());
    SetLastError(*error);
    return false;
  }
  Log(3, "winsock ok");

  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create();
  signaling_ = webrtc::Thread::Create();
  if (network_ == nullptr || worker_ == nullptr || signaling_ == nullptr ||
      !network_->Start() || !worker_->Start() || !signaling_->Start()) {
    *error = "failed to start webrtc threads";
    SetLastError(*error);
    return false;
  }
  Log(3, "webrtc threads started");

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network_.get();
  deps.worker_thread = worker_.get();
  deps.signaling_thread = signaling_.get();

  // 媒体默认关闭：不装编/解码器工厂、不调 EnableMedia 的话 media_factory 为 null，
  // AddTrack 出来的 m=video 不会挂编码器、RTP 静默不出包。
  deps.video_encoder_factory = std::make_unique<
      webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter>>();
  deps.video_decoder_factory = std::make_unique<
      webrtc::VideoDecoderFactoryTemplate<webrtc::LibvpxVp8DecoderTemplateAdapter>>();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  webrtc::EnableMedia(deps);

  pc_factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (pc_factory_ == nullptr) {
    *error = "CreateModularPeerConnectionFactory returned null";
    SetLastError(*error);
    return false;
  }

  // 本切片直接建编码器用的工厂与 Environment（与 PC 用的那份互不共享所有权）。
  encoder_factory_ =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter>>();
  env_ = webrtc::EnvironmentFactory().Create();

  // 探一下媒体引擎真的装上了：拿不到视频发送编解码器就是 EnableMedia 没生效。
  std::vector<std::string> codecs;
  signaling_->BlockingCall([&] {
    const webrtc::RtpCapabilities caps =
        pc_factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
    for (const webrtc::RtpCodecCapability& codec : caps.codecs) {
      codecs.push_back(codec.name);
    }
  });
  if (codecs.empty()) {
    *error = "media engine exposes no video send codec (EnableMedia missing?)";
    SetLastError(*error);
    return false;
  }
  std::string codec_list;
  for (const std::string& name : codecs) {
    codec_list += " " + name;
  }
  Log(3, "media engine ok, send video codecs:%s", codec_list.c_str());

  // v2：远端帧 sink 与 PeerConnection 观察者提前建好。两者都比 PC 活得久：
  // sink 可能被解码线程回调到最后一刻，observer 由 PC 在 signaling 线程上调用，
  // 所以只在析构函数里、三条线程都 Stop 之后才删。
  remote_sink_ = new RemoteFrameSink(this);
  pc_observer_ = new PcObserver(this);

  return true;
}

void MediaEngine::Fail(const std::string& message, std::string* error) {
  if (error != nullptr) *error = message;
  SetLastError(message);
  Log(1, "error: %s", message.c_str());
}

int MediaEngine::Start(const char* screen_id, std::string* error) {
  std::lock_guard<std::mutex> control_lock(control_mu_);

  if (running_) {
    Fail("screen share is already running", error);
    return PU_ERR_STATE;
  }

  // 本版只有合成源。空指针与 "synthetic" 都算它；别的 id 明确拒绝，不假装支持。
  const bool synthetic = (screen_id == nullptr || screen_id[0] == '\0' ||
                          std::strcmp(screen_id, "synthetic") == 0);
  if (!synthetic) {
    Fail(std::string("unknown screen source id '") + screen_id +
             "': only \"synthetic\" exists in ABI v1",
         error);
    return PU_ERR_INVALID_ARG;
  }

  const int32_t previous_state = state_;
  state_ = PU_STREAM_STARTING;
  PushState(PU_STREAM_STARTING, previous_state, PU_REASON_REQUESTED);

  if (source_ == nullptr) {
    source_ = webrtc::scoped_refptr<SyntheticVideoSource>(
        new webrtc::RefCountedObject<SyntheticVideoSource>());
  }

  // VideoTrack 的方法有 signaling 线程断言；worker 参数不能传 nullptr（spike 坑）。
  std::string track_error;
  signaling_->BlockingCall([&] {
    webrtc::scoped_refptr<webrtc::VideoTrack> track =
        webrtc::VideoTrack::Create("screen", source_, worker_.get());
    if (track == nullptr) {
      track_error = "VideoTrack::Create returned null";
      return;
    }
    track_ = track;
  });
  if (track_ == nullptr) {
    ReleaseStreamObjects();
    state_ = PU_STREAM_FAILED;
    PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
    PushError(PU_ERR_MEDIA, track_error);
    Fail(track_error, error);
    return PU_ERR_MEDIA;
  }
  Log(3, "video track created: id=%s", track_->id().c_str());

  // v2：建 PeerConnection 并把视频轨挂上去。AddTrack 必须在 CreateOffer 之前，
  // 否则 offer 里不会有 m=video。
  {
    std::string pc_error;
    if (!EnsurePeerConnection(/*for_sending=*/true, &pc_error)) {
      ReleaseStreamObjects();
      state_ = PU_STREAM_FAILED;
      PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
      PushError(PU_ERR_MEDIA, pc_error);
      Fail(pc_error, error);
      return PU_ERR_MEDIA;
    }
  }

  // 编码器在采集线程里创建并初始化（libvpx 的 encoder 不是线程安全的，
  // 让创建/使用/释放都落在同一个线程上，省掉所有跨线程假设）。
  stop_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(start_mu_);
    start_done_ = false;
    start_ok_ = false;
    start_error_.clear();
  }
  pump_ = std::thread([this] { PumpLoop(); });

  bool started = false;
  std::string start_error;
  {
    std::unique_lock<std::mutex> lock(start_mu_);
    const bool finished = start_cv_.wait_for(lock, std::chrono::seconds(10),
                                             [this] { return start_done_; });
    started = finished && start_ok_;
    start_error = finished ? start_error_ : std::string("encoder start timed out");
  }

  if (!started) {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (pump_.joinable()) pump_.join();
    ReleaseStreamObjects();
    state_ = PU_STREAM_FAILED;
    PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
    PushError(PU_ERR_MEDIA, start_error);
    Fail(start_error, error);
    return PU_ERR_MEDIA;
  }

  // v2：生成 offer 并设置本地描述。放在采集线程起来之后，这样编码器启动失败时
  // 不用回滚已经发出去的 SDP。
  {
    std::string sdp_error;
    if (!CreateOfferAndSetLocal(&sdp_error)) {
      stop_requested_.store(true, std::memory_order_relaxed);
      if (pump_.joinable()) pump_.join();
      ReleaseStreamObjects();
      state_ = PU_STREAM_FAILED;
      PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
      PushError(PU_ERR_MEDIA, sdp_error);
      Fail(sdp_error, error);
      return PU_ERR_MEDIA;
    }
  }

  running_ = true;
  state_ = PU_STREAM_RUNNING;
  PushState(PU_STREAM_RUNNING, PU_STREAM_STARTING, PU_REASON_REQUESTED);
  // offer 要等 ICE 收集完成才发得出去，交给独立的协商线程，不阻塞 start。
  StartNegotiationEmit("offer");
  Log(3, "screen share started (synthetic %dx%d @%dfps)", kWidth, kHeight, kFps);
  return PU_OK;
}

int MediaEngine::Stop() {
  std::lock_guard<std::mutex> control_lock(control_mu_);

  // v2：先停协商线程。它会在 ICE 收集完成后往事件队列推一条信令，必须在关闭 PC
  // 之前收掉，否则会出现「stop 之后还冒出一条 offer」。
  negotiator_stop_.store(true, std::memory_order_relaxed);
  if (negotiator_.joinable()) negotiator_.join();

  if (!running_) {
    // 正常路径是幂等返回；但如果 start() 在中途抛异常，采集线程可能已经起来而
    // running_ 还是 false，这里必须把它收掉，否则 std::thread 析构会 terminate。
    if (pump_.joinable()) {
      stop_requested_.store(true, std::memory_order_relaxed);
      pump_.join();
    }
    // 接收侧 PC（没 start、只收不发）也要关掉；没有 PC 时这里是无操作。
    ReleaseStreamObjects();
    return PU_OK;
  }

  state_ = PU_STREAM_STOPPING;
  PushState(PU_STREAM_STOPPING, PU_STREAM_RUNNING, PU_REASON_REQUESTED);

  stop_requested_.store(true, std::memory_order_relaxed);
  if (pump_.joinable()) pump_.join();

  running_ = false;
  ReleaseStreamObjects();

  state_ = PU_STREAM_IDLE;
  PushState(PU_STREAM_IDLE, PU_STREAM_STOPPING, PU_REASON_REQUESTED);
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    dropped = dropped_events_;
  }
  Log(3, "screen share stopped (event queue drops: %llu)",
      static_cast<unsigned long long>(dropped));
  return PU_OK;
}

void MediaEngine::ReleaseStreamObjects() {
  // 顺序照抄 spike：先 Close()，再释放 track / pc；这三件事都必须在 signaling
  // 线程上做（VideoTrack 与 PeerConnection 的方法都有线程断言）。
  if (signaling_ != nullptr) {
    signaling_->BlockingCall([this] {
      if (pc_ != nullptr) {
        // 摘远端 sink 要在释放 track / PC 之前，且只能在 signaling 线程。
        if (remote_track_ != nullptr && remote_sink_ != nullptr) {
          remote_track_->RemoveSink(remote_sink_);
        }
        remote_track_ = nullptr;
        pc_->Close();
      }
      track_ = nullptr;
      pc_ = nullptr;
    });
  } else {
    track_ = nullptr;
    pc_ = nullptr;
    remote_track_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(cand_mu_);
    gathered_candidates_.clear();
    buffered_candidates_.clear();
  }
  source_ = nullptr;
  has_remote_description_ = false;
  sharing_ = false;
  local_sdp_emitted_.store(false, std::memory_order_relaxed);
  stat_peer_state_.store(PU_PEER_NONE, std::memory_order_relaxed);
  stat_ice_state_.store(PU_ICE_NEW, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// v2：PeerConnection 与信令
// ---------------------------------------------------------------------------

// 把 SDP 设成本地描述。signaling 线程是 pc 的属主；回调只写 AsyncResult。
// 注意这组函数绝不能在 webrtc 线程上被调用（会在 SetLocalDescription 里等自己）。
bool ApplyLocalDescription(webrtc::Thread* signaling,
                           webrtc::PeerConnectionInterface* pc,
                           webrtc::SdpType type,
                           const std::string& sdp,
                           std::string* error) {
  if (signaling == nullptr || pc == nullptr) {
    *error = "no peer connection";
    return false;
  }
  AsyncResult result;
  webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> observer(
      new webrtc::RefCountedObject<SetLocalObserver>(&result));
  std::string parse_error;
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(type, sdp, &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc->SetLocalDescription(std::move(desc), observer);
  });
  if (!parse_error.empty()) {
    *error = "parse local SDP failed: " + parse_error;
    return false;
  }
  if (!WaitAsync(&result, kSetSdpTimeoutMs) || !result.ok) {
    *error = "SetLocalDescription failed: " + (result.done ? result.error : std::string("timeout"));
    return false;
  }
  return true;
}

bool ApplyRemoteDescription(webrtc::Thread* signaling,
                            webrtc::PeerConnectionInterface* pc,
                            webrtc::SdpType type,
                            const std::string& sdp,
                            std::string* error) {
  if (signaling == nullptr || pc == nullptr) {
    *error = "no peer connection";
    return false;
  }
  AsyncResult result;
  webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface> observer(
      new webrtc::RefCountedObject<SetRemoteObserver>(&result));
  std::string parse_error;
  signaling->BlockingCall([&] {
    webrtc::SdpParseError sdp_error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc =
        webrtc::CreateSessionDescription(type, sdp, &sdp_error);
    if (desc == nullptr) {
      parse_error = sdp_error.line + ": " + sdp_error.description;
      return;
    }
    pc->SetRemoteDescription(std::move(desc), observer);
  });
  if (!parse_error.empty()) {
    *error = "parse remote SDP failed: " + parse_error;
    return false;
  }
  if (!WaitAsync(&result, kSetSdpTimeoutMs) || !result.ok) {
    *error =
        "SetRemoteDescription failed: " + (result.done ? result.error : std::string("timeout"));
    return false;
  }
  return true;
}

bool MediaEngine::EnsurePeerConnection(bool for_sending, std::string* error) {
  if (pc_ != nullptr) {
    return true;  // 已有 PC：offerer 在 start 里建，answerer 在首个 offer 时建
  }
  if (signaling_ == nullptr || pc_factory_ == nullptr || pc_observer_ == nullptr) {
    *error = "engine is not initialised";
    return false;
  }

  std::string create_error;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    // 同进程回环只需要 host 候选；接 STUN/TURN 时在 config.ice_servers 里加。
    webrtc::PeerConnectionDependencies dependencies(pc_observer_);
    auto created = pc_factory_->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!created.ok()) {
      create_error = std::string("CreatePeerConnection failed: ") + created.error().message();
      return;
    }
    pc = created.value();
    if (for_sending) {
      if (track_ == nullptr) {
        create_error = "video track is not created";
        pc = nullptr;
        return;
      }
      auto added = pc->AddTrack(track_, {"partyup-screen"});
      if (!added.ok()) {
        create_error = std::string("AddTrack(video) failed: ") + added.error().message();
        pc = nullptr;
      }
    }
  });
  if (pc == nullptr) {
    *error = create_error.empty() ? "peer connection creation failed" : create_error;
    return false;
  }

  pc_ = pc;
  sharing_ = for_sending;
  has_remote_description_ = false;
  {
    std::lock_guard<std::mutex> lock(cand_mu_);
    gathered_candidates_.clear();
    buffered_candidates_.clear();
  }
  Log(3, "peer connection created (role=%s)", for_sending ? "offerer" : "answerer");
  return true;
}

bool MediaEngine::CreateOfferAndSetLocal(std::string* error) {
  if (pc_ == nullptr) {
    *error = "no peer connection";
    return false;
  }
  AsyncResult create;
  webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> observer(
      new webrtc::RefCountedObject<CreateSdpObserver>(&create));
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;  // 默认 sendrecv
    pc_->CreateOffer(observer.get(), options);
  });
  if (!WaitAsync(&create, kSdpTimeoutMs) || !create.ok) {
    *error = "CreateOffer failed: " + (create.done ? create.error : std::string("timeout"));
    return false;
  }
  Log(3, "offer generated (%zu bytes)", create.sdp.size());
  return ApplyLocalDescription(signaling_.get(), pc_.get(), webrtc::SdpType::kOffer, create.sdp,
                               error);
}

bool MediaEngine::CreateAnswerAndSetLocal(std::string* error) {
  if (pc_ == nullptr) {
    *error = "no peer connection";
    return false;
  }
  AsyncResult create;
  webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> observer(
      new webrtc::RefCountedObject<CreateSdpObserver>(&create));
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    pc_->CreateAnswer(observer.get(), options);
  });
  if (!WaitAsync(&create, kSdpTimeoutMs) || !create.ok) {
    *error = "CreateAnswer failed: " + (create.done ? create.error : std::string("timeout"));
    return false;
  }
  Log(3, "answer generated (%zu bytes)", create.sdp.size());
  return ApplyLocalDescription(signaling_.get(), pc_.get(), webrtc::SdpType::kAnswer, create.sdp,
                               error);
}

bool MediaEngine::SetRemoteDescriptionFromSdp(int type, const std::string& sdp,
                                              std::string* error) {
  return ApplyRemoteDescription(signaling_.get(), pc_.get(), static_cast<webrtc::SdpType>(type),
                                sdp, error);
}

std::vector<CandidateCopy> MediaEngine::SnapshotCandidates() const {
  std::lock_guard<std::mutex> lock(cand_mu_);
  return gathered_candidates_;
}

bool MediaEngine::WaitIceGatheringComplete(webrtc::PeerConnectionInterface* pc) {
  if (pc == nullptr || signaling_ == nullptr) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kGatherTimeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (negotiator_stop_.load(std::memory_order_relaxed)) return false;
    const auto state =
        signaling_->BlockingCall([pc] { return pc->ice_gathering_state(); });
    if (state == webrtc::PeerConnectionInterface::kIceGatheringComplete) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string MediaEngine::BuildLocalSdpWithCandidates(webrtc::PeerConnectionInterface* pc) {
  if (pc == nullptr) return {};
  const webrtc::SessionDescriptionInterface* local = pc->local_description();
  if (local == nullptr) return {};

  // local_description() 里可能没有 a=candidate（取决于版本与收集时机）。照抄 spike：
  // 先序列化一份，再把「SDP 里还没有的候选」补进 Clone，两种情况都正确。
  const std::string base = webrtc::SdpSerialize(*local);
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc = local->Clone();
  if (desc == nullptr) return base;

  int added = 0;
  int already = 0;
  for (const CandidateCopy& candidate : SnapshotCandidates()) {
    if (base.find(candidate.sdp) != std::string::npos) {
      ++already;
      continue;
    }
    std::unique_ptr<webrtc::IceCandidate> parsed =
        webrtc::IceCandidate::Create(candidate.mid, candidate.mline_index, candidate.sdp, nullptr);
    if (parsed != nullptr && desc->AddCandidate(parsed.get())) ++added;
  }

  std::string out = webrtc::SdpSerialize(*desc);
  int candidate_lines = 0;
  for (size_t pos = 0; (pos = out.find("a=candidate:", pos)) != std::string::npos; ++pos) {
    ++candidate_lines;
  }
  Log(3, "local sdp: %zu bytes, %d a=candidate line(s) (%d added here, %d already present)",
      out.size(), candidate_lines, added, already);
  return out;
}

void MediaEngine::StartNegotiationEmit(const char* signal_type) {
  if (negotiator_.joinable()) {
    negotiator_stop_.store(true, std::memory_order_relaxed);
    negotiator_.join();
  }
  negotiator_stop_.store(false, std::memory_order_relaxed);
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = pc_;
  if (pc == nullptr) return;
  const std::string type = signal_type;
  negotiator_ = std::thread([this, type, pc] { NegotiationEmitLoop(type, pc); });
}

void MediaEngine::NegotiationEmitLoop(
    std::string signal_type,
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
  const bool gathered = WaitIceGatheringComplete(pc.get());
  if (negotiator_stop_.load(std::memory_order_relaxed)) return;
  if (!gathered) {
    Log(2, "ICE gathering timed out (%d ms), emitting SDP with %zu candidate(s) anyway",
        kGatherTimeoutMs, SnapshotCandidates().size());
  }

  std::string sdp = signaling_->BlockingCall([this, pc] {
    return BuildLocalSdpWithCandidates(pc.get());
  });
  if (sdp.empty()) {
    PushError(PU_ERR_MEDIA, "local description is empty, cannot emit " + signal_type);
    return;
  }
  const uint32_t kind = (signal_type == "offer") ? PU_SIGNAL_OFFER : PU_SIGNAL_ANSWER;
  EmitSignal(signal_type.c_str(), MakeSdpEnvelope(signal_type.c_str(), sdp), kind);
  local_sdp_emitted_.store(true, std::memory_order_relaxed);
}

void MediaEngine::EmitSignal(const char* type, const std::string& payload, uint32_t kind) {
  pu_event event = MakeEvent(PU_EVENT_SIGNAL_OUT);
  event.data.signal.payload = nullptr;  // 真正投递时才填，见 Poll
  event.data.signal.payload_len = static_cast<uint32_t>(payload.size());
  event.data.signal.kind = kind;
  event.data.signal.reserved = 0;
  stat_signal_out_.fetch_add(1, std::memory_order_relaxed);
  Log(3, "signal out: %s (%zu bytes)", type, payload.size());
  PushEventWithPayload(event, payload);
}

void MediaEngine::ApplyBufferedCandidates() {
  std::vector<CandidateCopy> pending;
  {
    std::lock_guard<std::mutex> lock(cand_mu_);
    pending.swap(buffered_candidates_);
  }
  if (pending.empty() || pc_ == nullptr) return;
  for (const CandidateCopy& candidate : pending) {
    bool applied = false;
    signaling_->BlockingCall([this, &candidate, &applied] {
      std::unique_ptr<webrtc::IceCandidate> parsed = webrtc::IceCandidate::Create(
          candidate.mid, candidate.mline_index, candidate.sdp, nullptr);
      if (parsed != nullptr) applied = pc_->AddIceCandidate(parsed.get());
    });
    Log(3, "buffered candidate %s: %s", applied ? "applied" : "rejected", candidate.sdp.c_str());
  }
}

int MediaEngine::SignalIn(const char* payload, size_t len, std::string* error) {
  auto reject = [&](int code, const std::string& message) {
    if (error != nullptr) *error = message;
    SetLastError(message);
    return code;
  };

  if (payload == nullptr || len == 0) {
    return reject(PU_ERR_INVALID_ARG, "pu_signal_in: payload is null or empty");
  }

  SignalMessage message;
  if (!ParseSignal(payload, len, &message, error)) {
    SetLastError("pu_signal_in: " + (error != nullptr ? *error : std::string("bad payload")));
    return PU_ERR_INVALID_ARG;
  }
  Log(3, "signal in: type=%s (%zu bytes)", message.type.c_str(), len);

  if (message.type == "offer") {
    if (pc_ != nullptr && sharing_) {
      return reject(PU_ERR_STATE,
                    "received an offer while sharing (glare): ABI v2 has no perfect negotiation");
    }
    if (pc_ != nullptr) {
      return reject(PU_ERR_STATE,
                    "received a second offer on an answerer connection "
                    "(renegotiation is not supported in ABI v2)");
    }
    std::string local_error;
    if (!EnsurePeerConnection(/*for_sending=*/false, &local_error)) {
      return reject(PU_ERR_MEDIA, "pu_signal_in: " + local_error);
    }
    if (!SetRemoteDescriptionFromSdp(static_cast<int>(webrtc::SdpType::kOffer), message.sdp,
                                     &local_error)) {
      ReleaseStreamObjects();
      return reject(PU_ERR_MEDIA, "pu_signal_in: " + local_error);
    }
    has_remote_description_ = true;
    ApplyBufferedCandidates();
    if (!CreateAnswerAndSetLocal(&local_error)) {
      ReleaseStreamObjects();
      return reject(PU_ERR_MEDIA, "pu_signal_in: " + local_error);
    }
    // answer 同样要等 ICE 收集完成再发，交给协商线程。
    StartNegotiationEmit("answer");
    return PU_OK;
  }

  if (message.type == "answer") {
    if (pc_ == nullptr) {
      return reject(PU_ERR_STATE, "received an answer but this engine has no peer connection");
    }
    if (!sharing_) {
      return reject(PU_ERR_STATE,
                    "received an answer but this engine is the answerer (role mismatch)");
    }
    std::string local_error;
    if (!SetRemoteDescriptionFromSdp(static_cast<int>(webrtc::SdpType::kAnswer), message.sdp,
                                     &local_error)) {
      return reject(PU_ERR_MEDIA, "pu_signal_in: " + local_error);
    }
    has_remote_description_ = true;
    ApplyBufferedCandidates();
    return PU_OK;
  }

  // candidate / ice
  if (message.candidate.empty()) {
    // end-of-candidates：目前不需要额外动作（候选都已随 SDP 或单独的信令到了）。
    Log(3, "end-of-candidates received");
    return PU_OK;
  }
  // 先验一下候选本身合不合法：非法候选一律 PU_ERR_INVALID_ARG，与有没有 PC 无关，
  // 这样 UI 侧能稳定区分「信封坏了」和「状态不对」。
  {
    webrtc::SdpParseError sdp_error;
    std::unique_ptr<webrtc::IceCandidate> parsed = webrtc::IceCandidate::Create(
        message.mid, message.mline, message.candidate, &sdp_error);
    if (parsed == nullptr) {
      return reject(PU_ERR_INVALID_ARG,
                    "pu_signal_in: bad candidate (" + sdp_error.description + ")");
    }
  }
  if (pc_ == nullptr) {
    return reject(PU_ERR_STATE, "received a candidate but this engine has no peer connection");
  }
  if (!has_remote_description_) {
    // 远端描述还没设好就先存着（trickle 可能比 SDP 先到），等 SetRemote 之后统一加。
    std::lock_guard<std::mutex> lock(cand_mu_);
    CandidateCopy copy;
    copy.mid = message.mid;
    copy.mline_index = message.mline;
    copy.sdp = message.candidate;
    buffered_candidates_.push_back(copy);
    Log(3, "candidate buffered until remote description is set: %s", copy.sdp.c_str());
    return PU_OK;
  }
  bool applied = false;
  signaling_->BlockingCall([this, &message, &applied] {
    std::unique_ptr<webrtc::IceCandidate> parsed = webrtc::IceCandidate::Create(
        message.mid, message.mline, message.candidate, nullptr);
    if (parsed != nullptr) applied = pc_->AddIceCandidate(parsed.get());
  });
  if (!applied) {
    // 对重复/过期候选保持幂等容错：不把整条连接判死，只记一条日志。
    Log(2, "AddIceCandidate returned false for: %s", message.candidate.c_str());
  }
  return PU_OK;
}

int MediaEngine::PeerStats(pu_stats* out) {
  if (out == nullptr) {
    SetLastError("pu_peer_stats: out is null");
    return PU_ERR_INVALID_ARG;
  }
  if (out->struct_size < sizeof(pu_stats)) {
    SetLastError("pu_peer_stats: pu_stats.struct_size is smaller than the ABI struct");
    return PU_ERR_INVALID_ARG;
  }
  std::memset(out, 0, sizeof(*out));
  out->struct_size = static_cast<uint32_t>(sizeof(pu_stats));
  out->peer_state = stat_peer_state_.load(std::memory_order_relaxed);
  out->ice_state = stat_ice_state_.load(std::memory_order_relaxed);
  out->has_peer = pc_ != nullptr ? 1u : 0u;
  out->frames_sent = stat_frames_sent_.load(std::memory_order_relaxed);
  out->frames_encoded = stat_frames_encoded_.load(std::memory_order_relaxed);
  out->frames_dropped = stat_frames_dropped_.load(std::memory_order_relaxed);
  out->frames_received = stat_frames_received_.load(std::memory_order_relaxed);
  out->signal_out_count = stat_signal_out_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    out->event_drops = static_cast<uint32_t>(dropped_events_);
  }
  out->remote_width = stat_remote_width_.load(std::memory_order_relaxed);
  out->remote_height = stat_remote_height_.load(std::memory_order_relaxed);
  out->last_remote_frame_us = stat_last_remote_us_.load(std::memory_order_relaxed);
  return PU_OK;
}

void MediaEngine::OnRemoteFrame(const webrtc::VideoFrame& frame) {
  const uint32_t received =
      stat_frames_received_.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t sequence = stat_remote_sequence_.fetch_add(1, std::memory_order_relaxed);
  stat_remote_width_.store(frame.width(), std::memory_order_relaxed);
  stat_remote_height_.store(frame.height(), std::memory_order_relaxed);
  stat_last_remote_us_.store(NowUsSystem(), std::memory_order_relaxed);
  if (received == 1) {
    Log(3, "first remote frame: %dx%d ts=%lld", frame.width(), frame.height(),
        static_cast<long long>(frame.timestamp_us()));
  }

  pu_event event = MakeEvent(PU_EVENT_REMOTE_FRAME);
  event.data.remote.sequence = sequence;
  event.data.remote.timestamp_us = frame.timestamp_us();
  event.data.remote.received_at_us = NowUsSystem();
  event.data.remote.width = frame.width();
  event.data.remote.height = frame.height();
  event.data.remote.frames_received = received;
  event.data.remote.reserved = 0;
  PushEvent(event);
}

// ---------------------------------------------------------------------------
// 采集 / 编码（v1，未改语义，只加了统计计数）
// ---------------------------------------------------------------------------
void MediaEngine::PumpLoop() {

  std::unique_ptr<webrtc::VideoEncoder> encoder =
      encoder_factory_->Create(*env_, webrtc::SdpVideoFormat::VP8());
  if (encoder == nullptr) {
    SignalStart(false, "VP8 encoder unavailable (video encoder factory not installed?)");
    return;
  }

  const webrtc::VideoCodec codec = MakeVp8CodecSettings();
  const int init_rc = encoder->InitEncode(
      &codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false),
                                             static_cast<int>(std::thread::hardware_concurrency()),
                                             1200));
  if (init_rc != 0) {
    SignalStart(false, "VP8 InitEncode failed, rc=" + std::to_string(init_rc));
    return;
  }
  encoder->RegisterEncodeCompleteCallback(this);
  encoder_ = encoder.get();

  // 编码器就绪后才挂 sink：否则第一帧可能落进还没准备好的回调里。
  source_->AddOrUpdateSink(this, webrtc::VideoSinkWants());
  SignalStart(true, "");

  auto next_tick = std::chrono::steady_clock::now();
  // 帧时间戳必须用「像现在」的绝对值：从 0 起算（1970）会被当过期帧丢掉。
  int64_t timestamp_us = NowUsSystem();
  uint64_t index = 0;

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    webrtc::scoped_refptr<webrtc::I420Buffer> buffer = webrtc::I420Buffer::Create(kWidth, kHeight);
    FillI420(buffer.get(), static_cast<int>(index & 0xFF));
    const webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                         .set_video_frame_buffer(buffer)
                                         .set_timestamp_us(timestamp_us)
                                         .set_ntp_time_ms(timestamp_us / 1000)
                                         // RTP 时间戳是 90kHz；不设会全是 0。
                                         .set_rtp_timestamp(
                                             static_cast<uint32_t>(timestamp_us * 90 / 1000))
                                         .build();

    // 同步回调 OnFrame → encoder->Encode，都在本线程；同时 PC 的 VideoStreamEncoder
    // 也挂着同一个源，所以这一帧会被两路分别编码（见文件头注释的「已知代价」）。
    source_->PushFrame(frame);
    stat_frames_sent_.fetch_add(1, std::memory_order_relaxed);

    ++index;
    timestamp_us += kFrameIntervalUs;
    next_tick += std::chrono::microseconds(kFrameIntervalUs);
    if (next_tick < std::chrono::steady_clock::now()) {
      // 落后超过一个周期就直接对齐，不补帧（补帧只会让时间戳更假）。
      next_tick = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_until(next_tick);
  }

  encoder_ = nullptr;
  source_->RemoveSink(this);
  encoder->RegisterEncodeCompleteCallback(nullptr);
  encoder->Release();
  Log(3, "encoder released after %llu frames", static_cast<unsigned long long>(index));
}

void MediaEngine::SignalStart(bool ok, const std::string& error) {
  {
    std::lock_guard<std::mutex> lock(start_mu_);
    start_done_ = true;
    start_ok_ = ok;
    start_error_ = error;
  }
  start_cv_.notify_all();
}

void MediaEngine::OnFrame(const webrtc::VideoFrame& frame) {
  if (encoder_ == nullptr) return;
  capture_ts_us_ = frame.timestamp_us();
  // 丢弃返回值没关系：真的要丢帧时编码器会走 OnFrameDropped。
  static_cast<void>(encoder_->Encode(frame, nullptr));
}

webrtc::EncodedImageCallback::Result MediaEngine::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* codec_specific_info) {
  static_cast<void>(codec_specific_info);

  const uint64_t sequence = frame_sequence_++;
  stat_frames_encoded_.fetch_add(1, std::memory_order_relaxed);
  const int64_t now_us = NowUsSteady();
  rate_window_.emplace_back(now_us, static_cast<uint32_t>(encoded_image.size()));
  while (!rate_window_.empty() && now_us - rate_window_.front().first > kRateWindowUs) {
    rate_window_.pop_front();
  }
  uint64_t window_bytes = 0;
  for (const auto& entry : rate_window_) {
    window_bytes += entry.second;
  }
  // 早期窗口太短时按 100ms 兜底，避免第一帧算出天文数字。
  const int64_t span_us =
      std::max<int64_t>(100000, std::min<int64_t>(kRateWindowUs, now_us - rate_window_.front().first));
  const uint32_t bitrate_kbps =
      static_cast<uint32_t>(window_bytes * 8 * 1000000ULL / static_cast<uint64_t>(span_us) / 1000ULL);

  pu_event event = MakeEvent(PU_EVENT_FRAME);
  event.timestamp_us = NowUsSystem();
  event.data.frame.sequence = sequence;
  event.data.frame.capture_ts_us = capture_ts_us_;
  event.data.frame.width = kWidth;
  event.data.frame.height = kHeight;
  event.data.frame.bytes = static_cast<uint32_t>(encoded_image.size());
  event.data.frame.bitrate_kbps = bitrate_kbps;
  event.data.frame.keyframe =
      (encoded_image._frameType == webrtc::VideoFrameType::kVideoFrameKey) ? 1u : 0u;
  event.data.frame.reserved = 0;
  PushEvent(event);

  return webrtc::EncodedImageCallback::Result(
      webrtc::EncodedImageCallback::Result::OK, static_cast<uint32_t>(sequence));
}

void MediaEngine::OnFrameDropped(uint32_t rtp_timestamp,
                                 int spatial_id,
                                 bool is_end_of_temporal_unit) {
  static_cast<void>(rtp_timestamp);
  static_cast<void>(spatial_id);
  static_cast<void>(is_end_of_temporal_unit);
  stat_frames_dropped_.fetch_add(1, std::memory_order_relaxed);
  Log(4, "encoder dropped a frame");
}

pu_event MediaEngine::MakeEvent(uint32_t type) const {
  pu_event event;
  std::memset(&event, 0, sizeof(event));
  event.struct_size = static_cast<uint32_t>(sizeof(pu_event));
  event.type = type;
  event.timestamp_us = NowUsSystem();
  return event;
}

void MediaEngine::PushState(int32_t state, int32_t previous, int32_t reason) {
  pu_event event = MakeEvent(PU_EVENT_STATE);
  event.data.state.state = state;
  event.data.state.previous_state = previous;
  event.data.state.reason = reason;
  event.data.state.reserved = 0;
  PushEvent(event);
  Log(3, "state %d -> %d (reason %d)", previous, state, reason);
}

void MediaEngine::PushError(int32_t code, const std::string& message) {
  pu_event event = MakeEvent(PU_EVENT_ERROR);
  event.data.error.code = code;
  event.data.error.reserved = 0;
  std::snprintf(event.data.error.message, sizeof(event.data.error.message), "%s", message.c_str());
  PushEvent(event);
}

void MediaEngine::PushEvent(const pu_event& event) {
  PushEventWithPayload(event, std::string());
}

void MediaEngine::PushEventWithPayload(pu_event event, std::string payload) {
  std::lock_guard<std::mutex> lock(queue_mu_);
  if (queue_.size() >= capacity_) {
    ++dropped_events_;
    // 帧类事件丢了就丢了；状态/错误/信令更重要，挤掉队首的旧事件腾位置。
    // 注意：被挤掉的信令 JSON 从来没投递出去过，不会有调用方拿着它的指针。
    if (event.type == PU_EVENT_FRAME || event.type == PU_EVENT_REMOTE_FRAME) return;
    queue_.pop_front();
  }
  QueuedEvent entry;
  entry.event = event;
  if (event.type == PU_EVENT_SIGNAL_OUT) {
    entry.event.data.signal.payload = nullptr;  // 投递时才指向 delivered_signals_
    entry.event.data.signal.payload_len = static_cast<uint32_t>(payload.size());
  }
  entry.payload = std::move(payload);
  queue_.push_back(std::move(entry));
  queue_cv_.notify_one();
}

int MediaEngine::Poll(pu_event* out, int timeout_ms) {
  if (out == nullptr) {
    SetLastError("pu_poll_event: out is null");
    return PU_ERR_INVALID_ARG;
  }
  if (out->struct_size < sizeof(pu_event)) {
    SetLastError("pu_poll_event: pu_event.struct_size is smaller than the ABI struct");
    return PU_ERR_INVALID_ARG;
  }

  std::unique_lock<std::mutex> lock(queue_mu_);
  const auto has_event = [this] { return !queue_.empty(); };

  if (timeout_ms == 0) {
    if (!has_event()) return PU_ERR_TIMEOUT;
  } else if (timeout_ms < 0) {
    queue_cv_.wait(lock, has_event);
  } else if (!queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), has_event)) {
    return PU_ERR_TIMEOUT;
  }

  QueuedEvent& front = queue_.front();
  *out = front.event;
  if (front.event.type == PU_EVENT_SIGNAL_OUT) {
    // 把这条 JSON 移进「已投递」保留区，出队的只是队列项，字符串本体仍然有效
    // （std::deque<std::string> 的 push_back 不会让已有元素搬家）。
    delivered_signals_.push_back(std::move(front.payload));
    while (delivered_signals_.size() > kMaxRetainedSignals) delivered_signals_.pop_front();
    const std::string& payload = delivered_signals_.back();
    out->data.signal.payload = payload.c_str();
    out->data.signal.payload_len = static_cast<uint32_t>(payload.size());
  }
  queue_.pop_front();
  return PU_OK;
}

}  // namespace
}  // namespace partyup

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
struct pu_engine {
  partyup::MediaEngine core;
};

extern "C" {

PU_API pu_engine* pu_engine_create(const pu_config* cfg) {
  try {
    if (cfg != nullptr && cfg->struct_size != sizeof(pu_config)) {
      partyup::SetCreateError("pu_config.struct_size mismatch (header/library version skew?)");
      return nullptr;
    }
    std::unique_ptr<pu_engine> engine(new pu_engine());
    std::string error;
    if (!engine->core.Init(cfg, &error)) {
      partyup::SetCreateError(error.empty() ? "media engine init failed" : error);
      return nullptr;
    }
    partyup::SetCreateError("");
    return engine.release();
  } catch (const std::exception& ex) {
    partyup::SetCreateError(std::string("pu_engine_create exception: ") + ex.what());
    return nullptr;
  } catch (...) {
    partyup::SetCreateError("pu_engine_create: unknown exception");
    return nullptr;
  }
}

PU_API void pu_engine_destroy(pu_engine* e) {
  if (e == nullptr) return;
  try {
    delete e;
  } catch (...) {
    // 析构里不该有异常；真发生了也不能让它穿越 ABI。
  }
}

PU_API int pu_screen_start(pu_engine* e, pu_screen_id screen_id) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.Start(screen_id, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_start exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_start: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API int pu_screen_stop(pu_engine* e) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    return e->core.Stop();
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_stop exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_stop: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API int pu_poll_event(pu_engine* e, pu_event* out, int timeout_ms) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    return e->core.Poll(out, timeout_ms);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_poll_event exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_poll_event: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API const char* pu_last_error(pu_engine* e) {
  if (e == nullptr) return partyup::CreateErrorCString();
  try {
    return e->core.LastError();
  } catch (...) {
    return "pu_last_error: unknown exception";
  }
}

/* ---- v2：信令通道 ---- */

PU_API int pu_signal_in(pu_engine* e, const char* payload, size_t len) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.SignalIn(payload, len, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_signal_in exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_signal_in: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API int pu_peer_stats(pu_engine* e, pu_stats* out) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    return e->core.PeerStats(out);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_peer_stats exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_peer_stats: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

/* ---- 以下为下一版实现：签名冻结，当前只回 PU_NOT_IMPLEMENTED ---- */

PU_API int pu_voice_join(pu_engine* e, const pu_voice_params* p) {
  static_cast<void>(p);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_join: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_leave(pu_engine* e) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_leave: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_mode(pu_engine* e, pu_mode mode) {
  static_cast<void>(mode);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_mode: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_mute(pu_engine* e, int muted) {
  static_cast<void>(muted);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_mute: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_deafen(pu_engine* e, int deafened) {
  static_cast<void>(deafened);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_deafen: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_sources(pu_engine* e, pu_source_list* out) {
  static_cast<void>(out);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_sources: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_watch(pu_engine* e, const char* sharer_id) {
  static_cast<void>(sharer_id);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_watch: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_unwatch(pu_engine* e, const char* sharer_id) {
  static_cast<void>(sharer_id);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_unwatch: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out) {
  static_cast<void>(stream_id);
  static_cast<void>(out);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_frame_acquire: not implemented in ABI v2");
  return PU_NOT_IMPLEMENTED;
}

PU_API void pu_frame_release(pu_engine* e, pu_frame* f) {
  static_cast<void>(f);
  if (e == nullptr) return;
  e->core.SetLastError("pu_frame_release: not implemented in ABI v2");
}

}  // extern "C"
