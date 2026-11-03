// PartyUp 媒体核心 · 真实屏幕采集冒烟测试
//
// 目的：验证「pu_screen_sources → pu_screen_start(真实显示器) → WGC 抓帧 → libyuv
// I420 → VP8 编码」这条链路在本机能真的跑起来，并把验收要的数字打出来：
//   * 显示器枚举结果（id / 分辨率 / 主屏标记）；
//   * 抓 N 秒真桌面：实测 fps、每帧分段耗时（capture / readback / convert /
//     encode-wait）、码率；
//   * 共享方 pu_peer_stats().frames_encoded 是否持续增长（必须有对端在收）；
//   * 首帧回写 PNG 的绝对路径（由库在 I420 之后转回 BGRA 写出，供人工验图）。
//
// 判据（缺一不可）：
//   1. pu_screen_sources 至少返回一块 kind=0 的显示器；
//   2. pu_screen_start(显示器 id) 返回 PU_OK；
//   3. 采集期间 PU_EVENT_FRAME 条数 >= seconds * 5（真桌面 1080p 级远高于此）；
//   4. frames_encoded 在采集期间至少增长两次（持续增长，不是卡住）；
//   5. 日志里出现 ===CAPTURE_SUMMARY===（说明走的是 WGC 路径而不是合成路径）；
//   6. 全程没有 PU_EVENT_ERROR。
//
// 用法：screen_capture_smoke.exe [--seconds N] [--source <id>] [--png <path>]
//                                  [--frame-png <path>]
//   --source 省略时自动选主屏（列表里第一块 kind=0）。
//   --png 省略时把首帧写到当前目录的 capture-first-frame.png（库内部写）。
//   --frame-png 是 pu_frame_acquire 拿到的第一帧 BGRA 经 WIC 编出来的 PNG，
//   省略时写当前目录的 frame-acquire.png；写失败会回落到当前目录再报路径。
//
// 注意：本 exe 只会从 partyup_media.dll 同目录加载，必须放在 Windows 本地盘运行。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>     // CoInitializeEx / CoCreateInstance：WIC 需要 COM
#include <wincodec.h>    // 首帧 BGRA → PNG 直写（本机没有 ffmpeg/ImageMagick）
#include <wrl/client.h>  // ComPtr
#include <psapi.h>       // GetProcessMemoryInfo：pu_frame_acquire 300 帧的泄漏检查
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#include "partyup_media.h"

using Microsoft::WRL::ComPtr;

namespace {

const char* StateName(int state) {
  switch (state) {
    case PU_STREAM_IDLE:
      return "IDLE";
    case PU_STREAM_STARTING:
      return "STARTING";
    case PU_STREAM_RUNNING:
      return "RUNNING";
    case PU_STREAM_STOPPING:
      return "STOPPING";
    case PU_STREAM_FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

// 进程 CPU 时间（秒）：发送端编码 + 接收端解码都在本进程里，所以这个数包含了
// 「软件 VP8 编码 + 解码」的总开销，是判断要不要上硬件编码的直接依据。
double ProcessCpuSeconds() {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return -1.0;
  ULARGE_INTEGER k{};
  ULARGE_INTEGER u{};
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  return static_cast<double>(k.QuadPart + u.QuadPart) / 1.0e7;  // 100ns -> s
}

unsigned ProcessorCount() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors != 0 ? info.dwNumberOfProcessors : 1;
}

// 定义在后面（Options 之后），这里先声明：首帧 PNG 的日志要打绝对路径。
std::string AbsolutePath(const std::string& path);

// ---------------------------------------------------------------------------
// 首帧 BGRA → PNG（WIC）
//
// 为什么要在这里自己做：本机没有 ffmpeg / ImageMagick，WSL 里也没有 PIL，而
// WSL 不能写 /mnt/c、Windows 侧的 System.Drawing.Save() 又不接受 UNC 输出
// （写出 0 字节）。所以只能在 smoke 内部用 WIC 编码。链路照抄
// src/media-core/src/wgc_screen.cpp 的 MaybeDumpPng→WritePng：工厂 →
// CreateStream → CreateEncoder(PNG) → CreateNewFrame → WritePixels(BGRA)。
//
// **步长用 frame.stride，不是 width*4**：库已如实填行步长，图省事写 width*4
// 一旦将来真出现 padding 就会把每行错开，输出花屏。
// ---------------------------------------------------------------------------

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), needed);
  if (written <= 1) return std::wstring();
  wide.resize(static_cast<size_t>(written - 1));
  return wide;
}

std::string HrText(HRESULT hr) {
  char buffer[48] = {};
  std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

bool WritePngBgra(const std::string& path, const uint8_t* bgra, UINT width, UINT height,
                  UINT stride, std::string* error) {
  const std::wstring wide_path = Widen(path);
  if (wide_path.empty()) {
    if (error != nullptr) *error = "PNG path is not valid UTF-8";
    return false;
  }

  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICStream> stream;
  if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgra));
  }
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr)) {
    if (error != nullptr) *error = "WIC PNG write failed: " + HrText(hr);
    return false;
  }
  return true;
}

// 首帧 PNG 的目标路径由 --frame-png 决定（脚本会传仓库里的 out/ 绝对路径），
// 空字符串表示不写。只写一次：谁先拿到第一帧就谁写，之后一律跳过。
std::string g_frame_png_path;
bool g_frame_png_done = false;

void MaybeDumpFramePng(const pu_frame& frame) {
  if (g_frame_png_done || g_frame_png_path.empty()) return;
  if (frame.pixels == nullptr || frame.width == 0 || frame.height == 0) return;
  g_frame_png_done = true;

  const uint8_t* base = static_cast<const uint8_t*>(frame.pixels);
  std::string error;
  if (WritePngBgra(g_frame_png_path, base, frame.width, frame.height, frame.stride, &error)) {
    std::printf("frame png        : %s (%ux%u stride=%u)\n",
                AbsolutePath(g_frame_png_path).c_str(), frame.width, frame.height, frame.stride);
    return;
  }
  std::printf("WARN: frame PNG write to %s failed: %s\n",
              AbsolutePath(g_frame_png_path).c_str(), error.c_str());
  // 回落：写当前目录（Windows 本地盘），脚本再从那里拷回仓库，免得一次 UNC
  // 写失败就把整张图丢了。
  const std::string fallback = AbsolutePath("frame-acquire.png");
  std::string fallback_error;
  if (WritePngBgra(fallback, base, frame.width, frame.height, frame.stride, &fallback_error)) {
    std::printf("frame png (fallback): %s (%ux%u stride=%u)\n", fallback.c_str(), frame.width,
                frame.height, frame.stride);
  } else {
    std::printf("FAIL: fallback frame PNG write to %s failed: %s\n", fallback.c_str(),
                fallback_error.c_str());
  }
}

struct Options {
  int seconds = 10;
  std::string source_id;  // 空 = 自动选主屏
  std::string png_path = "capture-first-frame.png";
  // pu_frame_acquire 首帧的 PNG 输出路径；默认写当前目录，脚本会显式传绝对路径。
  std::string frame_png_path = "frame-acquire.png";
};

Options ParseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      options.seconds = std::atoi(argv[++i]);
      if (options.seconds < 1) options.seconds = 1;
    } else if (arg == "--source" && i + 1 < argc) {
      options.source_id = argv[++i];
    } else if (arg == "--png" && i + 1 < argc) {
      options.png_path = argv[++i];
    } else if (arg == "--frame-png" && i + 1 < argc) {
      options.frame_png_path = argv[++i];
    }
  }
  return options;
}

std::string AbsolutePath(const std::string& path) {
  char buffer[MAX_PATH] = {};
  const DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) return path;
  return std::string(buffer, length);
}

// 引擎日志文件：库用 GetEnvironmentVariableA 读 PARTYUP_MEDIA_LOG，所以必须用
// Win32 的 SetEnvironmentVariableA（exe 的 CRT 副本改不到 DLL 的）。
std::string ConfigureLogFile() {
  char temp[MAX_PATH] = {};
  const DWORD length = GetTempPathA(MAX_PATH, temp);
  std::string path = (length > 0 ? std::string(temp, length) : std::string(".") + "\\");
  path += "partyup-screen-capture-smoke.log";
  std::remove(path.c_str());
  SetEnvironmentVariableA("PARTYUP_MEDIA_LOG", path.c_str());
  return path;
}

// 引擎日志里 ===CAPTURE_SUMMARY=== 那一行（媒体核心停采时打）
bool FindCaptureSummary(const std::string& log_path, std::string* out) {
  std::FILE* file = std::fopen(log_path.c_str(), "rb");
  if (file == nullptr) return false;
  std::string content;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content.append(buffer, read);
  }
  std::fclose(file);

  const std::string marker = "===CAPTURE_SUMMARY===";
  const size_t at = content.find(marker);
  if (at == std::string::npos) return false;
  size_t end = content.find('\n', at);
  if (end == std::string::npos) end = content.size();
  *out = content.substr(at, end - at);
  return true;
}

// 从 "key=value key2=value2 ..." 里取一个 double
double PickNumber(const std::string& line, const char* key, double fallback) {
  const std::string needle = std::string(key) + "=";
  const size_t at = line.find(needle);
  if (at == std::string::npos) return fallback;
  const char* begin = line.c_str() + at + needle.size();
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  return end == begin ? fallback : value;
}

int PickInt(const std::string& line, const char* key, int fallback) {
  return static_cast<int>(PickNumber(line, key, static_cast<double>(fallback)));
}

struct EngineGuard {
  pu_engine* engine = nullptr;
  ~EngineGuard() {
    if (engine != nullptr) pu_engine_destroy(engine);
  }
};

// WIC 需要本线程有 COM。声明在最前面，保证比两个 EngineGuard 后析构。
struct ComGuard {
  bool usable = false;         // 本线程现在能调 COM（自己初始化成功或别人已初始化）
  bool owns_reference = false;  // 只有自己成功初始化才配对 CoUninitialize

  ComGuard() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    usable = SUCCEEDED(hr);
    owns_reference = SUCCEEDED(hr);
    // RPC_E_CHANGED_MODE：本线程已被初始化成别的模型（如 STA），COM 可用但不是
    // 我们加的那份引用，绝不能去 CoUninitialize。
    if (!usable && hr == RPC_E_CHANGED_MODE) usable = true;
  }
  ~ComGuard() {
    if (owns_reference) CoUninitialize();
  }
};

// 把 from 的 SIGNAL_OUT 原样灌给 to；顺手数一下 from 收到的远端帧。
// 返回 -1 表示出错（已打印原因）。
int ForwardSignals(pu_engine* from, pu_engine* to, int* remote_frames, const char* label) {
  for (;;) {
    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(from, &event, 0);
    if (rc == PU_ERR_TIMEOUT) return 0;
    if (rc != PU_OK) {
      std::printf("FAIL: %s pu_poll_event rc=%d (%s)\n", label, rc, pu_last_error(from));
      return -1;
    }
    if (event.type == PU_EVENT_SIGNAL_OUT) {
      const pu_signal_event& signal = event.data.signal;
      if (pu_signal_in(to, signal.payload, signal.payload_len) != PU_OK) {
        std::printf("FAIL: pu_signal_in(%s) from %s: %s\n", label, label, pu_last_error(to));
        return -1;
      }
    } else if (event.type == PU_EVENT_REMOTE_FRAME) {
      ++*remote_frames;
      // pu_frame_acquire / release 实测（观看方事件由这里统一消费）。
      static int acquired = 0;
      static int not_ready = 0;
      static int failed = 0;
      static SIZE_T private_first = 0;
      pu_frame grabbed;
      std::memset(&grabbed, 0, sizeof(grabbed));
      grabbed.struct_size = static_cast<uint32_t>(sizeof(grabbed));
      const int acq_rc = pu_frame_acquire(from, "remote", &grabbed);
      if (acq_rc == PU_OK) {
        ++acquired;
        if (acquired == 1) {
          PROCESS_MEMORY_COUNTERS counters;
          std::memset(&counters, 0, sizeof(counters));
          if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            private_first = counters.PagefileUsage;
          }
          const std::string bgra_path = AbsolutePath("frame-acquire.bgra");
          FILE* fp = std::fopen(bgra_path.c_str(), "wb");
          if (fp != nullptr) {
            const uint8_t* base = static_cast<const uint8_t*>(grabbed.pixels);
            for (uint32_t row = 0; row < grabbed.height; ++row) {
              std::fwrite(base + static_cast<size_t>(row) * grabbed.stride,
                          sizeof(uint8_t), static_cast<size_t>(grabbed.width) * 4, fp);
            }
            std::fclose(fp);
          }
          std::printf(
              "pu_frame_acquire #1: %ux%u stride=%u ts=%lld stream=%llu -> %s\n",
              grabbed.width, grabbed.height, grabbed.stride,
              static_cast<long long>(grabbed.timestamp_us),
              static_cast<unsigned long long>(grabbed.stream_id), bgra_path.c_str());
        }
        MaybeDumpFramePng(grabbed);
        pu_frame_release(from, &grabbed);
        if (acquired == 300) {
          PROCESS_MEMORY_COUNTERS counters;
          std::memset(&counters, 0, sizeof(counters));
          SIZE_T private_last = 0;
          if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            private_last = counters.PagefileUsage;
          }
          std::printf(
              "pu_frame acquire/release x300: ok=%d not_ready=%d failed=%d "
              "private_first=%llu private_last=%llu delta=%lld bytes\n",
              acquired, not_ready, failed,
              static_cast<unsigned long long>(private_first),
              static_cast<unsigned long long>(private_last),
              static_cast<long long>(static_cast<int64_t>(private_last) -
                                     static_cast<int64_t>(private_first)));
        }
      } else if (acq_rc == PU_ERR_TIMEOUT) {
        ++not_ready;
      } else {
        ++failed;
        if (failed == 1) {
          std::printf("FAIL: pu_frame_acquire rc=%d (%s)\n", acq_rc, pu_last_error(from));
        }
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const Options options = ParseArgs(argc, argv);

  std::printf("partyup media core · screen capture smoke\n");
  std::printf("  ABI version      : %d\n", PU_ABI_VERSION);
  std::printf("  planned duration : %d s\n", options.seconds);

  const std::string png_path = AbsolutePath(options.png_path);
  SetEnvironmentVariableA("PARTYUP_CAPTURE_DUMP_PNG", png_path.c_str());
  const std::string log_path = ConfigureLogFile();
  std::printf("  engine log       : %s\n", log_path.c_str());
  std::printf("  first frame png  : %s\n", png_path.c_str());

  // 必须在 pu_engine_create 之前初始化：WIC 以及引擎内部的 COM 组件都依赖它。
  ComGuard com;
  std::printf("  COM (main thread): %s\n", com.usable ? "ok" : "unavailable");
  if (!com.usable) std::printf("WARN: COM init failed on main thread; frame PNG will not be written\n");

  g_frame_png_path = AbsolutePath(options.frame_png_path);
  std::printf("  frame bgra png   : %s\n", g_frame_png_path.c_str());

  pu_config config;
  std::memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.client_name = "screen-capture-smoke";
  config.log_level = 3;
  config.event_queue_capacity = 1024;

  pu_engine* sharer = pu_engine_create(&config);
  if (sharer == nullptr) {
    std::printf("FAIL: pu_engine_create(sharer) returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }
  EngineGuard sharer_guard{sharer};

  pu_config viewer_config = config;
  viewer_config.client_name = "screen-capture-smoke-viewer";
  pu_engine* viewer = pu_engine_create(&viewer_config);
  if (viewer == nullptr) {
    std::printf("FAIL: pu_engine_create(viewer) returned null: %s\n", pu_last_error(nullptr));
    return 1;
  }
  EngineGuard viewer_guard{viewer};

  // ---- 1. 枚举源 ----
  pu_source_list sources;
  std::memset(&sources, 0, sizeof(sources));
  sources.struct_size = static_cast<uint32_t>(sizeof(sources));
  const int list_rc = pu_screen_sources(sharer, &sources);
  if (list_rc != PU_OK) {
    std::printf("FAIL: pu_screen_sources rc=%d (%s)\n", list_rc, pu_last_error(sharer));
    return 1;
  }
  std::printf("pu_screen_sources: %u source(s)\n", sources.count);
  if (sources.count == 0 || sources.items == nullptr) {
    std::printf("FAIL: pu_screen_sources returned no items\n");
    return 1;
  }

  // 立刻把 id/title 拷出来：下一次 pu_screen_sources / destroy 会回收这些指针。
  struct SourceCopy {
    std::string id;
    std::string title;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t kind = 0;
  };
  std::vector<SourceCopy> items;
  for (uint32_t i = 0; i < sources.count; ++i) {
    const pu_source_info& info = sources.items[i];
    SourceCopy copy;
    copy.id = info.id != nullptr ? info.id : "";
    copy.title = info.title != nullptr ? info.title : "";
    copy.width = info.width;
    copy.height = info.height;
    copy.kind = info.kind;
    items.push_back(copy);
    std::printf("  [%u] kind=%u id=%s %ux%u title=\"%s\"\n", i, copy.kind, copy.id.c_str(),
                copy.width, copy.height, copy.title.c_str());
  }

  // ---- 2. 选源 ----
  std::string chosen_id;
  SourceCopy chosen;
  if (!options.source_id.empty()) {
    for (const SourceCopy& item : items) {
      if (item.id == options.source_id || item.title.find(options.source_id) != std::string::npos) {
        chosen = item;
        chosen_id = item.id;
        break;
      }
    }
    if (chosen_id.empty()) {
      std::printf("FAIL: --source '%s' did not match any source\n", options.source_id.c_str());
      return 1;
    }
  } else {
    bool found_display = false;
    for (const SourceCopy& item : items) {
      if (item.kind == 0) {
        chosen = item;
        chosen_id = item.id;
        found_display = true;
        break;
      }
    }
    if (!found_display) {
      std::printf("FAIL: no real display (kind=0) in pu_screen_sources\n");
      return 1;
    }
  }
  std::printf("selected source : id=%s %ux%u\n", chosen_id.c_str(), chosen.width,
              chosen.height);

  // ---- 3. start ----
  const auto start_begin = std::chrono::steady_clock::now();
  const int start_rc = pu_screen_start(sharer, chosen_id.c_str());
  const auto start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_begin)
                            .count();
  if (start_rc != PU_OK) {
    std::printf("FAIL: pu_screen_start(%s) rc=%d (%s)\n", chosen_id.c_str(), start_rc,
                pu_last_error(sharer));
    return 1;
  }
  std::printf("pu_screen_start ok in %lld ms\n", static_cast<long long>(start_ms));

  // ---- 4. 采 N 秒 ----
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
  uint64_t frame_events = 0;
  uint64_t bytes_sum = 0;
  uint64_t bitrate_sum = 0;
  uint64_t bitrate_max = 0;
  uint64_t keyframes = 0;
  int error_events = 0;
  int state_events = 0;
  int viewer_remote_frames = 0;
  uint32_t frames_sent_last = 0;
  uint32_t frames_encoded_last = 0;
  int encoded_samples = 0;
  int encoded_growth = 0;
  bool saw_running = false;

  std::printf("sampling for %d s ...\n", options.seconds);
  const double cpu_begin = ProcessCpuSeconds();
  auto last_sample = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    // 观看方的事件只有一个消费者（本循环不 poll 它）：把它的 answer/candidate
    // 转给共享方。共享方的事件由下面主循环的 pu_poll_event 统一处理——同一队列
    // 两个消费者会互相偷事件（曾经因此把 offer 吞掉，对端永远连不上）。
    if (ForwardSignals(viewer, sharer, &viewer_remote_frames, "viewer->sharer") < 0) return 1;

    // 每秒采一次 stats：frames_encoded 要「持续增长」。
    if (std::chrono::steady_clock::now() - last_sample >= std::chrono::seconds(1)) {
      last_sample = std::chrono::steady_clock::now();
      pu_stats stats;
      std::memset(&stats, 0, sizeof(stats));
      stats.struct_size = static_cast<uint32_t>(sizeof(stats));
      if (pu_peer_stats(sharer, &stats) == PU_OK) {
        if (encoded_samples > 0 && stats.frames_encoded > frames_encoded_last) {
          ++encoded_growth;
        }
        ++encoded_samples;
        frames_encoded_last = stats.frames_encoded;
        frames_sent_last = stats.frames_sent;
        std::printf(
            "  t+%ds stats: peer=%d ice=%d sent=%u encoded=%u received=%u drops=%u\n",
            encoded_samples, stats.peer_state, stats.ice_state, stats.frames_sent,
            stats.frames_encoded, stats.frames_received, stats.event_drops);
      }
    }

    pu_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    const int rc = pu_poll_event(sharer, &event, 100);
    if (rc == PU_ERR_TIMEOUT) continue;
    if (rc != PU_OK) {
      std::printf("FAIL: sharer pu_poll_event rc=%d (%s)\n", rc, pu_last_error(sharer));
      return 1;
    }
    switch (event.type) {
      case PU_EVENT_FRAME: {
        const pu_frame_event& frame = event.data.frame;
        if (frame_events == 0) {
          std::printf("first frame event: sequence=%llu width=%u height=%u\n",
                      static_cast<unsigned long long>(frame.sequence), frame.width,
                      frame.height);
        }
        ++frame_events;
        bytes_sum += frame.bytes;
        bitrate_sum += frame.bitrate_kbps;
        bitrate_max = std::max<uint64_t>(bitrate_max, frame.bitrate_kbps);
        keyframes += frame.keyframe;
        break;
      }
      case PU_EVENT_STATE:
        ++state_events;
        if (event.data.state.state == PU_STREAM_RUNNING) saw_running = true;
        std::printf("state %s -> %s\n", StateName(event.data.state.previous_state),
                    StateName(event.data.state.state));
        break;
      case PU_EVENT_ERROR:
        ++error_events;
        std::printf("error event: code=%d message=%s\n", event.data.error.code,
                    event.data.error.message);
        break;
      case PU_EVENT_SIGNAL_OUT: {
        const pu_signal_event& signal = event.data.signal;
        if (pu_signal_in(viewer, signal.payload, signal.payload_len) != PU_OK) {
          std::printf("FAIL: pu_signal_in(viewer): %s\n", pu_last_error(viewer));
          return 1;
        }
        break;
      }
      case PU_EVENT_REMOTE_FRAME: {
        ++viewer_remote_frames;
        // ---- pu_frame_acquire / release 实测（v2.5 起是真实现）----
        static int acquired = 0;
        static int not_ready = 0;
        static int failed = 0;
        static SIZE_T private_first = 0;
        static uint32_t acq_w = 0;
        static uint32_t acq_h = 0;
        static uint32_t acq_stride = 0;
        pu_frame grabbed;
        std::memset(&grabbed, 0, sizeof(grabbed));
        grabbed.struct_size = static_cast<uint32_t>(sizeof(grabbed));
        const int acq_rc = pu_frame_acquire(viewer, "remote", &grabbed);
        if (acq_rc == PU_OK) {
          ++acquired;
          if (acquired == 1) {
            acq_w = grabbed.width;
            acq_h = grabbed.height;
            acq_stride = grabbed.stride;
            PROCESS_MEMORY_COUNTERS counters;
            std::memset(&counters, 0, sizeof(counters));
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
              private_first = counters.PagefileUsage;
            }
            const std::string bgra_path = AbsolutePath("frame-acquire.bgra");
            FILE* fp = std::fopen(bgra_path.c_str(), "wb");
            if (fp != nullptr) {
              const uint8_t* base = static_cast<const uint8_t*>(grabbed.pixels);
              for (uint32_t row = 0; row < grabbed.height; ++row) {
                std::fwrite(base + static_cast<size_t>(row) * grabbed.stride,
                            sizeof(uint8_t), static_cast<size_t>(grabbed.width) * 4, fp);
              }
              std::fclose(fp);
            }
            std::printf(
                "pu_frame_acquire #1: %ux%u stride=%u ts=%lld stream=%llu -> %s\n",
                grabbed.width, grabbed.height, grabbed.stride,
                static_cast<long long>(grabbed.timestamp_us),
                static_cast<unsigned long long>(grabbed.stream_id), bgra_path.c_str());
          }
          MaybeDumpFramePng(grabbed);
          pu_frame_release(viewer, &grabbed);
          if (acquired == 300) {
            PROCESS_MEMORY_COUNTERS counters;
            std::memset(&counters, 0, sizeof(counters));
            SIZE_T private_last = 0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
              private_last = counters.PagefileUsage;
            }
            std::printf(
                "pu_frame acquire/release x300: ok=%d not_ready=%d failed=%d "
                "private_first=%llu private_last=%llu delta=%lld bytes\n",
                acquired, not_ready, failed,
                static_cast<unsigned long long>(private_first),
                static_cast<unsigned long long>(private_last),
                static_cast<long long>(static_cast<int64_t>(private_last) -
                                       static_cast<int64_t>(private_first)));
          }
        } else if (acq_rc == PU_ERR_TIMEOUT) {
          ++not_ready;
        } else {
          ++failed;
          if (failed == 1) {
            std::printf("FAIL: pu_frame_acquire rc=%d (%s)\n", acq_rc,
                        pu_last_error(viewer));
          }
        }
        break;
      }
      default:
        break;
    }
  }

  // ---- 5. stop + stats ----
  pu_stats final_stats;
  std::memset(&final_stats, 0, sizeof(final_stats));
  final_stats.struct_size = static_cast<uint32_t>(sizeof(final_stats));
  const bool stats_ok = pu_peer_stats(sharer, &final_stats) == PU_OK;

  const int stop_rc = pu_screen_stop(sharer);
  std::printf("pu_screen_stop rc=%d\n", stop_rc);

  // ---- 6. 报告 ----
  std::printf("\n==== RESULT ====\n");
  std::printf("display        : %s (%ux%u)\n", chosen.title.c_str(), chosen.width,
              chosen.height);
  std::printf("frame events   : %llu in %d s (%.1f fps of PU_EVENT_FRAME)\n",
              static_cast<unsigned long long>(frame_events), options.seconds,
              static_cast<double>(frame_events) / static_cast<double>(options.seconds));
  if (frame_events > 0) {
    std::printf("encoded bytes  : avg %.1f B/frame over %llu frame(s)\n",
                static_cast<double>(bytes_sum) / static_cast<double>(frame_events),
                static_cast<unsigned long long>(frame_events));
    std::printf("bitrate        : avg %llu kbps, max %llu kbps, keyframes ~%llu\n",
                static_cast<unsigned long long>(bitrate_sum / frame_events),
                static_cast<unsigned long long>(bitrate_max),
                static_cast<unsigned long long>(keyframes));
  }
  if (stats_ok) {
    std::printf("stats (final)  : sent=%u encoded=%u received=%u peer=%d ice=%d\n",
                final_stats.frames_sent, final_stats.frames_encoded,
                final_stats.frames_received, final_stats.peer_state, final_stats.ice_state);
  }
  {
    // 观看方解码出来的分辨率：软件 VP8 在码率/帧率压力下会自动降分辨率
    // （kMaintainFramerate 的退化策略），所以这里必须报实测值，而不是采集尺寸。
    pu_stats viewer_stats;
    std::memset(&viewer_stats, 0, sizeof(viewer_stats));
    viewer_stats.struct_size = static_cast<uint32_t>(sizeof(viewer_stats));
    if (pu_peer_stats(viewer, &viewer_stats) == PU_OK) {
      std::printf("viewer decoded : %ux%u over %u frame(s)\n", viewer_stats.remote_width,
                  viewer_stats.remote_height, viewer_stats.frames_received);
    }
  }
  std::printf("viewer frames  : %d\n", viewer_remote_frames);
  std::printf("state events   : %d (saw_running=%s)\n", state_events,
              saw_running ? "yes" : "no");
  std::printf("encoded growth : %d increase(s) over %d sample(s)\n", encoded_growth,
              encoded_samples);
  {
    const double cpu_end = ProcessCpuSeconds();
    if (cpu_begin >= 0.0 && cpu_end >= 0.0) {
      const double wall_s = static_cast<double>(options.seconds);
      const double cpu_s = cpu_end - cpu_begin;
      const unsigned cores = ProcessorCount();
      std::printf("process cpu    : %.2f s over %.0f s wall = %.0f%% of one core, %.0f%% of %u cores\n",
                  cpu_s, wall_s, cpu_s / wall_s * 100.0,
                  cpu_s / (wall_s * static_cast<double>(cores)) * 100.0, cores);
    }
  }

  std::string summary;
  const bool have_summary = FindCaptureSummary(log_path, &summary);
  if (have_summary) {
    std::printf("capture summary: %s\n", summary.c_str());
    std::printf("capture fps    : %.2f\n", PickNumber(summary, "fps", 0.0));
    std::printf("stage avg ms   : capture=%.2f readback=%.2f convert=%.2f encode-wait=%.2f total=%.2f\n",
                PickNumber(summary, "interval_avg_ms", 0.0),
                PickNumber(summary, "readback_avg_ms", 0.0),
                PickNumber(summary, "convert_avg_ms", 0.0),
                PickNumber(summary, "push_avg_ms", 0.0),
                PickNumber(summary, "total_avg_ms", 0.0));
    std::printf("stage max ms   : capture=%.2f readback=%.2f convert=%.2f encode-wait=%.2f total=%.2f\n",
                PickNumber(summary, "interval_max_ms", 0.0),
                PickNumber(summary, "readback_max_ms", 0.0),
                PickNumber(summary, "convert_max_ms", 0.0),
                PickNumber(summary, "push_max_ms", 0.0),
                PickNumber(summary, "total_max_ms", 0.0));
    std::printf("dropped oldest : %d\n", PickInt(summary, "dropped", -1));
  } else {
    std::printf("capture summary: MISSING in %s\n", log_path.c_str());
  }

  {
    long png_bytes = -1;
    if (std::FILE* png = std::fopen(png_path.c_str(), "rb")) {
      std::fseek(png, 0, SEEK_END);
      png_bytes = std::ftell(png);
      std::fclose(png);
    }
    std::printf("png            : %s (%ld bytes)\n", png_path.c_str(), png_bytes);
    if (png_bytes <= 0) std::printf("WARN: first-frame PNG was not written\n");
  }
  {
    // pu_frame_acquire 首帧经 WIC 编出来的 PNG：尺寸就是 pu_frame 的 width/height，
    // 文件大小是判「真的编码了图像」还是「只建了个空文件」的最快证据。
    long frame_png_bytes = -1;
    if (!g_frame_png_path.empty()) {
      if (std::FILE* png = std::fopen(g_frame_png_path.c_str(), "rb")) {
        std::fseek(png, 0, SEEK_END);
        frame_png_bytes = std::ftell(png);
        std::fclose(png);
      }
    }
    std::printf("frame png      : %s (%ld bytes, written=%s)\n", g_frame_png_path.c_str(),
                frame_png_bytes, g_frame_png_done ? "yes" : "no");
    if (frame_png_bytes <= 0) std::printf("WARN: pu_frame BGRA PNG was not written\n");
  }

  // ---- 7. 判据 ----
  int failures = 0;
  if (!saw_running) {
    std::printf("FAIL: never saw PU_STREAM_RUNNING\n");
    ++failures;
  }
  if (frame_events < static_cast<uint64_t>(options.seconds) * 5) {
    std::printf("FAIL: only %llu frame event(s) in %d s (expected >= %d)\n",
                static_cast<unsigned long long>(frame_events), options.seconds,
                options.seconds * 5);
    ++failures;
  }
  if (encoded_growth < 2) {
    std::printf("FAIL: frames_encoded grew only %d time(s); expected >= 2\n", encoded_growth);
    ++failures;
  }
  if (!have_summary) {
    std::printf("FAIL: no ===CAPTURE_SUMMARY=== in engine log (did it take the WGC path?)\n");
    ++failures;
  }
  if (error_events != 0) {
    std::printf("FAIL: %d error event(s)\n", error_events);
    ++failures;
  }
  std::printf("================\n");
  if (failures != 0) {
    std::printf("RESULT: FAIL (%d)\n", failures);
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
