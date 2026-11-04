// PartyUp 媒体核心 · 硬件 H.264 编解码（Windows Media Foundation MFT）实现。
//
// 设计要点与踩过的坑都写在 hw_encoder.h 的文件头，这里只补实现层面的三件事：
//
//   1. **编/解码各跑在自己的线程上**。MFT 的创建、类型协商、ProcessInput/
//      ProcessOutput、销毁全部发生在该线程（COM 也是该线程初始化的 MTA），
//      libwebrtc 的调用线程只入队/取结果。好处：NVENC 的 ~11 ms 编码延迟不会
//      堵住 WebRTC 的编码队列；收尾顺序是确定的（先停线程、再释放 MFT）。
//   2. **异步 MFT 的协议**：ProcessInput 能在 MFT 缓冲满时回 MF_E_NOTACCEPTING，
//      ProcessOutput 必须等到 METransformHaveOutput 才能收（否则 E_UNEXPECTED）。
//      所以线程循环是「收事件 → 有帧就推 → 有 HaveOutput 就收一个」。
//   3. **不自己解析码流**：关键帧判定用输出样本上的 MFSampleExtension_CleanPoint，
//      关键帧请求用 ICodecAPI 的 CODECAPI_AVEncVideoForceKeyFrame；H.264 的
//      NAL 拆分与 SDP 协商交给 libwebrtc 的 rtp_format_h264（第三方实现）。
#include "hw_encoder.h"

#include <windows.h>

#include <codecapi.h>
#include <d3d11.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/environment/environment.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "modules/video_coding/codecs/h264/include/h264.h"  // CreateH264Format
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"  // WEBRTC_VIDEO_CODEC_*
#include "libyuv/convert.h"

using Microsoft::WRL::ComPtr;

namespace partyup {
namespace {

constexpr int kMaxQueuedFrames = 6;   // 编码器入口队列深度（超过丢最旧）
constexpr int kMaxInFlightInputs = 4; // 同时交给 MFT、还没取回输出的帧数上限

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 单调时钟微秒：只用于测耗时（不受系统时间调整影响）。
int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* HrName(HRESULT hr) {
  switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_UNEXPECTED: return "E_UNEXPECTED";
    case E_ACCESSDENIED: return "E_ACCESSDENIED";
    case MF_E_TRANSFORM_ASYNC_LOCKED: return "MF_E_TRANSFORM_ASYNC_LOCKED";
    case MF_E_TRANSFORM_NEED_MORE_INPUT: return "MF_E_TRANSFORM_NEED_MORE_INPUT";
    case MF_E_TRANSFORM_STREAM_CHANGE: return "MF_E_TRANSFORM_STREAM_CHANGE";
    case MF_E_NOTACCEPTING: return "MF_E_NOTACCEPTING";
    case MF_E_INVALIDMEDIATYPE: return "MF_E_INVALIDMEDIATYPE";
    case MF_E_ATTRIBUTENOTFOUND: return "MF_E_ATTRIBUTENOTFOUND";
    case MF_E_UNSUPPORTED_D3D_TYPE: return "MF_E_UNSUPPORTED_D3D_TYPE";
    case MF_E_TOPO_CODEC_NOT_FOUND: return "MF_E_TOPO_CODEC_NOT_FOUND";
    default: return "?";
  }
}

// 硬件 MFT 的 async 属性需要先解锁，否则连 MFT_MESSAGE_SET_D3D_MANAGER 都会
// 回 0xC00D6D77。这里把「读属性 + 解锁」封成一处，避免漏设。
void UnlockAsyncMft(IMFTransform* mft) {
  ComPtr<IMFAttributes> attributes;
  if (FAILED(mft->GetAttributes(&attributes)) || attributes == nullptr) return;
  UINT32 async = 0;
  attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
  if (async != 0) attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
}

std::string FriendlyNameOf(IMFActivate* activate) {
  LPWSTR wide = nullptr;
  UINT32 length = 0;
  if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wide, &length)) ||
      wide == nullptr) {
    return "<unnamed>";
  }
  char utf8[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
  CoTaskMemFree(wide);
  return utf8;
}

// MF 的 H.264 编码器 MFT 输出类型。分辨率/帧率/码率由 InitEncode/SetRates 决定。
bool ConfigureEncoderOutputType(IMFTransform* mft, int width, int height, int fps,
                                uint32_t bitrate_bps, std::string* error) {
  ComPtr<IMFMediaType> output;
  HRESULT hr = MFCreateMediaType(&output);
  if (FAILED(hr)) { *error = "MFCreateMediaType(out) failed"; return false; }
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                     static_cast<UINT32>(height));
  MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::max(1, fps)), 1);
  MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
  output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  // WebRTC 的 H.264 SDP 声明的是 constrained baseline；这里保持同一档，避免
  // 协商出去的 profile-level-id 与实际编码等级不一致。
  output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
  output->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(std::max(1, fps) * 2));
  hr = mft->SetOutputType(0, output.Get(), 0);
  if (FAILED(hr)) {
    // 有的驱动不接受 LEVEL/PROFILE 组合：退到最小集合再试一次。
    ComPtr<IMFMediaType> minimal;
    if (FAILED(MFCreateMediaType(&minimal))) {
      *error = std::string("SetOutputType(H264) failed: ") + HrName(hr);
      return false;
    }
    minimal->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    minimal->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(minimal.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(minimal.Get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(std::max(1, fps)), 1);
    minimal->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
    const HRESULT retry = mft->SetOutputType(0, minimal.Get(), 0);
    if (FAILED(retry)) {
      *error = std::string("SetOutputType(H264) failed: ") + HrName(retry);
      return false;
    }
  }
  return true;
}

bool ConfigureEncoderInputType(IMFTransform* mft, int width, int height, int fps) {
  ComPtr<IMFMediaType> input;
  if (FAILED(MFCreateMediaType(&input))) return false;
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                     static_cast<UINT32>(height));
  MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::max(1, fps)), 1);
  MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  input->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
  return SUCCEEDED(mft->SetInputType(0, input.Get(), 0));
}

// 系统自带的软件 H.264 编码 MFT（"H264 Encoder MFT"）。枚举里还会排到
// "Microsoft AVC DX12 Encoder"，那个要 D3D12 设备，不能当通用软件回退，
// 所以软件路径直接用 CLSID 创建这一个。
const CLSID kSoftwareH264EncoderMft = {
    0x6CA50344, 0x051A, 0x4DED, {0x97, 0x79, 0xA4, 0x33, 0x05, 0x16, 0x5E, 0x35}};

// 打开一个 H.264 编码 MFT：先硬件、再系统软件 MFT。返回 nullptr 表示都不可用。
ComPtr<IMFTransform> OpenEncoderMft(bool prefer_hardware, int width, int height, int fps,
                                    uint32_t bitrate_bps, bool* used_hardware,
                                    std::string* name, std::string* error) {
  if (prefer_hardware) {
    MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT enum_hr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                  &out_info, &activates, &count);
    if (SUCCEEDED(enum_hr) && count > 0) {
      for (UINT32 i = 0; i < count; ++i) {
        IMFActivate* activate = activates[i];
        ComPtr<IMFTransform> mft;
        if (FAILED(activate->ActivateObject(__uuidof(IMFTransform), (void**)&mft)) ||
            mft == nullptr) {
          continue;
        }
        UnlockAsyncMft(mft.Get());
        if (!ConfigureEncoderOutputType(mft.Get(), width, height, fps, bitrate_bps, error) ||
            !ConfigureEncoderInputType(mft.Get(), width, height, fps)) {
          continue;
        }
        *used_hardware = true;
        *name = FriendlyNameOf(activate);
        for (UINT32 j = 0; j < count; ++j) activates[j]->Release();
        CoTaskMemFree(activates);
        return mft;
      }
      for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
      CoTaskMemFree(activates);
    } else if (activates != nullptr) {
      CoTaskMemFree(activates);
    }
  }

  // 软件回退：系统 H.264 编码 MFT。
  ComPtr<IMFTransform> software;
  if (SUCCEEDED(CoCreateInstance(kSoftwareH264EncoderMft, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&software))) &&
      software != nullptr) {
    UnlockAsyncMft(software.Get());
    if (ConfigureEncoderOutputType(software.Get(), width, height, fps, bitrate_bps, error) &&
        ConfigureEncoderInputType(software.Get(), width, height, fps)) {
      *used_hardware = false;
      *name = "H264 Encoder MFT (system software)";
      return software;
    }
  }
  if (error->empty()) *error = "no usable H.264 encoder MFT";
  return nullptr;
}

std::atomic<int> g_last_encoder_kind{static_cast<int>(H264EncoderKind::kNone)};

}  // namespace

// ---------------------------------------------------------------------------
// H.264 编码器可用性探测
// ---------------------------------------------------------------------------

namespace {

H264Availability ProbeAvailability() {
  H264Availability availability;
  // 1) 硬件：只枚举 MFT_ENUM_FLAG_HARDWARE，并且「能激活 + 能配出类型」才算可用。
  //    本机的 Intel QSV MFT 会枚举出来但 ActivateObject 失败（没有集显），
  //    只看枚举数量会误判。
  {
    MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                  &out_info, &activates, &count);
    if (FAILED(hr)) {
      availability.error = std::string("MFTEnumEx(hardware) failed: ") + HrName(hr);
    } else {
      // 1280x720@30 是 H.264 各档都支持的最小档，用来验证「能协商出类型」。
      for (UINT32 i = 0; i < count && !availability.hardware; ++i) {
        ComPtr<IMFTransform> mft;
        if (FAILED(activates[i]->ActivateObject(__uuidof(IMFTransform), (void**)&mft)) ||
            mft == nullptr) {
          continue;
        }
        UnlockAsyncMft(mft.Get());
        std::string probe_error;
        if (ConfigureEncoderOutputType(mft.Get(), 1280, 720, 30, 2'000'000, &probe_error) &&
            ConfigureEncoderInputType(mft.Get(), 1280, 720, 30)) {
          availability.hardware = true;
          availability.hardware_name = FriendlyNameOf(activates[i]);
        }
      }
      for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
      CoTaskMemFree(activates);
    }
  }
  // 2) 软件：系统 H.264 编码 MFT 的 CLSID。
  {
    ComPtr<IMFTransform> software;
    if (SUCCEEDED(CoCreateInstance(kSoftwareH264EncoderMft, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&software))) &&
        software != nullptr) {
      UnlockAsyncMft(software.Get());
      std::string probe_error;
      if (ConfigureEncoderOutputType(software.Get(), 1280, 720, 30, 2'000'000, &probe_error) &&
          ConfigureEncoderInputType(software.Get(), 1280, 720, 30)) {
        availability.software = true;
        availability.software_name = "H264 Encoder MFT (system software)";
      }
    }
  }
  if (!availability.hardware && !availability.software) {
    availability.error = "no usable H.264 encoder MFT (hardware or software)";
  } else if (!availability.hardware) {
    availability.error = "no hardware H.264 MFT; fell back to the system software MFT";
  }
  return availability;
}

}  // namespace

const H264Availability& QueryH264Availability() {
  static const H264Availability availability = [] {
    // 探测放到一次性线程里做：MFT 的激活/配置要求 COM 已初始化，而调用方
    // （pu_engine_create 的控制线程）的 COM 状态不受我们控制。
    H264Availability result;
    std::thread probe([&result] {
      const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      result = ProbeAvailability();
      if (SUCCEEDED(co_hr)) CoUninitialize();
    });
    probe.join();
    return result;
  }();
  return availability;
}

bool HasH264Encoder() {
  const H264Availability& availability = QueryH264Availability();
  return availability.hardware || availability.software;
}

bool ForceVp8ForTesting() {
  // 只用于测量/对照：设 PARTYUP_MEDIA_FORCE_VP8=1 时工厂不声明 H.264，
  // 协商自然只剩 VP8 —— 既能拿「同为 1440p 的软件 VP8 基线 CPU」做对比，
  // 也是「H.264 不可用时回落 VP8」这条路径的实测入口。生产默认不设此变量。
  static const bool forced = [] {
    const char* value = std::getenv("PARTYUP_MEDIA_FORCE_VP8");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return forced;
}

H264EncoderKind LastCreatedEncoderKind() {
  return static_cast<H264EncoderKind>(g_last_encoder_kind.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// MfH264Encoder：MF 的 H.264 编码 MFT 包成 webrtc::VideoEncoder
// ---------------------------------------------------------------------------

namespace {

// 一帧待编码的输入 + 输出需要回填给 WebRTC 的元数据。
// webrtc::VideoFrame 没有默认构造函数，所以这里必须显式构造。
struct PendingFrame {
  explicit PendingFrame(const webrtc::VideoFrame& f) : frame(f) {}
  PendingFrame(PendingFrame&&) = default;
  PendingFrame& operator=(PendingFrame&&) = default;

  webrtc::VideoFrame frame;
  bool keyframe = false;
  int64_t enqueue_us = 0;
};

// 已推给 MFT、还没取回输出的帧的元数据（1 进 1 出，按 FIFO 对应）。
struct InFlightMeta {
  uint32_t rtp_timestamp = 0;
  int64_t ntp_time_ms = 0;
  int64_t capture_time_ms = 0;
  int64_t encode_start_ms = 0;
};

class MfH264Encoder final : public webrtc::VideoEncoder {
 public:
  MfH264Encoder() = default;
  ~MfH264Encoder() override { Release(); }

  MfH264Encoder(const MfH264Encoder&) = delete;
  MfH264Encoder& operator=(const MfH264Encoder&) = delete;

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) override;
  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

 private:
  // 工作线程主循环：收事件 → 推帧 → 收包 → 投递回调。
  void WorkerLoop();
  bool OpenOnWorkerThread();
  // 推一帧给 MFT：kNotAccepting = MFT 缓冲满（下一轮重试），kError = 硬失败。
  enum class PushResult { kPushed, kNotAccepting, kError };
  PushResult PushFrameToMft(const PendingFrame& pending);
  bool CollectOutputSamples(bool* got);
  void ApplyPendingRates();
  void CloseOnWorkerThread();

  // 由调用线程持有：callback / 队列 / 停止标志。
  std::mutex mutex_;
  std::condition_variable frame_cv_;
  std::condition_variable setup_cv_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  std::deque<PendingFrame> pending_;
  bool stop_ = false;
  bool setup_done_ = false;
  bool setup_ok_ = false;

  // 待应用的码率/帧率（SetRates 在调用线程，MFT 在工作线程）。
  std::mutex rate_mutex_;
  int requested_bitrate_bps_ = 0;
  int requested_fps_ = 0;
  bool rates_dirty_ = false;

  std::thread worker_;
  bool worker_started_ = false;

  // 以下只在工作线程上访问。
  ComPtr<IMFTransform> mft_;
  ComPtr<IMFMediaEventGenerator> event_generator_;
  ComPtr<ID3D11Device> d3d_device_;
  ComPtr<ID3D11DeviceContext> d3d_context_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT32 reset_token_ = 0;
  ComPtr<ICodecAPI> codec_api_;
  bool async_ = false;
  bool hardware_ = false;
  // 这三个字段在 InitEncode（调用线程）写、之后只读，用 atomic 让 Encode() 与工作
  // 线程无锁读取也不构成数据竞争。
  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<size_t> nv12_size_{0};
  int fps_ = 30;
  uint32_t bitrate_bps_ = 2'000'000;
  // 已推给 MFT 但还没取回输出的帧（FIFO，1 进 1 出）。
  std::deque<InFlightMeta> in_flight_;
  int64_t encode_failures_ = 0;
  // 上一次真正下发的码率与时刻（SetRates 限流用，见 ApplyPendingRates）。
  int64_t last_rate_apply_ms_ = 0;
  // 上一次真正下发成功的帧率上限（CODECAPI_AVEncMaxFrameRate，v2.13）。0 = 还没成功。
  int last_fps_applied_ = 0;
  // 这个驱动是否接受改帧率上限。S_FALSE/失败后置 false，不再每 250 ms 重试（只报一次）。
  bool fps_property_supported_ = true;
  // Release() 会被 libwebrtc 和析构各调一次；取证日志只打一次。
  bool release_reported_ = false;

  // ---- 吞吐取证计数器（Release 时打印一行，用来定位「编码数 < 推送数」）----
  // 这些不是给产品用的统计，是排查工具：没有它们只能猜瓶颈。
  int64_t encode_calls_ = 0;      // WebRTC 调 Encode() 的次数
  int64_t enqueue_drops_ = 0;     // 队列满、丢最旧的次数
  int64_t frames_pushed_ = 0;     // 成功交给 MFT 的帧数
  int64_t frames_out_ = 0;        // 从 MFT 收回的包数
  int64_t callback_us_ = 0;       // 投递 OnEncodedImage 的总耗时（微秒）
  int64_t push_us_ = 0;           // PushFrameToMft（含 I420→NV12 转换）的总耗时
  // ---- v2.13 新增的两个取证计数器 ----
  // 这两个数是「libwebrtc 想改分辨率、而我们改不了」这条路的探针：
  // 正常稳态（尺寸不变）应恒为 0；非 0 就说明降分辨率路径被触发过。
  int64_t size_mismatch_frames_ = 0;  // Encode 因尺寸不符被拒的帧数
  int64_t ignored_reconfigs_ = 0;     // InitEncode 在已运行时被再次调用且被忽略的次数
};

int MfH264Encoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                              const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (codec_settings == nullptr) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  if (codec_settings->codecType != webrtc::kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_started_) {
      // v2.13：libwebrtc 的 quality scaler / 降分辨率一旦触发，会**再调一次 InitEncode**
      // 带上更小的尺寸。MFT 的类型协商是静态的，这里没法原地改尺寸，于是只能忽略——
      // 而随后的 Encode() 会因为尺寸不符一路返回 ERR_PARAMETER（上层表现是画面卡死）。
      // 原来这里是静默 return，等于把这个地雷埋掉了；现在留一条日志当探针。
      const int wanted_w = codec_settings->width;
      const int wanted_h = codec_settings->height;
      if (wanted_w != width_.load() || wanted_h != height_.load()) {
        ++ignored_reconfigs_;
        std::fprintf(stderr,
                     "[partyup-media] h264 encoder: IGNORING re-init %dx%d (running %dx%d) -- "
                     "subsequent frames will be REJECTED (ignored_reconfigs=%lld)\n",
                     wanted_w, wanted_h, width_.load(), height_.load(),
                     static_cast<long long>(ignored_reconfigs_));
      }
      return WEBRTC_VIDEO_CODEC_OK;  // 重配走 Encode 内的尺寸检查
    }
    width_ = codec_settings->width;
    height_ = codec_settings->height;
    fps_ = codec_settings->maxFramerate > 0 ? static_cast<int>(codec_settings->maxFramerate) : 30;
    bitrate_bps_ = codec_settings->startBitrate > 0
                       ? static_cast<uint32_t>(codec_settings->startBitrate) * 1000u
                       : 2'000'000u;
    stop_ = false;
    setup_done_ = false;
    setup_ok_ = false;
  }
  {
    std::lock_guard<std::mutex> rate_lock(rate_mutex_);
    requested_bitrate_bps_ = static_cast<int>(bitrate_bps_);
    requested_fps_ = fps_;
  }
  worker_ = std::thread([this] { WorkerLoop(); });
  worker_started_ = true;

  std::unique_lock<std::mutex> lock(mutex_);
  setup_cv_.wait(lock, [this] { return setup_done_; });
  if (!setup_ok_) {
    lock.unlock();
    Release();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Encoder::Release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    callback_ = nullptr;
    pending_.clear();
  }
  frame_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  worker_started_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_.clear();
  }
  // 吞吐取证：encode_calls（WebRTC 真正交给我们的帧数）与 frames_out（真正出去的包数）
  // 一比，就能判断「丢帧」发生在我们内部还是 WebRTC 上游（帧丢弃器/管线）。
  // libwebrtc 会先 Release() 再析构（析构里又调一次 Release），所以只打一次。
  if (!release_reported_) {
    release_reported_ = true;
    std::fprintf(stderr,
                 "[partyup-media] h264 encoder: closed; encode_calls=%lld "
                 "enqueue_drops=%lld pushed=%lld out=%lld push_avg_us=%lld "
                 "callback_avg_us=%lld size_mismatch=%lld ignored_reinit=%lld fps_last=%d\n",
                 static_cast<long long>(encode_calls_),
                 static_cast<long long>(enqueue_drops_),
                 static_cast<long long>(frames_pushed_),
                 static_cast<long long>(frames_out_),
                 static_cast<long long>(frames_pushed_ > 0 ? push_us_ / frames_pushed_ : 0),
                 static_cast<long long>(frames_out_ > 0 ? callback_us_ / frames_out_ : 0),
                 static_cast<long long>(size_mismatch_frames_),
                 static_cast<long long>(ignored_reconfigs_), last_fps_applied_);
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Encoder::Encode(const webrtc::VideoFrame& frame,
                              const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (frame.video_frame_buffer() == nullptr) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  // 分辨率变化：MFT 的类型协商是静态的，这里直接拒绝（上层会重建编码器）。
  const int frame_width = frame.width();
  const int frame_height = frame.height();
  if (frame_width != width_.load() || frame_height != height_.load()) {
    // v2.13：不再静默拒绝。这条日志 + Release 时的计数是判断「降分辨率路是否被打通」
    // 的唯一证据（预期：正常稳态下恒为 0）。
    ++size_mismatch_frames_;
    if (size_mismatch_frames_ <= 3 || size_mismatch_frames_ % 100 == 0) {
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: REJECT frame %dx%d (encoder is %dx%d) "
                   "count=%lld\n",
                   frame_width, frame_height, width_.load(), height_.load(),
                   static_cast<long long>(size_mismatch_frames_));
    }
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  PendingFrame pending(frame);
  pending.enqueue_us = frame.timestamp_us();
  pending.keyframe = false;
  if (frame_types != nullptr && !frame_types->empty()) {
    pending.keyframe = (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    ++encode_calls_;
    if (pending_.size() >= static_cast<size_t>(kMaxQueuedFrames)) {
      pending_.pop_front();  // 编码器跟不上：丢最旧的，不阻塞采集线程
      ++enqueue_drops_;
    }
    pending_.push_back(std::move(pending));
  }
  frame_cv_.notify_one();
  return WEBRTC_VIDEO_CODEC_OK;
}

void MfH264Encoder::SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) {
  std::lock_guard<std::mutex> lock(rate_mutex_);
  const int bitrate_bps = static_cast<int>(parameters.target_bitrate.get_sum_bps());
  if (bitrate_bps > 0) requested_bitrate_bps_ = bitrate_bps;
  if (parameters.framerate_fps > 0.5) requested_fps_ = static_cast<int>(parameters.framerate_fps);
  rates_dirty_ = true;
}

webrtc::VideoEncoder::EncoderInfo MfH264Encoder::GetEncoderInfo() const {
  webrtc::VideoEncoder::EncoderInfo info;
  info.implementation_name = hardware_ ? "partyup-mf-h264-hw" : "partyup-mf-h264-sw";
  info.requested_resolution_alignment = 2;  // NV12 是 4:2:0，宽高必须偶数
  info.supports_native_handle = false;      // 输入是 I420（采集侧已转码）
  info.is_hardware_accelerated = hardware_;
  info.has_trusted_rate_controller = false;
  return info;
}

bool MfH264Encoder::OpenOnWorkerThread() {
  // 1. D3D11 设备 + DXGI device manager（硬件 MFT 必需；软件 MFT 用不到，
  //    但创建成本只有几毫秒，统一建出来让硬件路径可用）。
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
  const HRESULT device_hr =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_device_,
                        &chosen, &d3d_context_);
  if (SUCCEEDED(device_hr)) {
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(d3d_device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&reset_token_, &device_manager_)) &&
        device_manager_ != nullptr) {
      if (FAILED(device_manager_->ResetDevice(d3d_device_.Get(), reset_token_))) {
        device_manager_.Reset();
      }
    }
  }

  const H264Availability& availability = QueryH264Availability();
  const bool prefer_hardware = availability.hardware;
  std::string name;
  std::string error;
  bool used_hardware = false;
  mft_ = OpenEncoderMft(prefer_hardware, width_, height_, fps_, bitrate_bps_,
                        &used_hardware, &name, &error);
  if (mft_ == nullptr) return false;
  hardware_ = used_hardware;
  g_last_encoder_kind.store(static_cast<int>(hardware_ ? H264EncoderKind::kHardwareMft
                                                       : H264EncoderKind::kSoftwareMft),
                            std::memory_order_relaxed);

  if (hardware_ && device_manager_ != nullptr) {
    const HRESULT dm_hr = mft_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<UINT_PTR>(device_manager_.Get()));
    if (FAILED(dm_hr)) {
      // 拿不到 D3D manager 也能跑（系统内存 NV12 输入），只是少了 GPU 直通。
      std::fprintf(stderr, "[partyup-media] h264 encoder: SET_D3D_MANAGER failed 0x%08X\n",
                   static_cast<unsigned>(dm_hr));
    }
  }
  mft_->QueryInterface(IID_PPV_ARGS(&codec_api_));
  UnlockAsyncMft(mft_.Get());
  mft_->QueryInterface(IID_PPV_ARGS(&event_generator_));
  async_ = event_generator_ != nullptr;

  // v2.13：显式把 rate-control mode 设成**合法枚举**。
  // 历史 bug：ApplyPendingRates 曾把 fps 数值（30/60）写进这个属性——那是
  // CBR/PeakConstrainedVBR/UnconstrainedVBR/Quality 的枚举，等于每次收到 SetRates
  // 就把 rate-control mode 破坏一次，而且从未改到帧率。
  // 屏幕共享取 UnconstrainedVBR：允许按内容瞬时拉高码率（文字/滚动更友好），
  // 不套 CBR 的漏桶约束。失败不致命（驱动默认值仍生效），但必须留证。
  if (codec_api_ != nullptr) {
    VARIANT rc_value;
    VariantInit(&rc_value);
    rc_value.vt = VT_UI4;
    rc_value.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
    const HRESULT rc_hr =
        codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rc_value);
    VariantClear(&rc_value);
    if (rc_hr == S_OK) {
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: rate control mode = UnconstrainedVBR\n");
    } else if (rc_hr == S_FALSE) {
      // S_FALSE 不是失败，是「属性只读」。它同样意味着我们的意图没落地，必须报出来。
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: rate control mode is READ-ONLY (S_FALSE); "
                   "driver default stays\n");
    } else {
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: set rate control mode=UnconstrainedVBR "
                   "FAILED hr=0x%08X (driver default stays)\n",
                   static_cast<unsigned>(rc_hr));
    }
  }

  // 2. 事件流开始。之后的编码循环按 NeedInput/HaveOutput 驱动。
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  nv12_size_ = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3 / 2;
  std::fprintf(stderr,
               "[partyup-media] h264 encoder ready: %s (%s) %dx%d @%d, bitrate=%u bps, "
               "async=%d\n",
               name.c_str(), hardware_ ? "hardware MFT" : "software MFT", width_.load(),
               height_.load(), fps_, bitrate_bps_, async_ ? 1 : 0);
  return true;
}

void MfH264Encoder::CloseOnWorkerThread() {
  if (mft_ != nullptr) {
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  codec_api_.Reset();
  event_generator_.Reset();
  mft_.Reset();
  device_manager_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
}

void MfH264Encoder::ApplyPendingRates() {
  int bitrate_bps = 0;
  int fps = 0;
  {
    std::lock_guard<std::mutex> lock(rate_mutex_);
    if (!rates_dirty_) return;
    rates_dirty_ = false;
    bitrate_bps = requested_bitrate_bps_;
    fps = requested_fps_;
  }
  if (codec_api_ == nullptr) return;
  // 限流：WebRTC 的 SetRates 在拥塞控制下可能每帧都调一次，而 NVENC/Intel 的
  // ICodecAPI 重配是数十毫秒级的重活——每帧重配会把编码线程堵死、编码数掉到
  // 推送数的 0.67x（实测）。所以只在「码率变化超过 15%」或「帧率变了」且距上次
  // 下发 >= 250 ms 时才动 MFT。
  const int64_t now_ms = NowMs();
  const int previous_bitrate = static_cast<int>(bitrate_bps_);
  const bool bitrate_due =
      bitrate_bps > 0 &&
      (previous_bitrate <= 0 ||
       std::abs(static_cast<long long>(bitrate_bps) - previous_bitrate) * 100LL >
           static_cast<long long>(previous_bitrate) * 15LL);
  // v2.13：帧率**不再**写进 CODECAPI_AVEncCommonRateControlMode（那是
  // CBR/PeakConstrainedVBR/UnconstrainedVBR/Quality 的枚举，把 30/60 当枚举值写
  // 进去既改了错误的属性、也从来没改到帧率，还会把 rate-control mode 写坏）。
  // 正确的属性是 CODECAPI_AVEncMaxFrameRate：VT_UI8，高 32 位分子、低 32 位分母。
  // 抖动容差：libwebrtc 报的 framerate_fps 是「实测输入帧率」，在 55–60 之间来回跳。
  // 差 1–2 fps 就重配一次 MFT 没有意义，只会每 250 ms 打一次驱动；变化 >= 10% 才下发。
  const bool fps_due =
      fps_property_supported_ && fps > 0 &&
      (last_fps_applied_ == 0 ||
       std::abs(fps - last_fps_applied_) * 10 >= last_fps_applied_);
  if (!bitrate_due && !fps_due) return;
  if (now_ms - last_rate_apply_ms_ < 250) return;

  bool applied = false;
  if (bitrate_due) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = static_cast<ULONG>(bitrate_bps);
    const HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
    VariantClear(&value);
    if (FAILED(hr)) {
      // 不静默：失败要留证，且保留上一次成功下发的值（bitrate_bps_ 不动）。
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: set mean bitrate %d bps FAILED hr=0x%08X "
                   "(keeping %u bps)\n",
                   bitrate_bps, static_cast<unsigned>(hr), bitrate_bps_);
    } else {
      bitrate_bps_ = static_cast<uint32_t>(bitrate_bps);
      applied = true;
    }
  }
  if (fps_due) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI8;
    value.ullVal = (static_cast<ULONGLONG>(fps) << 32) | 1ULL;  // fps / 1
    const HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncMaxFrameRate, &value);
    VariantClear(&value);
    if (hr == S_OK) {
      last_fps_applied_ = fps;
      applied = true;
      std::fprintf(stderr, "[partyup-media] h264 encoder: max frame rate -> %d\n", fps);
    } else {
      // S_FALSE = 属性只读；其它 = 驱动不支持。两种情况都**不能每 250 ms 重试**：
      // 记一次、报一次，并说清「帧率仍然由 libwebrtc 在编码器上游节流」。
      fps_property_supported_ = false;
      std::fprintf(stderr,
                   "[partyup-media] h264 encoder: set max frame rate %d NOT APPLIED "
                   "(hr=0x%08X%s); frame pacing stays upstream of the encoder\n",
                   fps, static_cast<unsigned>(hr),
                   hr == S_FALSE ? " S_FALSE=read-only" : "");
    }
  }
  if (applied) last_rate_apply_ms_ = now_ms;
}

MfH264Encoder::PushResult MfH264Encoder::PushFrameToMft(const PendingFrame& pending) {
  const webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = pending.frame.video_frame_buffer();
  const webrtc::scoped_refptr<const webrtc::I420BufferInterface> i420 = buffer->ToI420();
  if (i420 == nullptr) return PushResult::kError;

  ComPtr<IMFMediaBuffer> mf_buffer;
  if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_size_), &mf_buffer)) ||
      mf_buffer == nullptr) {
    return PushResult::kError;
  }
  BYTE* destination = nullptr;
  DWORD max_length = 0;
  if (FAILED(mf_buffer->Lock(&destination, &max_length, nullptr))) return PushResult::kError;
  const int converted = libyuv::I420ToNV12(
      i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
      i420->StrideV(), destination, width_, destination + static_cast<size_t>(width_) * height_,
      width_, width_, height_);
  mf_buffer->Unlock();
  if (converted != 0) return PushResult::kError;
  mf_buffer->SetCurrentLength(static_cast<DWORD>(nv12_size_));

  ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample)) || sample == nullptr) return PushResult::kError;
  sample->AddBuffer(mf_buffer.Get());
  const LONGLONG timestamp_100ns = static_cast<LONGLONG>(pending.frame.timestamp_us()) * 10;
  const LONGLONG duration_100ns = fps_ > 0 ? 10'000'000LL / fps_ : 333'333LL;
  sample->SetSampleTime(timestamp_100ns);
  sample->SetSampleDuration(duration_100ns);
  if (pending.keyframe && codec_api_ != nullptr) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
    VariantClear(&value);
  }

  const HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) return PushResult::kNotAccepting;
  if (FAILED(hr)) {
    ++encode_failures_;
    if (encode_failures_ < 5) {
      std::fprintf(stderr, "[partyup-media] h264 encoder ProcessInput failed 0x%08X %s\n",
                   static_cast<unsigned>(hr), HrName(hr));
    }
    return PushResult::kError;
  }
  InFlightMeta meta;
  meta.rtp_timestamp = pending.frame.rtp_timestamp();
  meta.ntp_time_ms = pending.frame.timestamp_us() / 1000;
  meta.capture_time_ms = pending.frame.timestamp_us() / 1000;
  meta.encode_start_ms = pending.enqueue_us / 1000;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_.push_back(meta);
  }
  return PushResult::kPushed;
}

bool MfH264Encoder::CollectOutputSamples(bool* got) {
  *got = false;
  MFT_OUTPUT_STREAM_INFO info = {};
  mft_->GetOutputStreamInfo(0, &info);
  MFT_OUTPUT_DATA_BUFFER out_buffer = {};
  out_buffer.dwStreamID = 0;
  ComPtr<IMFSample> allocated;
  if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
    if (info.cbSize == 0) return true;
    if (FAILED(MFCreateSample(&allocated))) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(info.cbSize, &buffer))) return false;
    allocated->AddBuffer(buffer.Get());
    out_buffer.pSample = allocated.Get();
  }
  DWORD status = 0;
  const HRESULT hr = mft_->ProcessOutput(0, 1, &out_buffer, &status);
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    return true;
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // 编码器要求换输出类型（例如驱动换了 profile/尺寸）：按当前可用类型重设。
    ComPtr<IMFMediaType> available;
    if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &available))) {
      mft_->SetOutputType(0, available.Get(), 0);
    }
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    return true;
  }
  if (FAILED(hr)) {
    ++encode_failures_;
    if (encode_failures_ < 5) {
      std::fprintf(stderr, "[partyup-media] h264 encoder ProcessOutput failed 0x%08X %s\n",
                   static_cast<unsigned>(hr), HrName(hr));
    }
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
    return false;
  }

  IMFSample* delivered = out_buffer.pSample;
  if (delivered != nullptr) {
    UINT32 clean_point = 0;
    const bool keyframe =
        SUCCEEDED(delivered->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) &&
        clean_point != 0;
    // 1 进 1 出：把这一包对应的输入元数据取出来（RTP 时间戳要原样回填）。
    InFlightMeta meta;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!in_flight_.empty()) {
        meta = in_flight_.front();
        in_flight_.pop_front();
      }
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (SUCCEEDED(delivered->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr) {
      BYTE* data = nullptr;
      DWORD max_length = 0;
      DWORD current_length = 0;
      if (SUCCEEDED(buffer->Lock(&data, &max_length, &current_length)) && current_length > 0) {
        // EncodedImageBuffer::Create(data, size) 会**复制**一份；回调期间不能
        // 依赖 MFT 的输出缓冲还锁着，所以先复制再解锁。
        webrtc::scoped_refptr<webrtc::EncodedImageBuffer> encoded =
            webrtc::EncodedImageBuffer::Create(data, current_length);
        buffer->Unlock();
        std::lock_guard<std::mutex> lock(mutex_);
        if (callback_ != nullptr) {
          webrtc::EncodedImage image;
          image.SetEncodedData(encoded);
          image._encodedWidth = static_cast<uint32_t>(width_);
          image._encodedHeight = static_cast<uint32_t>(height_);
          image.SetFrameType(keyframe ? webrtc::VideoFrameType::kVideoFrameKey
                                      : webrtc::VideoFrameType::kVideoFrameDelta);
          image.SetRtpTimestamp(meta.rtp_timestamp);
          image.ntp_time_ms_ = meta.ntp_time_ms;
          image.capture_time_ms_ = meta.capture_time_ms;
          image.SetEncodeTime(meta.encode_start_ms, NowMs());
          webrtc::CodecSpecificInfo codec_info;
          codec_info.codecType = webrtc::kVideoCodecH264;
          codec_info.codecSpecific.H264.packetization_mode =
              webrtc::H264PacketizationMode::NonInterleaved;
          codec_info.codecSpecific.H264.temporal_idx = 0;  // 单 temporal layer
          codec_info.codecSpecific.H264.idr_frame = keyframe;
          codec_info.codecSpecific.H264.base_layer_sync = false;
          const int64_t callback_start_us = NowUs();
          callback_->OnEncodedImage(image, &codec_info);
          callback_us_ += NowUs() - callback_start_us;
          ++frames_out_;
          *got = true;
        }
      } else {
        buffer->Unlock();
      }
    }
  }
  if (delivered != nullptr && delivered != allocated.Get()) delivered->Release();
  if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
  return true;
}

void MfH264Encoder::WorkerLoop() {
  const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_owned = SUCCEEDED(co_hr);
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_) ok = OpenOnWorkerThread();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    setup_ok_ = ok;
    setup_done_ = true;
  }
  setup_cv_.notify_all();
  if (!ok) {
    if (com_owned) CoUninitialize();
    return;
  }

  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) break;
    }
    ApplyPendingRates();

    // 1. 先把 MFT 的事件收干净：只有收到 METransformHaveOutput 才允许收包。
    int outputs_ready = 0;
    if (async_ && event_generator_ != nullptr) {
      for (;;) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT event_hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (event_hr == MF_E_NO_EVENTS_AVAILABLE) break;
        if (FAILED(event_hr)) break;
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformHaveOutput) ++outputs_ready;
      }
    }

    // 2. 推输入：MFT 缓冲满会回 MF_E_NOTACCEPTING，留到下一轮再试。
    int pushed = 0;
    while (pushed < kMaxInFlightInputs) {
      std::optional<PendingFrame> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) break;
        pending.emplace(std::move(pending_.front()));
        pending_.pop_front();
      }
      const int64_t push_start_us = NowUs();
      const PushResult push_result = PushFrameToMft(*pending);
      if (push_result != PushResult::kPushed) {
        // 缓冲满：把同一帧放回队首，下一轮再试（不能丢）。
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_front(std::move(*pending));
        break;
      }
      push_us_ += NowUs() - push_start_us;
      ++frames_pushed_;
      ++pushed;
    }

    // 3. 收包：1 进 1 出，收到多少 HaveOutput 就收多少。
    int collected = 0;
    if (!async_) {
      // 同步 MFT（系统软件 H.264）：直接尝试收，NEED_MORE_INPUT 即停。
      for (int tries = 0; tries < 4; ++tries) {
        bool got = false;
        if (!CollectOutputSamples(&got)) break;
        if (!got) break;
        ++collected;
      }
    } else {
      for (int i = 0; i < outputs_ready; ++i) {
        bool got = false;
        if (!CollectOutputSamples(&got)) break;
        if (!got) break;
        ++collected;
      }
    }

    if (pushed == 0 && collected == 0) {
      // 没活干：等新帧或 MFT 事件（1 ms 粒度，帧预算 16.7 ms，可忽略）。
      std::unique_lock<std::mutex> lock(mutex_);
      frame_cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
        return stop_ || !pending_.empty();
      });
    }
  }

  CloseOnWorkerThread();
  if (com_owned) CoUninitialize();
}

}  // namespace

// ---------------------------------------------------------------------------
// MfH264Decoder：MF 的 H.264 解码 MFT 包成 webrtc::VideoDecoder
// ---------------------------------------------------------------------------

namespace {

class MfH264Decoder final : public webrtc::VideoDecoder {
 public:
  MfH264Decoder() = default;
  ~MfH264Decoder() override { Release(); }

  MfH264Decoder(const MfH264Decoder&) = delete;
  MfH264Decoder& operator=(const MfH264Decoder&) = delete;

  bool Configure(const Settings& /*settings*/) override { return true; }

  int32_t Decode(const webrtc::EncodedImage& input_image, bool /*missing_frames*/,
                 int64_t /*render_time_ms*/) override;
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override;
  webrtc::VideoDecoder::DecoderInfo GetDecoderInfo() const override {
    webrtc::VideoDecoder::DecoderInfo info;
    info.implementation_name = "partyup-mf-h264-decoder";
    info.is_hardware_accelerated = false;  // 系统 H264 解码 MFT 走软件解码
    return info;
  }

 private:
  bool EnsureOpen();
  bool DrainOutputs();
  bool RenegotiateOutputType();
  void Close();

  webrtc::DecodedImageCallback* callback_ = nullptr;
  ComPtr<IMFTransform> mft_;
  bool com_owned_ = false;
  bool com_initialized_ = false;
  // CoInitialize/CoUninitialize 必须同一个线程配对；WebRTC 的 Release 若换了
  // 线程，就只放 MFT、不动 COM 单元（线程退出时系统会回收）。
  DWORD com_thread_id_ = 0;
  bool streaming_ = false;
  // 输出类型是否已协商成功：SPS 之前可能拿不到，第一帧解码时再试。
  bool output_type_set_ = false;
  // 「打开失败」的原因只打一次，避免每帧刷屏（失败后下一帧还会重试）。
  bool decoder_open_logged_ = false;
  int64_t input_frames_ = 0;
  int64_t decode_failures_ = 0;
  int output_width_ = 0;
  int output_height_ = 0;
  // 当前处理中的输入包的 RTP 时间戳：解出来的 VideoFrame 用它（微秒）。
  uint32_t input_rtp_timestamp_ = 0;
  int64_t decoded_frames_ = 0;
};

bool MfH264Decoder::EnsureOpen() {
  if (mft_ != nullptr) return true;
  if (!com_initialized_) {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_owned_ = SUCCEEDED(co_hr);
    com_initialized_ = com_owned_ || co_hr == RPC_E_CHANGED_MODE;
    if (!com_initialized_) {
      std::fprintf(stderr, "[partyup-media] h264 decoder: CoInitializeEx failed 0x%08X\n",
                   static_cast<unsigned>(co_hr));
      return false;
    }
    com_thread_id_ = GetCurrentThreadId();
  }
  MFT_REGISTER_TYPE_INFO in_info = {MFMediaType_Video, MFVideoFormat_H264};
  MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_NV12};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT enum_hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_DECODER,
      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
      &in_info, &out_info, &activates, &count);
  if (FAILED(enum_hr) || count == 0) {
    if (activates != nullptr) CoTaskMemFree(activates);
    if (!decoder_open_logged_) {
      decoder_open_logged_ = true;
      std::fprintf(stderr,
                   "[partyup-media] h264 decoder: no H264->NV12 decoder MFT "
                   "(MFTEnumEx hr=0x%08X count=%u)\n",
                   static_cast<unsigned>(enum_hr), count);
    }
    return false;
  }
  HRESULT activate_hr = E_FAIL;
  for (UINT32 i = 0; i < count && mft_ == nullptr; ++i) {
    activate_hr = activates[i]->ActivateObject(__uuidof(IMFTransform), (void**)&mft_);
  }
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);
  if (FAILED(activate_hr) || mft_ == nullptr) {
    if (!decoder_open_logged_) {
      decoder_open_logged_ = true;
      std::fprintf(stderr, "[partyup-media] h264 decoder: ActivateObject failed 0x%08X\n",
                   static_cast<unsigned>(activate_hr));
    }
    return false;
  }

  ComPtr<IMFMediaType> input_type;
  if (FAILED(MFCreateMediaType(&input_type))) return false;
  input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  const HRESULT input_hr = mft_->SetInputType(0, input_type.Get(), 0);
  if (FAILED(input_hr)) {
    std::fprintf(stderr, "[partyup-media] h264 decoder: SetInputType(H264) failed 0x%08X\n",
                 static_cast<unsigned>(input_hr));
    Close();
    return false;
  }
  if (!RenegotiateOutputType()) {
    std::fprintf(stderr,
                 "[partyup-media] h264 decoder: no NV12 output type available before "
                 "first frame (will retry on STREAM_CHANGE)\n");
  }
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  streaming_ = true;
  std::fprintf(stderr, "[partyup-media] h264 decoder: open ok (initial output type %dx%d)\n",
               output_width_, output_height_);
  return true;
}

bool MfH264Decoder::RenegotiateOutputType() {
  // 解码器只提供 NV12 输出；逐个试直到拿到 NV12（与探针里验证过的路径一致）。
  for (DWORD index = 0;; ++index) {
    ComPtr<IMFMediaType> available;
    if (FAILED(mft_->GetOutputAvailableType(0, index, &available))) return false;
    GUID subtype = {};
    available->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype != MFVideoFormat_NV12) continue;
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(available.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(mft_->SetOutputType(0, available.Get(), 0))) return false;
    output_width_ = static_cast<int>(width);
    output_height_ = static_cast<int>(height);
    output_type_set_ = true;
    return true;
  }
}

bool MfH264Decoder::DrainOutputs() {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO info = {};
    mft_->GetOutputStreamInfo(0, &info);
    MFT_OUTPUT_DATA_BUFFER out_buffer = {};
    out_buffer.dwStreamID = 0;
    ComPtr<IMFSample> allocated;
    if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
      if (info.cbSize == 0) return false;
      if (FAILED(MFCreateSample(&allocated))) return false;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateMemoryBuffer(info.cbSize, &buffer))) return false;
      allocated->AddBuffer(buffer.Get());
      out_buffer.pSample = allocated.Get();
    }
    DWORD status = 0;
    const HRESULT hr = mft_->ProcessOutput(0, 1, &out_buffer, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // SPS/PPS 里带了真实尺寸：重新协商输出类型（cbSize 也会变）。
      if (!RenegotiateOutputType()) {
        std::fprintf(stderr,
                     "[partyup-media] h264 decoder: STREAM_CHANGE but no NV12 output type\n");
        if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
        return false;
      }
      std::fprintf(stderr, "[partyup-media] h264 decoder: stream change -> %dx%d\n",
                   output_width_, output_height_);
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      continue;
    }
    if (FAILED(hr)) {
      if (decode_failures_ < 5) {
        std::fprintf(stderr, "[partyup-media] h264 decoder: ProcessOutput failed 0x%08X %s\n",
                     static_cast<unsigned>(hr), HrName(hr));
      }
      ++decode_failures_;
      if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
      return false;
    }
    IMFSample* delivered = out_buffer.pSample;
    if (delivered != nullptr && callback_ != nullptr && output_width_ > 0 &&
        output_height_ > 0) {
      ComPtr<IMFMediaBuffer> buffer;
      if (SUCCEEDED(delivered->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr) {
        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        const HRESULT lock_hr = buffer->Lock(&data, &max_length, &current_length);
        if (SUCCEEDED(lock_hr) && data != nullptr) {
          const int width = output_width_;
          const int height = output_height_;
          // NV12 → I420：libyuv 一次完成，省掉一路自定义转换。
          webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
              webrtc::I420Buffer::Create(width, height);
          const int nv12_stride = width;
          const int converted = libyuv::NV12ToI420(
              data, nv12_stride, data + static_cast<size_t>(nv12_stride) * height,
              nv12_stride, i420->MutableDataY(), i420->StrideY(),
              i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
              i420->StrideV(), width, height);
          buffer->Unlock();
          if (converted == 0) {
            // 时间戳必须用 set_rtp_timestamp（90 kHz），不能只设 timestamp_us：
            // libwebrtc 的接收流（VideoReceiveStream2 / jitter buffer）按
            // `VideoFrame::timestamp_rtp()` 把解码帧对回「交给解码器的那一包」，
            // 对不上就静默丢弃。对照 libwebrtc 自己的 VP8 解码器
            // （libvpx_vp8_decoder.cc 的 ReturnFrame）也正是
            // `.set_rtp_timestamp(timestamp)` 且不设 timestamp_us。
            // 之前的写法只设 timestamp_us、rtp 恒为 0 ——「解出 620 帧但
            // frames_received 一直是 0」的根因就在这里。
            webrtc::VideoFrame frame =
                webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(i420)
                    .set_rtp_timestamp(input_rtp_timestamp_)
                    .build();
            ++decoded_frames_;
            if (decoded_frames_ == 1) {
              std::fprintf(stderr,
                           "[partyup-media] h264 decoder: first decoded frame %dx%d "
                           "(input #%lld, rtp=%u)\n",
                           width, height, static_cast<long long>(input_frames_),
                           input_rtp_timestamp_);
            }
            callback_->Decoded(frame);
          }
        } else {
          buffer->Unlock();
        }
      }
    }
    if (delivered != nullptr && delivered != allocated.Get()) delivered->Release();
    if (out_buffer.pEvents != nullptr) out_buffer.pEvents->Release();
  }
}

int32_t MfH264Decoder::Decode(const webrtc::EncodedImage& input_image, bool /*missing*/,
                              int64_t /*render_time_ms*/) {
  if (input_image.data() == nullptr || input_image.size() == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (!EnsureOpen()) return WEBRTC_VIDEO_CODEC_ERROR;
  input_rtp_timestamp_ = input_image.RtpTimestamp();
  if (input_frames_ == 0) {
    std::fprintf(stderr,
                 "[partyup-media] h264 decoder: first input frame %zu bytes, key=%d, "
                 "rtp=%u, capture_ms=%lld\n",
                 input_image.size(), input_image.IsKey() ? 1 : 0, input_image.RtpTimestamp(),
                 static_cast<long long>(input_image.capture_time_ms_));
  }
  ++input_frames_;
  // SPS 之前可能拿不到输出类型；第一帧到了就再试一次（此时尺寸已知）。
  if (!output_type_set_ && !RenegotiateOutputType()) {
    std::fprintf(stderr, "[partyup-media] h264 decoder: still no NV12 output type\n");
  }

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(input_image.size()), &buffer)) ||
      buffer == nullptr) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  BYTE* data = nullptr;
  DWORD max_length = 0;
  if (FAILED(buffer->Lock(&data, &max_length, nullptr))) return WEBRTC_VIDEO_CODEC_ERROR;
  std::memcpy(data, input_image.data(), input_image.size());
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(input_image.size()));
  ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample))) return WEBRTC_VIDEO_CODEC_ERROR;
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(static_cast<LONGLONG>(input_image.capture_time_ms_) * 10'000);

  HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    if (!DrainOutputs()) return WEBRTC_VIDEO_CODEC_ERROR;
    hr = mft_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    if (decode_failures_ < 5) {
      std::fprintf(stderr,
                   "[partyup-media] h264 decoder: ProcessInput failed 0x%08X %s "
                   "(input #%lld, %zu bytes)\n",
                   static_cast<unsigned>(hr), HrName(hr),
                   static_cast<long long>(input_frames_), input_image.size());
    }
    ++decode_failures_;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!DrainOutputs()) return WEBRTC_VIDEO_CODEC_ERROR;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Decoder::Release() {
  if (input_frames_ > 0 || decoded_frames_ > 0) {
    std::fprintf(stderr,
                 "[partyup-media] h264 decoder: closed after %lld input frame(s), "
                 "%lld decoded frame(s), %lld failure(s)\n",
                 static_cast<long long>(input_frames_), static_cast<long long>(decoded_frames_),
                 static_cast<long long>(decode_failures_));
  }
  Close();
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void MfH264Decoder::Close() {
  if (mft_ != nullptr) {
    if (streaming_) {
      mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    mft_.Reset();
    streaming_ = false;
  }
  if (com_owned_ && com_thread_id_ == GetCurrentThreadId()) {
    CoUninitialize();
  }
  com_owned_ = false;
  com_thread_id_ = 0;
  com_initialized_ = false;
  output_type_set_ = false;
  output_width_ = 0;
  output_height_ = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 编码器 / 解码器工厂
// ---------------------------------------------------------------------------

namespace {

// 协商用的 H.264 格式：constrained baseline + level 5.1 + packetization-mode=1。
// level 5.1 是 2560x1440@60 的最低可用档（level 4.0 只到 1080p30 附近），
// 声明低了观看方会拒绝接收。
webrtc::SdpVideoFormat PartyUpH264Format() {
  return webrtc::CreateH264Format(webrtc::H264Profile::kProfileConstrainedBaseline,
                                  webrtc::H264Level::kLevel5_1, "1");
}

class PartyUpEncoderFactory final : public webrtc::VideoEncoderFactory {
 public:
  PartyUpEncoderFactory()
      : vp8_(std::make_unique<
             webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter>>()) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    std::vector<webrtc::SdpVideoFormat> formats;
    if (HasH264Encoder() && !ForceVp8ForTesting()) {
      formats.push_back(PartyUpH264Format());
    }
    for (const webrtc::SdpVideoFormat& format : vp8_->GetSupportedFormats()) {
      formats.push_back(format);
    }
    return formats;
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
    if (format.name == "H264") {
      // 每路流一个编码器；具体用硬件还是软件 MFT 在 InitEncode 里决定。
      return std::make_unique<MfH264Encoder>();
    }
    return vp8_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> vp8_;
};

class PartyUpDecoderFactory final : public webrtc::VideoDecoderFactory {
 public:
  PartyUpDecoderFactory()
      : vp8_(std::make_unique<
             webrtc::VideoDecoderFactoryTemplate<webrtc::LibvpxVp8DecoderTemplateAdapter>>()) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    std::vector<webrtc::SdpVideoFormat> formats;
    formats.push_back(PartyUpH264Format());  // 系统 H264 解码 MFT 一直都在
    for (const webrtc::SdpVideoFormat& format : vp8_->GetSupportedFormats()) {
      formats.push_back(format);
    }
    return formats;
  }

  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
    if (format.name == "H264") {
      return std::make_unique<MfH264Decoder>();
    }
    return vp8_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> vp8_;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreatePartyUpVideoEncoderFactory() {
  return std::make_unique<PartyUpEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreatePartyUpVideoDecoderFactory() {
  return std::make_unique<PartyUpDecoderFactory>();
}

}  // namespace partyup
