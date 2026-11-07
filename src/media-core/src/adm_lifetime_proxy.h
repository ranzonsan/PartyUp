/* PartyUp 媒体核心 · 共享 AudioDeviceModule 的引擎级生命周期守卫
 *
 * 契约（一句话可审查）：**platform ADM 由 MediaEngine 创建并持有，引擎析构时
 * Terminate 一次；PeerConnectionFactory 只借用、不拥有终止权。**
 *
 * 为什么需要这一层：m152 的 WebRtcVoiceEngine（AudioState，随 PeerConnection 建立/
 * 销毁）在析构时会调用 `adm()->Terminate()`。真机实证（2026-11-07）：Windows 平台 ADM
 * 被 Terminate 之后无法再 `InitRecording()`/`StartRecording()`（`initialized=1`、
 * `mic_initialized=1`，返回码恒 -1，重试与 Terminate+Init 复位均无效），于是
 * 「加入语音 → 离开 → 再次加入」的第二次会话麦克风采集会静默失效（入会不报错）。
 *
 * 做法：转发全部 AudioDeviceModule 调用，但把 `Terminate()` 变成**延迟终止**——语音
 * 引擎销毁只记一笔并返回 0（满足其契约），ADM 保持 initialized；只有引擎析构前
 * `SetAllowTerminate(true)` 之后才真正 Terminate 一次。这样：
 *   * ADM 是工厂级对象、进程级生命周期的唯一实例；
 *   * 会话级只回收 PC/发送轨/音源，ADM 的 initialized 状态跨会话复用；
 *   * 平台设备（麦克风句柄）在最后一次 Terminate 前保持打开但**不采集**，
 *     不会出现第二条录音栈。
 *
 * 只转发，不改变任何方法语义。
 */
#ifndef PARTYUP_ADM_LIFETIME_PROXY_H_
#define PARTYUP_ADM_LIFETIME_PROXY_H_

#include <atomic>
#include <optional>
#include <utility>

#include "api/audio/audio_device.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"

namespace partyup {

class AdmLifetimeProxy : public webrtc::AudioDeviceModule {
 public:
  static webrtc::scoped_refptr<AdmLifetimeProxy> Create(
      webrtc::scoped_refptr<webrtc::AudioDeviceModule> impl) {
    return webrtc::make_ref_counted<AdmLifetimeProxy>(std::move(impl));
  }

  explicit AdmLifetimeProxy(webrtc::scoped_refptr<webrtc::AudioDeviceModule> impl)
      : impl_(std::move(impl)) {}

  /* 必须公开：webrtc::make_ref_counted 用 RefCountedObject<T> 持有并析构本对象。 */
  ~AdmLifetimeProxy() override = default;

  /* 引擎析构前的唯一真实终止点。 */
  void SetAllowTerminate(bool allow) { allow_terminate_.store(allow); }
  int terminate_calls() const { return terminate_calls_.load(); }
  int suppressed_terminate_calls() const { return suppressed_terminate_calls_.load(); }

  // ---- AudioDeviceModule（纯转发；Terminate 见下） ----
  int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override {
    return impl_->ActiveAudioLayer(audioLayer);
  }
  int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
    return impl_->RegisterAudioCallback(audioCallback);
  }
  int32_t Init() override { return impl_->Init(); }
  int32_t Terminate() override {
    terminate_calls_.fetch_add(1);
    if (!allow_terminate_.load(std::memory_order_acquire)) {
      suppressed_terminate_calls_.fetch_add(1);
      return 0;  // 语音引擎随 PC 析构：只停音频，不拆引擎级 ADM
    }
    return impl_->Terminate();
  }
  bool Initialized() const override { return impl_->Initialized(); }

  int16_t PlayoutDevices() override { return impl_->PlayoutDevices(); }
  int16_t RecordingDevices() override { return impl_->RecordingDevices(); }
  int32_t PlayoutDeviceName(uint16_t index,
                            char name[webrtc::kAdmMaxDeviceNameSize],
                            char guid[webrtc::kAdmMaxGuidSize]) override {
    return impl_->PlayoutDeviceName(index, name, guid);
  }
  int32_t RecordingDeviceName(uint16_t index,
                              char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override {
    return impl_->RecordingDeviceName(index, name, guid);
  }

  int32_t SetPlayoutDevice(uint16_t index) override {
    return impl_->SetPlayoutDevice(index);
  }
  int32_t SetPlayoutDevice(WindowsDeviceType device) override {
    return impl_->SetPlayoutDevice(device);
  }
  int32_t SetRecordingDevice(uint16_t index) override {
    return impl_->SetRecordingDevice(index);
  }
  int32_t SetRecordingDevice(WindowsDeviceType device) override {
    return impl_->SetRecordingDevice(device);
  }

  int32_t PlayoutIsAvailable(bool* available) override {
    return impl_->PlayoutIsAvailable(available);
  }
  int32_t InitPlayout() override { return impl_->InitPlayout(); }
  bool PlayoutIsInitialized() const override { return impl_->PlayoutIsInitialized(); }
  int32_t RecordingIsAvailable(bool* available) override {
    return impl_->RecordingIsAvailable(available);
  }
  int32_t InitRecording() override { return impl_->InitRecording(); }
  bool RecordingIsInitialized() const override { return impl_->RecordingIsInitialized(); }

  int32_t StartPlayout() override { return impl_->StartPlayout(); }
  int32_t StopPlayout() override { return impl_->StopPlayout(); }
  bool Playing() const override { return impl_->Playing(); }
  int32_t StartRecording() override { return impl_->StartRecording(); }
  int32_t StopRecording() override { return impl_->StopRecording(); }
  bool Recording() const override { return impl_->Recording(); }

  int32_t InitSpeaker() override { return impl_->InitSpeaker(); }
  bool SpeakerIsInitialized() const override { return impl_->SpeakerIsInitialized(); }
  int32_t InitMicrophone() override { return impl_->InitMicrophone(); }
  bool MicrophoneIsInitialized() const override { return impl_->MicrophoneIsInitialized(); }

  int32_t SpeakerVolumeIsAvailable(bool* available) override {
    return impl_->SpeakerVolumeIsAvailable(available);
  }
  int32_t SetSpeakerVolume(uint32_t volume) override {
    return impl_->SetSpeakerVolume(volume);
  }
  int32_t SpeakerVolume(uint32_t* volume) const override {
    return impl_->SpeakerVolume(volume);
  }
  int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override {
    return impl_->MaxSpeakerVolume(maxVolume);
  }
  int32_t MinSpeakerVolume(uint32_t* minVolume) const override {
    return impl_->MinSpeakerVolume(minVolume);
  }

  int32_t MicrophoneVolumeIsAvailable(bool* available) override {
    return impl_->MicrophoneVolumeIsAvailable(available);
  }
  int32_t SetMicrophoneVolume(uint32_t volume) override {
    return impl_->SetMicrophoneVolume(volume);
  }
  int32_t MicrophoneVolume(uint32_t* volume) const override {
    return impl_->MicrophoneVolume(volume);
  }
  int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const override {
    return impl_->MaxMicrophoneVolume(maxVolume);
  }
  int32_t MinMicrophoneVolume(uint32_t* minVolume) const override {
    return impl_->MinMicrophoneVolume(minVolume);
  }

  int32_t SpeakerMuteIsAvailable(bool* available) override {
    return impl_->SpeakerMuteIsAvailable(available);
  }
  int32_t SetSpeakerMute(bool enable) override { return impl_->SetSpeakerMute(enable); }
  int32_t SpeakerMute(bool* enabled) const override { return impl_->SpeakerMute(enabled); }

  int32_t MicrophoneMuteIsAvailable(bool* available) override {
    return impl_->MicrophoneMuteIsAvailable(available);
  }
  int32_t SetMicrophoneMute(bool enable) override {
    return impl_->SetMicrophoneMute(enable);
  }
  int32_t MicrophoneMute(bool* enabled) const override {
    return impl_->MicrophoneMute(enabled);
  }

  int32_t StereoPlayoutIsAvailable(bool* available) const override {
    return impl_->StereoPlayoutIsAvailable(available);
  }
  int32_t SetStereoPlayout(bool enable) override { return impl_->SetStereoPlayout(enable); }
  int32_t StereoPlayout(bool* enabled) const override {
    return impl_->StereoPlayout(enabled);
  }
  int32_t StereoRecordingIsAvailable(bool* available) const override {
    return impl_->StereoRecordingIsAvailable(available);
  }
  int32_t SetStereoRecording(bool enable) override {
    return impl_->SetStereoRecording(enable);
  }
  int32_t StereoRecording(bool* enabled) const override {
    return impl_->StereoRecording(enabled);
  }

  int32_t PlayoutDelay(uint16_t* delayMS) const override {
    return impl_->PlayoutDelay(delayMS);
  }

  bool BuiltInAECIsAvailable() const override { return impl_->BuiltInAECIsAvailable(); }
  bool BuiltInAGCIsAvailable() const override { return impl_->BuiltInAGCIsAvailable(); }
  bool BuiltInNSIsAvailable() const override { return impl_->BuiltInNSIsAvailable(); }

  int32_t EnableBuiltInAEC(bool enable) override { return impl_->EnableBuiltInAEC(enable); }
  int32_t EnableBuiltInAGC(bool enable) override { return impl_->EnableBuiltInAGC(enable); }
  int32_t EnableBuiltInNS(bool enable) override { return impl_->EnableBuiltInNS(enable); }

  int32_t GetPlayoutUnderrunCount() const override {
    return impl_->GetPlayoutUnderrunCount();
  }
  std::optional<Stats> GetStats() const override { return impl_->GetStats(); }

 private:
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> impl_;
  std::atomic<bool> allow_terminate_{false};
  std::atomic<int> terminate_calls_{0};
  std::atomic<int> suppressed_terminate_calls_{0};
};

}  // namespace partyup

#endif  // PARTYUP_ADM_LIFETIME_PROXY_H_
