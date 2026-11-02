// 纹理路径（零拷贝第一步）生产代码测量程序。
//
// 直接用 WgcCapture 的公共 API，把**同一个类**的两条输出路径各跑 300 帧：
//   A) SetOutputMode(kI420)        → Run()       ：staging 读回 + libyuv ARGBToI420
//   B) SetOutputMode(kNv12Texture) → RunTexture()：VideoProcessorBlt → NV12 纹理
// 报每帧 CPU、fps、blt_ms、texture_drops、以及 CaptureStageTimings 的分段。
//
// 纹理路径首帧会读回 NV12 → I420 → BGRA → PNG，供人工确认像素正确。
//
// 为什么需要自己造画面变化：WGC 是变化驱动的，静止桌面几乎不出帧，凑不满 300 帧。
// 这里跑一个置顶的动画窗口（GDI 定时重绘），保证稳定产帧。
//
// 构建：spikes/media-dll-smoke/build-wgc-texture-smoke.ps1（编 wgc_screen.cpp + webrtc.lib）

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "libyuv.h"
#include "wgc_screen.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------- 小工具 ----

double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fflush(stderr);
}

std::string HrText(HRESULT hr) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
  return std::string(buf);
}

double ProcessCpuMs() {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    return 0.0;
  }
  auto to_ms = [](const FILETIME& ft) {
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return static_cast<double>(v.QuadPart) / 10000.0;  // 100ns → ms
  };
  return to_ms(kernel) + to_ms(user);
}

// ------------------------------------------------------------ 动画刺激源 ----
// 置顶的无边框小窗，定时器每 8 ms 重绘一次滚动色块。只为制造屏幕变化。
class Stimulus {
 public:
  bool Start(int x, int y, int w, int h) {
    std::atomic<bool>* ready = &ready_;
    thread_ = std::thread([this, x, y, w, h, ready]() {
      WNDCLASSA wc{};
      wc.lpfnWndProc = &Stimulus::WndProc;
      wc.hInstance = GetModuleHandleA(nullptr);
      wc.lpszClassName = "PartyUpTexStimulus";
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      RegisterClassA(&wc);
      hwnd_ = CreateWindowExA(
          WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
          "stimulus", WS_POPUP, x, y, w, h, nullptr, nullptr, wc.hInstance, this);
      if (hwnd_ == nullptr) {
        ready->store(true);
        return;
      }
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
      SetTimer(hwnd_, 1, 8, nullptr);  // ~120 Hz 重绘，保证 WGC 一直有变化
      ready->store(true);
      MSG msg;
      while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
      }
      if (hwnd_ != nullptr) DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    });
    for (int i = 0; i < 500 && !ready_.load(); ++i) Sleep(2);
    return hwnd_ != nullptr;
  }

  void Stop() {
    if (hwnd_ != nullptr) PostMessageA(hwnd_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
    hwnd_ = nullptr;
  }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Stimulus* self = reinterpret_cast<Stimulus*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (msg == WM_CREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
      SetWindowLongPtrA(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return 0;
    }
    if (msg == WM_TIMER && self != nullptr) {
      self->paint_offset_ = (self->paint_offset_ + 7) % 512;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    if (msg == WM_PAINT && self != nullptr) {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      HBRUSH bg = CreateSolidBrush(RGB(16, 18, 24));
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      const int bars = 8;
      for (int i = 0; i < bars; ++i) {
        const int cx = (self->paint_offset_ + i * (rc.right / bars)) % (rc.right + 80) - 40;
        HBRUSH brush = CreateSolidBrush(
            RGB((i * 37) % 255, (i * 91 + self->paint_offset_) % 255, (i * 53) % 255));
        RECT bar{cx, 0, cx + rc.right / bars, rc.bottom};
        FillRect(dc, &bar, brush);
        DeleteObject(brush);
      }
      // 大号数字，保证 OCR/肉眼都能看出在动
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      char text[32];
      std::snprintf(text, sizeof(text), "%d", self->paint_offset_);
      HFONT font = CreateFontA(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, "Segoe UI");
      HGDIOBJ old = SelectObject(dc, font);
      DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, old);
      DeleteObject(font);
      EndPaint(hwnd, &ps);
      return 0;
    }
    if (msg == WM_CLOSE) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
  }

  std::thread thread_;
  std::atomic<bool> ready_{false};
  HWND hwnd_ = nullptr;
  int paint_offset_ = 0;
};

// --------------------------------------------------------------- PNG 写出 ----

bool WritePng(const std::wstring& path, IWICImagingFactory* wic, UINT width,
              UINT height, const BYTE* bgra) {
  ComPtr<IWICStream> stream;
  HRESULT hr = wic->CreateStream(&stream);
  if (FAILED(hr)) return false;
  hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (FAILED(hr)) return false;
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(&frame, &props);
  if (FAILED(hr)) return false;
  hr = frame->Initialize(props.Get());
  if (FAILED(hr)) return false;
  hr = frame->SetSize(width, height);
  if (FAILED(hr)) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) return false;
  hr = frame->WritePixels(height, width * 4, width * height * 4,
                          const_cast<BYTE*>(bgra));
  if (FAILED(hr)) return false;
  hr = frame->Commit();
  if (FAILED(hr)) return false;
  hr = encoder->Commit();
  return SUCCEEDED(hr);
}

// 把池内 NV12 纹理读回、转 BGRA、存 PNG。只在首帧调用一次（含回读，不计入统计）。
bool DumpNv12TexturePng(ID3D11Texture2D* texture, const std::wstring& path,
                        std::string* error) {
  D3D11_TEXTURE2D_DESC src_desc{};
  texture->GetDesc(&src_desc);
  ComPtr<ID3D11Device> device;
  texture->GetDevice(&device);
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);

  D3D11_TEXTURE2D_DESC staging_desc = src_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(hr)) {
    *error = "CreateTexture2D(staging): " + HrText(hr);
    return false;
  }
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    *error = "Map(staging): " + HrText(hr);
    return false;
  }
  const int w = static_cast<int>(src_desc.Width);
  const int h = static_cast<int>(src_desc.Height);
  const auto* base = static_cast<const uint8_t*>(mapped.pData);
  const int y_stride = static_cast<int>(mapped.RowPitch);
  const uint8_t* uv_plane = base + static_cast<size_t>(y_stride) * h;
  const int uv_stride = static_cast<int>(mapped.RowPitch);

  std::vector<uint8_t> i420(static_cast<size_t>(w) * h * 3 / 2);
  const int half_w = (w + 1) / 2;
  const int half_h = (h + 1) / 2;
  uint8_t* dst_y = i420.data();
  uint8_t* dst_u = dst_y + static_cast<size_t>(w) * h;
  uint8_t* dst_v = dst_u + static_cast<size_t>(half_w) * half_h;
  const int rc = libyuv::NV12ToI420(base, y_stride, uv_plane, uv_stride, dst_y, w,
                                    dst_u, half_w, dst_v, half_w, w, h);
  context->Unmap(staging.Get(), 0);
  if (rc != 0) {
    *error = "libyuv::NV12ToI420 failed";
    return false;
  }

  std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
  const int rc2 = libyuv::I420ToARGB(dst_y, w, dst_u, half_w, dst_v, half_w,
                                     bgra.data(), w * 4, w, h);
  if (rc2 != 0) {
    *error = "libyuv::I420ToARGB failed";
    return false;
  }

  ComPtr<IWICImagingFactory> wic;
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&wic));
  if (FAILED(hr)) {
    *error = "CoCreateInstance(WIC): " + HrText(hr);
    return false;
  }
  if (!WritePng(path, wic.Get(), static_cast<UINT>(w), static_cast<UINT>(h),
                bgra.data())) {
    *error = "WritePng failed";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 统计 ----

struct Stats {
  const char* name = "";
  int frames = 0;
  double wall_ms = 0.0;
  double cpu_ms = 0.0;        // 进程 CPU（含采集线程与测试自身）
  double interval_sum = 0.0;
  double readback_sum = 0.0;
  double convert_sum = 0.0;
  double blt_sum = 0.0;
  double push_sum = 0.0;
  double total_sum = 0.0;
  double total_max = 0.0;
  uint64_t queue_drops = 0;
  uint64_t texture_drops = 0;

  void Add(const partyup::CaptureStageTimings& t) {
    ++frames;
    interval_sum += t.interval_ms;
    readback_sum += t.readback_ms;
    convert_sum += t.convert_ms;
    blt_sum += t.blt_ms;
    push_sum += t.push_ms;
    total_sum += t.total_ms;
    total_max = std::max(total_max, t.total_ms);
  }

  void Print() const {
    const double n = frames > 0 ? frames : 1;
    Log("\n=== %s ===\n", name);
    Log("frames=%d  wall=%.0f ms  fps=%.2f  cpu=%.0f ms  cpu/frame=%.3f ms\n",
        frames, wall_ms, frames / (wall_ms / 1000.0), cpu_ms, cpu_ms / n);
    Log("stage avg: interval=%.2f readback=%.3f convert=%.3f blt=%.3f "
        "push=%.3f total=%.3f (max total=%.2f)\n",
        interval_sum / n, readback_sum / n, convert_sum / n, blt_sum / n,
        push_sum / n, total_sum / n, total_max);
    Log("drops: queue=%llu texture=%llu\n",
        static_cast<unsigned long long>(queue_drops),
        static_cast<unsigned long long>(texture_drops));
  }
};

// 跑一条路径。want_png 时把首帧纹理存 PNG（只有纹理路径能存）。
Stats RunPath(const std::string& source_id, partyup::CaptureOutput mode,
              int target_frames, const std::wstring& png_path,
              std::string* png_result, std::string* error) {
  Stats stats;
  stats.name = mode == partyup::CaptureOutput::kNv12Texture ? "B: Nv12Texture"
                                                            : "A: I420";
  std::unique_ptr<partyup::WgcCapture> capture =
      partyup::WgcCapture::Create(source_id, error);
  if (!capture) return stats;
  capture->SetOutputMode(mode);
  if (!capture->Open(error)) return stats;
  if (!capture->WaitFirstFrame(5000, error)) {
    capture->Close();
    return stats;
  }
  stats.queue_drops = capture->dropped_frames();
  if (mode == partyup::CaptureOutput::kNv12Texture && !capture->texture_ready()) {
    *error = "texture path unavailable: " + capture->last_error();
    capture->Close();
    return stats;
  }

  std::atomic<bool> stop{false};
  std::string dump_error;

  auto on_timing = [&stats](const partyup::CaptureStageTimings& t) {
    stats.Add(t);
  };

  const Clock::time_point wall_start = Clock::now();
  const double cpu_start = ProcessCpuMs();

  if (mode == partyup::CaptureOutput::kI420) {
    capture->Run(
        [&stats, target_frames](const webrtc::VideoFrame&) {
          return stats.frames < target_frames;
        },
        on_timing, &stop);
  } else {
    capture->RunTexture(
        [&stats, target_frames, &png_path, &png_result, &dump_error](
            const partyup::Nv12TextureFrame& frame) {
          if (stats.frames == 0) {
            std::string err;
            if (DumpNv12TexturePng(frame.texture, png_path, &err)) {
              *png_result = "ok";
            } else {
              dump_error = err;
            }
          }
          return stats.frames < target_frames;
        },
        on_timing, &stop);
  }

  stats.wall_ms = MsSince(wall_start);
  stats.cpu_ms = ProcessCpuMs() - cpu_start;
  if (capture) {
    stats.texture_drops = capture->texture_drops();
    const std::string& cap_error = capture->last_error();
    if (!cap_error.empty() && error->empty()) *error = cap_error;
  }
  if (!dump_error.empty() && error->empty()) *error = "png: " + dump_error;
  capture->Close();
  return stats;
}

std::string FirstDisplaySource(std::string* error) {
  std::string enum_error;
  std::vector<partyup::ScreenTargetInfo> targets =
      partyup::EnumerateScreenTargets(&enum_error);
  if (targets.empty()) {
    *error = "no screen target: " + enum_error;
    return std::string();
  }
  return targets.front().id;
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
  const std::string out_dir = argc > 2 ? argv[2] : ".";
  std::string error;
  const std::string source_id = FirstDisplaySource(&error);
  if (source_id.empty()) {
    Log("[fatal] %s\n", error.c_str());
    return 1;
  }
  Log("== source=%s frames=%d out=%s\n", source_id.c_str(), frames,
      out_dir.c_str());

  Stimulus stimulus;
  const bool stimulus_ok = stimulus.Start(40, 40, 640, 360);
  Log("== stimulus=%s (顶部 640x360 动画窗口，制造屏幕变化)\n",
      stimulus_ok ? "on" : "FAILED");

  const std::wstring png_path =
      std::wstring(out_dir.begin(), out_dir.end()) + L"\\wgc-texture-first-frame.png";

  std::string path_error;
  const Stats a = RunPath(source_id, partyup::CaptureOutput::kI420, frames,
                          png_path, nullptr, &path_error);
  std::string png_result;
  std::string b_error;
  const Stats b = RunPath(source_id, partyup::CaptureOutput::kNv12Texture, frames,
                          png_path, &png_result, &b_error);
  stimulus.Stop();

  a.Print();
  b.Print();

  const double a_total = a.frames > 0 ? a.total_sum / a.frames : 0.0;
  const double b_total = b.frames > 0 ? b.total_sum / b.frames : 0.0;
  const double a_cpu = a.frames > 0 ? a.cpu_ms / a.frames : 0.0;
  const double b_cpu = b.frames > 0 ? b.cpu_ms / b.frames : 0.0;
  Log("\n===COMPARE===\n");
  Log("%-16s %12s %12s\n", "metric", "A: I420", "B: Nv12Texture");
  Log("%-16s %12d %12d\n", "frames", a.frames, b.frames);
  Log("%-16s %12.2f %12.2f\n", "fps", a.frames / (a.wall_ms / 1000.0),
      b.frames / (b.wall_ms / 1000.0));
  Log("%-16s %12.3f %12.3f\n", "stage total ms", a_total, b_total);
  Log("%-16s %12.3f %12.3f\n", "proc cpu ms/frame", a_cpu, b_cpu);
  Log("%-16s %12.3f %12.3f\n", "texture blt ms", 0.0,
      b.frames > 0 ? b.blt_sum / b.frames : 0.0);
  Log("%-16s %12llu %12llu\n", "texture_drops",
      static_cast<unsigned long long>(a.texture_drops),
      static_cast<unsigned long long>(b.texture_drops));
  Log("speedup_stage_total=%.2fx\n", b_total > 0 ? a_total / b_total : 0.0);
  Log("png=%s (%s)\n", "wgc-texture-first-frame.png",
      png_result.empty() ? "FAILED" : png_result.c_str());
  Log("===ENDCOMPARE===\n");
  if (!path_error.empty()) Log("[error A] %s\n", path_error.c_str());
  if (!b_error.empty()) Log("[error B] %s\n", b_error.c_str());

  const bool pass = b.frames >= frames / 2 && b_total > 0.0 && b_total < 1.5 &&
                    b.texture_drops == 0 && !png_result.empty();
  Log("\nRESULT: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 2;
}
