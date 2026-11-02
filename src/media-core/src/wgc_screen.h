// PartyUp 媒体核心 · 真实屏幕采集（Windows Graphics Capture）
//
// 这一层只做「显示器 → I420 帧」这一段，不碰 libwebrtc 的 PeerConnection：
// 媒体核心（media_engine.cpp）负责把帧喂给 VideoTrack。分工的理由是收集采的
// 坑（D3D11 设备、COM 单元、FrameArrived 的 agile 委托、staging 复用）与信令
// / 编码无关，放在一个文件里既能单独量耗时，也方便将来换成窗口捕获。
//
// 关键约定（全部来自 spikes/wgc-probe 与 docs/spikes/2026-11-02-wgc-capture.md）：
//   * 显示器身份用 HMONITOR，**不要**用 GraphicsCaptureItem::DisplayName ——「显示 2」
//     和 \\.\DISPLAY1 的编号对不上，拿它匹配会张冠李戴；
//   * FrameArrived 的委托必须混入 Microsoft::WRL::FtmBase（否则 0x8000001C）；
//   * 帧池用 CreateFreeThreaded（不需要 dispatcher / 消息泵）；
//   * staging 纹理只建一次并复用，不每帧 CreateTexture2D；
//   * 帧到达是 DWM 节奏（本机实测 ~21 ms，抖动到 55 ms），不能按 16.7 ms 节拍取。
//
// 本层不做任何日志：每帧分段耗时通过 CaptureStageTimings 交给调用方，由媒体核心
// 用它自己的 PARTYUP_MEDIA_LOG 通道落盘（跨线程排序、引擎地址都在那一层）。
#ifndef PARTYUP_MEDIA_WGC_SCREEN_H
#define PARTYUP_MEDIA_WGC_SCREEN_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"

// 只做前向声明：公开头文件不把 <d3d11.h> 拖进每个使用方，纹理 sink 只需要句柄。
struct ID3D11Texture2D;

namespace partyup {

// 一台可采集的显示器。id 是稳定标识（HMONITOR 的十六进制），不是设备名。
struct ScreenTargetInfo {
  std::string id;      // "display:0x0000000000050001"
  std::string device;  // "\\.\DISPLAY1"（只作别名/展示，不作主键）
  std::string title;   // UTF-8，人类可读："\\.\DISPLAY1 · 2560x1440 · primary"
  uint32_t width = 0;
  uint32_t height = 0;
  bool primary = false;
};

// 枚举本机显示器：Identity 取 EnumDisplayMonitors 的 HMONITOR，尺寸/位置取 DXGI
// 输出的物理像素（GetMonitorInfo 在非 DPI-aware 线程上是缩放后的逻辑坐标）。
// 主屏排在最前，其余按设备名排序，保证列表稳定。
std::vector<ScreenTargetInfo> EnumerateScreenTargets(std::string* error);

// IGraphicsCaptureSessionStatics::IsSupported()。失败时 error 里带 HRESULT 文本。
bool IsWgcCaptureSupported(std::string* error);

// 采集输出模式。默认 I420（现状路径）；Nv12Texture 是零拷贝第一步，只在显式
// 开启（SetOutputMode 或 PARTYUP_CAPTURE_TEXTURE=1）时使用，出问题可立刻切回。
enum class CaptureOutput {
  kI420 = 0,        // BGRA → staging 读回 → libyuv ARGBToI420
  kNv12Texture = 1  // BGRA → VideoProcessorBlt → NV12 纹理（不回读）
};

// 纹理路径交给 sink 的一帧。
//
// **生命周期契约（必须遵守）**：
//   * texture 指向采集层 NV12 纹理池里的一张，**只在 sink 回调执行期间有效**；
//   * 回调返回后所有权回采集层，池轮转复用 —— 绝不能把指针存下来跨回调使用；
//   * 实现顺序是「Blt 进池纹理 → 调用 sink → sink 返回后才 Close WGC 原始帧」，
//     所以 sink 里看到的像素已是池里的副本，不可能拿到已失效的 WGC 句柄。
struct Nv12TextureFrame {
  ID3D11Texture2D* texture = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t capture_us = 0;
  double blt_ms = 0.0;  // 本帧 Blt 段耗时（RunTexture 在调用前后测得）
};

// RunTexture() 的每帧回调。返回 false 表示立刻停止采集。
using TextureSink = std::function<bool(const Nv12TextureFrame&)>;

// 每帧的分段耗时（毫秒）。口径与探针一致，方便横向对比历史数字：
//   interval  = 本帧到达与上一帧到达的间隔（DWM 的产帧节奏，不是我们花的钱）
//   readback  = CopyResource + Map（等 GPU 把 staging 写完，含 PCIe 回读）
//   convert   = libyuv ARGBToI420 + Unmap
//   blt       = VideoProcessorBlt（纹理路径；无回读、无 CPU 颜色转换）
//   push      = 交给编码器（source_->PushFrame → VideoBroadcaster → VideoStreamEncoder）
//   total     = readback + convert + push（I420）/ blt + push（纹理）
struct CaptureStageTimings {
  double interval_ms = 0.0;
  double readback_ms = 0.0;
  double convert_ms = 0.0;
  double blt_ms = 0.0;
  double push_ms = 0.0;
  double total_ms = 0.0;
};

// 一路 WGC 采集。生命周期全部在调用方（媒体核心）的采集线程上：
//   Create() → Open() → [WaitFirstFrame()] → Run() → Close() → 析构
// 只有 RequestStop() 允许从别的线程调用（置标志 + 唤醒 Run 的条件变量）。
class WgcCapture {
 public:
  // 每帧回调：Run() 所在的采集线程上调用。返回 false 表示立刻停止（等价于
  // RequestStop），用来实现「sink 端失败就收摊」。
  using FrameSink = std::function<bool(const webrtc::VideoFrame&)>;

  // source_id 形如 "display:0x...0001"；也接受 "\\.\DISPLAY1" 这类设备名别名。
  // 解析失败（格式不对/没有这台显示器）返回 nullptr 并写 error。
  static std::unique_ptr<WgcCapture> Create(const std::string& source_id, std::string* error);
  ~WgcCapture();

  WgcCapture(const WgcCapture&) = delete;
  WgcCapture& operator=(const WgcCapture&) = delete;

  // 建 D3D11 设备 + WinRT Direct3D 设备 + capture item + 3 缓冲 free-threaded
  // 帧池 + session + FrameArrived。必须在将调用 Run() 的线程上调用（COM 单元与
  // D3D 上下文都归属该线程）。
  bool Open(std::string* error);

  // 等第一帧（StartCapture 到首帧到达）。Open() 成功后调用；超时返回 false 并写
  // error —— 媒体核心据此让 pu_screen_start 明确失败，而不是「起来了但永远黑屏」。
  bool WaitFirstFrame(int timeout_ms, std::string* error);

  // 每帧完成后的分段耗时回调（在 sink 返回之后触发，因此 push/total 是完整的；
  // 也在采集线程上）。媒体核心用它做日志与汇总，传 nullptr 表示不关心耗时。
  using TimingSink = std::function<void(const CaptureStageTimings&)>;

  // 事件驱动的主循环：取帧 → staging 读回 → BGRA→I420（libyuv）→ sink。
  // stop_requested 由媒体核心的停止标志提供（可为 nullptr），RequestStop() 也能
  // 让它退出。返回时已经把 WGC 会话关干净（见 Close）。
  void Run(const FrameSink& sink, const TimingSink& on_timing,
           const std::atomic<bool>* stop_requested);

  // 线程安全：置停止标志并唤醒 Run（Run 在自己的线程上做真正的收尾）。
  void RequestStop();

  // 关掉 WGC 会话并释放 D3D/WinRT 对象。Run() 返回前已经调用过，这里是给
  // 「Open 成功但首帧超时」这类提前失败的路径用的；重复调用是安全的。
  // 必须与 Open() 在同一个线程上调用（COM 单元/设备上下文的归属线程）。
  void Close();

  uint32_t width() const;
  uint32_t height() const;
  // 因为「来不及处理」被丢掉的最旧帧数（帧池 3 缓冲，队列上限也是 3）。
  uint64_t dropped_frames() const;

  // ---- 纹理路径（零拷贝第一步）----
  // 选择输出模式。必须在 Open() 之前调用；默认 kI420（行为与旧版一致）。
  // PARTYUP_CAPTURE_TEXTURE=1 也会在 Open() 里切到 kNv12Texture，但显式调用优先。
  void SetOutputMode(CaptureOutput mode);
  CaptureOutput output_mode() const;
  // 纹理路径是否可用（Open() 里初始化成功）。I420 路径不受影响。
  bool texture_ready() const;
  // 纹理路径里转换失败而被跳过的帧数（不阻塞采集线程）。
  uint64_t texture_drops() const;

  // 事件驱动主循环（纹理路径）：取帧 → Blt 进 NV12 池纹理 → sink。
  // 与 Run() 共用队列/停止/间隔逻辑，只在出帧处换成纹理转换。
  // 必须先在 Open() 前 SetOutputMode(kNv12Texture)；否则立即返回。
  void RunTexture(const TextureSink& sink, const TimingSink& on_timing,
                  const std::atomic<bool>* stop_requested);
  // Run 期间出过的错误（Map/GetInterface 失败等）；空表示没出错。
  const std::string& last_error() const;
  // PARTYUP_CAPTURE_DUMP_PNG 指定的首帧回写路径（转换后 I420 → BGRA → PNG），
  // 空表示没要求落盘。用于人工确认「不是黑屏/颜色没错乱」。
  const std::string& dumped_png_path() const;

 private:
  WgcCapture();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace partyup

#endif  // PARTYUP_MEDIA_WGC_SCREEN_H
