// PartyUp 媒体核心 · 硬件 H.264 编解码（Windows Media Foundation MFT）
//
// 为什么不用 libwebrtc 自带的 H.264：本仓库 vendored 的 webrtc.lib 没有编进
// OpenH264（`strings webrtc.lib | grep -i wels` 为空），所以 `VideoEncoderFactory
// Template<OpenH264Adapter>` 既不能编也不能解。Windows 侧的等价物是 Media
// Foundation 的 H.264 MFT：
//   * 编码：NVIDIA / Intel / AMD 的硬件 H.264 MFT（MFT_ENUM_FLAG_HARDWARE），
//     本机实测是 "NVIDIA H.264 Encoder MFT"（CLSID {60F44560-5A20-4857-BFEF-D29773CB8040}）；
//   * 解码：系统自带 "Microsoft H264 Video Decoder MFT"（软件解码，够观看方用）。
//
// 关键实现约定（全部由 docs/spikes/2026-11-02-hw-encoder.md 的探针实测得出）：
//   * 硬件 MFT 是 async（V2）MFT，**必须**在 `IMFTransform::GetAttributes()` 上设
//     `MFT_TRANSFORM_ASYNC_UNLOCK=TRUE` 才能 `MFT_MESSAGE_SET_D3D_MANAGER`
//     （在 IMFActivate 上设无效，会拿到 0xC00D6D77 MF_E_TRANSFORM_ASYNC_LOCKED）；
//   * 解锁后按 async 协议驱动：ProcessInput 推进去，收到 METransformHaveOutput
//     才 ProcessOutput 收**一个**包（提前收会拿 E_UNEXPECTED）；
//   * 1 进 1 出、无 B 帧：关键帧用 ICodecAPI 的 CODECAPI_AVEncVideoForceKeyFrame 请求，
//     输出样本用 MFSampleExtension_CleanPoint 判定（不自己拆 NAL、不自己解析码流）；
//   * 编码器跑在自己的线程上（MFT 创建/使用/销毁都在该线程，COM 单元也在该线程），
//     `Encode()` 只入队；这样 1440p 的 NVENC 延迟（本机 ~11 ms）不会卡住
//     libwebrtc 的编码队列，也能让 1 帧以上的帧在飞。
//
// 本文件不做日志落盘：调用方（media_engine.cpp）通过 HwEncoderStats 与本层
// 唯一的 stderr 兜底日志拿到状态；日志格式与 PARTYUP_MEDIA_LOG 由上层统一。
#ifndef PARTYUP_MEDIA_HW_ENCODER_H
#define PARTYUP_MEDIA_HW_ENCODER_H

#include <cstdint>
#include <memory>
#include <string>

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace partyup {

// 编码器实际用的实现。写进 pu_stats.encoder（复用 reserved 字段，ABI 布局不变）。
enum class H264EncoderKind : uint32_t {
  kNone = 0,       // 没有 H.264 编码器（只能软件 VP8）
  kHardwareMft = 1,  // 硬件 MFT（NVIDIA/Intel/AMD）
  kSoftwareMft = 2,  // 系统软件 H.264 MFT（同编码回退，保住 H.264 协商）
};

// 进程内探测一次的结果（缓存）。media_engine 在 Init 里调用并在日志里打出来，
// 冒烟测试也从这里读「硬件到底有没有」。
struct H264Availability {
  bool hardware = false;    // 硬件 H.264 编码 MFT 可用
  bool software = false;    // 软件 H.264 编码 MFT 可用（hardware 失败时的同编码回退）
  std::string hardware_name;  // 例如 "NVIDIA H.264 Encoder MFT"
  std::string software_name;
  std::string error;        // 硬件不可用的原因（枚举为空 / 全部激活失败）
};

// 探测本机 H.264 编码 MFT 能力。可重复调用（结果缓存，线程安全）。
const H264Availability& QueryH264Availability();

// 最近一次实际创建（InitEncode）成功的编码器类型。0 = 还没有编码器。
// media_engine 用它填 pu_stats.encoder。
H264EncoderKind LastCreatedEncoderKind();

// 是否至少有一种 H.264 编码器（hardware 或 software MFT）。false 时上层应只协商 VP8。
bool HasH264Encoder();

// 视频编码器工厂：H.264（MF，硬件优先、软件 MFT 回退）+ VP8（libvpx）。
// H.264 不可用时只返回 VP8 —— 这就是「硬件不可用回落软件 VP8」的落点。
std::unique_ptr<webrtc::VideoEncoderFactory> CreatePartyUpVideoEncoderFactory();

// 视频解码器工厂：H.264（MF H264 解码器 MFT）+ VP8（libvpx）。
// 观看方要能解 H.264，否则 1440p 的硬件编码路径没有对端可用。
std::unique_ptr<webrtc::VideoDecoderFactory> CreatePartyUpVideoDecoderFactory();

}  // namespace partyup

#endif  // PARTYUP_MEDIA_HW_ENCODER_H
