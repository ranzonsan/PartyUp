// PartyUp 媒体核心 · C ABI 第一切片（v1）
//
// 只做一件事：把 spikes/media-link/main.cpp 里验证过的 libwebrtc 用法包成
// 一个动态库，用 6 个 C 函数暴露「创建引擎 → 开一路合成屏幕共享 → 轮询编码事件
// → 停止 → 销毁」。
//
// 初始化顺序完全照抄 spike（顺序本身是踩出来的）：
//   1. webrtc::WinsockInitializer（静态链接会丢掉 WSAStartup，只有 TCP 候选）
//   2. network / worker / signaling 三条 webrtc 线程
//   3. PeerConnectionFactoryDependencies + 自装的 VP8 编/解码器工厂
//   4. EnableMedia(deps)      —— 不调用则 media_factory 为 null，媒体静默不出包
//   5. CreateModularPeerConnectionFactory
// 收尾顺序也照抄：先停采集线程并释放 track（signaling 线程）→ 释放 factory →
// signaling → worker → network 依次 Stop，WinsockInitializer 最后析构。
//
// 与 spike 的差异：本切片没有 PeerConnection（没有信令函数），所以编码器不走
// VideoStreamEncoder，而是直接由 encoder_factory 创建、在采集线程里喂帧。
// PC 接入后这条路径会换成「track → VideoStreamEncoder」，其余不动。
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

// libwebrtc（m152）—— 与 spikes/media-link/main.cpp 同一组头文件
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/media_types.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
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

namespace partyup {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFps = 30;
constexpr int64_t kFrameIntervalUs = 1000000 / kFps;  // 33333us

// 编码码率的统计窗口
constexpr int64_t kRateWindowUs = 1000000;
constexpr uint32_t kDefaultQueueCapacity = 256;

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
// 引擎实现
// ---------------------------------------------------------------------------
class MediaEngine : public webrtc::VideoSinkInterface<webrtc::VideoFrame>,
                    public webrtc::EncodedImageCallback {
 public:
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

  // webrtc::VideoSinkInterface<VideoFrame>：采集线程（pump）上被调用
  void OnFrame(const webrtc::VideoFrame& frame) override;

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
  pu_event MakeEvent(uint32_t type) const;
  void Fail(const std::string& message, std::string* error);
  void ReleaseStreamObjects();
  void Log(int level, const char* format, ...);

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

  // 事件队列（有界环形缓冲的 deque 版）：多生产者（pump/控制线程）、单消费者
  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<pu_event> queue_;
  size_t capacity_ = kDefaultQueueCapacity;
  uint64_t dropped_events_ = 0;

  // 错误串
  std::mutex error_mu_;
  std::string last_error_;

  int log_level_ = 0;
};

MediaEngine::~MediaEngine() {
  Stop();

  // 顺序照抄 spike：track（signaling 线程）→ source → factory → 三条线程 Stop。
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

  running_ = true;
  state_ = PU_STREAM_RUNNING;
  PushState(PU_STREAM_RUNNING, PU_STREAM_STARTING, PU_REASON_REQUESTED);
  Log(3, "screen share started (synthetic %dx%d @%dfps)", kWidth, kHeight, kFps);
  return PU_OK;
}

int MediaEngine::Stop() {
  std::lock_guard<std::mutex> control_lock(control_mu_);

  if (!running_) {
    // 正常路径是幂等返回；但如果 start() 在中途抛异常，采集线程可能已经起来而
    // running_ 还是 false，这里必须把它收掉，否则 std::thread 析构会 terminate。
    if (pump_.joinable()) {
      stop_requested_.store(true, std::memory_order_relaxed);
      pump_.join();
      ReleaseStreamObjects();
    }
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
  // VideoTrack（及其内部 source 代理）必须在 signaling 线程释放。
  if (signaling_ != nullptr) {
    signaling_->BlockingCall([this] { track_ = nullptr; });
  } else {
    track_ = nullptr;
  }
  source_ = nullptr;
}

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

    // 同步回调 OnFrame → encoder->Encode，都在本线程。
    source_->PushFrame(frame);

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
  std::lock_guard<std::mutex> lock(queue_mu_);
  if (queue_.size() >= capacity_) {
    ++dropped_events_;
    // 帧事件丢了就丢了；状态/错误事件更重要，挤掉队首的旧事件腾位置。
    if (event.type == PU_EVENT_FRAME) return;
    queue_.pop_front();
  }
  queue_.push_back(event);
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

  *out = queue_.front();
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

/* ---- 以下为下一版实现：签名冻结，当前只回 PU_NOT_IMPLEMENTED ---- */

PU_API int pu_voice_join(pu_engine* e, const pu_voice_params* p) {
  static_cast<void>(p);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_join: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_leave(pu_engine* e) {
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_leave: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_mode(pu_engine* e, pu_mode mode) {
  static_cast<void>(mode);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_mode: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_mute(pu_engine* e, int muted) {
  static_cast<void>(muted);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_mute: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_voice_set_deafen(pu_engine* e, int deafened) {
  static_cast<void>(deafened);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_voice_set_deafen: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_sources(pu_engine* e, pu_source_list* out) {
  static_cast<void>(out);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_sources: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_watch(pu_engine* e, const char* sharer_id) {
  static_cast<void>(sharer_id);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_watch: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_screen_unwatch(pu_engine* e, const char* sharer_id) {
  static_cast<void>(sharer_id);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_screen_unwatch: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API int pu_frame_acquire(pu_engine* e, const char* stream_id, pu_frame* out) {
  static_cast<void>(stream_id);
  static_cast<void>(out);
  if (e == nullptr) return PU_ERR_INVALID_ARG;
  e->core.SetLastError("pu_frame_acquire: not implemented in ABI v1");
  return PU_NOT_IMPLEMENTED;
}

PU_API void pu_frame_release(pu_engine* e, pu_frame* f) {
  static_cast<void>(f);
  if (e == nullptr) return;
  e->core.SetLastError("pu_frame_release: not implemented in ABI v1");
}

}  // extern "C"
