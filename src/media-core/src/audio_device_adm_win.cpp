/* PartyUp 媒体核心 · 音频设备控制的生产端口（实现）
 *
 * 只做转发，不复制 ADM 的状态机逻辑；顺序（Stop->Set->Init->Start）、稳定 ID
 * 重映射与回滚都在 AudioDeviceController 里。
 */
#include "audio_device_adm_win.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <objbase.h>

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <utility>

#include "api/audio/audio_device.h"
#include "rtc_base/thread.h"

namespace partyup {
namespace {

// ADM 的对象归属线程与 controller 一致：在 signaling 线程上调用。已经在 signaling
// 线程时（例如引擎析构里那次 BlockingCall）直接执行，避免 BlockingCall 自锁。
template <typename Functor>
void RunOnSignaling(webrtc::Thread* signaling, Functor&& functor) {
  if (signaling != nullptr && !signaling->IsCurrent()) {
    signaling->BlockingCall(std::forward<Functor>(functor));
  } else {
    std::forward<Functor>(functor)();
  }
}

const char* DirectionLabel(AudioDeviceDirection dir) {
  return dir == AudioDeviceDirection::kInput ? "input" : "output";
}

std::string WideToUtf8(LPCWSTR text) {
  if (text == nullptr) return std::string();
  const int bytes =
      ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return std::string();
  std::string out(static_cast<size_t>(bytes - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], bytes, nullptr, nullptr);
  return out;
}

/* 每次查询自带 COM 初始化/释放：signaling 线程可能已被 ADM 初始化成 MTA
 * （CoInitializeEx 返回 S_FALSE，仍要配对 CoUninitialize）；若是 STA，
 * CoCreateInstance 与查询都在同一线程上完成，接口不跨线程。 */
class ScopedComInitialization {
 public:
  ScopedComInitialization() : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ScopedComInitialization() {
    if (SUCCEEDED(hr_)) ::CoUninitialize();
  }
  ScopedComInitialization(const ScopedComInitialization&) = delete;
  ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

  bool usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_;
};

}  // namespace

void AdmAudioDevicePort::SetLogSink(std::function<void(int, const std::string&)> sink) {
  log_ = std::move(sink);
}

void AdmAudioDevicePort::LogLine(int level, const std::string& message) const {
  if (log_) log_(level, message);
}

int AdmAudioDevicePort::EnsureInitialized() const {
  if (adm_ == nullptr) return -1;
  if (adm_->Initialized()) return 0;
  // m152 的 AudioDeviceModuleImpl 把「ADM 已 Init()」写成设备枚举/选择/启停的前置
  // 条件：AudioDeviceModuleImpl::RecordingDevices()/PlayoutDevices() 在 _initialized
  // 为 false 时**直接返回 -1**，根本不会转发到平台的 WASAPI 枚举。生产工厂要到
  // 第一个 PeerConnection 的 AudioState/WebRtcVoiceEngine::Init 才第一次调 ADM::Init，
  // 而 v2.15 的设备选择要求在「建 PC / 音频启动之前」枚举并写入 ADM——于是在第一次
  // 语音 join 时枚举必然 -1（真机表现为 pu_voice_join 返回 -5）。这里按 ADM 官方
  // 生命周期补一次 Init：已初始化时是 no-op；工厂随后的 Init 同样是 no-op；
  // Terminate 仍由引擎析构/工厂释放配对，之后再用会重新走到这里。
  const int rc = adm_->Init();
  if (rc != 0) {
    LogLine(1, "audio: ADM Init() failed for device control, rc=" + std::to_string(rc));
    return rc;
  }
  LogLine(3, "audio: ADM initialized on demand (device control before the first PeerConnection)");
  return 0;
}

int AdmAudioDevicePort::Enumerate(AudioDeviceDirection dir,
                                  std::vector<AudioDeviceEntry>* out) {
  if (out == nullptr) return -1;
  out->clear();
  if (adm_ == nullptr) {
    LogLine(1, "audio: ADM enumerate: audio device module is null");
    return -1;
  }
  // 记录「补 Init 之前」的状态：修复前这里就是 initialized=0 raw=-1 的现场。
  const bool initialized_before = adm_->Initialized();
  if (EnsureInitialized() != 0) {
    LogLine(1, std::string("audio: ADM enumerate ") + DirectionLabel(dir) +
                   ": ADM is not initialized (init failed)");
    return -1;
  }
  const int count =
      dir == AudioDeviceDirection::kInput ? adm_->RecordingDevices() : adm_->PlayoutDevices();
  // 观测点：v2.18 真机入会 -5 的现场。m152 的 AudioDeviceModuleImpl::RecordingDevices()
  // 在 ADM 尚未 Init() 时**直接返回 -1**（反汇编里 _initialized==false 走 0xffff 分支），
  // 与平台枚举无关；这条日志把「ADM 未初始化」和「平台枚举真的失败」分开。
  LogLine(count < 0 ? 1 : 3,
          std::string("audio: ADM enumerate ") + DirectionLabel(dir) +
              ": initialized(before ensure)=" + (initialized_before ? "1" : "0") +
              " raw=" + std::to_string(count));
  if (count < 0) return count;
  out->reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    AudioDeviceEntry entry;
    entry.index = i;
    char name[webrtc::kAdmMaxDeviceNameSize] = {0};
    char guid[webrtc::kAdmMaxGuidSize] = {0};
    const int rc = dir == AudioDeviceDirection::kInput
                       ? adm_->RecordingDeviceName(static_cast<uint16_t>(i), name, guid)
                       : adm_->PlayoutDeviceName(static_cast<uint16_t>(i), name, guid);
    if (rc == 0) {
      entry.name = name;
      entry.guid = guid;
    }
    out->push_back(std::move(entry));
  }
  return static_cast<int>(out->size());
}

int AdmAudioDevicePort::SetDevice(AudioDeviceDirection dir, int32_t index) {
  if (adm_ == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  int rc = 0;
  if (index < 0) {
    // 系统默认必须走 ADM 的 WindowsDeviceType::kDefaultDevice（Windows 下解析为
    // GetDefaultAudioEndpoint(..., eConsole)），不能映射成枚举 0——0 只是当前
    // 设备集合里的第一个具体设备。
    rc = dir == AudioDeviceDirection::kInput
             ? adm_->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultDevice)
             : adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
  } else {
    rc = dir == AudioDeviceDirection::kInput
             ? adm_->SetRecordingDevice(static_cast<uint16_t>(index))
             : adm_->SetPlayoutDevice(static_cast<uint16_t>(index));
  }
  // 诊断点：设备选择是 monitor start 事务里容易静默失败的一步（ADM 只在未初始化时接受）。
  LogLine(rc == 0 ? 3 : 1,
          std::string("audio: ADM set ") +
              (dir == AudioDeviceDirection::kInput ? "recording" : "playout") +
              " device index=" + std::to_string(index) + " rc=" + std::to_string(rc));
  return rc;
}

bool AdmAudioDevicePort::IsInitialized(AudioDeviceDirection dir) const {
  if (adm_ == nullptr) return false;
  return dir == AudioDeviceDirection::kInput ? adm_->RecordingIsInitialized()
                                             : adm_->PlayoutIsInitialized();
}

bool AdmAudioDevicePort::IsStarted(AudioDeviceDirection dir) const {
  if (adm_ == nullptr) return false;
  return dir == AudioDeviceDirection::kInput ? adm_->Recording() : adm_->Playing();
}

int AdmAudioDevicePort::Stop(AudioDeviceDirection dir) {
  if (adm_ == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  const int rc = dir == AudioDeviceDirection::kInput ? adm_->StopRecording()
                                                     : adm_->StopPlayout();
  if (dir == AudioDeviceDirection::kInput) {
    char state[256] = {0};
    std::snprintf(state, sizeof(state),
                  "audio: ADM StopRecording rc=%d initialized=%d recording_initialized=%d "
                  "recording=%d mic_initialized=%d",
                  rc, adm_->Initialized() ? 1 : 0, adm_->RecordingIsInitialized() ? 1 : 0,
                  adm_->Recording() ? 1 : 0, adm_->MicrophoneIsInitialized() ? 1 : 0);
    LogLine(3, state);
  }
  return rc;
}

int AdmAudioDevicePort::Init(AudioDeviceDirection dir) {
  if (adm_ == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  int rc = dir == AudioDeviceDirection::kInput ? adm_->InitRecording() : adm_->InitPlayout();
  if (rc != 0 && dir == AudioDeviceDirection::kInput) {
    // 诊断 + 自愈：平台 ADM 在「语音会话 Terminate -> 再次 Init」之后，InitRecording
    // 可能因为麦克风对象还没重建而返回 -1。公开 API 里麦克风是独立一步，这里显式
    // InitMicrophone 后重试一次，并把失败时的 ADM 自报状态落盘。
    char state[256] = {0};
    std::snprintf(state, sizeof(state),
                  "audio: ADM InitRecording rc=%d initialized=%d recording_initialized=%d "
                  "mic_initialized=%d",
                  rc, adm_->Initialized() ? 1 : 0, adm_->RecordingIsInitialized() ? 1 : 0,
                  adm_->MicrophoneIsInitialized() ? 1 : 0);
    LogLine(1, state);
    const int mic_rc = adm_->InitMicrophone();
    LogLine(2, "audio: ADM InitMicrophone rc=" + std::to_string(mic_rc));
    rc = adm_->InitRecording();
    LogLine(rc == 0 ? 3 : 1,
            "audio: ADM InitRecording retry after InitMicrophone rc=" + std::to_string(rc));
  }
  // 诊断点：InitRecording/InitPlayout 的真实返回值（monitor start 的中间步骤）。
  // 关键坑：InitRecording 可能返回 0 但 RecordingIsInitialized() 仍为 0（平台 ADM 的
  // 内部 recorder 已初始化失败/被复位而返回值未反映），之后 StartRecording 必 -1。
  // 所以这里把「返回值 + 四个自报状态」一起落盘，别只看返回码。
  char state[320] = {0};
  std::snprintf(state, sizeof(state),
                "audio: ADM %s rc=%d initialized=%d recording_initialized=%d recording=%d "
                "mic_initialized=%d",
                dir == AudioDeviceDirection::kInput ? "InitRecording" : "InitPlayout", rc,
                adm_->Initialized() ? 1 : 0, adm_->RecordingIsInitialized() ? 1 : 0,
                adm_->Recording() ? 1 : 0, adm_->MicrophoneIsInitialized() ? 1 : 0);
  LogLine(rc == 0 ? 3 : 1, state);
  return rc;
}

int AdmAudioDevicePort::Start(AudioDeviceDirection dir) {
  if (adm_ == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  const int rc = dir == AudioDeviceDirection::kInput ? adm_->StartRecording()
                                                     : adm_->StartPlayout();
  // 诊断点：StartRecording/StartPlayout 的真实返回值——「start 返回 0 但没有 PCM」
  // 是 AUD02 真机缺陷的现场，必须把这一步单独记下来。失败时把 ADM 自报状态一起带上，
  // 用来区分「没初始化」和「初始化了但起不来」。
  char state[320] = {0};
  std::snprintf(state, sizeof(state),
                "audio: ADM %s rc=%d initialized=%d recording_initialized=%d recording=%d "
                "mic_initialized=%d",
                dir == AudioDeviceDirection::kInput ? "StartRecording" : "StartPlayout", rc,
                adm_->Initialized() ? 1 : 0, adm_->RecordingIsInitialized() ? 1 : 0,
                adm_->Recording() ? 1 : 0, adm_->MicrophoneIsInitialized() ? 1 : 0);
  LogLine(rc == 0 ? 3 : 1, state);
  return rc;
}

int AdmAudioDevicePort::SetMuted(AudioDeviceDirection dir, bool muted) {
  if (adm_ == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  return dir == AudioDeviceDirection::kInput ? adm_->SetMicrophoneMute(muted)
                                             : adm_->SetSpeakerMute(muted);
}

int AdmAudioDevicePort::GetMuted(AudioDeviceDirection dir, bool* muted) {
  if (adm_ == nullptr || muted == nullptr) return -1;
  if (EnsureInitialized() != 0) return -1;
  // 与 SetMuted 使用同一个 ADM 设备状态；查询失败不能被当成 false（或 true）。
  return dir == AudioDeviceDirection::kInput ? adm_->MicrophoneMute(muted)
                                             : adm_->SpeakerMute(muted);
}

std::string WasapiDefaultEndpointProbe::DefaultEndpointId(AudioDeviceDirection dir) {
  ScopedComInitialization com;
  if (!com.usable()) return std::string();

  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) return std::string();

  IMMDevice* device = nullptr;
  const EDataFlow flow =
      dir == AudioDeviceDirection::kInput ? eCapture : eRender;
  // eConsole 与 ADM 的 kDefaultDevice 一致；kDefaultCommunicationDevice(eCommunications)
  // 是另一个角色，不能混用，否则「默认端点变了」会漏检/误报。
  hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  std::string id;
  if (SUCCEEDED(hr) && device != nullptr) {
    LPWSTR wide_id = nullptr;
    if (SUCCEEDED(device->GetId(&wide_id)) && wide_id != nullptr) {
      id = WideToUtf8(wide_id);
      ::CoTaskMemFree(wide_id);
    }
    device->Release();
  }
  enumerator->Release();
  return id;
}

// ---------------------------------------------------------------------------
// AUD02：AudioMonitorPort 的生产 adapter（只转发到同一个 controller/ADM）
// ---------------------------------------------------------------------------
AdmAudioMonitorPort::AdmAudioMonitorPort(webrtc::AudioDeviceModule* adm,
                                         AudioDeviceController* controller,
                                         webrtc::Thread* signaling)
    : adm_(adm), controller_(controller), signaling_(signaling) {}

void AdmAudioMonitorPort::SetLogSink(std::function<void(int, const std::string&)> sink) {
  log_ = std::move(sink);
  char line[160] = {0};
  std::snprintf(line, sizeof(line), "audio monitor port: adm=%p controller=%p",
                static_cast<void*>(adm_), static_cast<void*>(controller_));
  LogLine(3, line);
}

void AdmAudioMonitorPort::LogLine(int level, const std::string& message) const {
  if (log_) log_(level, message);
}

void AdmAudioMonitorPort::SetAdmThread(webrtc::Thread* thread) {
  adm_thread_ = thread;
  if (log_ != nullptr) {
    char line[192] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor port: ADM calls routed to %s thread (voice engine thread)",
                  thread != nullptr ? "worker" : "own");
    if (log_) log_(3, line);
  }
}

void AdmAudioMonitorPort::Rebind(webrtc::AudioDeviceModule* adm,
                                 AudioDeviceController* controller,
                                 webrtc::Thread* thread) {
  adm_ = adm;
  controller_ = controller;
  adm_thread_ = thread;
  if (log_ != nullptr) {
    char line[256] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor port: rebound to rebuilt ADM=%p controller=%p thread=%s",
                  static_cast<void*>(adm_), static_cast<void*>(controller_),
                  thread != nullptr ? "worker" : "own");
    if (log_) log_(3, line);
  }
}

int AdmAudioMonitorPort::StartInput(std::string* error) {
  int rc = PU_ERR_STATE;
  std::string detail = "audio monitor: audio device module is not created";
  bool recording_initialized = false;
  bool recording = false;
  RunOnSignaling(AdmThread(), [&] {
    if (adm_ == nullptr || controller_ == nullptr) return;
    rc = controller_->StartInputOnly(&detail);
    // 读回 ADM 自报状态：Start 成功不等于采集线程真的在出 PCM，这一步把二者分开。
    if (adm_ != nullptr) {
      recording_initialized = adm_->RecordingIsInitialized();
      recording = adm_->Recording();
    }
  });
  {
    char line[320] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor port: StartInput rc=%d adm=%p recording_initialized=%d "
                  "recording=%d error=%s",
                  rc, static_cast<void*>(adm_), recording_initialized ? 1 : 0,
                  recording ? 1 : 0, detail.empty() ? "-" : detail.c_str());
    LogLine(rc == PU_OK ? 3 : 1, line);
  }
  if (rc != PU_OK) {
    if (error != nullptr) *error = detail;
  } else if (error != nullptr) {
    error->clear();
  }
  return rc;
}

int AdmAudioMonitorPort::StopInput(std::string* error) {
  int rc = PU_ERR_STATE;
  std::string detail = "audio monitor: audio device module is not created";
  bool recording_initialized = false;
  bool recording = false;
  RunOnSignaling(AdmThread(), [&] {
    if (adm_ == nullptr || controller_ == nullptr) return;
    rc = controller_->StopInputOnly(&detail);
    if (adm_ != nullptr) {
      recording_initialized = adm_->RecordingIsInitialized();
      recording = adm_->Recording();
    }
  });
  {
    char line[320] = {0};
    std::snprintf(line, sizeof(line),
                  "audio monitor port: StopInput rc=%d adm=%p recording_initialized=%d "
                  "recording=%d error=%s",
                  rc, static_cast<void*>(adm_), recording_initialized ? 1 : 0,
                  recording ? 1 : 0, detail.empty() ? "-" : detail.c_str());
    LogLine(rc == PU_OK ? 3 : 1, line);
  }
  if (rc != PU_OK) {
    if (error != nullptr) *error = detail;
  } else if (error != nullptr) {
    error->clear();
  }
  return rc;
}

int AdmAudioMonitorPort::InputRunning() {
  int running = -1;
  RunOnSignaling(AdmThread(), [&] {
    if (controller_ != nullptr) running = controller_->InputRunning();
  });
  return running;
}

int64_t AdmAudioMonitorPort::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace partyup
