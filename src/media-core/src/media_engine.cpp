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
// v2.1（本版）删掉了 v1 的本地 VP8 编码器。以前采集源上并排挂着两个 sink
// （本地统计编码器 + PC 的 VideoStreamEncoder），同一帧被编码两次；现在采集源
// 只喂 VideoTrack，编码只在 PC 的 VideoStreamEncoder 里做一次。
//
// PU_EVENT_FRAME 的**语义与布局不变**（每帧一条、字段一个没动），但 bytes /
// keyframe / bitrate 改为从 PC 的 GetStats 派生：采样线程每 250 ms 发起一次
// 异步 GetStats（RTCStatsCollectorCallback），回调只把 outbound-rtp 的
// bytesSent / framesEncoded / keyFramesEncoded 累计量写进原子量，采集线程逐帧
// 发事件时读「最近一次采样窗口」的值。sequence / capture_ts_us / width / height
// 仍然由采集侧给出。GetStats 是异步回调，绝不能在调用线程上等它（见 §8）。
//
// v2.3（本版）把采集源从「合成帧」换成**真实 WGC 采集**：
//   pu_screen_sources  枚举真实显示器（id 用 HMONITOR，见 wgc_screen.h）+ 保留
//                      "synthetic" 作为测试源；
//   pu_screen_start    按 id 分派：synthetic 走原来的固定节拍合成循环，显示器 id
//                      走 wgc_screen.cpp（D3D11 → WGC → staging → libyuv I420）；
//   采集节拍          真实源是事件驱动的（WGC 的 FrameArrived），不用 16.7 ms 定时器；
//   每帧耗时           拆成 capture / readback / convert / encode-wait 四段，
//                      走 PARTYUP_MEDIA_LOG 的同一通道，停止时打印一行
//                      ===CAPTURE_SUMMARY=== 便于脚本取数。
// ABI 不变：pu_source_info 的布局没动，id/width/height/kind 都用 v2 已有字段。
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
#include <unordered_map>
#include <utility>
#include <vector>

// 第三方 JSON：nlohmann/json v3.11.3 单头文件（vendored，见
// src/media-core/vendor/nlohmann/README.md）。不用仓库自带的 jsoncpp，因为
// third_party/libwebrtc 只带了它的头文件、没有实现，webrtc.lib 里也没有符号。
#include "nlohmann/json.hpp"

// 真实屏幕采集（Windows Graphics Capture）：只做「显示器 → I420 帧」，
// 不依赖 libwebrtc 的 PeerConnection；DLL 里另一支编译单元见 wgc_screen.cpp。
#include "wgc_screen.h"

// v2.4 硬件 H.264 编解码（Media Foundation MFT）：编/解码器工厂 + 能力探测。
#include "hw_encoder.h"

// Media Foundation 进程级初始化（MFTEnumEx / MFCreateSample 都依赖 MFStartup）。
#include <mfapi.h>

// libwebrtc（m152）—— 与 spikes/media-link/main.cpp 同一组头文件
#include "api/audio/audio_device.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/media_types.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "libyuv.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video/video_codec_type.h"
#include "api/webrtc_sdp.h"
#include "modules/audio_device/include/audio_device_default.h"
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

// ---- PC 统计采样（v2.1）----
// PU_EVENT_FRAME 的 bytes / keyframe / bitrate 由 PC 的 GetStats 派生，采样节拍
// 250 ms：够快，让逐帧事件里的数字最多陈旧一个窗口；又够慢，不会让 stats 收集器
// 自己变成新的开销（一次 GetStats 约 1 ms）。
constexpr int kStatsSampleIntervalMs = 250;
// 采样线程的睡眠粒度：用它把「等 250 ms」切成可被 stop 打断的小片。
constexpr int kStatsSleepSliceMs = 10;
// stop 时在采样线程上等「最后一份报告」的上限：等不到就用手里的数，不阻塞控制线程。
// 只等 200 ms：正常一份 GetStats 是几毫秒，等太久只会拖慢 stop（实测曾把
// 「不带 stop 直接 destroy」拖到 580 ms）。
constexpr int kStatsFinalWaitMs = 200;

constexpr uint32_t kDefaultQueueCapacity = 256;

// ---- 信令 / 协商（v2）----
// ICE 收集超时：本机回环只会有 host 候选，实测 20–80 ms 收完；3 秒是给
// 「将来接 STUN/TURN」留的余量，超时也照发 SDP（里面带上当时已收集的候选）。
constexpr int kGatherTimeoutMs = 3000;
constexpr int kSdpTimeoutMs = 10000;     // CreateOffer / CreateAnswer
constexpr int kSetSdpTimeoutMs = 10000;  // SetLocal / SetRemote
// 已投递的信令 JSON 保留多少条。见头文件 pu_signal_event 注释。
constexpr size_t kMaxRetainedSignals = 64;

// ---- 信令会话标识（v2.10 第 2 步）----
// pu_signal_in_media 的 media 取值；也是 pu_signal_media 的输出值。
constexpr const char* kMediaVoice = "voice";
constexpr const char* kMediaScreen = "screen";

// 「已投递信令 → media」登记表（pu_signal_media 的实现基础）。
//
// 为什么不放进 pu_event：事件结构是对外 ABI 契约（见文件头的 static_assert 与头文件
// 注释），没有也不该有 media 字段；media 只在本库内部记录。而 pu_signal_media 的签名
// 是 (event, out, capacity)——没有引擎句柄，所以只能按「事件里的 payload 指针」查。
// 生命周期与 pu_signal_event.payload 完全一致：Poll 把指针交给调用方时登记，
// 该 payload 被回收（保留环滚出 64 条 / 引擎销毁）时删除。指针在 deque 里稳定，
// 同一时刻不可能被两条事件共用，所以指针做键是唯一的。
std::mutex g_delivered_media_mu;
std::unordered_map<const char*, std::string> g_delivered_media;

void RegisterDeliveredMedia(const char* payload, const std::string& media) {
  if (payload == nullptr) return;
  std::lock_guard<std::mutex> lock(g_delivered_media_mu);
  g_delivered_media[payload] = media;
}

void UnregisterDeliveredMedia(const char* payload) {
  if (payload == nullptr) return;
  std::lock_guard<std::mutex> lock(g_delivered_media_mu);
  g_delivered_media.erase(payload);
}

// pu_signal_media 的实现主体（无引擎参数，见头文件契约）。
int SignalMediaOfEvent(const pu_event* event, char* media_out, int capacity) {
  if (event == nullptr || media_out == nullptr || capacity <= 0) return PU_ERR_INVALID_ARG;
  if (event->struct_size < sizeof(pu_event)) return PU_ERR_INVALID_ARG;
  if (event->type != PU_EVENT_SIGNAL_OUT) return PU_ERR_INVALID_ARG;
  if (event->data.signal.payload == nullptr) return PU_ERR_STATE;
  std::lock_guard<std::mutex> lock(g_delivered_media_mu);
  const auto it = g_delivered_media.find(event->data.signal.payload);
  if (it == g_delivered_media.end() || it->second.empty()) return PU_ERR_STATE;
  const size_t needed = it->second.size() + 1;  // 含结尾 0
  if (static_cast<size_t>(capacity) < needed) return PU_ERR_INVALID_ARG;
  std::memcpy(media_out, it->second.c_str(), needed);
  return PU_OK;
}

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
// 本地视频源（合成帧与真实 WGC 采集共用）：直接实现 VideoTrackSourceInterface，
// 帧分发交给 VideoBroadcaster（线程安全）。类不能写 final —— 外面要套
// webrtc::RefCountedObject<T>。
// ---------------------------------------------------------------------------
class ScreenVideoSource : public webrtc::VideoTrackSourceInterface {
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
// PC 侧统计（v2.1）：PU_EVENT_FRAME 的 bytes / keyframe / bitrate 从这里来
// ---------------------------------------------------------------------------

// GetStats 回调与采样线程之间只共享这块状态：回调写累计量，采样线程读它算增量。
// 用 shared_ptr 是因为回调可能比引擎活得久（GetStats 是异步的）：它落下最后一份
// 报告时必须仍然安全，所以它只碰这块内存，绝不碰 MediaEngine。
struct PcStatsState {
  std::atomic<uint64_t> bytes_sent{0};          // outbound-rtp.bytesSent
  std::atomic<uint64_t> frames_encoded{0};      // outbound-rtp.framesEncoded
  std::atomic<uint64_t> key_frames_encoded{0};  // outbound-rtp.keyFramesEncoded
  std::atomic<uint32_t> reports{0};             // 收到过几份有效报告（0 = 还没有）
};

// GetStats 的回调（在 signaling 线程上被调用）。只读报告、只写原子量：
// 不阻塞、不重入、不回调引擎。不能加 final —— 外面要套 webrtc::RefCountedObject。
class StatsCollector : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsCollector(std::shared_ptr<PcStatsState> state) : state_(std::move(state)) {}

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    if (report == nullptr || state_ == nullptr) return;
    uint64_t bytes = 0;
    uint64_t frames = 0;
    uint64_t keys = 0;
    bool found_video = false;
    for (const webrtc::RTCOutboundRtpStreamStats* out :
         report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      if (out == nullptr) continue;
      // framesEncoded / keyFramesEncoded 只有视频流才有；音频的 outbound-rtp 不带
      // 这两项，直接跳过（本切片也只有一路视频）。
      if (!out->frames_encoded.has_value() && !out->key_frames_encoded.has_value()) continue;
      found_video = true;
      if (out->bytes_sent.has_value()) bytes += *out->bytes_sent;
      if (out->frames_encoded.has_value()) frames += *out->frames_encoded;
      if (out->key_frames_encoded.has_value()) keys += *out->key_frames_encoded;
    }
    if (!found_video) return;  // PC 刚建好、还没有 outbound video 统计
    state_->bytes_sent.store(bytes, std::memory_order_relaxed);
    state_->frames_encoded.store(frames, std::memory_order_relaxed);
    state_->key_frames_encoded.store(keys, std::memory_order_relaxed);
    state_->reports.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<PcStatsState> state_;
};

// ---------------------------------------------------------------------------
// 空音频设备模块（v2.2）：本版媒体核心不拥有任何音频硬件
// ---------------------------------------------------------------------------
// EnableMedia() 需要一个 AudioDeviceModule。不注入的话 libwebrtc 会建一个**真的**
// Windows Core Audio（WASAPI）ADM：构造时 CoCreateInstance（COM/MTA），
// WebRtcVoiceEngine::Init 在 worker 线程上 InitSpeaker/InitMicrophone，最后在
// PeerConnectionFactory 析构时走 ~AudioDeviceWindowsCore（Terminate + Release
// IMMDeviceEnumerator/COM 端点 + 卸载 Avrt.dll）。本版 pu_voice_* 全部返回
// PU_NOT_IMPLEMENTED，这个 ADM 一点也用不到，却把「COM + 音频端点 + 音频线程」的
// 收尾拉进了每次 pu_engine_destroy 的路径上。
//
// 2026-11-02 的 teardown 压测在 ~AudioDeviceWindowsCore 里抓到 0xC0000005
// （crash-iter-0001）——栈是 工厂析构 → 媒体引擎析构 → ADM Release → 平台 ADM
// 析构，正是这条路。修法不是把析构挪个位置，而是**让引擎根本不创建真实 ADM**：
// 用 AudioDeviceModuleDefault 的默认实现组装一个「什么都不做」的 ADM（语义等价于
// webrtc 的 kDummyAudio：Init/Terminate/InitPlayout/InitRecording 全返回 0，
// 没有平台资源、没有线程、没有 COM 对象），工厂析构因此不再触碰 WASAPI。
//
// 生命周期契约（后续实现语音时必须遵守）：
//   * 本类的实例由 PeerConnectionFactory 通过 deps.adm 独占（scoped_refptr），
//     随工厂一起析构；引擎不另存引用。
//   * 语音落地时把这里换成真实 ADM，并保证「创建与析构在同一条线程」，且该线程
//     在工厂析构时仍然存活（见 docs/spikes/2026-11-01-signaling-abi.md §10）。
class NullAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
 public:
  NullAudioDeviceModule() = default;
};

// ---------------------------------------------------------------------------
// 引擎实现
// ---------------------------------------------------------------------------
// 语音接收侧计数器：每收到一个 10 ms 音频帧 +1，并记录峰值振幅（mute 验收用）。
// 由 ADM 播放线程调用，只碰原子量。
// SFU 预协商槽位的坑（2026-11-03 实测）：relay 的 SDP 预协商 12 条 sendonly 槽，
// 客户端为每条 m-line 建一个远端音轨，libwebrtc **对空槽也按 10ms 送静音帧**，
// OnData 照常回调、峰值恒为 0。所以「有回调」不等于「有人在说话」——必须先算峰值，
// 只有峰值达到阈值才记帧数、才刷新 last_frame_ms；否则 tracks/活跃判定会把空槽全点亮。
//
// 阈值定位（重要）：这是**非零门控**，不是 VAD / 人声检测。
//   目标只有一个：挡掉 SFU 预协商空槽送来的**全零静音帧**（peak=0），
//   让 pu_voice_tracks() 不会把 12 条空槽都当成「有人」返回。默认 8 足够。
//   「谁在说话」要区分人声与环境噪声，取决于设备增益（实测某笔记本麦克风噪声峰值
//   可达 ~300），由客户端基于 peak/last_frame_ms 自适应判定，媒体核心只给原始量。
// 阈值默认 8（16 bit 采样绝对值），可用环境变量 PARTYUP_VOICE_PEAK_THRESHOLD 覆盖
// （1~32767）。注意：/MT DLL 有自己一份 CRT environ，宿主 _putenv_s 改不到，
// 必须用 GetEnvironmentVariableA 读 Win32 进程环境块（见 docs/spikes C ABI 文档）。
uint32_t VoicePeakThresholdFromEnv() {
  // 注意：本 DLL 用 /MT（静态 CRT），std::getenv 读的是**本模块 CRT 自己那份**
  // environ 副本，宿主进程 _putenv_s 改不到它。所以这里直接问 Win32 进程环境块
  // （GetEnvironmentVariableA），宿主用 SetEnvironmentVariable 或进程启动环境都能生效。
  char buffer[32] = {0};
  const DWORD len = ::GetEnvironmentVariableA("PARTYUP_VOICE_PEAK_THRESHOLD", buffer,
                                              static_cast<DWORD>(sizeof(buffer)));
  if (len == 0 || len >= sizeof(buffer)) return 8;
  const long parsed = std::strtol(buffer, nullptr, 10);
  if (parsed < 1) return 1;
  if (parsed > 32767) return 32767;
  return static_cast<uint32_t>(parsed);
}

class AudioCountSink : public webrtc::AudioTrackSinkInterface {
 public:
  AudioCountSink(std::atomic<uint32_t>* frames, std::atomic<uint32_t>* peak,
                 std::atomic<int64_t>* last_ms, std::atomic<uint32_t>* max_peak,
                 uint32_t threshold)
      : frames_(frames), peak_(peak), last_ms_(last_ms), max_peak_(max_peak),
        threshold_(threshold) {}

  void OnData(const void* audio_data, int bits_per_sample, int sample_rate,
              size_t number_of_channels, size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override {
    static_cast<void>(sample_rate);
    static_cast<void>(absolute_capture_timestamp_ms);
    if (audio_data == nullptr || bits_per_sample != 16) return;
    const int16_t* pcm = static_cast<const int16_t*>(audio_data);
    const size_t count = number_of_channels * number_of_frames;
    uint32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
      const int32_t v = pcm[i] < 0 ? -static_cast<int32_t>(pcm[i]) : pcm[i];
      if (static_cast<uint32_t>(v) > peak) peak = static_cast<uint32_t>(v);
    }
    peak_->store(peak, std::memory_order_relaxed);
    if (max_peak_ != nullptr && peak > max_peak_->load(std::memory_order_relaxed)) {
      max_peak_->store(peak, std::memory_order_relaxed);
    }
    if (peak < threshold_) return;  // 静音帧/空槽：不计帧、不刷新时间戳
    frames_->fetch_add(1, std::memory_order_relaxed);
    if (last_ms_ != nullptr) last_ms_->store(NowUsSteady() / 1000, std::memory_order_relaxed);
  }

 private:
  std::atomic<uint32_t>* frames_;
  std::atomic<uint32_t>* peak_;
  std::atomic<int64_t>* last_ms_;
  std::atomic<uint32_t>* max_peak_;
  uint32_t threshold_;
};

class MediaEngine {
 public:
  // 两个辅助类在类外定义（需要 MediaEngine 完整类型），这里只前置声明。
  class PcObserver;       // webrtc::PeerConnectionObserver：把状态写回原子量、挂远端 sink
  class RemoteFrameSink;  // 远端视频轨的 sink：把帧转给引擎

  MediaEngine() = default;
  ~MediaEngine();

  MediaEngine(const MediaEngine&) = delete;
  MediaEngine& operator=(const MediaEngine&) = delete;

  bool Init(const pu_config* cfg, std::string* error);
  // v2.11：flags bit0 = passive（被动共享方：采屏 + 等对端 offer，不发 offer）。
  // flags == 0 与旧行为完全一致；其余 bit 保留、非 0 返回 PU_ERR_INVALID_ARG。
  int Start(const char* screen_id, uint32_t flags, std::string* error);
  // v2.11：观看者预建屏幕会话（answerer，不采屏、不建轨）。幂等。
  int PrepareScreen(std::string* error);
  // v2.12：引擎级 ICE servers。urls 换行分隔、只接受 stun:（无凭证）；
  // nullptr / len==0 清空。只影响之后新建的 PC（屏幕与语音两条共用这份配置）。
  int SetIceServers(const char* urls, size_t len, std::string* error);
  // 建 PC 前把上面那份配置拷进 RTCConfiguration.servers（内部加锁，幂等）。
  void ApplyIceServersToConfig(webrtc::PeerConnectionInterface::RTCConfiguration* config);
  // v2.13（**测试专用，不是产品 ABI**）：从环境变量读「码率上限 / 降级偏好」并套到
  // 屏幕 sender 的 RtpParameters 上，用来实测 quality scaler 降分辨率时会发生什么。
  //   PARTYUP_MEDIA_TEST_MAX_BITRATE=<bps>       → encodings[0].max_bitrate_bps
  //   PARTYUP_MEDIA_TEST_DEGRADATION=maintain_resolution|maintain_framerate|balanced
  // 生产路径不会设这两个变量，函数第一行就返回，行为与之前完全一致。
  void ApplyVideoSenderParamsOnSignalingThread();
  // v2.6：语音（1:1 直连）。全部在控制线程调用，内部把 PC/ADM 操作投 signaling 线程。
  int VoiceJoin(const pu_voice_params* params, std::string* error);
  int VoiceLeave();
  int VoiceSetMode(pu_mode mode);
  int VoiceSetMute(bool muted);
  int VoiceSetDeafen(bool deafened);
  // v2.7：按轨读远端音频统计（out==nullptr 时只返回活跃轨数）。
  int VoiceTracks(pu_voice_track* out, uint32_t capacity);
  int Stop();
  // v2.3：枚举可共享的源（真实显示器 + "synthetic"），结果缓存在成员里。
  int Sources(pu_source_list* out);
  int Poll(pu_event* out, int timeout_ms);
  const char* LastError();
  void SetLastError(const std::string& message);

  // v2：把一条服务端信令灌进引擎（同步执行）
  // v2.10 第 2 步：media 显式指定会话（"voice"/"screen"）；nullptr/"" 退回 SDP 嗅探。
  int SignalIn(const char* media, const char* payload, size_t len, std::string* error);

  // v2.5：远端帧的 BGRA 快照（pu_frame_acquire / pu_frame_release 的后端）。
  // 没有比上次 acquire 更新的帧时返回 PU_ERR_TIMEOUT（"还没好"，不是错误）。
  int AcquireFrame(const char* stream_id, pu_frame* out);
  void ReleaseFrame(pu_frame* frame);

  // ---- 远端帧三槽缓冲 ----
  // 解码线程写、UI 线程读。写入方只在「没被 acquire 占用」的槽里转换，转完再把
  // 它挂成 ready；读取方拿到 ready 槽并置 busy，直到 release 才允许复用 ——
  // 读到的永远是完整一帧，不会撕裂；也不需要每帧分配。
  struct FrameSlot {
    std::vector<uint8_t> bytes;  // BGRA8，按需扩容后复用
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;         // 如实行步长（当前 == width*4，但不做假设）
    int64_t timestamp_us = 0;
    uint64_t seq = 0;
    bool busy = false;
  };
  static constexpr int kFrameSlots = 3;
  FrameSlot frame_slots_[kFrameSlots];
  std::mutex frame_mu_;
  int frame_write_slot_ = 0;   // 只有解码线程读写
  int frame_ready_slot_ = -1;  // 以下都受 frame_mu_ 保护
  int frame_acquired_slot_ = -1;
  uint64_t frame_ready_seq_ = 0;
  uint64_t frame_served_seq_ = 0;
  uint64_t frame_unpaired_release_ = 0;
  // v2：读统计快照
  int PeerStats(pu_stats* out);

  // v2：远端视频轨的 sink 回调，解码线程（worker）上被调用
  void OnRemoteFrame(const webrtc::VideoFrame& frame);

 private:
  void PumpLoop();
  // 真实采集（WGC）的事件驱动循环；由 PumpLoop 分派，不单独起线程。
  void RealCaptureLoop();
  void SignalStart(bool ok, const std::string& error);
  void PushState(int32_t state, int32_t previous, int32_t reason);
  void PushError(int32_t code, const std::string& message);
  void PushEvent(const pu_event& event);
  // v2.10 第 2 步：media 是这条事件所属的信令会话（"voice"/"screen"），
  // 只对 PU_EVENT_SIGNAL_OUT 有意义；其它事件传空串。
  void PushEventWithPayload(pu_event event, std::string payload, std::string media = {});
  pu_event MakeEvent(uint32_t type) const;
  void Fail(const std::string& message, std::string* error);
  void ReleaseStreamObjects();
  // v2.10：语音会话自己的收尾（关 voice_pc_、join voice_negotiator_、删 voice_observer_）。
  // 与 ReleaseStreamObjects（屏幕会话）互不依赖：两条 PC 各自关闭、各自释放。
  // TODO(step4): 合并到参数化的 MediaSession 版本，勿在此新增逻辑（只做搬家）。
  void ReleaseVoiceSession();
  void Log(int level, const char* format, ...);

  // ---- v2.1：PU_EVENT_FRAME 的数据源是 PC 的 GetStats ----
  // 逐帧发一条 PU_EVENT_FRAME；bytes / keyframe / bitrate 取最近一次采样值。
  void PushFrameEvent(int64_t capture_ts_us);
  void StartStatsSampler(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc);
  void StopStatsSampler();
  void StatsSampleLoop(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc);
  void RequestStats(webrtc::PeerConnectionInterface* pc);

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
  // v2.10 第 2 步：media 是该信令所属会话（kMediaVoice / kMediaScreen）。
  // 事件里没有 media 字段（ABI 不变），它随队列项走到 Poll，再由 pu_signal_media 查回。
  void EmitSignal(const char* media, const char* type, const std::string& payload, uint32_t kind);
  void ApplyBufferedCandidates();
  std::vector<CandidateCopy> SnapshotCandidates() const;
  void ResetSignalingStateLocked();
  // 等一个 ICE 收集完成的时刻（轮询，最多 kGatherTimeoutMs）
  bool WaitIceGatheringComplete(webrtc::PeerConnectionInterface* pc);

  // ---- v2.10 第 1 步：语音独立 PeerConnection 的协商 helper ----
  // 与上面那组屏幕 helper 一一对应，只是把 pc_ / cand_mu_ / gathered_candidates_ /
  // negotiator_* 换成 voice_pc_ / voice_cand_mu_ / voice_gathered_ / voice_negotiator_*。
  // TODO(step4): 合并到参数化的 MediaSession 版本，勿在此新增逻辑（只做搬家）。
  bool EnsureVoicePeerConnection(bool for_sending, std::string* error);
  bool VoiceCreateOfferAndSetLocal(std::string* error);
  bool VoiceCreateAnswerAndSetLocal(std::string* error);
  bool VoiceSetRemoteDescription(int type, const std::string& sdp, std::string* error);
  std::string VoiceBuildSdpWithCandidates(webrtc::PeerConnectionInterface* pc);
  void VoiceStartNegotiationEmit(const char* signal_type);
  void VoiceNegotiationEmitLoop(std::string signal_type,
                                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc);
  void VoiceApplyBufferedCandidates();

  // WinsockInitializer 必须先构造、最后析构（成员逆序析构）。
  webrtc::WinsockInitializer winsock_;

  std::unique_ptr<webrtc::Thread> network_;
  std::unique_ptr<webrtc::Thread> worker_;
  std::unique_ptr<webrtc::Thread> signaling_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory_;
  // v2.6：真实 WASAPI ADM（语音）。生命周期契约见 NullAudioDeviceModule 的注释：
  // 创建、Terminate、析构必须在同一条线程 —— 这里全程用 signaling_ 线程，
  // 且 ~MediaEngine 里「Terminate → 释放本引用 → 释放工厂」也在 signaling 线程上，
  // 那时三条线程都还活着（见 dtor）。
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> voice_adm_;

  // ---- v2.6：语音（1:1 直连）----
  bool voice_joined_ = false;            // 只在控制线程读写
  pu_mode voice_mode_ = PU_MODE_DIRECT;
  bool voice_muted_ = false;
  bool voice_deafened_ = false;
  std::string voice_channel_id_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> voice_source_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> voice_track_;
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> voice_sender_;
  // ---- v2.10 第 1 步：语音独立 PeerConnection（与屏幕的 pc_ 完全分开）----
  // 语音 PC 只承载 audio，屏幕 PC（pc_）只承载 video；两者角色/候选/协商线程/统计
  // 各自独立，一个失败或释放不影响另一个。
  // TODO(step4): 合并到参数化的 MediaSession 版本，勿在此新增逻辑（只做搬家）。
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> voice_pc_;
  PcObserver* voice_observer_ = nullptr;   // 必须比 voice_pc_ 活得久
  bool voice_offerer_ = false;             // 本端是语音会话的 offerer
  bool voice_has_remote_description_ = false;
  std::atomic<bool> voice_local_sdp_emitted_{false};
  std::thread voice_negotiator_;
  std::atomic<bool> voice_negotiator_stop_{false};
  mutable std::mutex voice_cand_mu_;
  std::vector<CandidateCopy> voice_gathered_;
  std::vector<CandidateCopy> voice_buffered_;
  std::atomic<int32_t> voice_peer_state_{PU_PEER_NONE};
  std::atomic<int32_t> voice_ice_state_{PU_ICE_NEW};
  // 每条远端音频轨一个计数器（v2.7）。SFU relay 下会有 N 路音频同时进来，
  // 全局单 sink 只能数第一路，所以要按 track id 分别挂 sink。
  // 线程：attach/detach 在 signaling 线程（audio_tracks_mu_ 保护）；
  // OnData 回调在音频线程，只碰 TrackAudioStat 里的原子量，不碰容器。
  struct TrackAudioStat {
    std::string id;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;
    std::unique_ptr<AudioCountSink> sink;
    std::atomic<uint32_t> frames{0};
    std::atomic<uint32_t> peak{0};
    std::atomic<int64_t> last_frame_ms{0};  // 单调时钟毫秒，用于判活/判「正在说话」
    std::atomic<uint32_t> max_peak{0};      // 历史最大峰值（调阈值用，日志可见）
  };
  std::mutex audio_tracks_mu_;
  std::vector<std::unique_ptr<TrackAudioStat>> audio_tracks_;
  void DetachAllAudioSinks();

  // 一路共享流的对象
  webrtc::scoped_refptr<ScreenVideoSource> source_;
  webrtc::scoped_refptr<webrtc::VideoTrack> track_;

  // ---- v2.3：真实屏幕采集 ----
  // capture_is_real_ / capture_source_id_ 只在控制线程读写（control_mu_ 保护），
  // 采集线程在起跑时读一次；尺寸由采集线程写、事件路径读，所以用原子量。
  std::atomic<bool> capture_is_real_{false};
  std::string capture_source_id_;                  // "synthetic" 或 "display:0x..."
  std::atomic<uint32_t> capture_width_{static_cast<uint32_t>(kWidth)};
  std::atomic<uint32_t> capture_height_{static_cast<uint32_t>(kHeight)};
  // pu_screen_sources 的输出缓存：id/title 的 c_str() 指针要活到下一次调用，
  // 所以把字符串和结构体都留在成员里（不要指向即将销毁的临时量）。
  std::vector<pu_source_info> source_items_;
  std::vector<std::string> source_ids_;
  std::vector<std::string> source_titles_;
  std::thread pump_;
  std::atomic<bool> stop_requested_{false};
  bool running_ = false;      // 只在控制线程读写（control_mu_ 保护）
  int32_t state_ = PU_STREAM_IDLE;

  std::mutex control_mu_;

  // start 握手：pump 线程起来后通知控制线程（v2.1 起没有编码器要初始化，
  // 这一步只保留「线程真的跑起来了」的语义）
  std::mutex start_mu_;
  std::condition_variable start_cv_;
  bool start_done_ = false;
  bool start_ok_ = false;
  std::string start_error_;

  // 采集侧仍负责的元数据：PU_EVENT_FRAME 的帧序号（只由 pump 线程写）。
  uint64_t frame_sequence_ = 0;

  // ---- v2：PeerConnection ----
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  PcObserver* pc_observer_ = nullptr;   // 必须比 pc_ 活得久（析构里最后删）
  RemoteFrameSink* remote_sink_ = nullptr;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_track_;
  std::thread negotiator_;
  std::atomic<bool> negotiator_stop_{false};
  bool sharing_ = false;               // 本引擎是 offerer（自己发 offer）
  bool has_remote_description_ = false;
  // ---- v2.11（第 3 步）：屏幕会话的被动角色 ----
  // screen_passive_：本端在采屏、但要发视频的一方是**对端**（relay 里服务端 offer，
  //   客户端 answer）。它和 sharing_ 互斥：passive 时 sharing_ 恒 false（本端不发 offer）。
  // screen_prepared_：屏幕 PC 已由 pu_screen_prepare 预建、还没被用过。首个 offer 进来时
  //   不能把它当「上一轮的残留」拆掉（stale 判定要排除它）。
  // video_sender_：passive 模式下把视频轨挂到对端 offer 的 m-line 上后留下的 sender，
  //   用来保证只挂一次（重复 offer 重协商时不再 AddTrack）。
  bool screen_passive_ = false;
  bool screen_prepared_ = false;
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;
  std::atomic<bool> local_sdp_emitted_{false};
  mutable std::mutex cand_mu_;
  std::vector<CandidateCopy> gathered_candidates_;   // OnIceCandidate 写
  std::vector<CandidateCopy> buffered_candidates_;   // SetRemote 之前到的一律先存

  // ---- v2.12：引擎级 ICE servers（pu_engine_set_ice_servers）----
  // 两条会话（屏幕 / 语音）共用同一份配置；建 PC 时由 ApplyIceServersToConfig()
  // 拷进 RTCConfiguration.servers。空 = host-only（与 v2.11 行为完全一致）。
  // 契约：只影响**之后**新建的 PC；已建立的会话要生效需 stop/leave 后重建。
  std::mutex ice_servers_mu_;
  webrtc::PeerConnectionInterface::IceServers ice_servers_;  // = vector<IceServer>

  // ---- v2：统计（全是原子量，pu_peer_stats 直接快照，不加锁） ----
  std::atomic<uint32_t> stat_frames_sent_{0};
  // 本地编码器删掉后就没人写它了：PC 侧的丢帧不在 outbound-rtp 统计里，留 0。
  std::atomic<uint32_t> stat_frames_dropped_{0};
  std::atomic<uint32_t> stat_frames_received_{0};
  std::atomic<uint32_t> stat_signal_out_{0};
  std::atomic<int32_t> stat_peer_state_{PU_PEER_NONE};
  std::atomic<int32_t> stat_ice_state_{PU_ICE_NEW};
  std::atomic<uint32_t> stat_remote_width_{0};
  std::atomic<uint32_t> stat_remote_height_{0};
  std::atomic<int64_t> stat_last_remote_us_{0};
  std::atomic<uint64_t> stat_remote_sequence_{0};

  // ---- v2.1：PC 侧统计采样（PU_EVENT_FRAME 的 bytes/key/bitrate 来源）----
  // pc_stats_ 与 GetStats 回调共享所有权：回调迟到（引擎已析构）也只是写一块
  // 独立内存。采样线程把自己那份 pc 引用揣在 lambda 里、不读 pc_，免得与控制
  // 线程释放 PC 打架。
  std::shared_ptr<PcStatsState> pc_stats_ = std::make_shared<PcStatsState>();
  std::thread stats_;
  std::atomic<bool> stats_stop_{false};
  // 最近一次采样窗口的派生值，采集线程逐帧发事件时直接读这三个原子量。
  std::atomic<uint32_t> sample_frame_bytes_{0};   // 窗口内平均每帧编码字节数
  std::atomic<uint32_t> sample_bitrate_kbps_{0};  // 窗口内码率（kbit/s）
  std::atomic<uint32_t> sample_keyframe_{0};      // 该窗口内 keyFramesEncoded 有增长

  // 事件队列（有界 deque）：多生产者（pump / signaling / 协商线程 / 控制线程）、
  // 单消费者。SIGNAL_OUT 的 JSON 跟着队列项一起走，投递时才把指针填进事件。
  struct QueuedEvent {
    pu_event event{};
    std::string payload;  // 只有 PU_EVENT_SIGNAL_OUT 会用到
    // v2.10 第 2 步：这条信令属于哪条会话（"voice"/"screen"）。内部结构，不跨 ABI；
    // Poll 投递时登记到 g_delivered_media，pu_signal_media 按 payload 指针查回。
    std::string media;
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
  // 可选的引擎日志文件（环境变量 PARTYUP_MEDIA_LOG）。teardown 压测用它把每个引擎
  // 实例的收尾阶段落到每次迭代自己的文件里；默认（未设置）只写 stderr。
  std::FILE* log_file_ = nullptr;
  // v2.4：本引擎是否成功调过 MFStartup（析构时配对 MFShutdown）。
  bool mf_started_ = false;
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
  // v2.10 第 1 步：同一个 observer 类服务两条会话，voice=true 时把状态/候选写到
  // 语音自己那套成员上（语音和屏幕各自独立 PC，互不影响）。
  // TODO(step4): 合并到参数化的 MediaSession 版本，勿在此新增逻辑（只做搬家）。
  explicit PcObserver(MediaEngine* engine, bool voice = false)
      : engine_(engine), voice_(voice) {}

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState /*state*/) override {}

  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    IceState().store(static_cast<int32_t>(state), std::memory_order_relaxed);
    engine_->Log(3, "ice_connection_state=%s",
                 std::string(webrtc::PeerConnectionInterface::AsString(state)).c_str());
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    PeerState().store(static_cast<int32_t>(state), std::memory_order_relaxed);
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
      std::lock_guard<std::mutex> lock(CandMu());
      GatheredCandidates().push_back(copy);
    }
    engine_->Log(3, "ice_candidate: %s", copy.sdp.c_str());
    // SDP 已经发出去了才来的候选，单独补一条 candidate 信令（trickle）。正常情况下
    // 我们要等收集完成才发 SDP，所以这条基本走不到；留着是为了将来接 TURN/SFU。
    // v2.10 第 2 步：candidate 信封里没有媒体类型，media 只能由 observer 自己的会话
    // 给出（这正是 pu_signal_in_media 存在的原因）。
    if (LocalSdpEmitted().load(std::memory_order_relaxed)) {
      engine_->EmitSignal(Media(), "candidate", MakeCandidateEnvelope(copy),
                          PU_SIGNAL_CANDIDATE);
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
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver = transceiver->receiver();
    AttachVideoSink(receiver);
    AttachAudioSink(receiver);
  }

  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> keep = receiver;
    AttachVideoSink(std::move(receiver));
    AttachAudioSink(keep);
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

  // 语音接收侧：每条远端音频轨挂**自己的**计数 sink（v2.7，relay 下多路）。
  // OnTrack 和 OnAddTrack 都会走到这里 → 按 track id 去重，重复调用不会重复挂载。
  void AttachAudioSink(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    if (receiver == nullptr) return;
    if (receiver->media_type() != webrtc::MediaType::AUDIO) return;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track = receiver->track();
    if (track == nullptr ||
        track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) {
      return;
    }
    auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
    const std::string id = track->id();
    std::lock_guard<std::mutex> lock(engine_->audio_tracks_mu_);
    for (const auto& existing : engine_->audio_tracks_) {
      if (existing->id == id) return;  // 已挂过（OnTrack / OnAddTrack 重复回调）
    }
    auto stat = std::make_unique<TrackAudioStat>();
    stat->id = id;
    stat->track = audio_track;
    const uint32_t peak_threshold = VoicePeakThresholdFromEnv();
    stat->sink = std::make_unique<AudioCountSink>(&stat->frames, &stat->peak,
                                                  &stat->last_frame_ms, &stat->max_peak,
                                                  peak_threshold);
    audio_track->AddSink(stat->sink.get());
    engine_->audio_tracks_.push_back(std::move(stat));
    engine_->Log(3, "remote audio track -> sink: id=%s (total tracks=%zu, peakThreshold=%u)",
                 id.c_str(), engine_->audio_tracks_.size(), peak_threshold);
  }

  std::atomic<int32_t>& IceState() {
    return voice_ ? engine_->voice_ice_state_ : engine_->stat_ice_state_;
  }
  // v2.10 第 2 步：本 observer 服务的会话名，用于给 SIGNAL_OUT 标 media。
  const char* Media() const { return voice_ ? kMediaVoice : kMediaScreen; }
  std::atomic<int32_t>& PeerState() {
    return voice_ ? engine_->voice_peer_state_ : engine_->stat_peer_state_;
  }
  std::atomic<bool>& LocalSdpEmitted() {
    return voice_ ? engine_->voice_local_sdp_emitted_ : engine_->local_sdp_emitted_;
  }
  std::mutex& CandMu() { return voice_ ? engine_->voice_cand_mu_ : engine_->cand_mu_; }
  std::vector<CandidateCopy>& GatheredCandidates() {
    return voice_ ? engine_->voice_gathered_ : engine_->gathered_candidates_;
  }

  MediaEngine* const engine_;
  bool voice_ = false;
  std::atomic<bool> gathered_{false};
};

MediaEngine::~MediaEngine() {
  Log(3, "dtor: enter");
  Stop();
  Log(3, "dtor: Stop() returned");

  // v2.10 第 2 步：把本引擎仍保留的「已投递信令 → media」登记清掉。
  // 那些 payload 指针随 delivered_signals_ 一起失效，登记表里不能留悬空键
  // （pu_signal_media 的指针有效期契约见头文件）。
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    for (const std::string& delivered : delivered_signals_) {
      UnregisterDeliveredMedia(delivered.c_str());
    }
    delivered_signals_.clear();
  }

  // 顺序照抄 spike：Close()/track/pc（signaling 线程）→ source → factory → 三条线程 Stop。
  ReleaseStreamObjects();
  Log(3, "dtor: ReleaseStreamObjects() returned");

  // v2.12（第 4 步紧急修复）：ReleaseStreamObjects() 现在只收屏幕会话，所以语音会话
  // 必须在这里显式收掉。位置必须在「释放工厂」与 signaling_->Stop() 之前：
  // ReleaseVoiceSession 要 join 语音协商线程、在 signaling 线程上 Close 语音 PC、
  // 再删 voice_observer_ —— 任何一步漏在三条线程 Stop() 之后就又是 0xC0000005。
  ReleaseVoiceSession();
  Log(3, "dtor: ReleaseVoiceSession() returned");

  // 工厂是 PeerConnectionFactoryProxy：它的析构会把内部 PeerConnectionFactory
  // 投递回 signaling 线程（并在控制线程上等待）。这里显式地在 signaling 线程上
  // 释放，把「webrtc 对象的归属线程」写成契约，而不是依赖 proxy 的实现细节；
  // 顺序必须在 ReleaseStreamObjects() 之后、三条线程 Stop() 之前。
  if (signaling_ != nullptr) {
    signaling_->BlockingCall([this] {
      Log(3, "dtor: destroying factory (signaling thread)");
      // 真 ADM 的生命周期契约：Terminate 与析构都必须与创建同线程（signaling）。
      // 顺序：先 Terminate（停掉 WASAPI 采集/播放），再释放工厂（它持有一份引用），
      // 最后释放本类持有的引用 —— 两条引用都在本线程释放，析构线程才是确定的。
      if (voice_adm_ != nullptr) {
        voice_adm_->Terminate();
        Log(3, "dtor: voice ADM terminated");
      }
      pc_factory_ = nullptr;
      voice_adm_ = nullptr;
      Log(3, "dtor: voice ADM released");
    });
  } else {
    pc_factory_ = nullptr;
    voice_adm_ = nullptr;
  }
  Log(3, "dtor: factory released");

  if (signaling_ != nullptr) signaling_->Stop();
  Log(3, "dtor: signaling stopped");
  if (worker_ != nullptr) worker_->Stop();
  Log(3, "dtor: worker stopped");
  if (network_ != nullptr) network_->Stop();
  Log(3, "dtor: network stopped");

  signaling_.reset();
  worker_.reset();
  network_.reset();
  Log(3, "dtor: thread objects destroyed");

  // 三条 webrtc 线程都停了，才删「可能被它们回调」的对象：远端 sink（解码线程）
  // 与 PC 观察者（signaling 线程）。这样即使有回调在飞也没有线程能再碰它们。
  delete remote_sink_;
  remote_sink_ = nullptr;  delete pc_observer_;
  pc_observer_ = nullptr;
  // v2.12：语音 observer 的常规回收在 dtor 上方那次 ReleaseVoiceSession()（v2.12 起
  // ReleaseStreamObjects 不再顺带收语音，所以那次调用是**必需**的，不是「可能已跑过」）。
  // 留一行兜底，保证任何提前 return 的收尾路径都不会把它带进下一个引擎实例
  // （历史上泄漏的 observer 是 0xC0000005 的来源之一）。
  delete voice_observer_;
  voice_observer_ = nullptr;
  Log(3, "dtor: sinks/observer deleted");
  // v2.4：与 Init 里的 MFStartup 配对。放在最后：编码/解码器线程已在
  // ReleaseStreamObjects() 里拆完，MFT 与 D3D 设备都已释放。
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
    Log(3, "dtor: MFShutdown ok");
  }
  if (log_file_ != nullptr) {
    std::fclose(log_file_);
    log_file_ = nullptr;
  }
}

void MediaEngine::Log(int level, const char* format, ...) {
  if (level > log_level_) return;
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  // 时间戳 + 线程名/线程号 + 引擎地址：收尾证据链要能跨线程排序，也要能分出 A/B
  // 两个引擎。秒表起点取进程内第一次打日志的时刻（只影响日志内相对时间）。
  static const int64_t log_t0_us = NowUsSteady();
  const double since_ms = static_cast<double>(NowUsSteady() - log_t0_us) / 1000.0;
  const webrtc::Thread* current_thread = webrtc::Thread::Current();
  const char* thread_name = current_thread != nullptr ? current_thread->name().c_str() : "control";
  char line[768];
  const int length = std::snprintf(
      line, sizeof(line), "[partyup-media][%9.1fms][%s/%lu][engine %p] %s\n", since_ms,
      thread_name, static_cast<unsigned long>(GetCurrentThreadId()), static_cast<void*>(this),
      buffer);
  if (length <= 0) return;
  const size_t bytes = std::min(static_cast<size_t>(length), sizeof(line) - 1);
  std::fwrite(line, 1, bytes, stderr);
  std::fflush(stderr);
  if (log_file_ != nullptr) {
    std::fwrite(line, 1, bytes, log_file_);
    std::fflush(log_file_);
  }
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
  // 可选：把本实例的全部日志另写一份到文件（压测/现场排查用）。用 Win32 的
  // GetEnvironmentVariableA 而不是 getenv：DLL 与 exe 各带一份 /MT CRT，exe 的
  // _putenv_s 不会刷新 DLL 的 CRT 环境副本，Win32 的进程环境是唯一共享的。
  {
    char log_path[1024];
    const DWORD length = GetEnvironmentVariableA("PARTYUP_MEDIA_LOG", log_path,
                                                static_cast<DWORD>(sizeof(log_path)));
    if (length > 0 && length < sizeof(log_path)) {
      log_file_ = std::fopen(log_path, "a");
    }
  }
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

  // v2.4：Media Foundation 进程级初始化。硬件 H.264 的 MFT 枚举 / 样本创建 /
  // DXGI device manager 全部依赖它；失败不致命（会回落到软件 VP8），但日志要留证。
  const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  mf_started_ = SUCCEEDED(mf_hr);
  if (!mf_started_) {
    Log(1, "MFStartup failed hr=0x%08X; hardware H.264 unavailable",
        static_cast<unsigned>(mf_hr));
  } else {
    Log(3, "MFStartup ok");
  }

  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create();
  signaling_ = webrtc::Thread::Create();
  if (network_ != nullptr) network_->SetName("pu-network", nullptr);
  if (worker_ != nullptr) worker_->SetName("pu-worker", nullptr);
  if (signaling_ != nullptr) signaling_->SetName("pu-signaling", nullptr);
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
  //
  // v2.4：视频编解码器换成「硬件 H.264 优先、软件 VP8 兜底」的工厂（hw_encoder.cpp）。
  // 工厂在 GetSupportedFormats() 里按 H.264 在前返回，SDP 协商就优先选 H.264；
  // 硬件 H.264 MFT 不可用时工厂不声明 H.264，协商自然只剩 VP8 —— 这就是
  // 「硬件不可用回落软件 VP8」的落点，不需要额外开关。
  const partyup::H264Availability& h264_availability = partyup::QueryH264Availability();
  {
    std::string summary;
    if (h264_availability.hardware) {
      summary = "hardware=" + h264_availability.hardware_name;
    } else {
      summary = "hardware=none";
    }
    if (h264_availability.software) summary += ", software=" + h264_availability.software_name;
    if (!h264_availability.hardware) {
      // 明确的回落警告：产品上 1440p60 靠硬件 NVENC，这条日志是「为什么 CPU 高」的答案。
      Log(1, "h264 encoder: %s; falling back to %s (%s)", summary.c_str(),
          h264_availability.software ? "software H.264 MFT" : "software VP8",
          h264_availability.error.empty() ? "unknown reason" : h264_availability.error.c_str());
    } else {
      Log(3, "h264 encoder: %s", summary.c_str());
    }
  }
  deps.video_encoder_factory = partyup::CreatePartyUpVideoEncoderFactory();
  deps.video_decoder_factory = partyup::CreatePartyUpVideoDecoderFactory();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  // v2.6：换成**真实** WASAPI ADM，语音才有麦克风/扬声器。
  // 生命周期契约（NullAudioDeviceModule 注释里写的那条）在这里落实：
  //   * 创建在 signaling 线程（本轮创建、Init、Terminate、析构都在这条线程）；
  //   * ~MediaEngine 在 signaling 线程上先 Terminate 再释放本引用，最后释放工厂
  //     （工厂自己也持有 deps.adm 的一个引用），因此 ADM 的析构同样发生在
  //     signaling 线程，且此刻三条线程都还没 Stop。
  signaling_->BlockingCall([&] {
    const webrtc::Environment adm_env = webrtc::CreateEnvironment();
    voice_adm_ = webrtc::CreateAudioDeviceModule(
        adm_env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  });
  if (voice_adm_ == nullptr) {
    *error = "CreateAudioDeviceModule(kPlatformDefaultAudio) returned null";
    SetLastError(*error);
    return false;
  }
  deps.adm = voice_adm_;
  webrtc::EnableMedia(deps);

  pc_factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (pc_factory_ == nullptr) {
    *error = "CreateModularPeerConnectionFactory returned null";
    SetLastError(*error);
    return false;
  }

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
  // v2.10：语音会话有自己的 observer（voice=true → 状态/候选/本地 SDP 分流到语音那套成员）。
  // 它的生命周期跟 voice_pc_ 绑定，由 ReleaseVoiceSession() 释放；这里先建好，
  // 保证「建 PC 之前 observer 一定存在」的次序。
  voice_observer_ = new PcObserver(this, /*voice=*/true);

  return true;
}

void MediaEngine::Fail(const std::string& message, std::string* error) {
  if (error != nullptr) *error = message;
  SetLastError(message);
  Log(1, "error: %s", message.c_str());
}


// ---------------------------------------------------------------------------
// v2.3：pu_screen_sources —— 真实显示器 + "synthetic"
// ---------------------------------------------------------------------------
int MediaEngine::Sources(pu_source_list* out) {
  if (out == nullptr) {
    SetLastError("pu_screen_sources: out is null");
    return PU_ERR_INVALID_ARG;
  }
  if (out->struct_size < sizeof(pu_source_list)) {
    SetLastError("pu_screen_sources: pu_source_list.struct_size is smaller than the ABI struct");
    return PU_ERR_INVALID_ARG;
  }

  // 每次都重建缓存：上一次返回的指针本来也只在「下一次调用之前」有效。
  source_items_.clear();
  source_ids_.clear();
  source_titles_.clear();

  std::string enum_error;
  const std::vector<ScreenTargetInfo> displays = EnumerateScreenTargets(&enum_error);
  if (displays.empty()) {
    Log(2, "pu_screen_sources: no displays (%s)", enum_error.c_str());
  }
  for (const ScreenTargetInfo& display : displays) {
    source_ids_.push_back(display.id);
    source_titles_.push_back(display.title);
    pu_source_info info{};
    info.struct_size = static_cast<uint32_t>(sizeof(pu_source_info));
    info.width = display.width;
    info.height = display.height;
    info.kind = 0;  // 0 = 屏幕
    info.reserved = 0;
    source_items_.push_back(info);
  }

  // 合成源保留在列表末尾：冒烟测试与「没有显示器/被策略拦」的兜底路径都靠它。
  source_ids_.push_back("synthetic");
  source_titles_.push_back("Synthetic test pattern 640x360 @30fps");
  {
    pu_source_info info{};
    info.struct_size = static_cast<uint32_t>(sizeof(pu_source_info));
    info.width = static_cast<uint32_t>(kWidth);
    info.height = static_cast<uint32_t>(kHeight);
    info.kind = 2;  // 2 = 合成（0=屏幕 1=窗口预留）
    info.reserved = 0;
    source_items_.push_back(info);
  }

  // 指针最后再填：此时两个 vector 不再增长，c_str() 的地址稳定到下一次调用。
  for (size_t i = 0; i < source_items_.size(); ++i) {
    source_items_[i].id = source_ids_[i].c_str();
    source_items_[i].title = source_titles_[i].c_str();
  }

  out->struct_size = static_cast<uint32_t>(sizeof(pu_source_list));
  out->count = static_cast<uint32_t>(source_items_.size());
  out->items = source_items_.data();

  for (const ScreenTargetInfo& display : displays) {
    Log(3, "screen source: id=%s title=\"%s\" %ux%u primary=%d", display.id.c_str(),
        display.title.c_str(), display.width, display.height, display.primary ? 1 : 0);
  }
  Log(3, "screen sources: %zu total (%zu display(s) + synthetic)", source_items_.size(),
      displays.size());
  return PU_OK;
}

int MediaEngine::Start(const char* screen_id, uint32_t flags, std::string* error) {
  std::lock_guard<std::mutex> control_lock(control_mu_);

  // v2.11（第 3 步）：角色位先验。目前只定义 bit0（passive）；其余 bit 保留，
  // 非 0 一律报错而不是静默忽略——「悄悄当 0 处理」会让调用方以为被动模式生效、
  // 实际却在发 offer，两个人同时共享时的症状极难查。
  if ((flags & ~1u) != 0u) {
    Fail("pu_screen_start_ex: unknown flags bits (only bit0 = passive is defined)", error);
    return PU_ERR_INVALID_ARG;
  }
  const bool passive = (flags & 1u) != 0u;

  if (running_) {
    Fail("screen share is already running", error);
    return PU_ERR_STATE;
  }

  // 角色先落到成员上：passive 时本端不发 offer，PC 是 answerer；
  // screen_prepared_ 表示「这个 PC 就是在等对端首个 offer」，SignalIn 的 stale 判定
  // 要排除它，否则刚建好的 PC 会被当成上一轮的残留拆掉。
  screen_passive_ = passive;
  screen_prepared_ = passive;

  // v2.3：源解析。空指针 / "synthetic" → 合成源；其余按真实显示器解析。
  // 显示器 id 只认 pu_screen_sources 给出的 "display:0x<HMONITOR>"，另外接受
  // "\\.\DISPLAY1" 这个别名（命令行手工传时方便）。两者都先翻译成 HMONITOR；
  // 绝不拿 GraphicsCaptureItem::DisplayName 反查（它和 DXGI 的编号对不上）。
  {
    const std::string requested = (screen_id != nullptr) ? screen_id : "";
    const bool synthetic = requested.empty() || requested == "synthetic";
    if (synthetic) {
      capture_is_real_ = false;
      capture_source_id_ = "synthetic";
      capture_width_.store(static_cast<uint32_t>(kWidth), std::memory_order_relaxed);
      capture_height_.store(static_cast<uint32_t>(kHeight), std::memory_order_relaxed);
    } else {
      std::string enum_error;
      const std::vector<ScreenTargetInfo> targets = EnumerateScreenTargets(&enum_error);
      const std::string lower = ToLowerAscii(requested);
      std::string matched;
      for (const ScreenTargetInfo& target : targets) {
        if (target.id == requested || ToLowerAscii(target.device) == lower) {
          matched = target.id;
          capture_width_.store(target.width, std::memory_order_relaxed);
          capture_height_.store(target.height, std::memory_order_relaxed);
          Log(3, "screen source resolved: %s -> %s %ux%u", requested.c_str(),
              target.device.c_str(), target.width, target.height);
          break;
        }
      }
      if (matched.empty()) {
        std::string message = "unknown screen source id '" + requested + "': " +
                              std::to_string(targets.size()) + " display(s) reported";
        if (!enum_error.empty()) message += " (" + enum_error + ")";
        Fail(message, error);
        return PU_ERR_INVALID_ARG;
      }
      capture_is_real_ = true;
      capture_source_id_ = matched;
    }
  }

  const int32_t previous_state = state_;
  state_ = PU_STREAM_STARTING;
  PushState(PU_STREAM_STARTING, previous_state, PU_REASON_REQUESTED);

  if (source_ == nullptr) {
    source_ = webrtc::scoped_refptr<ScreenVideoSource>(
        new webrtc::RefCountedObject<ScreenVideoSource>());
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
    // v2.13：屏幕共享的内容特性是「文字/细节」，不是摄像头视频。content_hint 会被
    // libwebrtc 内部用于：传给编码器的 VideoCodecMode（kScreensharing vs
    // kRealtimeVideo）、send-side BWE 的码率分配策略、quality scaler 的取舍。
    // 我们的 MFT 编码器目前不读 codec_settings->mode，但上游这几处决策都吃这个 hint，
    // 所以仍然必须设对（否则等于按"视频通话"来分配码率）。
    track_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kText);
    // 读回断言：content_hint 是虚函数、默认实现是空体，设错了不会报错。
    Log(1, "video content hint: set=kText(3) read-back=%d",
        static_cast<int>(track_->content_hint()));
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
  // v2.11：被动共享不在这里挂轨——PC 先以 answerer 身份建好，等对端 offer 到了再把
  // 视频轨挂到它那条 recvonly 的 video m-line 上（见 SignalIn 的 passive 分支）。
  {
    std::string pc_error;
    if (!EnsurePeerConnection(/*for_sending=*/!passive, &pc_error)) {
      ReleaseStreamObjects();
      state_ = PU_STREAM_FAILED;
      PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
      PushError(PU_ERR_MEDIA, pc_error);
      Fail(pc_error, error);
      return PU_ERR_MEDIA;
    }
  }

  // v2.1：采集线程只做「造帧 → 喂 VideoTrack → 发 PU_EVENT_FRAME」，不再建编码器。
  // 同时起 250 ms 的 PC 统计采样线程（PU_EVENT_FRAME 的 bytes/key/bitrate 由它派生）。
  // 采样线程揣着自己那份 pc 引用，Stop() 里先 join 再释放 PC。
  StartStatsSampler(pc_);
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
    start_error = finished ? start_error_ : std::string("capture thread start timed out");
  }

  if (!started) {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (pump_.joinable()) pump_.join();
    StopStatsSampler();
    ReleaseStreamObjects();
    state_ = PU_STREAM_FAILED;
    PushState(PU_STREAM_FAILED, PU_STREAM_STARTING, PU_REASON_MEDIA_FAILURE);
    PushError(PU_ERR_MEDIA, start_error);
    Fail(start_error, error);
    return PU_ERR_MEDIA;
  }

  // v2：生成 offer 并设置本地描述。放在采集线程起来之后，这样采集线程起不来时
  // 不用回滚已经发出去的 SDP。
  // v2.11：被动共享**不发 offer**——SDP 由对端给，本端只等 SetRemoteDescription +
  // CreateAnswer，然后由 SignalIn 走协商线程把 answer 发出去。
  if (!passive) {
    std::string sdp_error;
    if (!CreateOfferAndSetLocal(&sdp_error)) {
      stop_requested_.store(true, std::memory_order_relaxed);
      if (pump_.joinable()) pump_.join();
      StopStatsSampler();
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
  if (!passive) {
    // offer 要等 ICE 收集完成才发得出去，交给独立的协商线程，不阻塞 start。
    StartNegotiationEmit("offer");
  }
  if (capture_is_real_) {
    Log(3, "screen share started (wgc %s %ux%u, event-driven, role=%s)",
        capture_source_id_.c_str(), capture_width_.load(std::memory_order_relaxed),
        capture_height_.load(std::memory_order_relaxed), passive ? "answerer" : "offerer");
  } else {
    Log(3, "screen share started (synthetic %dx%d @%dfps, role=%s)", kWidth, kHeight, kFps,
        passive ? "answerer (waiting for the peer's offer)" : "offerer");
  }
  return PU_OK;
}

// v2.11（第 3 步）：观看者预建屏幕会话（answerer），不采屏、不建轨。
// 目的：在服务端 offer 到达之前就把 PC 建好（省掉 offer 到达时的建 PC 延迟），
// 并让 `pu_signal_in_media("screen", offer)` 有一条确定的会话可落。
// 幂等：PC 已存在直接 OK（不检查角色——如果已经有 PC，调用方的意图已经满足）。
int MediaEngine::PrepareScreen(std::string* error) {
  std::lock_guard<std::mutex> control_lock(control_mu_);
  if (pc_ != nullptr) {
    screen_prepared_ = true;
    Log(3, "screen: prepare skipped (peer connection already exists, role=%s)",
        sharing_ ? "offerer" : "answerer");
    return PU_OK;
  }
  std::string pc_error;
  if (!EnsurePeerConnection(/*for_sending=*/false, &pc_error)) {
    Fail("pu_screen_prepare: " + pc_error, error);
    return PU_ERR_MEDIA;
  }
  screen_prepared_ = true;
  Log(3, "screen: prepared (answerer, no capture, waiting for the peer's offer)");
  return PU_OK;
}

// ---------------------------------------------------------------------------
// v2.12：引擎级 ICE servers（直连回落 3a 的底座）
//
// 直连（direct）要求两端换到 srflx 候选才可能穿越 NAT，而本引擎此前把所有 PC 都建成
// host-only（两处 RTCConfiguration 都是默认构造）。这个入口让上层把 STUN 地址交进来，
// 之后新建的每一条 PC（屏幕 + 语音）都会带上。
//
// 语义上的三个决定：
//   * 只接受 "stun:" —— TURN 需要 username/credential，本版没有承载它们的字段，
//     遇到 turn:/turns:/stuns: 明确返回 INVALID_ARG 并写清原因，不静默忽略（静默忽略
//     会让上层以为配上了 TURN，跨 NAT 依旧失败却查不出原因）。
//   * 引擎级而不是会话级：上层在 pu_engine_create 之后、任何 start/join 之前设一次。
//     已建立的会话不会被追溯修改（RTCConfiguration 只在建 PC 时被读）。
//   * 幂等：重复调用整体覆盖；空串/nullptr 清空（回到 host-only，与 v2.11 完全一致）。
// ---------------------------------------------------------------------------
int MediaEngine::SetIceServers(const char* urls, size_t len, std::string* error) {
  webrtc::PeerConnectionInterface::IceServers parsed;
  if (urls != nullptr && len != 0) {
    // 逐行解析：\n 分隔，容忍 \r（Windows 换行）与行首尾空白，跳过空行。
    size_t begin = 0;
    while (begin <= len) {
      size_t end = begin;
      while (end < len && urls[end] != '\n') ++end;
      std::string entry(urls + begin, end - begin);
      const size_t first = entry.find_first_not_of(" \t\r");
      if (first == std::string::npos) {
        entry.clear();
      } else {
        entry.erase(0, first);
        const size_t last = entry.find_last_not_of(" \t\r");
        entry.erase(last + 1);
      }
      if (!entry.empty()) {
        if (ToLowerAscii(entry).rfind("stun:", 0) != 0) {
          if (error != nullptr) {
            *error = "pu_engine_set_ice_servers: unsupported url \"" + entry +
                     "\" (only stun: is supported; turn:/turns: need credentials and are "
                     "not implemented in this ABI version)";
          }
          return PU_ERR_INVALID_ARG;
        }
        const std::string rest = entry.substr(5);
        // 最小校验：host:port 形式（IPv6 用 [::1]:3478 也满足「有冒号」）。
        if (rest.empty() || rest.find(':') == std::string::npos ||
            rest.find(' ') != std::string::npos) {
          if (error != nullptr) {
            *error = "pu_engine_set_ice_servers: malformed url \"" + entry +
                     "\" (expected stun:host:port)";
          }
          return PU_ERR_INVALID_ARG;
        }
        webrtc::PeerConnectionInterface::IceServer server;
        server.urls.push_back(entry);
        parsed.push_back(server);
      }
      if (end >= len) break;
      begin = end + 1;
    }
  }
  {
    std::lock_guard<std::mutex> lock(ice_servers_mu_);
    ice_servers_ = std::move(parsed);
    Log(3, "ice servers configured: %zu entr%s", ice_servers_.size(),
        ice_servers_.size() == 1 ? "y" : "ies");
  }
  return PU_OK;
}

void MediaEngine::ApplyIceServersToConfig(
    webrtc::PeerConnectionInterface::RTCConfiguration* config) {
  if (config == nullptr) return;
  std::lock_guard<std::mutex> lock(ice_servers_mu_);
  // servers 是 RTCConfiguration 里的字段名（本版 libwebrtc 的 IceServers 别名就是
  // vector<IceServer>，W3C 里叫 ice_servers；这里照 vendored 头文件的实际字段名写）。
  config->servers = ice_servers_;
}

// v2.13：把 degradation 枚举写成日志用的短名（不用 libwebrtc 的
// DegradationPreferenceToString，避免依赖 webrtc.lib 里那个符号是否被链进来）。
// 注意 DISABLED 与 MAINTAIN_FRAMERATE_AND_RESOLUTION 是同一个值，只能列一个 case。
const char* DegradationName(webrtc::DegradationPreference preference) {
  switch (preference) {
    case webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION:
      return "maintain_framerate_and_resolution";
    case webrtc::DegradationPreference::MAINTAIN_FRAMERATE:
      return "maintain_framerate";
    case webrtc::DegradationPreference::MAINTAIN_RESOLUTION:
      return "maintain_resolution";
    case webrtc::DegradationPreference::BALANCED:
      return "balanced";
  }
  return "unknown";
}

// v2.13：屏幕 sender 的**产品级**发送参数（外加三个测试专用覆盖开关）。
// 必须跑在 signaling 线程（RtpSenderInterface 有线程断言）。
//
// 产品默认值（来自用户批准的码率五步方案）：
//   * degradation_preference = MAINTAIN_RESOLUTION —— 弱网**降帧不降分辨率**，
//     这是 PRODUCT.md 的明写承诺（文字不能糊）。注意 libwebrtc 默认是 BALANCED。
//   * max_bitrate_bps = 3 Mbps   —— 1080p30 的舒适值，也是内容突变时的硬上限。
//   * min_bitrate_bps = 300 kbps —— 别被压到完全没有可用画面。
//
// 为什么必须**读回**再断言：`RtpSenderInterface::SetParameters()` 在很多情况下
// （例如 sender 还没协商、或该字段被下层忽略）只是"没报错"，值并不会落地。
// 这里把 GetParameters() 的结果打出来，并对 degradation 做显式比对 —— 不静默。
//
// 测试专用覆盖（生产不设即零行为，见 h264_degrade_smoke / bitrate_policy_smoke）：
//   PARTYUP_MEDIA_TEST_MAX_BITRATE=<bps>       → 覆盖 max_bitrate_bps
//   PARTYUP_MEDIA_TEST_DEGRADATION=maintain_resolution|maintain_framerate|balanced
//   PARTYUP_MEDIA_TEST_SCALE_DOWN_BY=<n>       → 强制上游 1/n 缩放（用于逼降分辨率）
void MediaEngine::ApplyVideoSenderParamsOnSignalingThread() {
  if (video_sender_ == nullptr) return;

  constexpr int kProductMaxBitrateBps = 3'000'000;  // 3 Mbps
  constexpr int kProductMinBitrateBps = 300'000;    // 300 kbps

  webrtc::RtpParameters parameters = video_sender_->GetParameters();
  if (parameters.encodings.empty()) {
    parameters.encodings.push_back(webrtc::RtpEncodingParameters{});
  }
  parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
  parameters.encodings[0].max_bitrate_bps = kProductMaxBitrateBps;
  parameters.encodings[0].min_bitrate_bps = kProductMinBitrateBps;

  // ---- v2.14：编码尺寸上限 1080p（产品默认，不是省带宽而是画质）----
  // 同一个 3 Mbps 上限下，像素越少 = 每个像素分到的比特越多。屏幕共享的内容是文字，
  // 恰恰最吃「每像素比特数」：1440p 在 3 Mbps 下平均每像素不到 0.2 bit/frame，
  // 文字边缘会糊；压到 1080p 后同样的码率只摊在 56% 的像素上，文字明显更锐利。
  //
  // 输入端用 scale_resolution_down_by 做**静态**降采样（libwebrtc 的 VideoAdapter 在
  // 编码前缩帧，然后按新尺寸重建编码器 —— 这条路已由 PARTYUP_MEDIA_TEST_SCALE_DOWN_BY
  // 实测验证：编码器完整重建、size_mismatch=0、画面连续）。
  // 它和 degradation_preference 是两层：静态比例定「基准尺寸」，degradation 定「带宽
  // 紧张时先动谁」。MAINTAIN_RESOLUTION 下只会掉帧，不会在基准之上再降分辨率。
  //
  // 按 max(width/1920, height/1080) 算，保证宽高都进框（21:9 或竖屏也不会宽度超限），
  // 宽高比不变。PARTYUP_MAX_CAPTURE_HEIGHT 可覆盖（<=0 关闭，退回原始分辨率）。
  // 默认 **0 = 不缩放**（2026-11-04 实测后的决定，见 docs/spikes/2026-11-04-resolution-cap.md）：
  // 常见带宽（屏幕流实测 0.4–2.1 Mbps，3 Mbps 上限根本不 binding）下 1440p 明显更锐，
  // 强制降到 1080p 是画质回退（同区域 Laplacian 方差差 5–7 倍）。机制保留，需要时显式开会。
  int max_capture_height = 0;
  char max_height_text[32] = {0};
  const DWORD max_height_len =
      ::GetEnvironmentVariableA("PARTYUP_MAX_CAPTURE_HEIGHT", max_height_text,
                                static_cast<DWORD>(sizeof(max_height_text)));
  if (max_height_len > 0) max_capture_height = std::atoi(max_height_text);

  const uint32_t capture_w = capture_width_.load(std::memory_order_relaxed);
  const uint32_t capture_h = capture_height_.load(std::memory_order_relaxed);
  double scale_down_by = 1.0;
  int encoded_w = static_cast<int>(capture_w);
  int encoded_h = static_cast<int>(capture_h);
  if (max_capture_height > 0 && capture_w > 0 && capture_h > 0) {
    const double scale_for_width = static_cast<double>(capture_w) / 1920.0;
    const double scale_for_height =
        static_cast<double>(capture_h) / static_cast<double>(max_capture_height);
    scale_down_by = scale_for_width > scale_for_height ? scale_for_width : scale_for_height;
    if (scale_down_by < 1.0) scale_down_by = 1.0;
    // 和 libwebrtc 的取整保持一致（VideoAdapter 用整除 + 偶数化），只用于日志好看。
    if (scale_down_by > 1.0) {
      encoded_w = static_cast<int>(static_cast<double>(capture_w) / scale_down_by) & ~1;
      encoded_h = static_cast<int>(static_cast<double>(capture_h) / scale_down_by) & ~1;
    }
  }
  parameters.encodings[0].scale_resolution_down_by = scale_down_by;

  // ---- 测试覆盖：只影响显式设了环境变量的测试进程 ----
  char max_bitrate_text[64] = {0};
  const DWORD max_bitrate_len =
      ::GetEnvironmentVariableA("PARTYUP_MEDIA_TEST_MAX_BITRATE", max_bitrate_text,
                                static_cast<DWORD>(sizeof(max_bitrate_text)));
  const int test_max_bitrate_bps = max_bitrate_len > 0 ? std::atoi(max_bitrate_text) : 0;
  if (test_max_bitrate_bps > 0) {
    parameters.encodings[0].max_bitrate_bps = test_max_bitrate_bps;
    // 测试要压到 200 kbps 时，产品下限 300 kbps 会把目标夹回去（min>max 是空区间）。
    // 两处都守法：这里把 min 跟着降下来，encoder 侧还有一道 min<=max 的兜底。
    if (parameters.encodings[0].min_bitrate_bps &&
        *parameters.encodings[0].min_bitrate_bps > test_max_bitrate_bps) {
      parameters.encodings[0].min_bitrate_bps = test_max_bitrate_bps;
    }
  }

  char degradation_text[64] = {0};
  const DWORD degradation_len =
      ::GetEnvironmentVariableA("PARTYUP_MEDIA_TEST_DEGRADATION", degradation_text,
                                static_cast<DWORD>(sizeof(degradation_text)));
  const std::string test_degradation =
      degradation_len > 0 ? std::string(degradation_text) : "";
  if (test_degradation == "maintain_framerate") {
    parameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
  } else if (test_degradation == "balanced") {
    parameters.degradation_preference = webrtc::DegradationPreference::BALANCED;
  }

  // 这个才是「确定性触发降分辨率」的仪器：libwebrtc 会把帧在上游缩到 1/N，然后用
  // 新尺寸再调一次 InitEncode —— 正好压在我们那个「MFT 类型协商是静态的」地雷上。
  char scale_text[64] = {0};
  const DWORD scale_len =
      ::GetEnvironmentVariableA("PARTYUP_MEDIA_TEST_SCALE_DOWN_BY", scale_text,
                                static_cast<DWORD>(sizeof(scale_text)));
  const double test_scale_down_by = scale_len > 0 ? std::atof(scale_text) : 0.0;
  if (test_scale_down_by > 1.0) {
    parameters.encodings[0].scale_resolution_down_by = test_scale_down_by;
  }

  const webrtc::RTCError error = video_sender_->SetParameters(parameters);
  if (!error.ok()) {
    Log(1, "video sender params: SetParameters FAILED: %s", error.message());
  }

  // ---- 读回断言：不信 SetParameters 的返回值，只信 GetParameters 的结果 ----
  const webrtc::RtpParameters effective = video_sender_->GetParameters();
  const auto effective_degradation = effective.degradation_preference;
  int effective_max = 0;
  int effective_min = 0;
  if (!effective.encodings.empty()) {
    if (effective.encodings[0].max_bitrate_bps) effective_max = *effective.encodings[0].max_bitrate_bps;
    if (effective.encodings[0].min_bitrate_bps) effective_min = *effective.encodings[0].min_bitrate_bps;
  }
  const double effective_scale =
      effective.encodings.empty() || !effective.encodings[0].scale_resolution_down_by
          ? 1.0
          : *effective.encodings[0].scale_resolution_down_by;
  Log(1, "video sender params read-back: degradation=%s max_bitrate_bps=%d min_bitrate_bps=%d "
         "scale_resolution_down_by=%.4f",
      effective_degradation ? DegradationName(*effective_degradation) : "(unset)",
      effective_max, effective_min, effective_scale);
  Log(1, "video sender scale: capture=%ux%u cap_height=%d -> encode %dx%d (scale=%.4f)",
      capture_w, capture_h, max_capture_height, encoded_w, encoded_h, scale_down_by);
  if (!effective_degradation ||
      *effective_degradation != webrtc::DegradationPreference::MAINTAIN_RESOLUTION) {
    // 这是「承诺落空」的直接证据，必须显式报出来（用户明确要求用读回值断言）。
    Log(1, "video sender params: WARNING degradation_preference DID NOT STICK "
           "(wanted main-resolve, got %s) -- 弱网仍会降分辨率",
        effective_degradation ? DegradationName(*effective_degradation) : "(unset)");
  }
  if (effective_max != parameters.encodings[0].max_bitrate_bps.value_or(0)) {
    Log(1, "video sender params: WARNING max_bitrate_bps did not stick (wanted %d, got %d)",
        parameters.encodings[0].max_bitrate_bps.value_or(0), effective_max);
  }
}

int MediaEngine::Stop() {
  std::lock_guard<std::mutex> control_lock(control_mu_);
  Log(3, "stop: enter (running=%d)", running_ ? 1 : 0);
  // v2：先停协商线程。它会在 ICE 收集完成后往事件队列推一条信令，必须在关闭 PC
  // 之前收掉，否则会出现「stop 之后还冒出一条 offer」。
  negotiator_stop_.store(true, std::memory_order_relaxed);
  if (negotiator_.joinable()) negotiator_.join();
  Log(3, "stop: negotiator joined");

  if (!running_) {
    // 正常路径是幂等返回；但如果 start() 在中途抛异常，采集线程可能已经起来而
    // running_ 还是 false，这里必须把它收掉，否则 std::thread 析构会 terminate。
    if (pump_.joinable()) {
      stop_requested_.store(true, std::memory_order_relaxed);
      pump_.join();
    }
    Log(3, "stop: pump joined (idle path)");
    // v2.1：pump 停了之后才停采样线程——它在收尾时会再取一份报告，把
    // frames_encoded 追到终值（没有新帧要说，这份就是最终数）。
    StopStatsSampler();
    Log(3, "stop: stats stopped (idle path)");
    // 接收侧 PC（没 start、只收不发）也要关掉；没有 PC 时这里是无操作。
    ReleaseStreamObjects();
    Log(3, "stop: released (idle path), returning");
    return PU_OK;
  }

  state_ = PU_STREAM_STOPPING;
  PushState(PU_STREAM_STOPPING, PU_STREAM_RUNNING, PU_REASON_REQUESTED);

  stop_requested_.store(true, std::memory_order_relaxed);
  if (pump_.joinable()) pump_.join();
  Log(3, "stop: pump joined");

  // v2.1：同上——pump 已停，采样线程取完最后一份报告再退（幂等）。
  StopStatsSampler();
  Log(3, "stop: stats stopped");

  running_ = false;
  ReleaseStreamObjects();
  Log(3, "stop: released");

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

void MediaEngine::DetachAllAudioSinks() {
  // 必须在 signaling 线程调用：AudioTrackInterface::RemoveSink 有线程断言。
  std::lock_guard<std::mutex> lock(audio_tracks_mu_);
  for (auto& stat : audio_tracks_) {
    if (stat->track != nullptr && stat->sink != nullptr) {
      stat->track->RemoveSink(stat->sink.get());
    }
  }
  audio_tracks_.clear();
}

void MediaEngine::ReleaseStreamObjects() {
  Log(3, "release: enter (pc=%d track=%d remote_track=%d)", pc_ != nullptr ? 1 : 0,
      track_ != nullptr ? 1 : 0, remote_track_ != nullptr ? 1 : 0);
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
        // v2.12（第 4 步紧急修复）：这里**不再**调 DetachAllAudioSinks()。
        // 远端音频 sink 属于**语音会话**，和屏幕 PC 没有关系；屏幕停止时摘掉它们会让
        // 语音 PCM 计数直接归零（UI 上就是「一共享就没声」）。语音侧的摘除统一由
        // ReleaseVoiceSession() 负责（显式 leave 与引擎析构两条路径）。
        pc_->Close();
      }
      track_ = nullptr;
      // v2.11：被动共享挂上去的 sender 也要在这里放掉（它持有 PC 的引用）。
      video_sender_ = nullptr;
      pc_ = nullptr;
    });
  } else {
    track_ = nullptr;
    video_sender_ = nullptr;
    pc_ = nullptr;
    remote_track_ = nullptr;
  }
  Log(3, "release: pc/track closed and released");
  {
    std::lock_guard<std::mutex> lock(cand_mu_);
    gathered_candidates_.clear();
    buffered_candidates_.clear();
  }
  source_ = nullptr;
  has_remote_description_ = false;
  sharing_ = false;
  // v2.11：屏幕会话的角色状态随 PC 一起复位。漏掉 screen_passive_ 会让下一次
  // （非被动的）共享在收到 offer 时误挂视频轨；漏掉 screen_prepared_ 会让新会话的
  // 首个 offer 被误判成上一轮的残留。
  screen_passive_ = false;
  screen_prepared_ = false;
  local_sdp_emitted_.store(false, std::memory_order_relaxed);
  stat_peer_state_.store(PU_PEER_NONE, std::memory_order_relaxed);
  stat_ice_state_.store(PU_ICE_NEW, std::memory_order_relaxed);
  // v2.12（第 4 步紧急修复）：本函数**只收屏幕会话**，不再碰语音。
  // 之前这里在末尾调 ReleaseVoiceSession()，于是：
  //   * pu_screen_start / pu_screen_stop 都会把正在进行的语音拆掉
  //     （实测 release-voice: enter (pc=1 joined=1)，pcmRx 7989 → 0）；
  //   * 屏幕 start 失败回滚、屏幕信令报错等路径也会误伤语音。
  // 语音会话现在只有两条释放路径：显式 pu_voice_leave()/VoiceLeave 与 ~MediaEngine。
  // 停止共享不影响语音，是「共享与语音两条 PC 独立」（ADR-0002）的直接要求。
  Log(3, "release: done (voice untouched)");
}

// ---------------------------------------------------------------------------
// v2.10 第 1 步：语音会话收尾
// ---------------------------------------------------------------------------

void MediaEngine::ReleaseVoiceSession() {
  Log(3, "release-voice: enter (pc=%d joined=%d)", voice_pc_ != nullptr ? 1 : 0,
      voice_joined_ ? 1 : 0);
  // 1) 协商线程先收掉。它会在 ICE 收集完成后往事件队列推 answer/offer ——
  //    「stop 之后还冒出一条信令」就是漏了这一 join。
  voice_negotiator_stop_.store(true, std::memory_order_relaxed);
  if (voice_negotiator_.joinable()) voice_negotiator_.join();
  // 2) 关 PC 必须在 signaling 线程上（VideoTrack/AudioTrack 与 PC 都有线程断言）。
  //    正常调用点都在控制线程；万一有人在 signaling 线程上进来，BlockingCall 会自锁，
  //    所以显式跳过（此时也只剩把引用放掉这一条安全路径）。
  if (signaling_ != nullptr && !signaling_->IsCurrent()) {
    signaling_->BlockingCall([this] {
      if (voice_pc_ != nullptr) {
        // 远端音频 sink 属于语音会话，先摘再关（RemoveSink 有线程断言）。
        DetachAllAudioSinks();
        voice_pc_->Close();
      }
      // 引用在 signaling 线程上放掉：PC 的析构由它检查线程归属。
      voice_pc_ = nullptr;
    });
  } else {
    voice_pc_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(voice_cand_mu_);
    voice_gathered_.clear();
    voice_buffered_.clear();
  }
  voice_sender_ = nullptr;
  voice_track_ = nullptr;
  voice_source_ = nullptr;
  voice_joined_ = false;
  voice_offerer_ = false;
  voice_has_remote_description_ = false;
  voice_local_sdp_emitted_.store(false, std::memory_order_relaxed);
  voice_peer_state_.store(PU_PEER_NONE, std::memory_order_relaxed);
  voice_ice_state_.store(PU_ICE_NEW, std::memory_order_relaxed);
  // 3) 最后删 observer：voice_pc_ 已经在 signaling 线程上放掉了，之后没有对象会再回调它。
  //    下一次 VoiceJoin 由 EnsureVoicePeerConnection 重建。
  delete voice_observer_;
  voice_observer_ = nullptr;
  Log(3, "release-voice: done");
}

// ---------------------------------------------------------------------------
// v2.10 第 1 步：语音独立 PeerConnection 的协商 helper
//
// 这 8 个函数是上面「v2：PeerConnection 与信令」那组的逐行搬运，唯一区别是把
// pc_ / cand_mu_ / gathered_candidates_ / negotiator_* 换成
// voice_pc_ / voice_cand_mu_ / voice_gathered_ / voice_negotiator_*。
// 两条会话因此互不干扰：屏幕 PC 的 CreateOffer 不会把语音 PC 的 signaling state 拖进
// have-local-offer（relay 下点共享曾因此把语音 answer 打成 role mismatch）。
// TODO(step4): 合并到参数化的 MediaSession 版本，勿在此新增逻辑（只做搬家）。
// ---------------------------------------------------------------------------

// 下面「v2：PeerConnection 与信令」段里的两个公共实现（线程归属 + 超时 + 观测者）。
// 语音这组直接复用，不另写一遍；先前置声明是因为它们的定义在本段之后。
bool ApplyLocalDescription(webrtc::Thread* signaling,
                           webrtc::PeerConnectionInterface* pc,
                           webrtc::SdpType type,
                           const std::string& sdp,
                           std::string* error);
bool ApplyRemoteDescription(webrtc::Thread* signaling,
                            webrtc::PeerConnectionInterface* pc,
                            webrtc::SdpType type,
                            const std::string& sdp,
                            std::string* error);

bool MediaEngine::EnsureVoicePeerConnection(bool for_sending, std::string* error) {
  // observer 由 ReleaseVoiceSession 删除，重进频道时在这里补建。
  if (voice_observer_ == nullptr) {
    voice_observer_ = new PcObserver(this, /*voice=*/true);
  }
  if (voice_pc_ != nullptr) {
    // 已有语音 PC：把（可能后建的）音频轨补挂上去，但不要重复 AddTrack。
    if (for_sending && voice_track_ != nullptr && voice_sender_ == nullptr) {
      std::string add_error;
      signaling_->BlockingCall([&] {
        auto added = voice_pc_->AddTrack(voice_track_, {"voice"});
        if (!added.ok()) {
          add_error = std::string("AddTrack(audio) failed: ") + added.error().message();
        } else {
          voice_sender_ = added.value();
        }
      });
      if (!add_error.empty()) {
        *error = add_error;
        return false;
      }
    }
    return true;
  }
  if (signaling_ == nullptr || pc_factory_ == nullptr) {
    *error = "engine is not initialised";
    return false;
  }

  std::string create_error;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    // v2.12：带上引擎级 ICE servers（未设置时为空 = host-only，行为与 v2.11 一致）。
    ApplyIceServersToConfig(&config);
    // 与屏幕会话同策略：来源同上；上层若要跨 NAT 直连，先 pu_engine_set_ice_servers。
    webrtc::PeerConnectionDependencies dependencies(voice_observer_);
    auto created = pc_factory_->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!created.ok()) {
      create_error = std::string("CreatePeerConnection (voice) failed: ") +
                     created.error().message();
      return;
    }
    pc = created.value();
    // 音频轨由 VoiceJoin 在 PC 建好之后创建并 AddTrack；这里 for_sending 只表示角色。
  });
  if (pc == nullptr) {
    *error = create_error.empty() ? "voice peer connection creation failed" : create_error;
    return false;
  }

  voice_pc_ = pc;
  voice_offerer_ = for_sending;
  voice_has_remote_description_ = false;
  {
    std::lock_guard<std::mutex> lock(voice_cand_mu_);
    voice_gathered_.clear();
    voice_buffered_.clear();
  }
  Log(3, "voice peer connection created (role=%s)", for_sending ? "offerer" : "answerer");
  return true;
}

bool MediaEngine::VoiceCreateOfferAndSetLocal(std::string* error) {
  if (voice_pc_ == nullptr) {
    *error = "no voice peer connection";
    return false;
  }
  AsyncResult create;
  webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> observer(
      new webrtc::RefCountedObject<CreateSdpObserver>(&create));
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;  // 默认 sendrecv
    voice_pc_->CreateOffer(observer.get(), options);
  });
  if (!WaitAsync(&create, kSdpTimeoutMs) || !create.ok) {
    *error = "CreateOffer (voice) failed: " +
             (create.done ? create.error : std::string("timeout"));
    return false;
  }
  Log(3, "voice offer generated (%zu bytes)", create.sdp.size());
  return ApplyLocalDescription(signaling_.get(), voice_pc_.get(), webrtc::SdpType::kOffer,
                               create.sdp, error);
}

bool MediaEngine::VoiceCreateAnswerAndSetLocal(std::string* error) {
  if (voice_pc_ == nullptr) {
    *error = "no voice peer connection";
    return false;
  }
  AsyncResult create;
  webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> observer(
      new webrtc::RefCountedObject<CreateSdpObserver>(&create));
  signaling_->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    voice_pc_->CreateAnswer(observer.get(), options);
  });
  if (!WaitAsync(&create, kSdpTimeoutMs) || !create.ok) {
    *error = "CreateAnswer (voice) failed: " +
             (create.done ? create.error : std::string("timeout"));
    return false;
  }
  Log(3, "voice answer generated (%zu bytes)", create.sdp.size());
  return ApplyLocalDescription(signaling_.get(), voice_pc_.get(), webrtc::SdpType::kAnswer,
                               create.sdp, error);
}

bool MediaEngine::VoiceSetRemoteDescription(int type, const std::string& sdp,
                                            std::string* error) {
  return ApplyRemoteDescription(signaling_.get(), voice_pc_.get(),
                                static_cast<webrtc::SdpType>(type), sdp, error);
}

std::string MediaEngine::VoiceBuildSdpWithCandidates(webrtc::PeerConnectionInterface* pc) {
  if (pc == nullptr) return {};
  const webrtc::SessionDescriptionInterface* local = pc->local_description();
  if (local == nullptr) return {};

  // 与屏幕版同算法：先序列化一份，再把「SDP 里还没有的候选」补进 Clone。
  const std::string base = webrtc::SdpSerialize(*local);
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc = local->Clone();
  if (desc == nullptr) return base;

  std::vector<CandidateCopy> candidates;
  {
    std::lock_guard<std::mutex> lock(voice_cand_mu_);
    candidates = voice_gathered_;
  }
  int added = 0;
  int already = 0;
  for (const CandidateCopy& candidate : candidates) {
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
  Log(3, "voice local sdp: %zu bytes, %d a=candidate line(s) (%d added here, %d already present)",
      out.size(), candidate_lines, added, already);
  return out;
}

void MediaEngine::VoiceStartNegotiationEmit(const char* signal_type) {
  if (voice_negotiator_.joinable()) {
    voice_negotiator_stop_.store(true, std::memory_order_relaxed);
    voice_negotiator_.join();
  }
  voice_negotiator_stop_.store(false, std::memory_order_relaxed);
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = voice_pc_;
  if (pc == nullptr) return;
  const std::string type = signal_type;
  voice_negotiator_ = std::thread([this, type, pc] { VoiceNegotiationEmitLoop(type, pc); });
}

void MediaEngine::VoiceNegotiationEmitLoop(
    std::string signal_type,
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
  // 等 ICE 收集完成（轮询，最多 kGatherTimeoutMs）——与 WaitIceGatheringComplete 同一算法，
  // 但判的是语音自己的停止标志，不能借用屏幕那条（否则停屏幕会打断语音的 SDP 发送）。
  bool gathered = false;
  if (signaling_ != nullptr) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kGatherTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (voice_negotiator_stop_.load(std::memory_order_relaxed)) return;
      const auto state = signaling_->BlockingCall([pc] { return pc->ice_gathering_state(); });
      if (state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
        gathered = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (voice_negotiator_stop_.load(std::memory_order_relaxed)) return;
  if (!gathered) {
    size_t candidates = 0;
    {
      std::lock_guard<std::mutex> lock(voice_cand_mu_);
      candidates = voice_gathered_.size();
    }
    Log(2, "voice ICE gathering timed out (%d ms), emitting SDP with %zu candidate(s) anyway",
        kGatherTimeoutMs, candidates);
  }

  std::string sdp = signaling_->BlockingCall(
      [this, pc] { return VoiceBuildSdpWithCandidates(pc.get()); });
  if (sdp.empty()) {
    PushError(PU_ERR_MEDIA, "voice local description is empty, cannot emit " + signal_type);
    return;
  }
  const uint32_t kind = (signal_type == "offer") ? PU_SIGNAL_OFFER : PU_SIGNAL_ANSWER;
  EmitSignal(kMediaVoice, signal_type.c_str(), MakeSdpEnvelope(signal_type.c_str(), sdp), kind);
  voice_local_sdp_emitted_.store(true, std::memory_order_relaxed);
}

void MediaEngine::VoiceApplyBufferedCandidates() {
  std::vector<CandidateCopy> pending;
  {
    std::lock_guard<std::mutex> lock(voice_cand_mu_);
    pending.swap(voice_buffered_);
  }
  if (pending.empty() || voice_pc_ == nullptr) return;
  for (const CandidateCopy& candidate : pending) {
    bool applied = false;
    signaling_->BlockingCall([this, &candidate, &applied] {
      std::unique_ptr<webrtc::IceCandidate> parsed = webrtc::IceCandidate::Create(
          candidate.mid, candidate.mline_index, candidate.sdp, nullptr);
      if (parsed != nullptr) applied = voice_pc_->AddIceCandidate(parsed.get());
    });
    Log(3, "voice buffered candidate %s: %s", applied ? "applied" : "rejected",
        candidate.sdp.c_str());
  }
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
    // v2.6：PC 已存在时走到这里有两种情况——
    //   * 语音先起，屏幕共享后加入：必须把视频轨补挂到现有 PC 上，否则重协商的
    //     offer 里没有 m=video，画面永远不来（addtrack 之后 libwebrtc 会置
    //     negotiation-needed，由 start 里的 CreateOffer 带出去）。
    //   * 别的路径：沿用既有 PC。
    if (for_sending && track_ != nullptr) {
      std::string add_error;
      signaling_->BlockingCall([&] {
        auto added = pc_->AddTrack(track_, {"partyup-screen"});
        if (!added.ok()) {
          add_error = std::string("AddTrack(video) failed: ") + added.error().message();
          return;
        }
        // v2.13：两条建轨路径都留下 sender（测试钩子与将来的 degradation 配置要用）。
        video_sender_ = added.value();
        ApplyVideoSenderParamsOnSignalingThread();
      });
      if (!add_error.empty()) {
        *error = add_error;
        return false;
      }
    }
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
    // v2.12：带上引擎级 ICE servers（未设置时为空 = host-only，行为与 v2.11 一致）。
    ApplyIceServersToConfig(&config);
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
        return;
      }
      // v2.13：留下 sender，并按测试环境变量决定是否收紧码率上限/降级偏好。
      video_sender_ = added.value();
      ApplyVideoSenderParamsOnSignalingThread();
      // v2.4：把 H.264 钉到编解码器偏好首位。硬件 NVENC 只做 H.264，VP8 只能
      // 软件编码；工厂已经按 H.264 在前返回，这里再把偏好显式写一次，免得 SDP
      // 里 VP8 的 payload type 更小而被对端优先选中。H.264 不可用时列表里没有
      // 它，循环原样保序，等于什么都没改。
      const webrtc::RtpCapabilities sender_caps =
          pc_factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
      std::vector<webrtc::RtpCodecCapability> preferred;
      for (const webrtc::RtpCodecCapability& codec : sender_caps.codecs) {
        if (codec.name == "H264") preferred.push_back(codec);
      }
      for (const webrtc::RtpCodecCapability& codec : sender_caps.codecs) {
        if (codec.name != "H264") preferred.push_back(codec);
      }
      for (const webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>& transceiver :
           pc->GetTransceivers()) {
        if (transceiver->media_type() != webrtc::MediaType::VIDEO) continue;
        const webrtc::RTCError preference_error =
            transceiver->SetCodecPreferences(preferred);
        if (!preference_error.ok()) {
          Log(1, "SetCodecPreferences failed: %s", preference_error.message());
        } else {
          Log(3, "video codec preference: %s first (%zu codecs)",
              preferred.empty() ? "(none)" : preferred.front().name.c_str(),
              preferred.size());
        }
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
  EmitSignal(kMediaScreen, signal_type.c_str(), MakeSdpEnvelope(signal_type.c_str(), sdp), kind);
  local_sdp_emitted_.store(true, std::memory_order_relaxed);
}

void MediaEngine::EmitSignal(const char* media, const char* type, const std::string& payload,
                             uint32_t kind) {
  pu_event event = MakeEvent(PU_EVENT_SIGNAL_OUT);
  event.data.signal.payload = nullptr;  // 真正投递时才填，见 Poll
  event.data.signal.payload_len = static_cast<uint32_t>(payload.size());
  event.data.signal.kind = kind;
  event.data.signal.reserved = 0;
  stat_signal_out_.fetch_add(1, std::memory_order_relaxed);
  Log(3, "signal out [%s]: %s (%zu bytes)", media != nullptr ? media : "?", type, payload.size());
  PushEventWithPayload(event, payload, media != nullptr ? media : std::string());
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

int MediaEngine::SignalIn(const char* media, const char* payload, size_t len,
                          std::string* error) {
  auto reject = [&](int code, const std::string& message) {
    if (error != nullptr) *error = message;
    SetLastError(message);
    return code;
  };

  if (payload == nullptr || len == 0) {
    return reject(PU_ERR_INVALID_ARG, "pu_signal_in: payload is null or empty");
  }

  // v2.10 第 2 步：会话归属**优先取显式 media**（pu_signal_in_media 传入 "voice"/"screen"）。
  // 先验 media、再解析 payload：未知 media 与 payload 内容无关，早失败更便宜。
  // media == nullptr 或空串时退回 SDP 嗅探（pu_signal_in 兼容入口）：
  //   * 只带 m=audio、不带 m=video → 语音会话（voice_pc_）
  //   * 带 m=video（含音视频混合的老式重协商）→ 屏幕会话（pc_）
  // 嗅探的已知缺陷：candidate 信封里没有 m= 行，两条 PC 并存时只能推断（见 candidate 段）。
  bool explicit_media = false;
  bool route_to_voice = false;
  if (media != nullptr && media[0] != '\0') {
    if (std::strcmp(media, kMediaVoice) == 0) {
      route_to_voice = true;
      explicit_media = true;
    } else if (std::strcmp(media, kMediaScreen) == 0) {
      route_to_voice = false;
      explicit_media = true;
    } else {
      return reject(PU_ERR_INVALID_ARG,
                    std::string("pu_signal_in: unknown media \"") + media +
                        "\" (want \"voice\" or \"screen\")");
    }
  }

  SignalMessage message;
  if (!ParseSignal(payload, len, &message, error)) {
    SetLastError("pu_signal_in: " + (error != nullptr ? *error : std::string("bad payload")));
    return PU_ERR_INVALID_ARG;
  }
  Log(3, "signal in [%s]: type=%s (%zu bytes)", media != nullptr ? media : "sniff",
      message.type.c_str(), len);

  if (!explicit_media) {
    route_to_voice = message.sdp.find("m=audio") != std::string::npos &&
                     message.sdp.find("m=video") == std::string::npos;
  }

  if (message.type == "offer") {
    if (route_to_voice) {
      // ---- 语音会话：只有 audio 的 SDP ----
      // 与屏幕侧同构的三态日志（断线重连的验收门，不能误报）：
      //   1) 已有远端描述 → 同一语音会话上的真重协商；
      //   2) 有 PC 但没有远端描述、且当前不在语音里 → 上一轮残留，拆掉重建；
      //   3) 有 PC、没有远端描述、语音在通话中 → 全新 PC 的第一个 offer。
      if (voice_pc_ != nullptr && voice_has_remote_description_) {
        Log(3, "voice offer: renegotiation on the existing voice session");
      } else if (voice_pc_ != nullptr && !voice_joined_) {
        Log(3, "voice offer: dropping stale answerer session (voice pc exists, not joined)");
        ReleaseVoiceSession();
      } else if (voice_pc_ != nullptr) {
        Log(3, "voice offer: first offer on a fresh voice session");
      }
      std::string local_error;
      if (!EnsureVoicePeerConnection(/*for_sending=*/false, &local_error)) {
        return reject(PU_ERR_MEDIA, "pu_signal_in(voice): " + local_error);
      }
      if (!VoiceSetRemoteDescription(static_cast<int>(webrtc::SdpType::kOffer), message.sdp,
                                     &local_error)) {
        ReleaseVoiceSession();
        return reject(PU_ERR_MEDIA, "pu_signal_in(voice): " + local_error);
      }
      voice_has_remote_description_ = true;
      VoiceApplyBufferedCandidates();
      if (!VoiceCreateAnswerAndSetLocal(&local_error)) {
        ReleaseVoiceSession();
        return reject(PU_ERR_MEDIA, "pu_signal_in(voice): " + local_error);
      }
      // answer 同样要等 ICE 收集完成再发，交给语音自己的协商线程。
      VoiceStartNegotiationEmit("answer");
      return PU_OK;
    }
    // ---- 屏幕会话（原逻辑）----
    if (pc_ != nullptr && sharing_) {
      return reject(PU_ERR_STATE,
                    "received an offer while sharing (glare): ABI v2 has no perfect negotiation");
    }
    // 收到 offer 时屏幕 PC 的三种状态要分清（这条日志是断线重连的验收门，不能误报）：
    //   1) PC 上已有远端描述 → 同一个会话上的**真重协商**；
    //   2) PC 存在但没有远端描述、且本端不是 offerer → 上一轮会话的残留，拆掉重建；
    //   3) PC 存在但没有远端描述、本端是 offerer → 全新 PC 的第一个 offer。
    if (pc_ != nullptr && has_remote_description_) {
      Log(3, "offer: renegotiation on the existing screen session");
    } else if (pc_ != nullptr && !sharing_ && !screen_passive_ && !screen_prepared_) {
      Log(3, "offer: dropping stale answerer session (pc exists, local is viewer)");
      ReleaseStreamObjects();
    } else if (pc_ != nullptr) {
      Log(3, "offer: first offer on a fresh screen session (pc has no remote description yet)");
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
    // v2.11（第 3 步）：被动共享方。对端 offer 里那条 video m-line 的方向是「对端收」，
    // SetRemote 之后它已经变成一条 sendonly 的 transceiver（sender 上还没有轨）。此刻
    // AddTrack 会被 libwebrtc 关联到这条 m-line 上，CreateAnswer 就带出了 sendonly 的
    // 视频段——不需要任何重协商，也就不需要（也不能）发 offer。
    if (screen_passive_ && video_sender_ == nullptr && track_ != nullptr) {
      std::string add_error;
      signaling_->BlockingCall([&] {
        auto added = pc_->AddTrack(track_, {"partyup-screen"});
        if (!added.ok()) {
          add_error = std::string("AddTrack(video) on the passive screen session failed: ") +
                      added.error().message();
        } else {
          video_sender_ = added.value();
          // v2.13：被动方（relay 里本端采屏、对端 offer）同样要产品级发送参数：
          // 降帧不降分辨率 + 码率上下限。两条建轨路径必须一致。
          ApplyVideoSenderParamsOnSignalingThread();
        }
      });
      if (!add_error.empty()) {
        ReleaseStreamObjects();
        return reject(PU_ERR_MEDIA, "pu_signal_in(passive screen): " + add_error);
      }
      Log(3, "passive screen: video track attached to the peer's offered m-line (no offer sent)");
    }
    if (!CreateAnswerAndSetLocal(&local_error)) {
      ReleaseStreamObjects();
      return reject(PU_ERR_MEDIA, "pu_signal_in: " + local_error);
    }
    // answer 同样要等 ICE 收集完成再发，交给协商线程。
    StartNegotiationEmit("answer");
    return PU_OK;
  }

  if (message.type == "answer") {
    if (route_to_voice) {
      // ---- 语音会话：本端必须是这条会话的 offerer ----
      if (voice_pc_ == nullptr) {
        return reject(PU_ERR_STATE,
                      "received a voice answer but this engine has no voice peer connection");
      }
      if (!voice_offerer_) {
        return reject(PU_ERR_STATE,
                      "received an answer but this engine is the voice answerer (role mismatch)");
      }
      std::string local_error;
      if (!VoiceSetRemoteDescription(static_cast<int>(webrtc::SdpType::kAnswer), message.sdp,
                                     &local_error)) {
        return reject(PU_ERR_MEDIA, "pu_signal_in(voice): " + local_error);
      }
      voice_has_remote_description_ = true;
      VoiceApplyBufferedCandidates();
      return PU_OK;
    }
    // ---- 屏幕会话（原逻辑）----
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
  // 选目标会话：有显式 media 时直接用它（无歧义）；否则退回「哪条会话存在」的推断：
  //   * 只有语音 PC（纯语音会话）→ 语音；
  //   * 其余（只有屏幕 PC，或两条都在）→ 屏幕。两条都在时给屏幕是**兼容入口的兜底**
  //     （语音候选在等 ICE 收集完成阶段就已随 SDP 一起发出，late candidate 极少）；
  //     客户端现在一律走 pu_signal_in_media 显式传 media。
  const bool candidate_to_voice = explicit_media ? route_to_voice
                                                 : (voice_pc_ != nullptr && pc_ == nullptr);
  if (candidate_to_voice) {
    if (voice_pc_ == nullptr) {
      return reject(PU_ERR_STATE,
                    "received a voice candidate but this engine has no voice peer connection");
    }
    if (!voice_has_remote_description_) {
      // 远端描述还没设好就先存着（trickle 可能比 SDP 先到），等 SetRemote 之后统一加。
      std::lock_guard<std::mutex> lock(voice_cand_mu_);
      CandidateCopy copy;
      copy.mid = message.mid;
      copy.mline_index = message.mline;
      copy.sdp = message.candidate;
      voice_buffered_.push_back(copy);
      Log(3, "voice candidate buffered until remote description is set: %s", copy.sdp.c_str());
      return PU_OK;
    }
    bool voice_applied = false;
    signaling_->BlockingCall([this, &message, &voice_applied] {
      std::unique_ptr<webrtc::IceCandidate> parsed = webrtc::IceCandidate::Create(
          message.mid, message.mline, message.candidate, nullptr);
      if (parsed != nullptr) voice_applied = voice_pc_->AddIceCandidate(parsed.get());
    });
    if (!voice_applied) {
      Log(2, "voice AddIceCandidate returned false for: %s", message.candidate.c_str());
    }
    return PU_OK;
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
  // v2.10：语音会话有独立的 PC 与状态原子量。纯语音（没有屏幕 PC）时报语音那套，
  // 这样 peer_state/ice_state 的语义不变（= 当前唯一那条会话的状态）；屏幕在时报屏幕那套
  // （它才是 UI 的「共享连接」指标）。
  const bool has_screen_pc = pc_ != nullptr;
  out->peer_state = has_screen_pc ? stat_peer_state_.load(std::memory_order_relaxed)
                                  : voice_peer_state_.load(std::memory_order_relaxed);
  out->ice_state = has_screen_pc ? stat_ice_state_.load(std::memory_order_relaxed)
                                 : voice_ice_state_.load(std::memory_order_relaxed);
  out->has_peer = (has_screen_pc || voice_pc_ != nullptr) ? 1u : 0u;
  out->frames_sent = stat_frames_sent_.load(std::memory_order_relaxed);
  // v2.1：本地编码器已删除，这个数就是 PC 的 outbound-rtp.framesEncoded（1:1 编码
  // 时等于 PU_EVENT_FRAME 的条数）。采样节拍 250 ms，最多陈旧一个窗口。
  out->frames_encoded = static_cast<uint32_t>(
      std::min<uint64_t>(pc_stats_->frames_encoded.load(std::memory_order_relaxed), 0xFFFFFFFFu));
  out->frames_dropped = stat_frames_dropped_.load(std::memory_order_relaxed);
  out->frames_received = stat_frames_received_.load(std::memory_order_relaxed);
  // v2.6：语音模式下 frames_received 表示「远端解出的音频 PCM 帧数」（每帧 10 ms），
  // 因为语音没有视频帧可数；两者互斥，不会同时有值。峰值振幅靠日志观察（mute 验收）。
  // v2.7：按轨聚合。relay 预协商 12 条槽 → 远端音轨对象恒为 12 条，但只有**真有声音**
  // 的轨才计帧（AudioCountSink 里按峰值阈值过滤，见 VoicePeakThresholdFromEnv），
  // 所以这里把「轨对象数」和「活跃（有声音）轨数」分开报，别混。
  // frames_received = 各活跃轨的 voiced PCM 帧数之和。
  if (voice_joined_) {
    uint32_t total_frames = 0;
    uint32_t max_peak = 0;
    size_t tracks = 0;
    size_t active = 0;
    const int64_t now_ms = NowUsSteady() / 1000;
    {
      std::lock_guard<std::mutex> lock(audio_tracks_mu_);
      tracks = audio_tracks_.size();
      for (const auto& stat : audio_tracks_) {
        const uint32_t f = stat->frames.load(std::memory_order_relaxed);
        const uint32_t p = stat->peak.load(std::memory_order_relaxed);
        const int64_t last = stat->last_frame_ms.load(std::memory_order_relaxed);
        total_frames += f;
        if (p > max_peak) max_peak = p;
        if (last != 0 && now_ms - last <= 1000) ++active;
        Log(3, "voice track: id=%s pcmFrames=%u peak=%u maxPeak=%u active=%d", stat->id.c_str(),
            f, p, stat->max_peak.load(std::memory_order_relaxed),
            (last != 0 && now_ms - last <= 1000) ? 1 : 0);
      }
    }
    out->frames_received = total_frames;
    Log(3, "voice stats: tracks=%zu active=%zu pcmFrames=%u peak=%u", tracks, active,
        total_frames, max_peak);
  }
  out->signal_out_count = stat_signal_out_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    out->event_drops = static_cast<uint32_t>(dropped_events_);
  }
  out->remote_width = stat_remote_width_.load(std::memory_order_relaxed);
  out->remote_height = stat_remote_height_.load(std::memory_order_relaxed);
  out->last_remote_frame_us = stat_last_remote_us_.load(std::memory_order_relaxed);
  // v2.4：reserved 复用为「本次实际用的视频编码器」——0=还没有编码器，
  // 1=硬件 H.264 MFT，2=软件 H.264 MFT，3=软件 VP8（硬件/软件 H.264 都不可用）。
  // 布局不变（还是 uint32_t reserved），ABI 仍是 v2。
  {
    const partyup::H264EncoderKind kind = partyup::LastCreatedEncoderKind();
    if (kind != partyup::H264EncoderKind::kNone) {
      out->reserved = static_cast<uint32_t>(kind);
    } else if (pc_ != nullptr) {
      out->reserved = 3;  // 有 PC 但没有 H.264 编码器 = 走软件 VP8
    }
  }
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

  // v2.5：把解码后的 I420 转成 BGRA 存进三槽之一，供 pu_frame_acquire 取用。
  // 只在解码线程写：先挑一个没被 acquire 占用的槽（占用中的绝不碰），转换完成
  // 后再挂成 ready —— 读取方看到 ready 时像素一定是完整的。
  {
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = frame.video_frame_buffer();
    if (buffer != nullptr && buffer->type() == webrtc::VideoFrameBuffer::Type::kI420) {
      const int width = buffer->width();
      const int height = buffer->height();
      const uint32_t stride = static_cast<uint32_t>(width) * 4u;
      const size_t needed = static_cast<size_t>(stride) * static_cast<size_t>(height);
      int slot_index = -1;
      int picked_width = 0;
      {
        std::lock_guard<std::mutex> lock(frame_mu_);
        for (int i = 0; i < kFrameSlots; ++i) {
          const int candidate = (frame_write_slot_ + i) % kFrameSlots;
          if (!frame_slots_[candidate].busy) {
            slot_index = candidate;
            break;
          }
        }
      }
      if (slot_index >= 0) {
        FrameSlot& slot = frame_slots_[slot_index];
        if (slot.bytes.size() < needed) slot.bytes.resize(needed);
        const webrtc::I420BufferInterface* i420 = buffer->GetI420();
        if (i420 != nullptr && i420->width() == width && i420->height() == height) {
          // 行步长如实填 stride：绝不让下游假设 stride == width*4。
          libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(),
                             i420->StrideU(), i420->DataV(), i420->StrideV(),
                             slot.bytes.data(), static_cast<int>(stride), width,
                             height);
          std::lock_guard<std::mutex> lock(frame_mu_);
          slot.width = static_cast<uint32_t>(width);
          slot.height = static_cast<uint32_t>(height);
          slot.stride = stride;
          slot.timestamp_us = frame.timestamp_us();
          slot.seq = ++frame_ready_seq_;
          frame_ready_slot_ = slot_index;
          frame_write_slot_ = (slot_index + 1) % kFrameSlots;
          picked_width = width;
        }
      }
      static_cast<void>(picked_width);
    }
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

// v2.5：pu_frame_acquire 的后端。语义见 partyup_media.h 的 pu_frame 注释。
int MediaEngine::AcquireFrame(const char* stream_id, pu_frame* out) {
  if (out == nullptr) {
    SetLastError("pu_frame_acquire: out is null");
    return PU_ERR_INVALID_ARG;
  }
  const std::string requested = stream_id != nullptr ? stream_id : "";
  if (!requested.empty() && requested != "remote") {
    SetLastError("pu_frame_acquire: unknown stream_id '" + requested + "'");
    return PU_ERR_INVALID_ARG;
  }
  if (out->struct_size != 0 && out->struct_size < sizeof(pu_frame)) {
    SetLastError("pu_frame_acquire: pu_frame.struct_size is smaller than the ABI struct");
    return PU_ERR_INVALID_ARG;
  }

  std::lock_guard<std::mutex> lock(frame_mu_);
  if (frame_ready_slot_ < 0 ||
      frame_slots_[frame_ready_slot_].seq <= frame_served_seq_) {
    // 还没比上次更新过的帧：这是「未就绪」，不是失败。
    return PU_ERR_TIMEOUT;
  }
  FrameSlot& slot = frame_slots_[frame_ready_slot_];
  slot.busy = true;
  frame_acquired_slot_ = frame_ready_slot_;
  frame_ready_slot_ = -1;
  frame_served_seq_ = slot.seq;

  out->struct_size = static_cast<uint32_t>(sizeof(pu_frame));
  out->width = slot.width;
  out->height = slot.height;
  out->stride = slot.stride;
  out->timestamp_us = slot.timestamp_us;
  out->pixels = slot.bytes.data();
  out->stream_id = 1;  // 当前只有远端视频流
  return PU_OK;
}

void MediaEngine::ReleaseFrame(pu_frame* frame) {
  static_cast<void>(frame);
  std::lock_guard<std::mutex> lock(frame_mu_);
  if (frame_acquired_slot_ < 0) {
    // 不配对的 release：计数 + 记日志，但不影响引擎（也不能泄漏内存）。
    ++frame_unpaired_release_;
    Log(2, "pu_frame_release without a matching acquire (%llu total)",
        static_cast<unsigned long long>(frame_unpaired_release_));
    return;
  }
  frame_slots_[frame_acquired_slot_].busy = false;
  frame_acquired_slot_ = -1;
}

// ---------------------------------------------------------------------------
// 采集 / 发事件（v2.1：本地编码器已删除）
// ---------------------------------------------------------------------------
void MediaEngine::PumpLoop() {
  // 真实采集自己会做「起跑能不能成功」的握手（建 WGC 会话 / 等首帧），失败要让
  // pu_screen_start 返回错误，所以分派到独立实现。
  if (capture_is_real_.load(std::memory_order_relaxed)) {
    RealCaptureLoop();
    return;
  }

  // 以前这里先 InitEncode 一个本地 VP8 编码器、把它挂成采集源的 sink。现在采集源
  // 只挂 VideoTrack，唯一的编码点就是 PC 的 VideoStreamEncoder，所以起跑时没有
  // 可失败的东西，直接通知控制线程「采集线程已在跑」。
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

    // 只喂 VideoTrack：唯一的编码点就是 PC 的 VideoStreamEncoder。
    // （v2 里这里还同步回调 OnFrame → 本地 encoder->Encode，同一帧被编码两次。）
    source_->PushFrame(frame);
    stat_frames_sent_.fetch_add(1, std::memory_order_relaxed);

    // 事件仍然逐帧产出；bytes/key/bitrate 用最近一次 250 ms 采样窗口的值。
    PushFrameEvent(timestamp_us);

    ++index;
    timestamp_us += kFrameIntervalUs;
    next_tick += std::chrono::microseconds(kFrameIntervalUs);
    if (next_tick < std::chrono::steady_clock::now()) {
      // 落后超过一个周期就直接对齐，不补帧（补帧只会让时间戳更假）。
      next_tick = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_until(next_tick);
  }

  Log(3, "capture stopped after %llu frames", static_cast<unsigned long long>(index));
}


// ---------------------------------------------------------------------------
// v2.3：真实屏幕采集（Windows Graphics Capture）
//
// 与合成循环的区别：
//   * 节拍：WGC 的 FrameArrived 驱动，不是 sleep_until(16.7 ms)。本机实测到达节奏
//     约 21 ms、抖动到 55 ms，按固定节拍取样只会把抖动变成丢帧/重复帧；
//   * 起跑握手：Open() + 首帧都成功之后才 SignalStart(true)，失败让 start 报错；
//   * 每帧耗时拆四段：capture（到达间隔）/ readback / convert / encode-wait，
//     停止时打一行 ===CAPTURE_SUMMARY===，冒烟测试直接抓这行。
// ---------------------------------------------------------------------------
void MediaEngine::RealCaptureLoop() {
  const auto loop_start = std::chrono::steady_clock::now();
  std::string error;

  std::unique_ptr<WgcCapture> capture = WgcCapture::Create(capture_source_id_, &error);
  if (capture == nullptr) {
    Log(1, "wgc capture create failed: %s", error.c_str());
    SignalStart(false, error);
    return;
  }
  if (!capture->Open(&error)) {
    Log(1, "wgc capture open failed: %s", error.c_str());
    SignalStart(false, error);
    return;
  }
  capture_width_.store(capture->width(), std::memory_order_relaxed);
  capture_height_.store(capture->height(), std::memory_order_relaxed);
  const double open_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - loop_start)
                             .count();
  Log(3, "wgc capture open: source=%s %ux%u open_ms=%.1f", capture_source_id_.c_str(),
      capture->width(), capture->height(), open_ms);

  // 首帧超时 = 起跑失败：锁屏 / 捕获策略禁用时 WGC 一帧都不给，不能让 start
  // 返回 OK 然后永远黑屏。
  if (!capture->WaitFirstFrame(5000, &error)) {
    Log(1, "wgc capture first frame timeout: %s", error.c_str());
    capture->Close();
    SignalStart(false, error);
    return;
  }
  const double first_frame_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - loop_start)
                                    .count();
  Log(3, "wgc capture first frame ready (cold start %.1f ms), start handshake ok",
      first_frame_ms);
  SignalStart(true, "");

  // 逐帧统计：唯一写者是本线程，不需要原子量。
  uint64_t frames = 0;
  double interval_sum = 0.0;
  double interval_max = 0.0;
  double readback_sum = 0.0;
  double readback_max = 0.0;
  double convert_sum = 0.0;
  double convert_max = 0.0;
  double push_sum = 0.0;
  double push_max = 0.0;
  double total_sum = 0.0;
  double total_max = 0.0;

  const auto sink = [this, &frames](const webrtc::VideoFrame& frame) -> bool {
    // 唯一的编码点是 PC 的 VideoStreamEncoder（v2.1 删掉了本地编码器）。
    source_->PushFrame(frame);
    stat_frames_sent_.fetch_add(1, std::memory_order_relaxed);
    PushFrameEvent(frame.timestamp_us());
    ++frames;
    return !stop_requested_.load(std::memory_order_relaxed);
  };

  const auto on_timing = [&](const CaptureStageTimings& timings) {
    interval_sum += timings.interval_ms;
    interval_max = std::max(interval_max, timings.interval_ms);
    readback_sum += timings.readback_ms;
    readback_max = std::max(readback_max, timings.readback_ms);
    convert_sum += timings.convert_ms;
    convert_max = std::max(convert_max, timings.convert_ms);
    push_sum += timings.push_ms;
    push_max = std::max(push_max, timings.push_ms);
    total_sum += timings.total_ms;
    total_max = std::max(total_max, timings.total_ms);
    // 长间隔单独打一条：WGC 正常情况下 16-25 ms 到一帧，出现 >200 ms 的空档
    // 要么是 DWM 停了（锁屏/全屏独显切换），要么是采集线程被别的活堵住了。
    if (timings.interval_ms > 200.0) {
      Log(2, "capture gap %.1f ms before frame %llu", timings.interval_ms,
          static_cast<unsigned long long>(frames));
    }
    // 前 3 帧 + 每 30 帧一条：够看出稳态，又不会把日志刷爆。
    if (frames <= 3 || frames % 30 == 0) {
      Log(3,
          "capture frame=%llu capture_ms=%.2f readback_ms=%.2f convert_ms=%.2f "
          "encode_wait_ms=%.2f total_ms=%.2f",
          static_cast<unsigned long long>(frames), timings.interval_ms, timings.readback_ms,
          timings.convert_ms, timings.push_ms, timings.total_ms);
    }
  };

  capture->Run(sink, on_timing, &stop_requested_);

  const uint64_t dropped = capture->dropped_frames();
  const double measured = frames > 0 ? static_cast<double>(frames) : 1.0;
  // fps 用「到达间隔之和」算（不含最后一帧之后的空档），比用墙钟更接近采集节奏。
  const double fps = (frames > 1 && interval_sum > 0.0)
                         ? (static_cast<double>(frames) - 1.0) * 1000.0 / interval_sum
                         : 0.0;
  Log(3,
      "===CAPTURE_SUMMARY=== source=%s frames=%llu dropped=%llu fps=%.2f "
      "interval_avg_ms=%.2f interval_max_ms=%.2f readback_avg_ms=%.2f "
      "readback_max_ms=%.2f convert_avg_ms=%.2f convert_max_ms=%.2f "
      "push_avg_ms=%.2f push_max_ms=%.2f total_avg_ms=%.2f total_max_ms=%.2f",
      capture_source_id_.c_str(), static_cast<unsigned long long>(frames),
      static_cast<unsigned long long>(dropped), fps, interval_sum / measured, interval_max,
      readback_sum / measured, readback_max, convert_sum / measured, convert_max,
      push_sum / measured, push_max, total_sum / measured, total_max);
  if (!capture->dumped_png_path().empty()) {
    Log(3, "capture first frame dumped to %s (verify it shows the real desktop)",
        capture->dumped_png_path().c_str());
  }
  if (!capture->last_error().empty()) {
    Log(1, "capture error: %s", capture->last_error().c_str());
    PushError(PU_ERR_MEDIA, capture->last_error());
  }
  Log(3, "wgc capture stopped after %llu frames", static_cast<unsigned long long>(frames));
  // 析构在采集线程上发生（本函数返回时 capture 离开作用域），COM/D3D 对象的
  // 释放与创建落在同一个线程上。
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

void MediaEngine::PushFrameEvent(int64_t capture_ts_us) {
  const uint64_t sequence = frame_sequence_++;
  pu_event event = MakeEvent(PU_EVENT_FRAME);
  event.data.frame.sequence = sequence;
  event.data.frame.capture_ts_us = capture_ts_us;
  // 尺寸来自真正的采集源（合成 640x360 / 显示器原生分辨率）。
  event.data.frame.width = capture_width_.load(std::memory_order_relaxed);
  event.data.frame.height = capture_height_.load(std::memory_order_relaxed);
  // 下面三个字段来自 PC 的 GetStats 采样（见 StatsSampleLoop）：逐帧拿不到真实
  // 编码字节数，用「最近一个采样窗口的均值」代表一帧，码率同窗口。
  event.data.frame.bytes = sample_frame_bytes_.load(std::memory_order_relaxed);
  event.data.frame.bitrate_kbps = sample_bitrate_kbps_.load(std::memory_order_relaxed);
  event.data.frame.keyframe = sample_keyframe_.load(std::memory_order_relaxed);
  event.data.frame.reserved = 0;
  PushEvent(event);
}

// ---------------------------------------------------------------------------
// v2.1：PC 统计采样（PU_EVENT_FRAME 的 bytes / keyframe / bitrate 由此派生）
// ---------------------------------------------------------------------------

void MediaEngine::StartStatsSampler(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
  if (pc == nullptr) return;
  if (stats_.joinable()) return;  // 已经在跑（正常路径不会重复 start）
  stats_stop_.store(false, std::memory_order_relaxed);
  stats_ = std::thread([this, pc] { StatsSampleLoop(pc); });
}

void MediaEngine::StopStatsSampler() {
  stats_stop_.store(true, std::memory_order_relaxed);
  if (!stats_.joinable()) {
    Log(3, "stats-stop: no sampler thread");
    return;
  }
  Log(3, "stats-stop: joining sampler");
  stats_.join();
  Log(3, "stats-stop: sampler joined, draining signaling queue");
  // 采样线程可能在队列里留下最后一条 GetStats 任务：它揣着 PC 的引用，如果让它
  // 留到 signaling_->Stop() 被丢弃，PC 就可能在控制线程上析构（libwebrtc 的
  // PeerConnection 是要在 signaling 线程上析构的）。这里投一条空任务把它排干。
  if (signaling_ != nullptr) signaling_->BlockingCall([] {});
  Log(3, "stats-stop: queue drained");
  // 收尾时把 PC 侧的累计量报一次：这是「一共编码了多少帧」的直接证据（1:1 时
  // 应该等于采集推出去的帧数，而不是它的两倍）。
  Log(3, "pc stats at stop: framesEncoded=%llu bytesSent=%llu keyFramesEncoded=%llu (samples=%u)",
      static_cast<unsigned long long>(pc_stats_->frames_encoded.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(pc_stats_->bytes_sent.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          pc_stats_->key_frames_encoded.load(std::memory_order_relaxed)),
      pc_stats_->reports.load(std::memory_order_relaxed));
}

// 发起一次异步 GetStats 并把回调挂到 signaling 线程上。这里只投递、不等待：
// 回调在 signaling 线程送达，阻塞等待会直接死锁（spike 里写死过的坑）。
void MediaEngine::RequestStats(webrtc::PeerConnectionInterface* pc) {
  if (pc == nullptr || signaling_ == nullptr || pc_stats_ == nullptr) return;
  std::shared_ptr<PcStatsState> state = pc_stats_;
  // 任务自己揣着 pc 的引用：即使引擎已经收摊，队列里这一条也不会打到悬空指针上。
  signaling_->PostTask(
      [pc_ref = webrtc::scoped_refptr<webrtc::PeerConnectionInterface>(pc), state] {
        // 裸指针参数由 PC 接管所有权（rtc 的注释：Takes shared ownership）。
        pc_ref->GetStats(new webrtc::RefCountedObject<StatsCollector>(state));
      });
}

void MediaEngine::StatsSampleLoop(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
  int64_t prev_us = NowUsSteady();
  uint64_t prev_bytes = 0;
  uint64_t prev_frames = 0;
  uint64_t prev_keys = 0;

  // 读一次共享状态、算一个窗口的增量、写回三个派生量。只在本线程调用。
  const auto sample_once = [&] {
    const uint32_t reports = pc_stats_->reports.load(std::memory_order_relaxed);
    const uint64_t bytes = pc_stats_->bytes_sent.load(std::memory_order_relaxed);
    const uint64_t frames = pc_stats_->frames_encoded.load(std::memory_order_relaxed);
    const uint64_t keys = pc_stats_->key_frames_encoded.load(std::memory_order_relaxed);
    const int64_t now_us = NowUsSteady();

    if (voice_joined_) {
      // v2.7：分轨明细（relay 下每路一条），由各自的 AudioCountSink 累加。
      std::lock_guard<std::mutex> lock(audio_tracks_mu_);
      for (const auto& stat : audio_tracks_) {
        Log(3, "voice track: id=%s pcmFrames=%u peak=%u", stat->id.c_str(),
            stat->frames.load(std::memory_order_relaxed),
            stat->peak.load(std::memory_order_relaxed));
      }
    }

    if (reports > 0) {
      // 计数器只会在「换了一个新 PC」时倒退；这时把当前值整个当成一轮增量。
      const uint64_t d_bytes = bytes >= prev_bytes ? bytes - prev_bytes : bytes;
      const uint64_t d_frames = frames >= prev_frames ? frames - prev_frames : frames;
      const uint64_t d_keys = keys >= prev_keys ? keys - prev_keys : keys;
      const int64_t span_us = std::max<int64_t>(1, now_us - prev_us);

      if (d_frames > 0) {
        sample_frame_bytes_.store(
            static_cast<uint32_t>(std::min<uint64_t>(d_bytes / d_frames, 0xFFFFFFFFu)),
            std::memory_order_relaxed);
        sample_bitrate_kbps_.store(
            static_cast<uint32_t>(std::min<uint64_t>(
                d_bytes * 8 * 1000000ULL / static_cast<uint64_t>(span_us) / 1000ULL, 0xFFFFFFFFu)),
            std::memory_order_relaxed);
        sample_keyframe_.store(d_keys > 0 ? 1u : 0u, std::memory_order_relaxed);
      } else {
        // 这一窗没有编码帧（例如 DTLS 还没通）：key 归 0，byte/码率沿用上次读数。
        sample_keyframe_.store(0, std::memory_order_relaxed);
      }

      Log(4,
          "pc stats: framesEncoded=%llu (+%llu) bytesSent=%llu (+%llu) "
          "keyFramesEncoded=%llu (+%llu) -> %u B/frame, %u kbps",
          static_cast<unsigned long long>(frames), static_cast<unsigned long long>(d_frames),
          static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(d_bytes),
          static_cast<unsigned long long>(keys), static_cast<unsigned long long>(d_keys),
          sample_frame_bytes_.load(std::memory_order_relaxed),
          sample_bitrate_kbps_.load(std::memory_order_relaxed));
    }

    prev_bytes = bytes;
    prev_frames = frames;
    prev_keys = keys;
    prev_us = now_us;
  };

  // 先要一份：这样第一个 250 ms 窗口结束时就有可用的累计量做差。
  RequestStats(pc.get());

  while (!stats_stop_.load(std::memory_order_relaxed)) {
    // 把 250 ms 切成 10 ms 的片，stop 时最多 10 ms 就能退出来。
    for (int slept = 0; slept < kStatsSampleIntervalMs; slept += kStatsSleepSliceMs) {
      if (stats_stop_.load(std::memory_order_relaxed)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(kStatsSleepSliceMs));
    }
    if (stats_stop_.load(std::memory_order_relaxed)) break;
    sample_once();
    RequestStats(pc.get());  // 为下一拍取数
  }

  // 收尾：Stop() 里 pump 已经先停了（不会再有新帧），这里再取一份报告把
  // framesEncoded 追到终值——调用方在 stop 之后读到的编码次数就不是「最多陈旧
  // 250 ms」的数了。等报告也在本线程上等，绝不阻塞控制线程。
  //
  // 只在「已经拿到过报告」时才等：连 outbound 统计都还没有（例如对端还没连上）
  // 的时候再等也不会有新东西，白白把 stop 拖慢几百毫秒。
  if (pc_stats_->reports.load(std::memory_order_relaxed) > 0) {
    const uint32_t reports_before = pc_stats_->reports.load(std::memory_order_relaxed);
    RequestStats(pc.get());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kStatsFinalWaitMs);
    while (std::chrono::steady_clock::now() < deadline &&
           pc_stats_->reports.load(std::memory_order_relaxed) <= reports_before) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sample_once();
  }
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

void MediaEngine::PushEventWithPayload(pu_event event, std::string payload, std::string media) {
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
  entry.media = std::move(media);
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
    // v2.10 第 2 步：登记 (payload 指针 → media)，供 pu_signal_media 查回。
    // 必须在滚动淘汰之前登记，淘汰时同步注销，保证表里只有「指针仍有效」的项。
    const std::string& payload = delivered_signals_.back();
    RegisterDeliveredMedia(payload.c_str(), front.media);
    while (delivered_signals_.size() > kMaxRetainedSignals) {
      UnregisterDeliveredMedia(delivered_signals_.front().c_str());
      delivered_signals_.pop_front();
    }
    out->data.signal.payload = payload.c_str();
    out->data.signal.payload_len = static_cast<uint32_t>(payload.size());
  }
  queue_.pop_front();
  return PU_OK;
}

}  // namespace

// v2.10：pu_signal_media 的桥。导出函数在 extern "C" 里、不能直接引用匿名命名空间的
// 符号，所以在这里（partyup 命名空间内）转一手。
int SignalMediaOfDeliveredEvent(const pu_event* event, char* media_out, int capacity) {
  return SignalMediaOfEvent(event, media_out, capacity);
}

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

// v2.12：引擎级 ICE servers（详见 MediaEngine::SetIceServers 的注释）。
// 空串 / len==0 / urls==nullptr 都表示「清空」，回到 host-only（v2.11 行为）。
PU_API int pu_engine_set_ice_servers(pu_engine* e, const char* urls, size_t len) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  if (urls == nullptr && len != 0) {
    e->core.SetLastError("pu_engine_set_ice_servers: urls is null but len != 0");
    return PU_ERR_INVALID_ARG;
  }
  try {
    std::string error;
    const int rc = e->core.SetIceServers(urls, len, &error);
    if (rc != PU_OK) {
      e->core.SetLastError(error.empty() ? "pu_engine_set_ice_servers failed" : error);
    }
    return rc;
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_engine_set_ice_servers exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_engine_set_ice_servers: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API int pu_screen_start(pu_engine* e, pu_screen_id screen_id) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    // v2.11：老入口语义不变 = flags 0（本端 offerer）。
    return e->core.Start(screen_id, 0u, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_start exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_start: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

/* v2.11：带角色标志的屏幕共享入口。flags bit0 = passive。 */
PU_API int pu_screen_start_ex(pu_engine* e, pu_screen_id screen_id, uint32_t flags) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.Start(screen_id, flags, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_start_ex exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_start_ex: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

/* v2.11：观看者预建屏幕会话（answerer，不采屏、不建轨），幂等。 */
PU_API int pu_screen_prepare(pu_engine* e) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.PrepareScreen(&error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_prepare exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_prepare: unknown exception");
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

/* v2.10 起是兼容入口：media == nullptr，会话归属靠 SDP 嗅探（见头文件）。 */
PU_API int pu_signal_in(pu_engine* e, const char* payload, size_t len) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.SignalIn(nullptr, payload, len, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_signal_in exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_signal_in: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

/* v2.10：显式指定信令投给哪条会话（"voice" / "screen"），不做嗅探。 */
PU_API int pu_signal_in_media(pu_engine* e, const char* media, const char* payload, size_t len) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.SignalIn(media, payload, len, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_signal_in_media exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_signal_in_media: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

/* v2.10：读回一条 SIGNAL_OUT 事件的 media（"voice"/"screen"）。无引擎参数，
 * 实现见文件顶部的 g_delivered_media 登记表。 */
PU_API int pu_signal_media(const pu_event* event, char* media_out, int capacity) {
  return partyup::SignalMediaOfDeliveredEvent(event, media_out, capacity);
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

namespace partyup {
namespace {

int MediaEngine::VoiceJoin(const pu_voice_params* params, std::string* error) {
  if (params == nullptr || params->struct_size < sizeof(pu_voice_params)) {
    if (error != nullptr) *error = "pu_voice_params.struct_size too small";
    return PU_ERR_INVALID_ARG;
  }
  if (params->mode == PU_MODE_RELAY) {
    if (error != nullptr) *error = "voice relay mode not implemented: SFU 未接";
    return PU_NOT_IMPLEMENTED;
  }
  if (voice_joined_) return PU_OK;
  // v2.6：flags bit0 = 1 表示「被动加入」——只接不发 offer，等对方 offer 后回 answer。
  // 1:1 场景里后加入的一方走这条路（双方都发 offer 会 glare，SignalIn 会直接报角色冲突）。
  // 字段本身是预留 u32，不涉及 ABI 布局变更。
  const bool passive = (params->flags & 1u) != 0;

  // v2.10 第 1 步：语音改用**自己的** PeerConnection（voice_pc_），不再借用屏幕的 pc_。
  // for_sending = !passive：主动加入的一方是 offerer，被动加入（1:1 的后加入者 / relay 下
  // 服务端是 offerer）是 answerer。两条 PC 因此各自有独立的 signaling state：
  // relay 语音（answerer）与直连共享（offerer）同时存在也不会互相打成 role mismatch。
  if (!EnsureVoicePeerConnection(/*for_sending=*/!passive, error)) return PU_ERR_STATE;
  std::string add_error;
  signaling_->BlockingCall([&] {
    voice_source_ = pc_factory_->CreateAudioSource(webrtc::AudioOptions());
    if (voice_source_ == nullptr) {
      add_error = "CreateAudioSource returned null";
      return;
    }
    voice_track_ = pc_factory_->CreateAudioTrack("audio0", voice_source_.get());
    if (voice_track_ == nullptr) {
      add_error = "CreateAudioTrack returned null";
      return;
    }
    auto added = voice_pc_->AddTrack(voice_track_, {"voice"});
    if (!added.ok()) {
      add_error = added.error().message();
      return;
    }
    voice_sender_ = added.value();
  });
  if (!add_error.empty()) {
    if (error != nullptr) *error = add_error;
    return PU_ERR_STATE;
  }
  // v2.6：显式选一个**物理**录音设备。默认设备可能是 NVIDIA Broadcast 这类虚拟麦，
  // 无输入时输出数字静音，mute 的音频效果就没法验证。
  signaling_->BlockingCall([&] {
    if (voice_adm_ == nullptr) return;
    const int16_t count = voice_adm_->RecordingDevices();
    for (int16_t i = 0; i < count; ++i) {
      char name[webrtc::kAdmMaxDeviceNameSize] = {0};
      char guid[webrtc::kAdmMaxGuidSize] = {0};
      if (voice_adm_->RecordingDeviceName(i, name, guid) != 0) continue;
      const std::string n(name);
      if (n.find("NVIDIA") != std::string::npos ||
          n.find("Broadcast") != std::string::npos) {
        continue;
      }
      if (voice_adm_->SetRecordingDevice(i) == 0) {
        Log(3, "voice: recording device -> [%d] %s", static_cast<int>(i), name);
        return;
      }
    }
    Log(2, "voice: no physical recording device found (count=%d), keeping default",
        static_cast<int>(count));
  });
  voice_channel_id_ = params->channel_id != nullptr ? params->channel_id : "";
  voice_mode_ = params->mode;
  voice_muted_ = false;
  voice_deafened_ = false;
  voice_joined_ = true;
  if (passive) {
    Log(3, "voice: joined passive (waiting for the peer's offer)");
    return PU_OK;
  }
  // 和 pu_screen_start 一样：先 CreateOffer + SetLocalDescription，再起信令发射线程
  // （VoiceNegotiationEmitLoop 要读 pc->local_description()，没 SetLocal 的话 SDP 是空的）。
  // 角色位必须写在**语音自己的** voice_offerer_ 上，不能动屏幕的 sharing_——那是屏幕会话
  // 的 offerer 标志，语音置 true 会让屏幕侧的 answer 被误判成 role mismatch。
  voice_offerer_ = true;
  {
    std::string sdp_error;
    if (!VoiceCreateOfferAndSetLocal(&sdp_error)) {
      voice_joined_ = false;
      if (error != nullptr) *error = sdp_error;
      return PU_ERR_MEDIA;
    }
  }
  VoiceStartNegotiationEmit("offer");
  Log(3, "voice: joined channel=%s mode=direct", voice_channel_id_.c_str());
  return PU_OK;
}

int MediaEngine::VoiceLeave() {
  // v2.10 第 1 步：语音会话有自己的 PC，释放它不再依赖屏幕是否在跑。
  // 旧代码用 !running_ 判据是因为语音/屏幕共用一个 pc_（共享中不能拆）；现在两条 PC
  // 独立，恒拆即可——重进频道也从干净会话开始，不会留下「同一会话重协商」的旧 DTLS。
  voice_joined_ = false;
  voice_channel_id_.clear();
  ReleaseVoiceSession();
  Log(3, "voice: left (voice session released)");
  return PU_OK;
}

int MediaEngine::VoiceSetMode(pu_mode mode) {
  if (mode == PU_MODE_RELAY) {
    SetLastError("pu_voice_set_mode: relay not implemented (SFU 未接)");
    return PU_NOT_IMPLEMENTED;
  }
  if (mode != PU_MODE_DIRECT) {
    SetLastError("pu_voice_set_mode: unknown mode");
    return PU_ERR_INVALID_ARG;
  }
  voice_mode_ = mode;
  return PU_OK;
}

int MediaEngine::VoiceSetMute(bool muted) {
  if (!voice_joined_) {
    SetLastError("pu_voice_set_mute: not in voice");
    return PU_ERR_STATE;
  }
  int rc = PU_OK;
  bool adm_muted = false;
  // ADM 有线程归属：所有调用都投 signaling 线程（创建/Terminate 也在这条线程）。
  signaling_->BlockingCall([&] {
    if (voice_adm_ == nullptr) {
      rc = PU_ERR_STATE;
      return;
    }
    rc = voice_adm_->SetMicrophoneMute(muted) == 0 ? PU_OK : PU_ERR_STATE;
    voice_adm_->MicrophoneMute(&adm_muted);
  });
  if (rc == PU_OK) voice_muted_ = muted;
  Log(3, "voice: mute=%d rc=%d adm_muted=%d", muted ? 1 : 0, rc, adm_muted ? 1 : 0);
  return rc;
}

int MediaEngine::VoiceSetDeafen(bool deafened) {
  if (!voice_joined_) {
    SetLastError("pu_voice_set_deafen: not in voice");
    return PU_ERR_STATE;
  }
  int rc = PU_OK;
  signaling_->BlockingCall([&] {
    if (voice_adm_ == nullptr) {
      rc = PU_ERR_STATE;
      return;
    }
    rc = voice_adm_->SetSpeakerMute(deafened) == 0 ? PU_OK : PU_ERR_STATE;
  });
  if (rc == PU_OK) voice_deafened_ = deafened;
  Log(3, "voice: deafen=%d rc=%d", deafened ? 1 : 0, rc);
  return rc;
}

// v2.7：把每条还在出声的远端音频轨交给调用方。超过 kAudioTrackActiveMs 没有新帧的
// 轨视为「已离开的冻结记录」，直接过滤——这样对端走了不需要等 transceiver 回调。
int MediaEngine::VoiceTracks(pu_voice_track* out, uint32_t capacity) {
  constexpr int64_t kAudioTrackActiveMs = 1000;
  if (!voice_joined_) return 0;

  const int64_t now_ms = NowUsSteady() / 1000;
  uint32_t written = 0;
  std::lock_guard<std::mutex> lock(audio_tracks_mu_);
  for (const auto& stat : audio_tracks_) {
    const int64_t last_ms = stat->last_frame_ms.load(std::memory_order_relaxed);
    if (last_ms == 0 || now_ms - last_ms > kAudioTrackActiveMs) continue;
    if (out != nullptr && written < capacity) {
      pu_voice_track& item = out[written];
      // struct_size 由调用方给；只按它允许的长度填充，向后兼容加字段。
      const uint32_t limit = item.struct_size == 0 ? static_cast<uint32_t>(sizeof(item))
                                                   : item.struct_size;
      std::memset(&item, 0, limit < sizeof(item) ? limit : sizeof(item));
      item.struct_size = static_cast<uint32_t>(sizeof(item));
      std::snprintf(item.id, sizeof(item.id), "%s", stat->id.c_str());
      item.pcm_frames = stat->frames.load(std::memory_order_relaxed);
      item.peak = stat->peak.load(std::memory_order_relaxed);
      item.last_frame_ms = static_cast<uint64_t>(last_ms);
    }
    ++written;
  }
  return static_cast<int>(written);
}

}  // namespace
}  // namespace partyup

PU_API int pu_voice_join(pu_engine* e, const pu_voice_params* p) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    std::string error;
    return e->core.VoiceJoin(p, &error);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_voice_join exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  }
}

PU_API int pu_voice_leave(pu_engine* e) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  return e->core.VoiceLeave();
}

PU_API int pu_voice_set_mode(pu_engine* e, pu_mode mode) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  return e->core.VoiceSetMode(mode);
}

PU_API int pu_voice_set_mute(pu_engine* e, int muted) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  return e->core.VoiceSetMute(muted != 0);
}

PU_API int pu_voice_set_deafen(pu_engine* e, int deafened) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  return e->core.VoiceSetDeafen(deafened != 0);
}

PU_API int pu_voice_tracks(pu_engine* e, pu_voice_track* out, uint32_t capacity) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  if (out == nullptr || capacity == 0) return e->core.VoiceTracks(nullptr, 0);
  return e->core.VoiceTracks(out, capacity);
}

PU_API int pu_screen_sources(pu_engine* e, pu_source_list* out) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    return e->core.Sources(out);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_screen_sources exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_screen_sources: unknown exception");
    return PU_ERR_INTERNAL;
  }
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
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  try {
    return e->core.AcquireFrame(stream_id, out);
  } catch (const std::exception& ex) {
    e->core.SetLastError(std::string("pu_frame_acquire exception: ") + ex.what());
    return PU_ERR_INTERNAL;
  } catch (...) {
    e->core.SetLastError("pu_frame_acquire: unknown exception");
    return PU_ERR_INTERNAL;
  }
}

PU_API void pu_frame_release(pu_engine* e, pu_frame* f) {
  if (e == nullptr) return;
  try {
    e->core.ReleaseFrame(f);
  } catch (...) {
    e->core.SetLastError("pu_frame_release: unknown exception");
  }
}

}  // extern "C"
