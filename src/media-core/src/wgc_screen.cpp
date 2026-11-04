// PartyUp 媒体核心 · 真实屏幕采集实现（Windows Graphics Capture）
//
// 链路（与 spikes/wgc-probe 同一条，已验证）：
//   D3D11CreateDevice → IDXGIDevice → CreateDirect3D11DeviceFromDXGIDevice
//   → GraphicsCaptureItem::CreateForMonitor(HMONITOR)
//   → Direct3D11CaptureFramePool::CreateFreeThreaded(BGRA8, 3 缓冲)
//   → CreateCaptureSession → StartCapture → FrameArrived(agile 委托) → 队列
//   → 采集线程：CopyResource → 复用 staging → Map → libyuv ARGBToI420
//   → webrtc::VideoFrame → sink
//
// 为什么不用零拷贝纹理：VP8 是软件编码器，只吃 I420，GPU 纹理必须先落到 CPU。
// 真零拷贝要等硬件编码器（NVENC/MFT）直接吃 D3D11 纹理，见
// .docs/spikes/2026-11-02-wgc-in-media-core.md 的结论一节。
#include "wgc_screen.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>  // ID3D11VideoDevice / ID3D11VideoContext / VideoProcessorBlt
#include <dxgi1_2.h>
#include <roapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <winstring.h>

#include <wrl/client.h>
#include <wrl/ftm.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"
#include "libyuv/convert_argb.h"

namespace partyup {
namespace {

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::Wrappers::HStringReference;

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgdx = ABI::Windows::Graphics::DirectX;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;

using Clock = std::chrono::steady_clock;

// 帧池 3 缓冲：探针用 2 缓冲时只要处理耗时略微超过帧间隔就开始丢帧；WGC 的
// 产帧节奏是 DWM 驱动的（本机 ~21 ms，抖动到 55 ms），3 缓冲能吸收一次长抖动。
constexpr int kFramePoolBuffers = 3;
// 我们自己队列的上限也是 3：来不及处理就丢最旧，不追帧。
constexpr size_t kMaxQueuedFrames = 3;
constexpr int kPopTimeoutMs = 100;  // 让 Run 至少每 100 ms 看一眼停止标志

double MsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

double MsSince(Clock::time_point t0) { return MsBetween(t0, Clock::now()); }

std::string HrText(HRESULT hr) {
  char buf[512] = {};
  const DWORD n = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
      static_cast<DWORD>(sizeof(buf)), nullptr);
  std::string text;
  if (n > 0) {
    text.assign(buf, n);
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
      text.pop_back();
    }
  }
  char code[32];
  std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
  return text.empty() ? std::string(code) : (std::string(code) + " " + text);
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                       static_cast<int>(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return {};
  std::string narrow(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      narrow.data(), size, nullptr, nullptr);
  return narrow;
}

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      wide.data(), size);
  return wide;
}

std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

bool StartsWith(const std::string& text, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return text.size() >= n && std::memcmp(text.data(), prefix, n) == 0;
}

// ---------------------------------------------------------------------------
// 帧队列：FrameArrived（任意线程）→ Push，采集线程 Pop
// ---------------------------------------------------------------------------

struct FrameArrival {
  ComPtr<wgc::IDirect3D11CaptureFrame> frame;
  Clock::time_point arrived;
};

// 释放一帧并把它占着的帧池缓冲还回去。WGC 的帧是 IClosable；只 Release 也能
// 回收，但显式 Close 更符合探针里验证过的收尾顺序。
void CloseFrame(FrameArrival* item) {
  if (item == nullptr || item->frame == nullptr) return;
  ComPtr<ABI::Windows::Foundation::IClosable> closable;
  if (SUCCEEDED(item->frame.As(&closable)) && closable) {
    closable->Close();
  }
  item->frame.Reset();
}

class FrameQueue {
 public:
  // 返回被挤掉的最旧帧数（0 或 1）。
  int Push(FrameArrival item) {
    int dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_.load(std::memory_order_acquire)) {
        CloseFrame(&item);
        return 0;
      }
      items_.push_back(std::move(item));
      while (items_.size() > kMaxQueuedFrames) {
        FrameArrival oldest = std::move(items_.front());
        items_.pop_front();
        CloseFrame(&oldest);
        ++dropped;
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    cv_.notify_one();
    return dropped;
  }

  // 超时返回 false；Stop() 之后也返回 false。
  bool Pop(FrameArrival* out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this] { return !items_.empty() || stopping_.load(); });
    if (!got || items_.empty()) return false;
    *out = std::move(items_.front());
    items_.pop_front();
    return true;
  }

  // 等第一帧到达（不取走）。被 Stop() 打断时返回 false。
  bool WaitNonEmpty(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !items_.empty() || stopping_.load(); }) &&
           !items_.empty();
  }

  void Stop() {
    stopping_.store(true, std::memory_order_release);
    cv_.notify_all();
  }

  bool stopping() const { return stopping_.load(std::memory_order_acquire); }

  size_t Drain(std::vector<FrameArrival>* out) {
    std::deque<FrameArrival> leftovers;
    {
      std::lock_guard<std::mutex> lock(mu_);
      leftovers.swap(items_);
    }
    out->reserve(out->size() + leftovers.size());
    for (auto& item : leftovers) out->push_back(std::move(item));
    return leftovers.size();
  }

  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<FrameArrival> items_;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> dropped_{0};
};

// ---------------------------------------------------------------------------
// FrameArrived 委托
//
// 必须混入 FtmBase：CreateFreeThreaded 的帧池在任意线程上回调，委托必须实现
// IAgileObject，否则 add_FrameArrived 直接 0x8000001C（RO_E_NO_AGILE_OBJECT）。
// ---------------------------------------------------------------------------
class FrameArrivedHandler final
    : public RuntimeClass<
          RuntimeClassFlags<ClassicCom>,
          ABI::Windows::Foundation::ITypedEventHandler<
              wgc::Direct3D11CaptureFramePool*, IInspectable*>,
          FtmBase> {
 public:
  explicit FrameArrivedHandler(FrameQueue* queue) : queue_(queue) {}

  HRESULT STDMETHODCALLTYPE Invoke(wgc::IDirect3D11CaptureFramePool* sender,
                                   IInspectable* /*args*/) override {
    if (sender == nullptr || queue_ == nullptr) return S_OK;
    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    if (FAILED(sender->TryGetNextFrame(&frame)) || frame == nullptr) return S_OK;
    // 到达时刻在委托线程上取：这才是「WGC 什么时候把帧给我们」，不受采集线程
    // 排队影响，interval 因此是真实的 DWM 节奏。
    queue_->Push(FrameArrival{std::move(frame), Clock::now()});
    return S_OK;
  }

 private:
  FrameQueue* const queue_;
};

// ---------------------------------------------------------------------------
// 显示器枚举
// ---------------------------------------------------------------------------

struct RawDisplay {
  HMONITOR monitor = nullptr;
  RECT rect{};
  bool primary = false;
  bool attached = false;
  bool have_dxgi = false;
  std::wstring device;  // \\.\DISPLAY1
  UINT dpi_x = 96;
  UINT dpi_y = 96;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  auto* displays = reinterpret_cast<std::vector<RawDisplay>*>(param);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) return TRUE;
  RawDisplay info;
  info.monitor = monitor;
  info.rect = mi.rcMonitor;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  info.device = mi.szDevice;
  UINT dx = 96;
  UINT dy = 96;
  if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) {
    info.dpi_x = dx;
    info.dpi_y = dy;
  }
  displays->push_back(std::move(info));
  return TRUE;
}

// 线程作用域的 per-monitor v2 DPI awareness：不 set 的话 GetMonitorInfo 给的是
// 缩放后的逻辑坐标（本机 2560x1440 会变成 1707x960），枚举出来的分辨率就是错的。
// 用线程作用域而不是 SetProcessDpiAwarenessContext，避免改宿主进程（C# UI）的设置。
class ScopedThreadDpiAwareness {
 public:
  ScopedThreadDpiAwareness() {
    previous_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
  ~ScopedThreadDpiAwareness() {
    if (previous_ != nullptr) SetThreadDpiAwarenessContext(previous_);
  }

 private:
  DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

std::vector<RawDisplay> EnumerateRawDisplays() {
  ScopedThreadDpiAwareness dpi;
  std::vector<RawDisplay> displays;
  EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                      reinterpret_cast<LPARAM>(&displays));

  // DXGI 的 DesktopCoordinates 是物理像素，且能给出 AttachedToDesktop；用它覆盖
  // EnumDisplayMonitors 的（可能被虚拟化的）矩形。匹配键只能是 HMONITOR。
  ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
      ComPtr<IDXGIOutput> output;
      for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
        DXGI_OUTPUT_DESC od{};
        if (FAILED(output->GetDesc(&od))) continue;
        for (RawDisplay& display : displays) {
          if (display.monitor != od.Monitor) continue;
          display.rect = od.DesktopCoordinates;
          display.attached = od.AttachedToDesktop != FALSE;
          display.have_dxgi = true;
          display.device = od.DeviceName;
          break;
        }
      }
    }
  }
  return displays;
}

// "display:0x0000000000050001"
std::string MakeDisplayId(HMONITOR monitor) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "display:0x%016llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(monitor)));
  return std::string(buf);
}

bool ParseDisplayId(const std::string& source_id, HMONITOR* out, std::string* error) {
  const std::string lower = ToLowerAscii(source_id);
  if (!StartsWith(lower, "display:0x")) {
    if (error != nullptr) {
      *error = "source id '" + source_id +
               "' is not a display id (expected display:0x<HMONITOR hex>)";
    }
    return false;
  }
  const std::string hex = lower.substr(std::strlen("display:0x"));
  if (hex.empty() || hex.size() > 16) {
    if (error != nullptr) *error = "source id '" + source_id + "' has a malformed HMONITOR";
    return false;
  }
  uint64_t value = 0;
  for (char c : hex) {
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      if (error != nullptr) *error = "source id '" + source_id + "' has a non-hex digit";
      return false;
    }
    value = (value << 4) | static_cast<uint64_t>(digit);
  }
  *out = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(value));
  return true;
}

// ---------------------------------------------------------------------------
// 首帧回写 PNG（只用于人工验图，正常路径不落盘）
// ---------------------------------------------------------------------------

bool WritePng(const std::string& path, const uint8_t* bgra, UINT width, UINT height,
              UINT stride, std::string* error) {
  const std::wstring wide_path = Widen(path);
  if (wide_path.empty()) {
    if (error != nullptr) *error = "PNG path is not valid UTF-8";
    return false;
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (error != nullptr) *error = "CoCreateInstance(WICImagingFactory): " + HrText(hr);
    return false;
  }

  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) {
    hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
  }
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr)) {
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(&frame, nullptr);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Initialize(nullptr);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->SetSize(width, height);
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(hr)) {
    hr = frame->SetPixelFormat(&format);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgra));
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }
  if (FAILED(hr)) {
    if (error != nullptr) *error = "WIC PNG write failed: " + HrText(hr);
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 显示器枚举（对外）
// ---------------------------------------------------------------------------

std::vector<ScreenTargetInfo> EnumerateScreenTargets(std::string* error) {
  std::vector<ScreenTargetInfo> targets;
  const std::vector<RawDisplay> displays = EnumerateRawDisplays();
  if (displays.empty()) {
    if (error != nullptr) *error = "EnumDisplayMonitors reported no displays";
    return targets;
  }

  for (const RawDisplay& display : displays) {
    ScreenTargetInfo info;
    info.id = MakeDisplayId(display.monitor);
    info.device = Narrow(display.device);
    info.width = static_cast<uint32_t>(std::max<LONG>(0, display.rect.right - display.rect.left));
    info.height = static_cast<uint32_t>(std::max<LONG>(0, display.rect.bottom - display.rect.top));
    info.primary = display.primary;
    char title[256];
    std::snprintf(title, sizeof(title), "%s %ux%u%s%s", info.device.c_str(), info.width,
                  info.height, info.primary ? " primary" : "",
                  display.attached ? "" : " (detached)");
    info.title = title;
    targets.push_back(std::move(info));
  }

  std::sort(targets.begin(), targets.end(),
            [](const ScreenTargetInfo& a, const ScreenTargetInfo& b) {
              if (a.primary != b.primary) return a.primary;
              return a.device < b.device;
            });
  return targets;
}

bool IsWgcCaptureSupported(std::string* error) {
  ComPtr<wgc::IGraphicsCaptureSessionStatics> statics;
  const HRESULT hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession).Get(),
      IID_PPV_ARGS(&statics));
  if (FAILED(hr)) {
    if (error != nullptr) *error = "RoGetActivationFactory(GraphicsCaptureSession): " + HrText(hr);
    return false;
  }
  if (statics == nullptr) {
    if (error != nullptr) *error = "GraphicsCaptureSession statics are null";
    return false;
  }
  boolean supported = FALSE;
  if (FAILED(statics->IsSupported(&supported))) {
    if (error != nullptr) *error = "IGraphicsCaptureSessionStatics::IsSupported failed";
    return false;
  }
  if (!supported) {
    if (error != nullptr) *error = "Windows Graphics Capture reports IsSupported()==false";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// WgcCapture
// ---------------------------------------------------------------------------

struct WgcCapture::Impl {
  std::string source_id;
  HMONITOR monitor = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;

  bool com_initialized = false;
  bool ro_initialized = false;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<wgc::IGraphicsCaptureItem> item;
  ComPtr<wgc::IDirect3D11CaptureFramePool> frame_pool;
  ComPtr<wgc::IGraphicsCaptureSession> session;
  ComPtr<FrameArrivedHandler> handler;
  EventRegistrationToken token{};

  // 复用同一张 staging：每帧 CreateTexture2D 会带来一次 GPU 资源分配 +
  // 第一次 CopyResource 的隐式同步，探针里那 1.3 ms 的 copy_map 有一半是它。
  ComPtr<ID3D11Texture2D> staging;
  uint32_t staging_width = 0;
  uint32_t staging_height = 0;
  DXGI_FORMAT staging_format = DXGI_FORMAT_UNKNOWN;

  // ---- 纹理路径（零拷贝第一步）的 GPU 侧状态 ----
  // 第 1 片只做「建好但不启用」：Open() 里初始化，失败则 texture_ready=false，
  // 旧 I420 路径照常工作；Run() 里暂时不调用 ConvertFrameToTexture()。
  //
  // 生命周期契约（第 2 片接进 Run() 时必须遵守）：
  //   WGC 的帧纹理在 frame->Close() 之后失效，所以顺序**只能是**
  //     Surface → ID3D11Texture2D → VideoProcessorBlt 进 NV12 池纹理
  //     → 调用 sink（此时池纹理有效）→ 返回后再 Close WGC 原始帧。
  //   绝不能让 WGC 纹理句柄活过 Close，也绝不能把池纹理指针交给 sink 跨回调保存。
  bool texture_ready = false;
  // 失败原因只留在本字段：本层没有日志通道（见文件头），且**绝不能**写进
  // last_error —— 那是「这次采集出过错」的对外语义，把纹理路径的降级原因塞进去
  // 会让正常的 I420 会话看起来像坏了。
  std::string texture_error;
  ComPtr<ID3D11VideoDevice> video_device;
  ComPtr<ID3D11VideoContext> video_context;
  ComPtr<ID3D11VideoProcessorEnumerator> vp_enum;
  ComPtr<ID3D11VideoProcessor> vp;
  std::vector<ComPtr<ID3D11Texture2D>> nv12_pool;
  std::vector<ComPtr<ID3D11VideoProcessorOutputView>> nv12_out_views;
  uint32_t vp_width = 0;
  uint32_t vp_height = 0;
  size_t nv12_next = 0;

  // 输出模式：默认 I420（旧行为）。SetOutputMode() 显式设置后 env 不再覆盖。
  CaptureOutput output_mode = CaptureOutput::kI420;
  bool output_mode_explicit = false;
  uint64_t texture_drops = 0;  // 纹理路径转换失败而跳过的帧（不阻塞采集线程）

  FrameQueue queue;
  std::atomic<bool> stop_requested{false};
  std::string last_error;

  std::string dump_png_path;
  bool dumped_png = false;

  bool Open(std::string* error);
  bool ConvertFrame(FrameArrival* arrival,
                    webrtc::scoped_refptr<webrtc::I420Buffer>* out_buffer,
                    CaptureStageTimings* timings);
  // 建 VideoDevice / VideoContext / VideoProcessor + NV12 纹理池。任何一步失败只写
  // texture_error 并返回 false，不 fatal —— 调用方不得让它影响 Open 的成败。尺寸
  // 由调用方给（Open 用 capture item 的尺寸），所以调用前尺寸必须已确定。
  bool InitTexturePath(uint32_t target_width, uint32_t target_height);
  // BGRA 源纹理 → VideoProcessorBlt → 池内一张 NV12 纹理（不回读、不做 CPU 颜色
  // 转换）。*out_pool_texture 指向池内纹理，**调用方不得 Release**：所有权在
  // Impl，有效期到池轮转一圈（5 帧）为止。
  //
  // 调用契约：src 只能来自 WGC 帧，而 WGC 的帧纹理在 frame->Close() 之后失效，
  // 所以必须在 Close 之前调用本函数。第 1 片不接进 Run()，供第 2 片与探针复用。
  bool ConvertFrameToTexture(ID3D11Texture2D* src,
                             ID3D11Texture2D** out_pool_texture);
  void MaybeDumpPng(const uint8_t* y, int y_stride, const uint8_t* u, int u_stride,
                    const uint8_t* v, int v_stride, int width, int height,
                    int64_t timestamp_us);
};

WgcCapture::WgcCapture() : impl_(new Impl()) {}
WgcCapture::~WgcCapture() { Close(); }

uint32_t WgcCapture::width() const { return impl_->width; }
uint32_t WgcCapture::height() const { return impl_->height; }
uint64_t WgcCapture::dropped_frames() const { return impl_->queue.dropped(); }

void WgcCapture::SetOutputMode(CaptureOutput mode) {
  impl_->output_mode = mode;
  impl_->output_mode_explicit = true;
}
CaptureOutput WgcCapture::output_mode() const { return impl_->output_mode; }
bool WgcCapture::texture_ready() const { return impl_->texture_ready; }
uint64_t WgcCapture::texture_drops() const { return impl_->texture_drops; }

// 纹理路径主循环。取帧/停止/间隔逻辑与 Run() 保持一致（同样的 pop 超时与
// 停止标志检查），只在出帧处换成「Surface → Blt 进 NV12 池纹理 → sink」。
//
// **生命周期契约**：Blt 与 sink 都在 WGC 原始帧 Close 之前完成（见
// Nv12TextureFrame 注释）。转换失败的帧丢掉并计数，绝不阻塞采集线程。
void WgcCapture::RunTexture(const TextureSink& sink, const TimingSink& on_timing,
                            const std::atomic<bool>* stop_requested) {
  Impl& self = *impl_;
  if (!self.texture_ready) return;  // 没准备好就立刻返回，调用方自行回落 Run()

  Clock::time_point previous_arrival{};
  bool have_previous = false;

  for (;;) {
    if (stop_requested != nullptr &&
        stop_requested->load(std::memory_order_relaxed)) {
      break;
    }
    if (self.stop_requested.load(std::memory_order_relaxed)) break;

    FrameArrival arrival;
    if (!self.queue.Pop(&arrival, kPopTimeoutMs)) {
      if (self.queue.stopping()) break;
      continue;
    }

    // capture 段：两帧到达的间隔（DWM 节奏）。首帧没有前一段，记 0。
    CaptureStageTimings timings;
    if (have_previous) {
      timings.interval_ms = MsBetween(previous_arrival, arrival.arrived);
    }
    previous_arrival = arrival.arrived;
    have_previous = true;

    // Blt 段：Surface → ID3D11Texture2D → VideoProcessorBlt → 池内 NV12 纹理。
    const auto t_blt = Clock::now();
    ComPtr<wgd3d::IDirect3DSurface> surface;
    HRESULT hr = arrival.frame->get_Surface(&surface);
    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
        access;
    if (SUCCEEDED(hr) && surface != nullptr) hr = surface.As(&access);
    ComPtr<ID3D11Texture2D> source;
    if (SUCCEEDED(hr) && access != nullptr) {
      hr = access->GetInterface(IID_PPV_ARGS(&source));
    }
    ID3D11Texture2D* pool_texture = nullptr;
    const bool converted =
        SUCCEEDED(hr) && source != nullptr &&
        self.ConvertFrameToTexture(source.Get(), &pool_texture) &&
        pool_texture != nullptr;
    timings.blt_ms = MsSince(t_blt);
    timings.total_ms = timings.blt_ms;

    bool keep_going = true;
    if (converted) {
      Nv12TextureFrame frame;
      frame.texture = pool_texture;
      frame.width = self.vp_width;
      frame.height = self.vp_height;
      frame.capture_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              arrival.arrived.time_since_epoch())
              .count();
      frame.blt_ms = timings.blt_ms;
      if (sink) keep_going = sink(frame);
    } else {
      // 这一帧转换不出来：丢掉并计数，不阻断采集、不终止会话。
      ++self.texture_drops;
    }

    // 只有 sink 消费完之后才 Close WGC 原始帧（生命周期契约）。
    CloseFrame(&arrival);
    if (on_timing) on_timing(timings);
    if (!keep_going) break;
  }
}
const std::string& WgcCapture::last_error() const { return impl_->last_error; }
const std::string& WgcCapture::dumped_png_path() const { return impl_->dump_png_path; }

std::unique_ptr<WgcCapture> WgcCapture::Create(const std::string& source_id,
                                               std::string* error) {
  HMONITOR monitor = nullptr;
  const std::string lower = ToLowerAscii(source_id);
  if (StartsWith(lower, "display:")) {
    if (!ParseDisplayId(source_id, &monitor, error)) return nullptr;
  } else {
    // 别名：\\.\DISPLAY1（给人用，内部仍然翻译成 HMONITOR）
    std::vector<ScreenTargetInfo> targets = EnumerateScreenTargets(error);
    const std::string wanted = ToLowerAscii(source_id);
    for (const ScreenTargetInfo& target : targets) {
      if (ToLowerAscii(target.device) == wanted) {
        HMONITOR resolved = nullptr;
        std::string parse_error;
        if (ParseDisplayId(target.id, &resolved, &parse_error)) monitor = resolved;
        break;
      }
    }
    if (monitor == nullptr) {
      if (error != nullptr) {
        *error = "unknown screen source id '" + source_id + "' (use pu_screen_sources)";
      }
      return nullptr;
    }
  }

  // 用枚举结果校验 HMONITOR 仍然有效（比如显示器被拔掉了）。
  bool found = false;
  for (const ScreenTargetInfo& target : EnumerateScreenTargets(nullptr)) {
    HMONITOR candidate = nullptr;
    if (ParseDisplayId(target.id, &candidate, nullptr) && candidate == monitor) {
      found = true;
      break;
    }
  }
  if (!found) {
    if (error != nullptr) {
      *error = "display '" + source_id + "' is no longer attached (waited for enumeration)";
    }
    return nullptr;
  }

  std::unique_ptr<WgcCapture> capture(new WgcCapture());
  capture->impl_->source_id = source_id;
  capture->impl_->monitor = monitor;
  return capture;
}

bool WgcCapture::Open(std::string* error) {
  Impl& self = *impl_;

  // 采集线程自己初始化 COM/WinRT：DLL 被 C# 宿主加载时，宿主线程的单元状态与
  // 我们无关，不能假设；也不能依赖宿主替我们 RoInitialize。
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) {
    self.com_initialized = true;
  } else if (hr != RPC_E_CHANGED_MODE) {
    if (error != nullptr) *error = "CoInitializeEx(MTA) failed: " + HrText(hr);
    return false;
  }
  hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (SUCCEEDED(hr)) {
    self.ro_initialized = true;
  } else if (hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
    if (error != nullptr) *error = "RoInitialize(MTA) failed: " + HrText(hr);
    return false;
  }

  std::string support_error;
  if (!IsWgcCaptureSupported(&support_error)) {
    if (error != nullptr) *error = support_error;
    return false;
  }

  // 1. D3D11 设备（硬件优先，失败回落 WARP —— 虚拟机/远程桌面下会走到这里）
  // VIDEO_SUPPORT 是 ID3D11VideoDevice / VideoProcessorBlt 的前置条件（纹理路径用）；
  // 对 I420 路径无影响，缺失时只是 texture_ready 会保持 false。
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                         ARRAYSIZE(levels), D3D11_SDK_VERSION, &self.device, nullptr,
                         &self.context);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &self.device, nullptr,
                           &self.context);
  }
  if (FAILED(hr) || self.device == nullptr) {
    if (error != nullptr) *error = "D3D11CreateDevice failed: " + HrText(hr);
    return false;
  }

  // VideoProcessor 在 NVIDIA 驱动上需要 multithread protection，否则偶发 E_FAIL。
  // 与探针（capture-texture-probe）里的处理保持一致。
  {
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(self.device.As(&multithread))) {
      multithread->SetMultithreadProtected(TRUE);
    }
  }

  // 2. capture item（按 HMONITOR；绝不用 DisplayName 反查）
  ComPtr<IGraphicsCaptureItemInterop> interop;
  hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(),
      IID_PPV_ARGS(&interop));
  if (FAILED(hr)) {
    if (error != nullptr) *error = "RoGetActivationFactory(GraphicsCaptureItem): " + HrText(hr);
    return false;
  }
  hr = interop->CreateForMonitor(self.monitor, IID_PPV_ARGS(&self.item));
  if (FAILED(hr) || self.item == nullptr) {
    if (error != nullptr) *error = "CreateForMonitor failed: " + HrText(hr);
    return false;
  }
  ABI::Windows::Graphics::SizeInt32 item_size{};
  hr = self.item->get_Size(&item_size);
  if (FAILED(hr) || item_size.Width <= 0 || item_size.Height <= 0) {
    if (error != nullptr) *error = "capture item has an invalid size: " + HrText(hr);
    return false;
  }
  self.width = static_cast<uint32_t>(item_size.Width);
  self.height = static_cast<uint32_t>(item_size.Height);

  // 2b. 纹理路径（零拷贝第一步）的 GPU 资源：**建好，但不接进任何调用链**。
  // 必须放在尺寸确定之后：VideoProcessor 的 content desc 与 NV12 池都按分辨率建。
  // 失败一律不 fatal（虚拟机 / WARP / 老驱动上可能建不出来）：InitTexturePath()
  // 自己把原因写进 texture_error 并让 texture_ready 保持 false，旧 I420 路径照常。
  self.InitTexturePath(self.width, self.height);

  // 运行时开关：PARTYUP_CAPTURE_TEXTURE=1 → 默认走纹理路径；默认仍是 I420。
  // 显式 SetOutputMode() 优先（output_mode_explicit），出问题可立刻切回 I420。
  if (!self.output_mode_explicit) {
    char env_value[8] = {};
    const DWORD env_len = GetEnvironmentVariableA(
        "PARTYUP_CAPTURE_TEXTURE", env_value,
        static_cast<DWORD>(sizeof(env_value)));
    if (env_len > 0 && env_len < sizeof(env_value) && env_value[0] != '0' &&
        self.texture_ready) {
      self.output_mode = CaptureOutput::kNv12Texture;
    }
  }

  // 3. WinRT Direct3D 设备（帧池要的是 IDirect3DDevice，不是 ID3D11Device）
  ComPtr<IDXGIDevice> dxgi_device;
  hr = self.device.As(&dxgi_device);
  if (FAILED(hr)) {
    if (error != nullptr) *error = "ID3D11Device as IDXGIDevice failed: " + HrText(hr);
    return false;
  }
  ComPtr<IInspectable> inspectable;
  hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable);
  if (FAILED(hr)) {
    if (error != nullptr) *error = "CreateDirect3D11DeviceFromDXGIDevice: " + HrText(hr);
    return false;
  }
  ComPtr<wgd3d::IDirect3DDevice> capture_device;
  hr = inspectable.As(&capture_device);
  if (FAILED(hr)) {
    if (error != nullptr) *error = "IInspectable as IDirect3DDevice: " + HrText(hr);
    return false;
  }

  // 4. 3 缓冲 free-threaded 帧池
  ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
  hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(),
      IID_PPV_ARGS(&pool_statics));
  if (FAILED(hr)) {
    if (error != nullptr) *error = "RoGetActivationFactory(CaptureFramePool): " + HrText(hr);
    return false;
  }
  hr = pool_statics->CreateFreeThreaded(
      capture_device.Get(), wgdx::DirectXPixelFormat_B8G8R8A8UIntNormalized,
      kFramePoolBuffers, item_size, &self.frame_pool);
  if (FAILED(hr) || self.frame_pool == nullptr) {
    if (error != nullptr) *error = "CreateFreeThreaded failed: " + HrText(hr);
    return false;
  }

  // 5. 会话 + 光标
  hr = self.frame_pool->CreateCaptureSession(self.item.Get(), &self.session);
  if (FAILED(hr) || self.session == nullptr) {
    if (error != nullptr) *error = "CreateCaptureSession failed: " + HrText(hr);
    return false;
  }
  ComPtr<wgc::IGraphicsCaptureSession2> session2;
  if (SUCCEEDED(self.session.As(&session2)) && session2 != nullptr) {
    session2->put_IsCursorCaptureEnabled(static_cast<boolean>(TRUE));
  }

  // 6. 事件委托（FtmBase 见 FrameArrivedHandler 注释）
  self.handler = Make<FrameArrivedHandler>(&self.queue);
  if (self.handler == nullptr) {
    if (error != nullptr) *error = "failed to create FrameArrived handler";
    return false;
  }
  hr = self.frame_pool->add_FrameArrived(self.handler.Get(), &self.token);
  if (FAILED(hr)) {
    if (error != nullptr) *error = "add_FrameArrived failed: " + HrText(hr);
    return false;
  }

  // 7. 首帧回写（调试用）：PARTYUP_CAPTURE_DUMP_PNG=<Windows 路径>
  {
    char path[1024];
    const DWORD length = GetEnvironmentVariableA("PARTYUP_CAPTURE_DUMP_PNG", path,
                                                static_cast<DWORD>(sizeof(path)));
    if (length > 0 && length < sizeof(path)) {
      self.dump_png_path.assign(path, length);
    }
  }

  hr = self.session->StartCapture();
  if (FAILED(hr)) {
    if (error != nullptr) *error = "StartCapture failed: " + HrText(hr);
    return false;
  }
  return true;
}

bool WgcCapture::WaitFirstFrame(int timeout_ms, std::string* error) {
  if (!impl_->queue.WaitNonEmpty(timeout_ms)) {
    if (error != nullptr) {
      *error = "no WGC frame within " + std::to_string(timeout_ms) +
               " ms after StartCapture (locked session or capture policy?)";
    }
    return false;
  }
  return true;
}

void WgcCapture::RequestStop() {
  impl_->stop_requested.store(true, std::memory_order_release);
  impl_->queue.Stop();
}

// ---- 纹理路径（零拷贝第一步 · 第 1 片：建好但不启用）----------------------
//
// 这一步只准备 GPU 资源与「一帧 BGRA 纹理 → 一帧 NV12 池纹理」的转换能力，
// Run() 仍然走旧的 staging + libyuv I420 路径，行为完全不变。等第 2 片把
// RunTexture() 接上之后，才会真正用到这里建的东西。
//
// 为什么 NV12 池要 5 张：下游（编码器）可能还握着上一两张纹理，池太小会在
// 转换时覆盖正在被读的那张。池满时的策略在第 2 片里定为「丢最旧 + 计数」。
//
// 只建纹理路径自己的对象，不碰任何 I420 路径的状态；失败时把原因写进
// texture_error 并让 texture_ready=false，调用方（Open/ConvertFrameToTexture）
// 继续走旧路径。
bool WgcCapture::Impl::InitTexturePath(uint32_t target_width, uint32_t target_height) {
  texture_ready = false;
  texture_error.clear();
  video_device.Reset();
  video_context.Reset();
  vp_enum.Reset();
  vp.Reset();
  nv12_pool.clear();
  nv12_out_views.clear();
  vp_width = 0;
  vp_height = 0;
  nv12_next = 0;

  if (device == nullptr || context == nullptr || target_width == 0 ||
      target_height == 0) {
    texture_error = "texture path: device/context/size not ready";
    return false;
  }

  HRESULT hr = device.As(&video_device);
  if (FAILED(hr) || video_device == nullptr) {
    texture_error = "ID3D11VideoDevice failed: " + HrText(hr);
    return false;
  }
  hr = context.As(&video_context);
  if (FAILED(hr) || video_context == nullptr) {
    texture_error = "ID3D11VideoContext failed: " + HrText(hr);
    return false;
  }

  // 只有 GPU 支持把 NV12 当 VideoProcessor 输出时，这条路才有意义。
  UINT support = 0;
  hr = device->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
  if (FAILED(hr) || !(support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT)) {
    texture_error = "NV12 is not a video-processor output (hr=" + HrText(hr) + ")";
    return false;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate.Numerator = 60;
  content.InputFrameRate.Denominator = 1;
  content.InputWidth = target_width;
  content.InputHeight = target_height;
  content.OutputFrameRate = content.InputFrameRate;
  content.OutputWidth = target_width;
  content.OutputHeight = target_height;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  hr = video_device->CreateVideoProcessorEnumerator(&content, &vp_enum);
  if (FAILED(hr) || vp_enum == nullptr) {
    texture_error = "CreateVideoProcessorEnumerator failed: " + HrText(hr);
    return false;
  }
  hr = video_device->CreateVideoProcessor(vp_enum.Get(), 0, &vp);
  if (FAILED(hr) || vp == nullptr) {
    texture_error = "CreateVideoProcessor failed: " + HrText(hr);
    return false;
  }
  video_context->VideoProcessorSetStreamFrameFormat(
      vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  // 第 4 个参数是 repeat_frame，第 5 个是自定义帧率（nullptr 表示不自定义）。
  video_context->VideoProcessorSetStreamOutputRate(
      vp.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
  video_context->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, nullptr);

  constexpr size_t kTexturePoolSize = 5;
  nv12_pool.resize(kTexturePoolSize);
  nv12_out_views.resize(kTexturePoolSize);
  for (size_t i = 0; i < nv12_pool.size(); ++i) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = target_width;
    desc.Height = target_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;  // VideoProcessor 输出的必要条件
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    hr = device->CreateTexture2D(&desc, nullptr, &nv12_pool[i]);
    if (FAILED(hr) || nv12_pool[i] == nullptr) {
      texture_error = "CreateTexture2D(NV12 pool) failed: " + HrText(hr);
      nv12_pool.clear();
      nv12_out_views.clear();
      return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ov.Texture2D.MipSlice = 0;
    hr = video_device->CreateVideoProcessorOutputView(nv12_pool[i].Get(),
                                                      vp_enum.Get(), &ov,
                                                      &nv12_out_views[i]);
    if (FAILED(hr) || nv12_out_views[i] == nullptr) {
      texture_error = "CreateVideoProcessorOutputView failed: " + HrText(hr);
      nv12_pool.clear();
      nv12_out_views.clear();
      return false;
    }
  }

  vp_width = target_width;
  vp_height = target_height;
  texture_ready = true;
  return true;
}

// 一帧 BGRA 纹理 → 池内一张 NV12 纹理。不回读、不做 CPU 颜色转换。
//
// **调用方契约**（第 2 片接进 Run() 时照着写）：
//   * src 只能来自 WGC 帧：get_Surface → IDirect3DDxgiInterfaceAccess →
//     GetInterface(ID3D11Texture2D)。WGC 的帧纹理在 frame->Close() 之后失效，
//     所以 Blt 必须发生在 Close 之前。
//   * *out_pool_texture 指向池内纹理，所有权在 Impl：调用方不得 Release，也不得
//     存下来跨帧用（池轮转一圈后会被下一帧覆盖）。
bool WgcCapture::Impl::ConvertFrameToTexture(ID3D11Texture2D* src,
                                             ID3D11Texture2D** out_pool_texture) {
  if (out_pool_texture != nullptr) *out_pool_texture = nullptr;
  if (!texture_ready || vp == nullptr || vp_enum == nullptr ||
      video_device == nullptr || video_context == nullptr || context == nullptr ||
      nv12_pool.empty() || nv12_out_views.size() != nv12_pool.size()) {
    if (texture_error.empty()) texture_error = "texture path is not ready";
    return false;
  }
  if (src == nullptr || out_pool_texture == nullptr) {
    texture_error = "texture path: null source texture / output pointer";
    return false;
  }

  // 尺寸变了（改缩放、远程桌面重连）就按新尺寸重建一次池；失败就保持降级。
  D3D11_TEXTURE2D_DESC source_desc{};
  src->GetDesc(&source_desc);
  if (source_desc.Width != vp_width || source_desc.Height != vp_height) {
    if (!InitTexturePath(source_desc.Width, source_desc.Height)) return false;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
  iv.FourCC = 0;
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  iv.Texture2D.MipSlice = 0;
  iv.Texture2D.ArraySlice = 0;
  ComPtr<ID3D11VideoProcessorInputView> in_view;
  HRESULT hr = video_device->CreateVideoProcessorInputView(src, vp_enum.Get(), &iv,
                                                           &in_view);
  if (FAILED(hr) || in_view == nullptr) {
    texture_error = "CreateVideoProcessorInputView failed: " + HrText(hr);
    return false;
  }

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = in_view.Get();
  hr = video_context->VideoProcessorBlt(vp.Get(), nv12_out_views[nv12_next].Get(), 0,
                                        1, &stream);
  if (FAILED(hr)) {
    texture_error = "VideoProcessorBlt failed: " + HrText(hr);
    return false;
  }
  // 必须 Flush：VideoProcessorBlt 只是提交命令，不 Flush 的话（a）计时只量到提交、
  // 会虚低，（b）下游可能在 GPU 写完之前就去读这张池纹理。
  context->Flush();

  *out_pool_texture = nv12_pool[nv12_next].Get();
  nv12_next = (nv12_next + 1) % nv12_pool.size();
  return true;
}

bool WgcCapture::Impl::ConvertFrame(FrameArrival* arrival,
                                    webrtc::scoped_refptr<webrtc::I420Buffer>* out_buffer,
                                    CaptureStageTimings* timings) {
  const Clock::time_point convert_start = Clock::now();

  ComPtr<wgd3d::IDirect3DSurface> surface;
  HRESULT hr = arrival->frame->get_Surface(&surface);
  if (FAILED(hr) || surface == nullptr) {
    last_error = "get_Surface failed: " + HrText(hr);
    return false;
  }
  // 注意：这个接口在 um\windows.graphics.directx.direct3d11.interop.h 里声明为
  // Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess（没有
  // ABI:: 前缀），和同命名空间下的 IDirect3DDevice 不同——照探针的写法。
  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
  hr = surface.As(&access);
  if (FAILED(hr) || access == nullptr) {
    last_error = "surface as IDirect3DDxgiInterfaceAccess failed: " + HrText(hr);
    return false;
  }
  ComPtr<ID3D11Texture2D> texture;
  hr = access->GetInterface(IID_PPV_ARGS(&texture));
  if (FAILED(hr) || texture == nullptr) {
    last_error = "GetInterface(ID3D11Texture2D) failed: " + HrText(hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    last_error = "unexpected capture texture format " + std::to_string(desc.Format);
    return false;
  }

  if (staging == nullptr || staging_width != desc.Width || staging_height != desc.Height ||
      staging_format != desc.Format) {
    staging.Reset();
    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = desc.Width;
    staging_desc.Height = desc.Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || staging == nullptr) {
      last_error = "CreateTexture2D(staging) failed: " + HrText(hr);
      return false;
    }
    staging_width = desc.Width;
    staging_height = desc.Height;
    staging_format = desc.Format;
  }
  width = desc.Width;
  height = desc.Height;

  context->CopyResource(staging.Get(), texture.Get());
  texture.Reset();

  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    last_error = "Map(staging) failed: " + HrText(hr);
    return false;
  }
  timings->readback_ms = MsSince(convert_start);

  // BGRA → I420 用 libyuv（libwebrtc 自带）。每帧新建 I420Buffer：VideoFrame 会
  // 把它的引用交给编码器，复用同一块内存就是数据竞争；I420Buffer::Create 内部走
  // libwebrtc 的缓冲池，分配成本可以忽略。
  const Clock::time_point convert_begin = Clock::now();
  webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
      webrtc::I420Buffer::Create(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
  const int converted = libyuv::ARGBToI420(
      static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch),
      buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(),
      buffer->MutableDataV(), buffer->StrideV(), static_cast<int>(desc.Width),
      static_cast<int>(desc.Height));
  context->Unmap(staging.Get(), 0);
  timings->convert_ms = MsSince(convert_begin);

  if (converted != 0) {
    last_error = "libyuv::ARGBToI420 failed with " + std::to_string(converted);
    return false;
  }

  MaybeDumpPng(buffer->DataY(), buffer->StrideY(), buffer->DataU(), buffer->StrideU(),
               buffer->DataV(), buffer->StrideV(), buffer->width(), buffer->height(),
               arrival->arrived.time_since_epoch().count());

  // 时间戳用「到达时刻」而不是 T0+index*interval：真实采集的节奏由 DWM 决定，
  // 编造固定间隔会让 RTP 时间戳与实际帧间隔不一致（抖动被抹平成假的）。由 Run()
  // 在把帧交给 sink 之前统一设置。
  *out_buffer = buffer;
  return true;
}

void WgcCapture::Impl::MaybeDumpPng(const uint8_t* y, int y_stride, const uint8_t* u, int u_stride,
                              const uint8_t* v, int v_stride, int width, int height,
                              int64_t /*timestamp_us*/) {
  Impl& self = *this;
  if (self.dump_png_path.empty() || self.dumped_png) return;
  self.dumped_png = true;

  // I420 → BGRA（libyuv 的 ARGB 命名指的是内存字节序 B,G,R,A），再交给 WIC。
  // 这一步存在的意义就是「把喂给编码器的那份像素原样倒出来看」，所以必须从
  // I420Buffer 转回去，而不是复用 staging 里的 BGRA。
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> bgra(stride * static_cast<size_t>(height));
  const int rc = libyuv::I420ToARGB(y, y_stride, u, u_stride, v, v_stride, bgra.data(),
                                    static_cast<int>(stride), width, height);
  if (rc != 0) {
    self.last_error = "libyuv::I420ToARGB failed with " + std::to_string(rc);
    return;
  }
  std::string write_error;
  if (!WritePng(self.dump_png_path, bgra.data(), static_cast<UINT>(width),
                static_cast<UINT>(height), static_cast<UINT>(stride), &write_error)) {
    self.last_error = write_error;
  }
}

void WgcCapture::Run(const FrameSink& sink, const TimingSink& on_timing,
                     const std::atomic<bool>* stop_requested) {
  Impl& self = *impl_;
  Clock::time_point previous_arrival{};
  bool have_previous = false;

  for (;;) {
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed)) break;
    if (self.stop_requested.load(std::memory_order_relaxed)) break;

    FrameArrival arrival;
    if (!self.queue.Pop(&arrival, kPopTimeoutMs)) {
      if (self.queue.stopping()) break;
      continue;
    }

    // capture 段：两帧到达的间隔（DWM 节奏）。首帧没有前一段，记 0。
    CaptureStageTimings timings;
    if (have_previous) timings.interval_ms = MsBetween(previous_arrival, arrival.arrived);
    previous_arrival = arrival.arrived;
    have_previous = true;

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer;
    if (!self.ConvertFrame(&arrival, &buffer, &timings)) break;

    const int64_t now_system_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const webrtc::VideoFrame frame =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_timestamp_us(now_system_us)
            .set_ntp_time_ms(now_system_us / 1000)
            .set_rtp_timestamp(static_cast<uint32_t>(now_system_us * 90 / 1000))
            .build();

    // encode-wait 段：把帧交给编码器（PushFrame → 广播器 → VideoStreamEncoder）。
    // 编码本身是异步的，这里量到的是「交出去要不要排队等」。
    const Clock::time_point push_begin = Clock::now();
    const bool keep_going = sink(frame);
    timings.push_ms = MsSince(push_begin);
    timings.total_ms = timings.readback_ms + timings.convert_ms + timings.push_ms;
    if (on_timing) on_timing(timings);

    CloseFrame(&arrival);
    if (!keep_going) break;
  }

  // WGC 对象的收尾顺序与探针一致：先摘委托，再关 session，最后关帧池。
  // 全部在采集线程上做，避免 COM 对象跨单元析构。
  Close();
}

void WgcCapture::Close() {
  Impl& self = *impl_;
  self.queue.Stop();
  if (self.frame_pool != nullptr && self.token.value != 0) {
    self.frame_pool->remove_FrameArrived(self.token);
    self.token.value = 0;
  }
  std::vector<FrameArrival> leftovers;
  self.queue.Drain(&leftovers);
  for (FrameArrival& leftover : leftovers) CloseFrame(&leftover);
  self.handler.Reset();
  self.session.Reset();
  self.frame_pool.Reset();
  self.item.Reset();
  self.staging.Reset();
  // 纹理路径的 GPU 对象：按依赖反序释放（output view 持有纹理的引用，所以先放
  // view 再放池；processor 依赖 enumerator；video context/device 是 device/context
  // 的同一批 COM 对象的另一组接口，最后放）。每张 ComPtr 各自负责一次 Release，
  // 这里全部 Reset 后引用计数归零，不留任何可泄漏的纹理。
  self.nv12_out_views.clear();
  self.nv12_pool.clear();
  self.vp.Reset();
  self.vp_enum.Reset();
  self.video_context.Reset();
  self.video_device.Reset();
  self.texture_ready = false;
  self.vp_width = 0;
  self.vp_height = 0;
  self.nv12_next = 0;
  self.context.Reset();
  self.device.Reset();
  if (self.ro_initialized) {
    RoUninitialize();
    self.ro_initialized = false;
  }
  if (self.com_initialized) {
    CoUninitialize();
    self.com_initialized = false;
  }
}

}  // namespace partyup
