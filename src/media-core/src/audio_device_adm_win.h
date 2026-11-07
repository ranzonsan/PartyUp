/* PartyUp 媒体核心 · 音频设备控制的生产端口（内部头）
 *
 * AdmAudioDevicePort 直接转发到**通话正在用的同一个** webrtc::AudioDeviceModule，
 * 不创建第二条录音栈；WasapiDefaultEndpointProbe 用 Windows CoreAudio 的
 * IMMDeviceEnumerator::GetDefaultAudioEndpoint(eConsole) 读取系统默认端点稳定 ID，
 * 与 ADM 的 WindowsDeviceType::kDefaultDevice 解析的是同一角色。
 *
 * 线程：与 AudioDeviceController 相同，只在 media core 的 signaling 线程上调用。
 */
#ifndef PARTYUP_MEDIA_SRC_AUDIO_DEVICE_ADM_WIN_H_
#define PARTYUP_MEDIA_SRC_AUDIO_DEVICE_ADM_WIN_H_

#include <functional>
#include <string>
#include <vector>

#include "audio_device_control.h"
#include "audio_monitor.h"

namespace webrtc {
class AudioDeviceModule;
class Thread;
}  // namespace webrtc

namespace partyup {

/* 把 webrtc::AudioDeviceModule 的枚举/选择/启停/静音接口映射成 AudioDevicePort。
 * adm 指针由 MediaEngine 持有，生命周期必须覆盖本对象（引擎析构时先停控制器）。 */
class AdmAudioDevicePort final : public AudioDevicePort {
 public:
  explicit AdmAudioDevicePort(webrtc::AudioDeviceModule* adm) : adm_(adm) {}

  // 诊断日志出口（与 AudioDeviceController::SetLogSink 同一模式）。ADM 的枚举失败
  // 在原生层是静默的（RecordingDevices() 只返回 -1），必须落进 native log 才能区分
  // 「ADM 还没 Init」与「平台枚举失败」。
  void SetLogSink(std::function<void(int, const std::string&)> sink);

  int Enumerate(AudioDeviceDirection dir, std::vector<AudioDeviceEntry>* out) override;
  int SetDevice(AudioDeviceDirection dir, int32_t index) override;
  bool IsInitialized(AudioDeviceDirection dir) const override;
  bool IsStarted(AudioDeviceDirection dir) const override;
  int Stop(AudioDeviceDirection dir) override;
  int Init(AudioDeviceDirection dir) override;
  int Start(AudioDeviceDirection dir) override;
  int SetMuted(AudioDeviceDirection dir, bool muted) override;
  int GetMuted(AudioDeviceDirection dir, bool* muted) override;

 private:
  void LogLine(int level, const std::string& message) const;
  // m152 的 AudioDeviceModuleImpl 把 ADM 已 Init() 当作设备枚举/选择/启停的前置条件；
  // 首次语音 join 时生产工厂还没建过 PeerConnection，ADM 仍是未初始化状态。这里按
  // ADM 官方生命周期补一次 Init（幂等；已初始化直接返回 0）。
  int EnsureInitialized() const;

  webrtc::AudioDeviceModule* adm_ = nullptr;
  std::function<void(int, const std::string&)> log_;
};

/* 系统默认端点的稳定 ID（Windows CoreAudio 端点 ID 字符串）。
 * 失败/无默认设备时返回空串；控制层据此退化为「只按枚举变化判断」。 */
class WasapiDefaultEndpointProbe final : public AudioDefaultEndpointProbe {
 public:
  std::string DefaultEndpointId(AudioDeviceDirection dir) override;
};

/* AUD02：AudioMonitorPort 的生产 adapter。只把「启停输入/读回/时钟」转发到同一个
 * AudioDeviceController（进而到同一条通话 ADM）；不创建第二条录音栈、不碰输出。
 * controller_/adm_ 由 MediaEngine 持有，生命周期必须覆盖本对象；signaling_ 用于
 * 把 ADM 调用投递到与 ADM 创建/Terminate 相同的线程（已在 signaling 线程时直接调用，
 * 避免自锁）。 */
class AdmAudioMonitorPort final : public AudioMonitorPort {
 public:
  AdmAudioMonitorPort(webrtc::AudioDeviceModule* adm,
                      AudioDeviceController* controller,
                      webrtc::Thread* signaling);

  int StartInput(std::string* error) override;
  int StopInput(std::string* error) override;
  int InputRunning() override;
  int64_t NowMs() override;

  /* 诊断日志出口（与 AdmAudioDevicePort 同一模式）：把「monitor 用的到底是哪块
   * ADM、StartInput 之后 ADM 自报的 initialized/recording 是什么」落进 native log。 */
  void SetLogSink(std::function<void(int, const std::string&)> sink);

  /* ADM 调用线程覆盖。共享 ADM 在语音会话期间由工厂的 WebRtcVoiceEngine 在
   * **worker 线程**上 Init()/Terminate()；之后再从 signaling 线程调 InitRecording
   * 会失败。工厂可用后把这里指向 worker 线程，monitor 的 ADM 调用就与语音同线程。 */
  void SetAdmThread(webrtc::Thread* thread);

  /* AUD02 post-voice 重建：把本端口改指向一块新建的 platform ADM 与其控制器。
   * 只在共享 ADM 已不可用、且语音没有占用录音方向时调用（由 MediaEngine 把关）。 */
  void Rebind(webrtc::AudioDeviceModule* adm, AudioDeviceController* controller,
              webrtc::Thread* thread);

 private:
  void LogLine(int level, const std::string& message) const;
  webrtc::Thread* AdmThread() const {
    return adm_thread_ != nullptr ? adm_thread_ : signaling_;
  }

  webrtc::AudioDeviceModule* adm_ = nullptr;
  AudioDeviceController* controller_ = nullptr;
  webrtc::Thread* signaling_ = nullptr;
  webrtc::Thread* adm_thread_ = nullptr;
  std::function<void(int, const std::string&)> log_;
};

}  // namespace partyup

#endif  // PARTYUP_MEDIA_SRC_AUDIO_DEVICE_ADM_WIN_H_
