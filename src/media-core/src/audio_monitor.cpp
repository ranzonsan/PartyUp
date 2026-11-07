/* PartyUp 媒体核心 · 独立输入电平监测 Module（实现）
 *
 * 只做状态机、代次与时序；真正的 ADM 启动/停止/读回由 AudioMonitorPort 的实现
 * 负责（生产是 AudioDeviceController，测试是脚本化 fake）。
 */
#include "audio_monitor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace partyup {
namespace {

// 与 pu_voice_level 的「新鲜」口径一致：1 秒内没有新 PCM 就不算有效电平。
constexpr int64_t kFreshWindowMs = 1000;
constexpr uint32_t kMaxAmplitude = 32767;

}  // namespace

AudioMonitor::~AudioMonitor() {
  std::string ignored;
  (void)Stop(&ignored);
}

void AudioMonitor::SetLogSink(std::function<void(int, const std::string&)> sink) {
  std::lock_guard<std::mutex> lock(mu_);
  log_ = std::move(sink);
}

void AudioMonitor::LogLine(int level, const std::string& message) const {
  if (log_) log_(level, message);
}

void AudioMonitor::Bind(AudioMonitorPort* port) {
  std::lock_guard<std::mutex> lock(mu_);
  // nullptr is teardown-only detachment, after the owner has stopped/terminated
  // its ADM. It must work after a failed Stop without falsely reporting stopped.
  if (port == nullptr) {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    capturing_ = false;
    ClearMeterLocked();
    port_ = nullptr;
    return;
  }
  // 运行中换端口会让 Stop 停到错误的设备：只允许空闲时绑定。
  if (state_ != AudioMonitorState::kIdle || capturing_) return;
  port_ = port;
}

void AudioMonitor::SetVoiceOwnsCapture(bool owns) {
  voice_owns_capture_.store(owns, std::memory_order_relaxed);
}

bool AudioMonitor::VoiceOwnsCapture() const {
  return voice_owns_capture_.load(std::memory_order_relaxed);
}

uint64_t AudioMonitor::CaptureEpoch() const {
  return epoch_.load(std::memory_order_acquire);
}

void AudioMonitor::SetVoiceLevelEnabled(bool enabled) {
  // 进入语音：清掉上一轮残留的帧数/电平，避免上一段会话的峰值被当成本次读数。
  if (enabled) {
    voice_level_frames_.store(0, std::memory_order_relaxed);
    voice_level_peak_.store(0, std::memory_order_relaxed);
    voice_level_rms_.store(0, std::memory_order_relaxed);
    voice_level_last_ms_.store(0, std::memory_order_relaxed);
  }
  voice_level_enabled_.store(enabled, std::memory_order_relaxed);
}

void AudioMonitor::ClearMeterLocked() {
  valid_ = false;
  peak_ = 0;
  rms_ = 0;
  frames_ = 0;
  last_frame_ms_ = 0;
}

int AudioMonitor::Start(std::string* error) {
  AudioMonitorPort* port = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (voice_owns_capture_.load(std::memory_order_relaxed)) {
      last_error_ = "audio monitor: voice session owns the capture device";
      if (error != nullptr) *error = last_error_;
      return PU_ERR_STATE;
    }
    if (state_ == AudioMonitorState::kActive) {
      // 重复 Start：幂等，不重复开设备。
      if (error != nullptr) error->clear();
      return PU_OK;
    }
    if (port_ == nullptr) {
      last_error_ = "audio monitor: no audio device port is bound";
      if (error != nullptr) *error = last_error_;
      return PU_ERR_STATE;
    }
    port = port_;
    // 新采集代次：上一轮/上一条设备的迟到 PCM 立即失效。
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    capturing_ = true;
    ClearMeterLocked();
    state_ = AudioMonitorState::kIdle;  // 成功前不宣称 Active
  }

  std::string start_error;
  const int start_rc = port->StartInput(&start_error);
  const int running = port->InputRunning();
  {
    char line[256] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor: StartInput rc=%d running=%d error=%s", start_rc, running,
                  start_error.empty() ? "-" : start_error.c_str());
    LogLine(3, line);
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Start 期间可能被 voice 声明接管（防御性；正常由引擎串行）。
    if (voice_owns_capture_.load(std::memory_order_relaxed)) {
      capturing_ = false;
      ClearMeterLocked();
      state_ = AudioMonitorState::kIdle;
      last_error_ = "audio monitor: voice claimed the capture device during start";
      if (error != nullptr) *error = last_error_;
      return PU_ERR_STATE;
    }
    running_ = running;
    if (start_rc == 0 && running == 1) {
      state_ = AudioMonitorState::kActive;
      last_error_.clear();
      if (error != nullptr) error->clear();
      return PU_OK;
    }
    capturing_ = false;
    ClearMeterLocked();
    state_ = AudioMonitorState::kFaulted;
    last_error_ = start_error.empty() ? "audio monitor: input did not start" : start_error;
    if (error != nullptr) *error = last_error_;
    return PU_ERR_MEDIA;
  }
}

int AudioMonitor::Stop(std::string* error) {
  AudioMonitorPort* port = nullptr;
  bool voice_takeover = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // 幂等：没有由本 Module 启动的采集时，绝不触碰端口（尤其不能停 voice 的录音）。
    if (state_ == AudioMonitorState::kIdle && !capturing_) {
      if (error != nullptr) error->clear();
      return PU_OK;
    }
    if (voice_owns_capture_.load(std::memory_order_relaxed)) {
      // 输入已经由 voice 接管：停它等于切断通话录音，必须拒绝并如实置故障。
      epoch_.fetch_add(1, std::memory_order_acq_rel);
      capturing_ = false;
      ClearMeterLocked();
      state_ = AudioMonitorState::kFaulted;
      last_error_ =
          "audio monitor: capture is owned by the voice session; refusing to stop it";
      voice_takeover = true;
      port = port_;
    } else {
      port = port_;
      // 先推进代次并停用旧电平：从这一刻起，任何旧回调都不能再写入。
      epoch_.fetch_add(1, std::memory_order_acq_rel);
      capturing_ = false;
      ClearMeterLocked();
      state_ = AudioMonitorState::kIdle;
    }
  }

  if (voice_takeover) {
    const int running = port != nullptr ? port->InputRunning() : -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      running_ = running;
      if (error != nullptr) *error = last_error_;
    }
    return PU_ERR_STATE;
  }

  std::string stop_error;
  const int stop_rc = port != nullptr ? port->StopInput(&stop_error) : -1;
  const int running = port != nullptr ? port->InputRunning() : -1;
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = running;
    if (stop_rc == 0 && running == 0) {
      state_ = AudioMonitorState::kIdle;
      last_error_.clear();
      if (error != nullptr) error->clear();
      return PU_OK;
    }
    // Stop 调用失败或读回不能确认已停：保留 Faulted，并如实报告 running。
    // 不能只清 active 镜像就宣称停止，也不会自动重试/自动重新开采集。
    state_ = AudioMonitorState::kFaulted;
    ClearMeterLocked();
    std::string detail = stop_error.empty() ? "audio monitor: input stop failed" : stop_error;
    if (running == 1) {
      detail += "; the input is still running";
    } else if (running == 0) {
      detail += "; the readback reports the input is stopped";
    } else {
      detail += "; could not confirm whether the input stopped (running unknown)";
    }
    last_error_ = detail;
    if (error != nullptr) *error = detail;
    return PU_ERR_MEDIA;
  }
}

void AudioMonitor::Status(pu_audio_monitor_info* out) const {
  if (out == nullptr) return;
  const uint32_t struct_size = out->struct_size;
  std::memset(out, 0, sizeof(*out));
  out->struct_size = struct_size;
  std::lock_guard<std::mutex> lock(mu_);
  out->state = static_cast<int32_t>(state_);
  out->running = running_;
  out->frames = frames_;
  out->peak = peak_;
  out->rms = rms_;
  out->last_frame_ms = last_frame_ms_ > 0 ? static_cast<uint64_t>(last_frame_ms_) : 0;
  if (state_ == AudioMonitorState::kActive && frames_ > 0 && port_ != nullptr &&
      last_frame_ms_ > 0 && port_->NowMs() - last_frame_ms_ <= kFreshWindowMs) {
    out->valid = 1;
  }
}

void AudioMonitor::OnCapturePcm(uint64_t epoch, const void* samples, size_t frames,
                                size_t bytes_per_sample, size_t channels,
                                uint32_t sample_rate) {
  // 诊断：先记「observer 回调真的发生了」，再判断是否接受；两者必须分开看。
  const uint32_t seq = callback_total_.fetch_add(1, std::memory_order_relaxed);
  // v2.19：与内容无关的采集计数放在最前面——即使下面因为形状/静音被过滤，也必须增长。
  // 这是「采集链路活着」的判据，不是电平（全零 PCM 也算一帧）。
  raw_frames_.fetch_add(1, std::memory_order_relaxed);
  {
    const uint64_t bytes = static_cast<uint64_t>(frames) *
                           static_cast<uint64_t>(channels > 0 ? channels : 1) *
                           static_cast<uint64_t>(bytes_per_sample > 0 ? bytes_per_sample : 2);
    raw_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    raw_last_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count(),
                       std::memory_order_relaxed);
  }
  // 平台 ADM 传给 AudioTransport 的 bytes_per_sample 实测是**每帧字节数**
  // （立体声 int16 → 4），不是每样本字节数。真机实证：m152 + Windows 麦克风报
  // frames=480 ch=2 bps=4，而缓冲区真实只有 1920 B（=480×2×2，int16），第 480 个字
  // 之后全是未初始化垃圾。这里只接受能唯一确定格式的三种组合，其余整帧丢弃：
  //   (ch=1, bps=2) → int16；(ch=2, bps=2) → int16；(ch=2, bps=4) → int16（每帧字节）；
  //   (ch=2, bps=8) → 32 bit（float32/int32 以 float 读）。
  // (ch=1, bps=4) 无法区分「32 bit 单声道」与「两声道打包」，按契约丢弃、不猜。
  size_t sample_bytes = 0;
  if (channels == 1 && bytes_per_sample == 2) {
    sample_bytes = 2;
  } else if (channels == 2 && bytes_per_sample == 2) {
    sample_bytes = 2;
  } else if (channels == 2 && bytes_per_sample == 4) {
    sample_bytes = 2;
  } else if (channels == 2 && bytes_per_sample == 8) {
    sample_bytes = 4;
  }
  if (samples == nullptr || frames == 0 || sample_bytes == 0 || sample_rate == 0) {
    if (seq < 3) {
      char line[256] = {0};
      std::snprintf(line, sizeof(line),
                    "audio monitor: PCM callback #%u rejected shape samples=%p frames=%zu "
                    "bps=%zu ch=%zu rate=%u",
                    static_cast<unsigned>(seq) + 1, samples, frames, bytes_per_sample,
                    channels, sample_rate);
      LogLine(1, line);
    }
    return;
  }
  if (seq < 3 || (seq % 200) == 199) {
    char line[256] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor: PCM callback #%u frames=%zu ch=%zu bps=%zu(bpf) "
                  "sample_bytes=%zu rate=%u",
                  static_cast<unsigned>(seq) + 1, frames, channels, bytes_per_sample,
                  sample_bytes, sample_rate);
    LogLine(3, line);
  }
  const size_t count = frames * channels;
  uint32_t peak = 0;
  double sum_squares = 0.0;
  if (sample_bytes == 2) {
    const int16_t* pcm = static_cast<const int16_t*>(samples);
    for (size_t i = 0; i < count; ++i) {
      const int32_t value = pcm[i];
      const int32_t magnitude = value < 0 ? -value : value;
      if (magnitude > static_cast<int32_t>(peak)) peak = static_cast<uint32_t>(magnitude);
      sum_squares += static_cast<double>(value) * static_cast<double>(value);
    }
  } else {
    // float32 采集样本值域 [-1,1]；统一换算到 16 bit 幅度尺（0..32767）后再统计，
    // 这样对 ABI 的 peak/rms 语义不变。NaN/Inf 按 0 处理，不让一帧坏数据污染统计。
    const float* pcm = static_cast<const float*>(samples);
    for (size_t i = 0; i < count; ++i) {
      const float raw = pcm[i];
      const double scaled =
          std::isfinite(raw) ? static_cast<double>(raw) * 32767.0 : 0.0;
      const double clamped =
          scaled > 32767.0 ? 32767.0 : (scaled < -32768.0 ? -32768.0 : scaled);
      const double magnitude = std::fabs(clamped);
      if (magnitude > static_cast<double>(peak)) {
        peak = static_cast<uint32_t>(magnitude);
      }
      sum_squares += clamped * clamped;
    }
  }
  if (peak > kMaxAmplitude) peak = kMaxAmplitude;
  const double mean_square = sum_squares / static_cast<double>(count);
  const double rms_value = std::sqrt(mean_square);
  const uint32_t rms = rms_value >= static_cast<double>(kMaxAmplitude)
                           ? kMaxAmplitude
                           : static_cast<uint32_t>(rms_value);

  // v2.19：语音电平表（通话中 UI 的「我在说话」指示）。挂不到通话音频轨的 sink 上：
  // m152 的本地 audio track 从不调用 AddSink 注册的 sink（真机实测 calls=0），所以电平
  // 只能从**同一条 ADM 的采集回调**里算——不额外开录音栈、不影响 monitor。
  // 与 capture_* 的区别：这里真的做 peak/rms 计算，frames 计的是「电平表测量过的帧」，
  // 且只在 voice 会话期间开启；capture_* 是与内容无关的采集 liveness 计数。
  if (voice_level_enabled_.load(std::memory_order_relaxed)) {
    voice_level_peak_.store(peak, std::memory_order_relaxed);
    voice_level_rms_.store(rms, std::memory_order_relaxed);
    voice_level_last_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    voice_level_frames_.fetch_add(1, std::memory_order_relaxed);
  }

  // 计算在锁外完成；提交时只做「代次 + 状态」检查与几个字段的写入。
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t current_epoch = epoch_.load(std::memory_order_relaxed);
  if (!capturing_ || epoch != current_epoch || state_ == AudioMonitorState::kFaulted) {
    if (seq < 5) {
      char line[256] = {0};
      std::snprintf(line, sizeof(line),
                    "audio monitor: PCM dropped capturing=%d epoch_match=%d state=%d",
                    capturing_ ? 1 : 0, epoch == current_epoch ? 1 : 0,
                    static_cast<int>(state_));
      LogLine(2, line);
    }
    return;
  }
  if (frames_ != 0xFFFFFFFFu) ++frames_;
  peak_ = peak;
  rms_ = rms;
  last_frame_ms_ = port_ != nullptr ? port_->NowMs() : 0;
  valid_ = true;
}

void AudioMonitor::ReconcileInputRunning(bool running) {
  std::lock_guard<std::mutex> lock(mu_);
  // Idle 表示 monitor 没有在采集；此时外部（voice/设备事务）的运行状态不属于自己，
  // 不能把别人的录音算成 monitor 的 running。
  if (state_ == AudioMonitorState::kIdle) return;
  if (voice_owns_capture_.load(std::memory_order_relaxed)) return;
  running_ = running ? 1 : 0;
  if (running || state_ == AudioMonitorState::kFaulted) return;
  // Active 却被设备事务停掉了输入：进入 Faulted 并清电平，绝不自动重新开采集。
  epoch_.fetch_add(1, std::memory_order_acq_rel);
  capturing_ = false;
  ClearMeterLocked();
  state_ = AudioMonitorState::kFaulted;
  last_error_ = "audio monitor: the input stopped during a device transaction";
}

void AudioMonitor::OnInputRestarted() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ != AudioMonitorState::kActive) return;
  if (voice_owns_capture_.load(std::memory_order_relaxed)) return;
  // 设备重启后旧输入代次的 PCM 不能继续累计；新代次的回调会重新填满统计。
  epoch_.fetch_add(1, std::memory_order_acq_rel);
  ClearMeterLocked();
  running_ = 1;
}

}  // namespace partyup
