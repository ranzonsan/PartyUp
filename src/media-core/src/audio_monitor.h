/* PartyUp 媒体核心 · 独立输入电平监测 Module（内部头，不导出 ABI）
 *
 * 为什么单独一层：AUD02 要在**同一条通话 ADM** 上显式打开/关闭输入采集并读回真实
 * 电平，而不进入语音区、不建 PeerConnection、不另建录音栈。生产 adapter 把
 * AudioDeviceController（Stop->Set->Init->静音门禁->Start 的安全事务）和官方
 * CreateAudioDeviceWithDataObserver 接到这里；测试 adapter 注入脚本化 PCM。
 *
 * 线程模型：
 *   * Start/Stop/Status/Reconcile/SetVoiceOwnsCapture/Bind 只在控制线程调用；
 *   * OnCapturePcm 由 ADM 的音频线程调用，只计算小型统计数据并在极短的锁内提交，
 *     不调用设备、不做 I/O、不等待控制线程。
 *   * 采集代次（epoch）在每次 Start/Stop/Reset 时推进；observer 在回调时取
 *     CaptureEpoch() 并把 token 交给 OnCapturePcm，旧代次的迟到 PCM 被丢弃。
 *
 * 状态语义：state 是**请求状态**（Idle/Active/Faulted），running 是最近一次启停/
 * 读回得到的**实测状态**（-1 未知/0 否/1 是）。Stop 失败不会自动重试或自动重新
 * 打开采集；只有显式再次 Start/Stop 成功才离开 Faulted。
 */
#ifndef PARTYUP_MEDIA_SRC_AUDIO_MONITOR_H_
#define PARTYUP_MEDIA_SRC_AUDIO_MONITOR_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "partyup_media.h"

namespace partyup {

enum class AudioMonitorState : int32_t {
  kIdle = PU_AUDIO_MONITOR_IDLE,
  kActive = PU_AUDIO_MONITOR_ACTIVE,
  kFaulted = PU_AUDIO_MONITOR_FAULTED,
};

/* 生产/测试 adapter 看到的窄端口。实现方负责线程归属与真正的 ADM 调用：
 *   * 生产：AudioDeviceController（只操作录音方向、复用同一 ADM 与静音门禁）；
 *   * 测试：脚本化 fake，不打开任何设备。
 * 返回 0 表示成功，非 0 表示失败；错误细节写 error（可空）。
 * InputRunning：1=输入正在采集，0=没有采集，-1=无法确认。 */
class AudioMonitorPort {
 public:
  virtual ~AudioMonitorPort() = default;

  virtual int StartInput(std::string* error) = 0;
  virtual int StopInput(std::string* error) = 0;
  virtual int InputRunning() = 0;
  /* 单调时钟毫秒（与 std::chrono::steady_clock 同源；不随系统时间跳变）。 */
  virtual int64_t NowMs() = 0;
};

class AudioMonitor {
 public:
  AudioMonitor() = default;
  ~AudioMonitor();

  AudioMonitor(const AudioMonitor&) = delete;
  AudioMonitor& operator=(const AudioMonitor&) = delete;

  /* 绑定端口。必须在任何 Start/Status 之前调用；非空端口只能在 Idle 时重绑。
   * nullptr 专用于 owner 停止/Terminate ADM 后的析构脱离，Faulted 时亦允许；
   * 不再调用旧端口，不把缓存的仍运行/未知状态篡改为已停。 */
  void Bind(AudioMonitorPort* port);

  /* 显式开始/停止。Stop 无 out 参数，可重复调用；错误细节写 error（可空）。
   * Start：语音已占用时返回 PU_ERR_STATE 且不触碰端口；重复 Start（已 Active）
   *        返回 PU_OK 且不重复开设备。
   * Stop：Idle 时幂等返回 PU_OK 且不触碰端口（避免停掉 voice 的录音）；失败时
   *       进入 Faulted 并保留真实 running，只有显式重试成功才清故障。 */
  int Start(std::string* error);
  int Stop(std::string* error);

  /* 读快照；out 由调用方保证非空且 struct_size 合法（引擎层校验）。 */
  void Status(pu_audio_monitor_info* out) const;

  /* 引擎告诉 monitor「voice 现在拥有采集」/「voice 已释放采集」。Start 在 true 时
   * 拒绝；Stop 在 true 时不得停掉 input（那是 voice 的录音）。 */
  void SetVoiceOwnsCapture(bool owns);
  bool VoiceOwnsCapture() const;

  /* 采集代次：observer 回调时取它作为 token，Stop/Start 会推进。 */
  uint64_t CaptureEpoch() const;

  /* v2.19：与内容无关的 ADM 采集计数（每次 OnCapturePcm 都 +1，静音/全零也计）。
   * 这是「采集链路是否活着」的判据，**不是音量**：不要拿它做电平/dBFS 显示。 */
  uint32_t RawCallbackCount() const { return raw_frames_.load(std::memory_order_relaxed); }
  uint64_t RawBytes() const { return raw_bytes_.load(std::memory_order_relaxed); }
  int64_t RawLastCallbackMs() const { return raw_last_ms_.load(std::memory_order_relaxed); }

  /* v2.19：语音电平表开关（只在 voice 会话期间开）。开启后每次 ADM 采集回调都做
   * peak/rms 计算并累计「电平表测量过的帧数」；关闭后不再更新。
   * 为什么不做在通话音频轨的 sink 上：m152 的本地 audio track 从不调用 AddSink 注册的
   * sink（真机实测 calls=0，见 `.docs/.../aud02-voice-level-sink.log`），所以电平只能从
   * **同一条 ADM 的采集回调**里算——不额外开录音栈，也不影响 monitor。 */
  void SetVoiceLevelEnabled(bool enabled);
  uint32_t VoiceLevelFrames() const { return voice_level_frames_.load(std::memory_order_relaxed); }
  uint32_t VoiceLevelPeak() const { return voice_level_peak_.load(std::memory_order_relaxed); }
  uint32_t VoiceLevelRms() const { return voice_level_rms_.load(std::memory_order_relaxed); }
  int64_t VoiceLevelLastMs() const {
    return voice_level_last_ms_.load(std::memory_order_relaxed);
  }

  /* 诊断日志出口（与 AudioDeviceController::SetLogSink 同一模式）。
   * 真机上「start 成功但 frames 不涨」必须能区分三件事：ADM 根本没回调 observer、
   * 回调被代次/状态过滤、还是端口层面就没开起来。 */
  void SetLogSink(std::function<void(int, const std::string&)> sink);

  /* 官方 observer 的 OnCaptureData 转发入口。samples 指向 frames*channels 个采样。
   * 注意：ADM 透传的 bytes_per_sample 实测是**每帧字节数**（立体声 int16 = 4），
   * 本函数按 channels 还原每样本字节数（2 = int16，4 = 32 bit float/整型），再统计；
   * channels 只接受 1/2，frames>0，sample_rate>0，否则整帧丢弃（不猜测格式）。
   * token 与当前代次不符时丢弃。 */
  void OnCapturePcm(uint64_t epoch, const void* samples, size_t frames,
                    size_t bytes_per_sample, size_t channels, uint32_t sample_rate);

  /* 设备事务（pu_audio_status 的 Refresh / pu_audio_set_device）可能停掉输入。
   * 只用于 Active/Faulted 的 monitor：实测不再运行就进入 Faulted 并清电平，
   * **绝不自动重新开始采集**。Idle 时忽略（不能把 voice 的运行当成自己的）。 */
  void ReconcileInputRunning(bool running);

  /* 设备事务成功切换/重启输入后调用：保持 Active/running，但推进采集代次并清掉
   * 旧设备/旧会话的电平，旧回调的迟到 PCM 不再计入。 */
  void OnInputRestarted();

 private:
  void ClearMeterLocked();
  void LogLine(int level, const std::string& message) const;

  AudioMonitorPort* port_ = nullptr;
  mutable std::mutex mu_;
  AudioMonitorState state_ = AudioMonitorState::kIdle;
  int running_ = 0;
  std::atomic<bool> voice_owns_capture_{false};
  std::atomic<uint64_t> epoch_{0};
  /* 诊断：observer 回调总次数（含被过滤的），与 frames_（只计被接受的）分开。 */
  std::atomic<uint32_t> callback_total_{0};
  /* v2.19：与内容无关的采集计数（静音也增长；衡量 liveness，不是电平）。 */
  std::atomic<uint32_t> raw_frames_{0};
  std::atomic<uint64_t> raw_bytes_{0};
  std::atomic<int64_t> raw_last_ms_{0};
  /* v2.19：语音电平表（通话期间 peak/rms；数据来自同一条 ADM 的采集回调） */
  std::atomic<bool> voice_level_enabled_{false};
  std::atomic<uint32_t> voice_level_frames_{0};
  std::atomic<uint32_t> voice_level_peak_{0};
  std::atomic<uint32_t> voice_level_rms_{0};
  std::atomic<int64_t> voice_level_last_ms_{0};
  std::function<void(int, const std::string&)> log_;
  bool capturing_ = false;
  bool valid_ = false;
  uint32_t peak_ = 0;
  uint32_t rms_ = 0;
  uint32_t frames_ = 0;
  int64_t last_frame_ms_ = 0;
  std::string last_error_;
};

}  // namespace partyup

#endif  // PARTYUP_MEDIA_SRC_AUDIO_MONITOR_H_
