/* PartyUp 媒体核心 · AUD02 官方 ADM observer 的薄 adapter（仅生产/SDK 测试复用）
 *
 * 只做一件事：把 webrtc::AudioDeviceDataObserver::OnCaptureData 的**原始 PCM 与真实
 * 时间**转交给 AudioMonitor（16 bit int 或 32 bit float，格式由平台 ADM 决定；真机
 * 实测 m152 Windows ADM 交付 float32）。它不算电平、不解析格式、不碰设备状态；
 * 所有状态/代次/新鲜度判断都在 AudioMonitor 里（同一份实现被 fake 回归驱动）。
 *
 * 生命周期：本对象由 CreateAudioDeviceWithDataObserver 独占（unique_ptr），随 ADM
 * wrapper 一起析构；monitor_ 由 MediaEngine 持有，必须晚于 ADM wrapper 释放。
 * owner 在 Terminate 后先脱离端口，再释放 wrapper，最后销毁 monitor。
 * 线程：OnCaptureData 在 ADM 的采集线程上调用；monitor_->OnCapturePcm 只做小型
 * 线程安全统计，不阻塞控制线程、不做 I/O。
 */
#ifndef PARTYUP_MEDIA_SRC_AUDIO_MONITOR_OBSERVER_H_
#define PARTYUP_MEDIA_SRC_AUDIO_MONITOR_OBSERVER_H_

#include <cstddef>
#include <cstdint>

#include "audio_monitor.h"
#include "modules/audio_device/include/audio_device_data_observer.h"

namespace partyup {

class AudioMonitorDataObserver final : public webrtc::AudioDeviceDataObserver {
 public:
  explicit AudioMonitorDataObserver(AudioMonitor* monitor) : monitor_(monitor) {}

  void OnCaptureData(const void* audio_samples,
                     size_t num_samples,
                     size_t bytes_per_sample,
                     size_t num_channels,
                     uint32_t samples_per_sec) override {
    if (monitor_ == nullptr) return;
    // 代次在回调时读取；Stop/Start 推进代次后，这里拿到的旧 token 会被丢弃。
    monitor_->OnCapturePcm(monitor_->CaptureEpoch(), audio_samples, num_samples,
                           bytes_per_sample, num_channels, samples_per_sec);
  }

  // AUD02 只监测输入；不消费/不读取渲染路径。
  void OnRenderData(const void* audio_samples,
                    size_t num_samples,
                    size_t bytes_per_sample,
                    size_t num_channels,
                    uint32_t samples_per_sec) override {
    static_cast<void>(audio_samples);
    static_cast<void>(num_samples);
    static_cast<void>(bytes_per_sample);
    static_cast<void>(num_channels);
    static_cast<void>(samples_per_sec);
  }

 private:
  AudioMonitor* monitor_ = nullptr;
};

}  // namespace partyup

#endif  // PARTYUP_MEDIA_SRC_AUDIO_MONITOR_OBSERVER_H_
