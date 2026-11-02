// ---------------------------------------------------------------------------
// PartyUp · Windows Graphics Capture (WGC) capability probe
//
// Purpose: prove this machine can grab a real desktop frame through
//   D3D11 -> WGC (CreateForMonitor / CreateFreeThreaded) -> staging copy ->
//   WIC PNG, without any libwebrtc dependency.
//
// The probe prints:
//   * platform / WGC support status
//   * every DXGI adapter + output (display) with resolution, position, DPI
//   * capturable top-level windows (HWND / title / class / rect / pid)
//   * primary-monitor capture timings: cold first frame, 10 consecutive frames
//   * the absolute PNG path and its byte size
//
// Exit code 0 means the whole path worked and a PNG was written.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <roapi.h>

#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>
#include <wrl/ftm.h>

#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winstring.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::Wrappers::HStringReference;

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgdx = ABI::Windows::Graphics::DirectX;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDefaultFrameCount = 10;
constexpr int kFirstFrameTimeoutMs = 5000;
constexpr int kNextFrameTimeoutMs = 3000;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  std::fflush(stdout);
}

std::string HrText(HRESULT hr) {
  char buf[512] = {};
  DWORD n = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
      static_cast<DWORD>(sizeof(buf)), nullptr);
  std::string text;
  if (n > 0) {
    text.assign(buf, n);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                             text.back() == ' ')) {
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

void Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(what) + " failed: " + HrText(hr));
  }
}

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Returns "yes"/"no".
const char* YesNo(bool v) { return v ? "yes" : "no"; }

// ---------------------------------------------------------------------------
// FrameArrived handler (WRL, raw ABI -- no C++/WinRT, no windowsapp.lib)
// ---------------------------------------------------------------------------

struct FrameArrival {
  ComPtr<wgc::IDirect3D11CaptureFrame> frame;
  Clock::time_point arrived;
};

struct FrameQueue {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<FrameArrival> items;
  std::atomic<bool> stopping{false};
  std::atomic<int> delivered{0};
  std::atomic<int> overflow{0};
};

class FrameArrivedHandler final
    : public RuntimeClass<
          RuntimeClassFlags<ClassicCom>,
          ABI::Windows::Foundation::ITypedEventHandler<
              wgc::Direct3D11CaptureFramePool*, IInspectable*>,
          // The free-threaded frame pool raises FrameArrived on arbitrary
          // threads, so the delegate must be agile; FtmBase supplies
          // IAgileObject/IMarshal. Without it add_FrameArrived fails with
          // RO_E_NO_AGILE_OBJECT (0x8000001C).
          FtmBase> {
 public:
  explicit FrameArrivedHandler(FrameQueue* queue) : queue_(queue) {}

  HRESULT STDMETHODCALLTYPE Invoke(wgc::IDirect3D11CaptureFramePool* sender,
                                   IInspectable* /*args*/) override {
    if (!sender || !queue_ || queue_->stopping.load(std::memory_order_acquire)) {
      return S_OK;
    }
    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    HRESULT hr = sender->TryGetNextFrame(&frame);
    if (FAILED(hr) || !frame) {
      return S_OK;
    }
    const auto arrived = Clock::now();
    queue_->delivered.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(queue_->mu);
      queue_->items.push_back(FrameArrival{std::move(frame), arrived});
      // Keep the queue shallow: a frame pins a pool buffer, and the pool only
      // has two. Dropping the oldest keeps the newest frames flowing.
      while (queue_->items.size() > 4) {
        queue_->items.pop_front();
        queue_->overflow.fetch_add(1, std::memory_order_relaxed);
      }
    }
    queue_->cv.notify_one();
    return S_OK;
  }

 private:
  FrameQueue* queue_ = nullptr;
};

bool WaitForFrame(FrameQueue* queue, FrameArrival* out, int timeout_ms) {
  std::unique_lock<std::mutex> lock(queue->mu);
  const bool got = queue->cv.wait_for(
      lock, std::chrono::milliseconds(timeout_ms),
      [queue] { return !queue->items.empty(); });
  if (!got) {
    return false;
  }
  *out = std::move(queue->items.front());
  queue->items.pop_front();
  return true;
}

// ---------------------------------------------------------------------------
// Stimulus overlay
//
// WGC (like the Desktop Duplication API) only produces frames when the
// captured content actually changes.  On a static desktop the frame pool can
// sit idle indefinitely, which would make a "10 consecutive frames" timing
// measurement meaningless.  This small topmost, click-through, non-activating
// window cycles a solid fill every 33 ms so DWM keeps composing new frames.
//
// The window is a few hundred pixels in the corner; the rest of the captured
// PNG is the real desktop.
// ---------------------------------------------------------------------------

class StimulusOverlay {
 public:
  bool Start(HMONITOR monitor) {
    std::promise<bool> ready;
    auto future = ready.get_future();
    thread_ = std::thread(&StimulusOverlay::ThreadMain, this, std::move(ready),
                          monitor);
    return future.get();
  }

  void Stop() {
    if (hwnd_) {
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    hwnd_ = nullptr;
  }

 private:
  static constexpr wchar_t kClass[] = L"PartyUpWgcProbeOverlay";

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StimulusOverlay* self = reinterpret_cast<StimulusOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = reinterpret_cast<StimulusOverlay*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
      if (self) {
        self->hwnd_ = hwnd;
      }
      return 0;
    }
    if (!self) {
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->HandleMessage(hwnd, msg, wp, lp);
  }

  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_TIMER:
        // Alternate the layered-window opacity: every change forces DWM to
        // composite a new frame for this window. No GDI drawing needed, so the
        // probe stays on user32.lib only.
        alpha_ = (alpha_ == 255) ? 96 : 255;
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha_),
                                   LWA_ALPHA);
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  void ThreadMain(std::promise<bool> ready, HMONITOR monitor) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StimulusOverlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    // Stock system brush: DefWindowProc erases with it, so filling the window
    // does not require gdi32.lib.
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_HIGHLIGHT + 1);
    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      ready.set_value(false);
      return;
    }

    int x = 32;
    int y = 32;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
      x = mi.rcWork.left + 32;
      y = mi.rcWork.top + 32;
    }
    const int width = 260;
    const int height = 140;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_LAYERED,
        kClass, L"PartyUp WGC probe", WS_POPUP | WS_VISIBLE, x, y, width,
        height, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
      ready.set_value(false);
      return;
    }
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(alpha_), LWA_ALPHA);
    SetTimer(hwnd_, 1, 33, nullptr);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    ready.set_value(true);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  HWND hwnd_ = nullptr;
  std::thread thread_;
  int alpha_ = 255;
};

// ---------------------------------------------------------------------------
// display + window enumeration
// ---------------------------------------------------------------------------

struct DisplayInfo {
  HMONITOR monitor = nullptr;
  RECT rect{};
  bool primary = false;
  std::wstring device;   // \\.\DISPLAY1 (EnumDisplayMonitors)
  std::wstring adapter;  // DXGI adapter description
  UINT adapter_index = 0;
  UINT output_index = 0;
  bool attached = false;
  UINT dpi_x = 96;
  UINT dpi_y = 96;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  auto* displays = reinterpret_cast<std::vector<DisplayInfo>*>(param);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) {
    return TRUE;
  }
  DisplayInfo info;
  info.monitor = monitor;
  info.rect = mi.rcMonitor;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  info.device = mi.szDevice;
  displays->push_back(std::move(info));
  return TRUE;
}

std::vector<DisplayInfo> EnumerateDisplays() {
  std::vector<DisplayInfo> displays;
  EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                      reinterpret_cast<LPARAM>(&displays));

  ComPtr<IDXGIFactory1> factory;
  Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++ai) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    ComPtr<IDXGIOutput> output;
    for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND;
         ++oi) {
      DXGI_OUTPUT_DESC od{};
      if (FAILED(output->GetDesc(&od))) {
        continue;
      }
      for (auto& d : displays) {
        if (d.monitor == od.Monitor) {
          d.adapter_index = ai;
          d.output_index = oi;
          d.attached = od.AttachedToDesktop != FALSE;
          d.device = od.DeviceName;
          d.adapter = desc.Description;
          UINT dx = 96, dy = 96;
          if (SUCCEEDED(GetDpiForMonitor(d.monitor, MDT_EFFECTIVE_DPI, &dx,
                                         &dy))) {
            d.dpi_x = dx;
            d.dpi_y = dy;
          }
          break;
        }
      }
    }
  }
  return displays;
}

struct WindowInfo {
  HWND hwnd = nullptr;
  std::wstring title;
  std::wstring klass;
  RECT rect{};
  DWORD pid = 0;
  std::wstring process;
};

BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM param) {
  auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(param);

  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
    return TRUE;
  }
  const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
  if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
    return TRUE;
  }
  if (GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;  // owned dialogs/popups: not interesting standalone targets
  }
  const int title_len = GetWindowTextLengthW(hwnd);
  if (title_len <= 0) {
    return TRUE;
  }
  RECT rect{};
  if (!GetWindowRect(hwnd, &rect)) {
    return TRUE;
  }
  const LONG width = rect.right - rect.left;
  const LONG height = rect.bottom - rect.top;
  if (width < 64 || height < 64) {
    return TRUE;
  }

  WindowInfo info;
  info.hwnd = hwnd;
  info.rect = rect;
  info.title.resize(static_cast<size_t>(title_len) + 1);
  const int copied = GetWindowTextW(hwnd, info.title.data(), title_len + 1);
  info.title.resize(copied > 0 ? static_cast<size_t>(copied) : 0);

  wchar_t klass[256] = {};
  if (GetClassNameW(hwnd, klass, ARRAYSIZE(klass)) > 0) {
    info.klass = klass;
  }

  GetWindowThreadProcessId(hwnd, &info.pid);
  if (info.pid != 0) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 info.pid);
    if (process) {
      wchar_t image[MAX_PATH] = {};
      DWORD size = ARRAYSIZE(image);
      if (QueryFullProcessImageNameW(process, 0, image, &size)) {
        info.process = std::filesystem::path(image).filename().wstring();
      }
      CloseHandle(process);
    }
  }
  windows->push_back(std::move(info));
  return TRUE;
}

std::vector<WindowInfo> EnumerateWindows() {
  std::vector<WindowInfo> windows;
  EnumWindows(WindowEnumProc, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

// ---------------------------------------------------------------------------
// D3D11 / WinRT plumbing
// ---------------------------------------------------------------------------

struct D3DContext {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
  bool hardware = true;
  std::wstring adapter_name;
};

D3DContext CreateD3DDevice() {
  D3DContext out;
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // no DEBUG layer
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
      ARRAYSIZE(levels), D3D11_SDK_VERSION, &out.device, &out.feature_level,
      &out.context);
  if (FAILED(hr)) {
    Log("[warn] hardware D3D11 device failed (%s), falling back to WARP\n",
        HrText(hr).c_str());
    out.hardware = false;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                            &out.device, &out.feature_level, &out.context),
          "D3D11CreateDevice(WARP)");
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (SUCCEEDED(out.device.As(&dxgi_device))) {
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
      DXGI_ADAPTER_DESC desc{};
      if (SUCCEEDED(adapter->GetDesc(&desc))) {
        out.adapter_name = desc.Description;
      }
    }
  }
  return out;
}

std::wstring WindowsVersionString() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                        GetProcAddress(ntdll, "RtlGetVersion"))
                  : nullptr;
  if (!fn) {
    return L"unknown";
  }
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn(&info) != 0) {
    return L"unknown";
  }
  wchar_t buf[96] = {};
  std::swprintf(buf, ARRAYSIZE(buf), L"%lu.%lu.%lu", info.dwMajorVersion,
                info.dwMinorVersion, info.dwBuildNumber);
  return buf;
}

// ---------------------------------------------------------------------------
// PNG writer (WIC)
// ---------------------------------------------------------------------------

HRESULT WritePng(const std::wstring& path, IWICImagingFactory* factory, UINT width,
                 UINT height, UINT stride, const BYTE* bgra) {
  ComPtr<IWICStream> stream;
  HRESULT hr = factory->CreateStream(&stream);
  if (FAILED(hr)) return hr;
  hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (FAILED(hr)) return hr;
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmapFrameEncode> frame;
  hr = encoder->CreateNewFrame(&frame, nullptr);
  if (FAILED(hr)) return hr;
  hr = frame->Initialize(nullptr);
  if (FAILED(hr)) return hr;
  hr = frame->SetSize(width, height);
  if (FAILED(hr)) return hr;

  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) return hr;

  ComPtr<IWICBitmap> bitmap;
  hr = factory->CreateBitmapFromMemory(width, height,
                                       GUID_WICPixelFormat32bppBGRA, stride,
                                       stride * height, const_cast<BYTE*>(bgra),
                                       &bitmap);
  if (FAILED(hr)) return hr;

  // Let WIC convert if the PNG encoder picked a different pixel format
  // (it usually keeps 32bppBGRA, but do not assume).
  if (format == GUID_WICPixelFormat32bppBGRA) {
    hr = frame->WriteSource(bitmap.Get(), nullptr);
  } else {
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    hr = frame->WriteSource(converter.Get(), nullptr);
  }
  if (FAILED(hr)) return hr;

  hr = frame->Commit();
  if (FAILED(hr)) return hr;
  return encoder->Commit();
}

unsigned long long FileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
         data.nFileSizeLow;
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------

struct Options {
  std::filesystem::path out_dir;
  std::wstring label = L"run";
  int frames = kDefaultFrameCount;
  bool stimulus = false;
  int first_timeout_ms = kFirstFrameTimeoutMs;
};

Options ParseArgs(int argc, wchar_t** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto next = [&](const wchar_t* name) -> std::wstring {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + Narrow(name));
      }
      return argv[++i];
    };
    if (arg == L"--out") {
      opt.out_dir = next(L"--out");
    } else if (arg == L"--label") {
      opt.label = next(L"--label");
    } else if (arg == L"--frames") {
      opt.frames = std::stoi(next(L"--frames"));
    } else if (arg == L"--stimulus") {
      opt.stimulus = true;
    } else if (arg == L"--no-stimulus") {
      opt.stimulus = false;
    } else if (arg == L"--timeout-ms") {
      opt.first_timeout_ms = std::stoi(next(L"--timeout-ms"));
    } else if (arg == L"--help" || arg == L"-h") {
      std::printf(
          "usage: wgc_probe.exe [--out DIR] [--label NAME] [--frames N]\n"
          "                     [--timeout-ms N] [--stimulus]\n");
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument (see --help): " +
                               Narrow(arg));
    }
  }
  if (opt.out_dir.empty()) {
    opt.out_dir = std::filesystem::current_path() / L"shots";
  }
  if (opt.frames < 1) {
    opt.frames = 1;
  }
  return opt;
}

// ---------------------------------------------------------------------------
// main probe
// ---------------------------------------------------------------------------

class Probe {
 public:
  explicit Probe(Options opt) : opt_(std::move(opt)) {}

  int Run() {
    Log("=== PartyUp WGC probe ===\n");

    // Per-monitor DPI awareness: without it the monitor rectangles are
    // virtualized and the captured PNG would not match the desktop 1:1.
    if (!SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      SetProcessDPIAware();
    }
    SetConsoleOutputCP(CP_UTF8);

    const std::wstring version = WindowsVersionString();
    Log("[info] windows_version=%s\n", Narrow(version).c_str());

    Check(RoInitialize(RO_INIT_MULTITHREADED), "RoInitialize");
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(co_hr);
    struct Cleanup {
      bool co;
      ~Cleanup() {
        if (co) CoUninitialize();
        RoUninitialize();
      }
    } cleanup{co_initialized};

    CheckWgcSupport();

    const auto t_device = Clock::now();
    d3d_ = CreateD3DDevice();
    Log("[info] d3d11_device_ms=%.1f feature_level=0x%X adapter=\"%s\" "
        "hardware=%s\n",
        MsSince(t_device), static_cast<unsigned>(d3d_.feature_level),
        Narrow(d3d_.adapter_name).c_str(), YesNo(d3d_.hardware));

    const auto displays = EnumerateDisplays();
    if (displays.empty()) {
      throw std::runtime_error("no displays reported by EnumDisplayMonitors");
    }
    Log("[displays] count=%zu\n", displays.size());
    HMONITOR primary = nullptr;
    for (const auto& d : displays) {
      const int width = d.rect.right - d.rect.left;
      const int height = d.rect.bottom - d.rect.top;
      Log("[display] id=%s adapter=\"%s\" a%u/o%u rect=(%ld,%ld)-(%ld,%ld) "
          "res=%dx%d dpi=%ux%u attached=%s primary=%s\n",
          Narrow(d.device).c_str(), Narrow(d.adapter).c_str(), d.adapter_index,
          d.output_index,
          d.rect.left, d.rect.top, d.rect.right, d.rect.bottom, width, height,
          d.dpi_x, d.dpi_y, YesNo(d.attached), YesNo(d.primary));
      if (d.primary) {
        primary = d.monitor;
        primary_device_ = d.device;
      }
    }
    if (!primary) {
      primary = displays.front().monitor;
      primary_device_ = displays.front().device;
      Log("[warn] no MONITORINFOF_PRIMARY flag; using %s\n",
          Narrow(primary_device_).c_str());
    }
    Log("[primary] id=%s\n", Narrow(primary_device_).c_str());

    const auto windows = EnumerateWindows();
    Log("[windows] capturable=%zu (visible, titled, not tool/owned/minimized, "
        ">=64x64)\n",
        windows.size());
    const size_t max_print = std::min<size_t>(windows.size(), 40);
    for (size_t i = 0; i < max_print; ++i) {
      const auto& w = windows[i];
      Log("[window] hwnd=0x%08llX res=%ldx%ld pid=%lu process=\"%s\" "
          "class=\"%s\" title=\"%s\"\n",
          static_cast<unsigned long long>(
              reinterpret_cast<uintptr_t>(w.hwnd)),
          w.rect.right - w.rect.left, w.rect.bottom - w.rect.top, w.pid,
          Narrow(w.process).c_str(), Narrow(w.klass).c_str(),
          Narrow(w.title).c_str());
    }
    if (windows.size() > max_print) {
      Log("[window] ... %zu more not printed\n", windows.size() - max_print);
    }

    // ---- capture item for the primary monitor -------------------------------
    const auto t_item = Clock::now();
    ComPtr<IGraphicsCaptureItemInterop> interop;
    Check(RoGetActivationFactory(
              HStringReference(
                  RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem)
                  .Get(),
              IID_PPV_ARGS(&interop)),
          "RoGetActivationFactory(GraphicsCaptureItem)");
    ComPtr<wgc::IGraphicsCaptureItem> item;
    Check(interop->CreateForMonitor(primary, IID_PPV_ARGS(&item)),
          "IGraphicsCaptureItemInterop::CreateForMonitor");
    const double item_ms = MsSince(t_item);

    ABI::Windows::Graphics::SizeInt32 item_size{};
    Check(item->get_Size(&item_size), "IGraphicsCaptureItem::get_Size");
    HSTRING display_name = nullptr;
    if (SUCCEEDED(item->get_DisplayName(&display_name)) && display_name) {
      UINT32 len = 0;
      const wchar_t* raw = WindowsGetStringRawBuffer(display_name, &len);
      if (raw) {
        item_display_name_.assign(raw, len);
      }
      WindowsDeleteString(display_name);
    }
    Log("[capture_item] monitor=%s size=%dx%d display_name=\"%s\" "
        "create_ms=%.1f\n",
        Narrow(primary_device_).c_str(), item_size.Width, item_size.Height,
        Narrow(item_display_name_).c_str(), item_ms);

    // ---- WinRT Direct3D device ---------------------------------------------
    ComPtr<IDXGIDevice> dxgi_device;
    Check(d3d_.device.As(&dxgi_device), "ID3D11Device as IDXGIDevice");
    ComPtr<IInspectable> inspectable;
    Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable),
          "CreateDirect3D11DeviceFromDXGIDevice");
    ComPtr<wgd3d::IDirect3DDevice> capture_device;
    Check(inspectable.As(&capture_device),
          "IInspectable as IDirect3DDevice");

    // ---- frame pool: free-threaded, BGRA8, 2 buffers -----------------------
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
    Check(RoGetActivationFactory(
              HStringReference(
                  RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool)
                  .Get(),
              IID_PPV_ARGS(&pool_statics)),
          "RoGetActivationFactory(Direct3D11CaptureFramePool)");
    ComPtr<wgc::IDirect3D11CaptureFramePool> frame_pool;
    Check(pool_statics->CreateFreeThreaded(
              capture_device.Get(),
              wgdx::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, item_size,
              &frame_pool),
          "CreateFreeThreaded");

    // ---- session ------------------------------------------------------------
    ComPtr<wgc::IGraphicsCaptureSession> session;
    Check(frame_pool->CreateCaptureSession(item.Get(), &session),
          "CreateCaptureSession");
    {
      ComPtr<wgc::IGraphicsCaptureSession2> session2;
      if (SUCCEEDED(session.As(&session2))) {
        Check(session2->put_IsCursorCaptureEnabled(static_cast<boolean>(TRUE)),
              "put_IsCursorCaptureEnabled");
        Log("[info] cursor_capture=enabled\n");
      }
    }

    // ---- event wiring -------------------------------------------------------
    FrameQueue queue;
    ComPtr<FrameArrivedHandler> handler = Make<FrameArrivedHandler>(&queue);
    EventRegistrationToken token{};
    Check(frame_pool->add_FrameArrived(handler.Get(), &token),
          "add_FrameArrived");

    // ---- WIC ----------------------------------------------------------------
    ComPtr<IWICImagingFactory> wic;
    Check(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
          "CoCreateInstance(WICImagingFactory2)");

    std::filesystem::create_directories(opt_.out_dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t file_name[256] = {};
    std::swprintf(file_name, ARRAYSIZE(file_name),
                  L"wgc-%ls-%04d%02d%02d-%02d%02d%02d.png", opt_.label.c_str(),
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    const std::wstring png_path = (opt_.out_dir / file_name).wstring();

    StimulusOverlay overlay;
    if (opt_.stimulus) {
      if (overlay.Start(primary)) {
        Log("[stimulus] overlay started (260x140, 33 ms color cycle)\n");
      } else {
        Log("[warn] stimulus overlay failed to start\n");
      }
    }

    // ---- start capture ------------------------------------------------------
    const auto t_start = Clock::now();
    Check(session->StartCapture(), "StartCapture");

    FrameArrival first;
    if (!WaitForFrame(&queue, &first, opt_.first_timeout_ms)) {
      throw std::runtime_error(
          "no frame within " + std::to_string(opt_.first_timeout_ms) +
          " ms after StartCapture (WGC produced nothing; see doc for causes)");
    }
    cold_first_frame_ms_ = std::chrono::duration<double, std::milli>(
                               first.arrived - t_start)
                               .count();
    Log("[frame] #1 cold_first_frame_ms=%.1f\n", cold_first_frame_ms_);

    const FrameCost first_cost = ProcessFrame(first, wic.Get(), png_path, true);
    if (!first_cost.ok) {
      throw std::runtime_error("failed to process first frame");
    }

    // ---- 10 consecutive frames ---------------------------------------------
    double interval_sum = 0.0;
    double interval_max = 0.0;
    double readback_sum = 0.0;
    double readback_max = 0.0;
    double copy_map_sum = 0.0;
    double copy_map_max = 0.0;
    double pack_sum = 0.0;
    double pack_max = 0.0;
    int interval_count = 0;
    Clock::time_point previous = first.arrived;

    for (int i = 0; i < opt_.frames; ++i) {
      FrameArrival next;
      if (!WaitForFrame(&queue, &next, kNextFrameTimeoutMs)) {
        Log("[warn] frame #%d not delivered within %d ms; stopping at %d/%d\n",
            i + 2, kNextFrameTimeoutMs, i + 1, opt_.frames);
        break;
      }
      const double interval_ms =
          std::chrono::duration<double, std::milli>(next.arrived - previous)
              .count();
      previous = next.arrived;
      const FrameCost cost = ProcessFrame(next, wic.Get(), png_path, false);
      if (!cost.ok) {
        throw std::runtime_error("failed to read back frame");
      }
      interval_sum += interval_ms;
      interval_max = std::max(interval_max, interval_ms);
      readback_sum += cost.total_ms;
      readback_max = std::max(readback_max, cost.total_ms);
      copy_map_sum += cost.copy_map_ms;
      copy_map_max = std::max(copy_map_max, cost.copy_map_ms);
      pack_sum += cost.pack_ms;
      pack_max = std::max(pack_max, cost.pack_ms);
      ++interval_count;
      if (i == 0 || i + 1 == opt_.frames) {
        Log("[frame] #%d interval_ms=%.2f total_ms=%.2f\n", i + 2,
            interval_ms, cost.total_ms);
      }
    }

    if (interval_count == 0) {
      throw std::runtime_error("no consecutive frames were delivered");
    }

    // ---- stop capture / cleanup --------------------------------------------
    Check(frame_pool->remove_FrameArrived(token), "remove_FrameArrived");
    queue.stopping.store(true, std::memory_order_release);
    if (opt_.stimulus) {
      overlay.Stop();
      Log("[stimulus] overlay stopped\n");
    }

    DrainQueue(&queue);
    CloseClosable(session.Get(), "session");
    CloseClosable(frame_pool.Get(), "frame_pool");
    frame_pool.Reset();
    session.Reset();
    handler.Reset();
    item.Reset();
    capture_device.Reset();
    inspectable.Reset();
    dxgi_device.Reset();
    d3d_.context.Reset();
    d3d_.device.Reset();
    wic.Reset();
    Log("[cleanup] frame.Close / session.Close / framePool.Close done\n");

    const unsigned long long png_bytes = FileSize(png_path);
    if (png_bytes == 0) {
      throw std::runtime_error("PNG was not written: " + Narrow(png_path));
    }

    CaptureStats stats{};
    stats.png_path = png_path;
    stats.png_bytes = png_bytes;
    stats.width = static_cast<UINT>(item_size.Width);
    stats.height = static_cast<UINT>(item_size.Height);
    stats.interval_avg_ms = interval_sum / interval_count;
    stats.interval_max_ms = interval_max;
    stats.readback_avg_ms = readback_sum / interval_count;
    stats.readback_max_ms = readback_max;
    stats.copy_map_avg_ms = copy_map_sum / interval_count;
    stats.copy_map_max_ms = copy_map_max;
    stats.pack_avg_ms = pack_sum / interval_count;
    stats.pack_max_ms = pack_max;
    stats.frames_measured = interval_count;
    stats.overflow = queue.overflow.load();

    PrintResult(stats);
    return 0;
  }

 private:
  struct CaptureStats {
    std::wstring png_path;
    unsigned long long png_bytes = 0;
    UINT width = 0;
    UINT height = 0;
    double interval_avg_ms = 0.0;
    double interval_max_ms = 0.0;
    double readback_avg_ms = 0.0;
    double readback_max_ms = 0.0;
    double copy_map_avg_ms = 0.0;
    double copy_map_max_ms = 0.0;
    double pack_avg_ms = 0.0;
    double pack_max_ms = 0.0;
    int frames_measured = 0;
    int overflow = 0;
  };

  void CheckWgcSupport() {
    ComPtr<wgc::IGraphicsCaptureSessionStatics> statics;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession)
            .Get(),
        IID_PPV_ARGS(&statics));
    if (FAILED(hr)) {
      Log("[info] wgc_supported=unknown (activation failed: %s)\n",
          HrText(hr).c_str());
      return;
    }
    boolean supported = FALSE;
    Check(statics->IsSupported(&supported),
          "IGraphicsCaptureSessionStatics::IsSupported");
    Log("[info] wgc_supported=%s\n", YesNo(supported != FALSE));
    if (!supported) {
      throw std::runtime_error(
          "Windows Graphics Capture reports IsSupported()==false on this "
          "machine (policy or OS version); see docs for fallbacks");
    }
  }

  static void CloseClosable(IUnknown* object, const char* label) {
    if (!object) return;
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&closable))) && closable) {
      const HRESULT hr = closable->Close();
      Log("[cleanup] %s.Close hr=%s\n", label, HrText(hr).c_str());
    } else {
      Log("[cleanup] %s: IClosable not available\n", label);
    }
  }

  static void DrainQueue(FrameQueue* queue) {
    std::deque<FrameArrival> leftovers;
    {
      std::lock_guard<std::mutex> lock(queue->mu);
      leftovers.swap(queue->items);
    }
    int closed = 0;
    for (auto& entry : leftovers) {
      ComPtr<ABI::Windows::Foundation::IClosable> closable;
      if (entry.frame &&
          SUCCEEDED(entry.frame.As(&closable)) && closable) {
        closable->Close();
        ++closed;
      }
    }
    if (closed > 0) {
      Log("[cleanup] closed %d queued frame(s)\n", closed);
    }
  }

  // Copies the frame texture into a staging texture, maps it and -- when
  // want_png -- writes the first frame as PNG.  Returns the readback time in
  // ms, or -1 on failure.
  struct FrameCost {
    double total_ms = 0.0;     // CopyResource + Map + pack + Unmap
    double copy_map_ms = 0.0;  // GPU->staging copy and the Map wait
    double pack_ms = 0.0;      // CPU row copy + alpha fix-up
    bool ok = true;
  };

  // Copies the frame texture into a staging texture, maps it and -- when
  // want_png -- writes the first frame as PNG.  Timing is split into the GPU
  // readback (CopyResource + Map) and the CPU pack (row copy + alpha fix-up).
  FrameCost ProcessFrame(const FrameArrival& arrival, IWICImagingFactory* wic,
                         const std::wstring& png_path, bool want_png) {
    FrameCost cost;
    const auto t_start = Clock::now();

    ComPtr<wgd3d::IDirect3DSurface> surface;
    HRESULT hr = arrival.frame->get_Surface(&surface);
    if (FAILED(hr) || !surface) {
      Log("[error] get_Surface hr=%s\n", HrText(hr).c_str());
      CloseFrame(arrival.frame);
      return FrameCost{0.0, 0.0, 0.0, false};
    }

    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
        access;
    hr = surface.As(&access);
    if (FAILED(hr)) {
      Log("[error] surface->IDirect3DDxgiInterfaceAccess hr=%s\n",
          HrText(hr).c_str());
      CloseFrame(arrival.frame);
      return FrameCost{0.0, 0.0, 0.0, false};
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = access->GetInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr) || !texture) {
      Log("[error] GetInterface(ID3D11Texture2D) hr=%s\n", HrText(hr).c_str());
      CloseFrame(arrival.frame);
      return FrameCost{0.0, 0.0, 0.0, false};
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    if (!staging_ || staging_width_ != desc.Width ||
        staging_height_ != desc.Height) {
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
      hr = d3d_.device->CreateTexture2D(&staging_desc, nullptr, &staging_);
      if (FAILED(hr)) {
        Log("[error] CreateTexture2D(staging) hr=%s\n", HrText(hr).c_str());
        CloseFrame(arrival.frame);
        return FrameCost{0.0, 0.0, 0.0, false};
      }
      staging_width_ = desc.Width;
      staging_height_ = desc.Height;
    }

    d3d_.context->CopyResource(staging_.Get(), texture.Get());
    texture.Reset();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = d3d_.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
      Log("[error] Map(staging) hr=%s\n", HrText(hr).c_str());
      CloseFrame(arrival.frame);
      return FrameCost{0.0, 0.0, 0.0, false};
    }

    const auto t_mapped = Clock::now();
    cost.copy_map_ms =
        std::chrono::duration<double, std::milli>(t_mapped - t_start).count();

    const UINT width = desc.Width;
    const UINT height = desc.Height;
    const UINT stride = width * 4;
    pixels_.resize(static_cast<size_t>(stride) * height);
    for (UINT y = 0; y < height; ++y) {
      const BYTE* src = static_cast<const BYTE*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;
      BYTE* dst = pixels_.data() + static_cast<size_t>(y) * stride;
      std::memcpy(dst, src, stride);
      for (UINT x = 0; x < width; ++x) {
        dst[x * 4 + 3] = 255;  // force opaque: WGC alpha is not meaningful here
      }
    }
    d3d_.context->Unmap(staging_.Get(), 0);

    const auto t_unmapped = Clock::now();
    cost.pack_ms = std::chrono::duration<double, std::milli>(
                       t_unmapped - t_mapped)
                       .count();
    cost.total_ms = std::chrono::duration<double, std::milli>(
                        t_unmapped - t_start)
                        .count();

    Log("[frame] pixels=%ux%u stride=%u format=%u copy_map_ms=%.2f "
        "pack_ms=%.2f total_ms=%.2f\n",
        width, height, stride, static_cast<unsigned>(desc.Format),
        cost.copy_map_ms, cost.pack_ms, cost.total_ms);

    if (want_png) {
      const auto t_png = Clock::now();
      hr = WritePng(png_path, wic, width, height, stride, pixels_.data());
      if (FAILED(hr)) {
        Log("[error] WritePng hr=%s\n", HrText(hr).c_str());
        CloseFrame(arrival.frame);
        return FrameCost{0.0, 0.0, 0.0, false};
      }
      Log("[frame] png_write_ms=%.1f path=\"%s\"\n", MsSince(t_png),
          Narrow(png_path).c_str());
    }

    CloseFrame(arrival.frame);
    return cost;
  }

  static void CloseFrame(const ComPtr<wgc::IDirect3D11CaptureFrame>& frame) {
    if (!frame) return;
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (SUCCEEDED(frame.As(&closable)) && closable) {
      closable->Close();
    }
  }

  void PrintResult(const CaptureStats& stats) {
    Log("===RESULT===\n");
    Log("windows_version=%s\n", Narrow(WindowsVersionString()).c_str());
    Log("wgc_supported=1\n");
    Log("adapter=%s\n", Narrow(d3d_.adapter_name.empty() ? L"unknown"
                                                          : d3d_.adapter_name)
                           .c_str());
    Log("monitor_primary=%s\n", Narrow(primary_device_).c_str());
    Log("resolution=%ux%u\n", stats.width, stats.height);
    Log("cold_first_frame_ms=%.1f\n", cold_first_frame_ms_);
    Log("frames_measured=%d\n", stats.frames_measured);
    Log("frame_interval_avg_ms=%.2f\n", stats.interval_avg_ms);
    Log("frame_interval_max_ms=%.2f\n", stats.interval_max_ms);
    Log("readback_avg_ms=%.2f\n", stats.readback_avg_ms);
    Log("readback_max_ms=%.2f\n", stats.readback_max_ms);
    Log("copy_map_avg_ms=%.2f\n", stats.copy_map_avg_ms);
    Log("copy_map_max_ms=%.2f\n", stats.copy_map_max_ms);
    Log("pack_avg_ms=%.2f\n", stats.pack_avg_ms);
    Log("pack_max_ms=%.2f\n", stats.pack_max_ms);
    Log("queue_overflow=%d\n", stats.overflow);
    Log("png_path=%s\n", Narrow(stats.png_path).c_str());
    Log("png_bytes=%llu\n", stats.png_bytes);
    Log("===END===\n");
  }

  Options opt_;
  D3DContext d3d_;
  ComPtr<ID3D11Texture2D> staging_;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;
  std::vector<BYTE> pixels_;
  std::wstring primary_device_;
  std::wstring item_display_name_;
  double cold_first_frame_ms_ = 0.0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    Options options = ParseArgs(argc, argv);
    Probe probe(std::move(options));
    return probe.Run();
  } catch (const std::exception& e) {
    Log("[fatal] %s\n", e.what());
    return 2;
  }
}
