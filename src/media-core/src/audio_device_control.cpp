/* PartyUp 媒体核心 · 通话中的音频设备控制（实现）
 *
 * 关键事实（来自实际链接的 libwebrtc Windows ADM，audio_device_core_win.cc）：
 *   * SetRecordingDevice(uint16_t)/SetPlayoutDevice(uint16_t) 在
 *     _recIsInitialized/_playIsInitialized 为真时返回 -1；
 *   * StopRecording()/StopPlayout() 一定把对应 initialized 标志清回 false，
 *     即使是「已初始化但没启动」的空闲路径；
 *   * InitRecording()/InitPlayout() 在已经 initialized 时直接返回 0（不会切设备），
 *     所以换设备必须真正 Stop 之后再 Set 再 Init；
 *   * InitMicrophoneLocked()/InitSpeakerLocked() 会按**当前索引**从刚刷新的
 *     设备集合里重新取设备；设备顺序变化时索引会指向别人，因此选择必须按 ADM
 *     给出的稳定端点 ID（guid）保存和重解析。
 *   * SetRecordingDevice(kDefaultDevice)/SetPlayoutDevice(kDefaultDevice) 走
 *     GetDefaultAudioEndpoint(eCapture|eRender, eConsole)；探针也用 eConsole，
 *     两者必须一致，否则「默认端点变了」会漏检或误报。
 */
#include "audio_device_control.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace partyup {
namespace {

constexpr uint32_t kInputBit = 1u;
constexpr uint32_t kOutputBit = 2u;

const char* DirectionName(AudioDeviceDirection dir) {
  return dir == AudioDeviceDirection::kInput ? "input" : "output";
}

}  // namespace

void AudioDeviceController::Init(AudioDevicePort* port, AudioDefaultEndpointProbe* probe) {
  port_ = port;
  probe_ = probe;
}

void AudioDeviceController::SetLogSink(std::function<void(int, const std::string&)> sink) {
  log_ = std::move(sink);
}

void AudioDeviceController::LogLine(int level, const std::string& message) const {
  if (log_) log_(level, message);
}

AudioDeviceController::DirState& AudioDeviceController::State(AudioDeviceDirection dir) {
  return dir == AudioDeviceDirection::kInput ? input_ : output_;
}

const AudioDeviceController::DirState& AudioDeviceController::State(
    AudioDeviceDirection dir) const {
  return dir == AudioDeviceDirection::kInput ? input_ : output_;
}

bool AudioDeviceController::IsActive(AudioDeviceDirection dir) const {
  if (port_ == nullptr) return false;
  return port_->IsStarted(dir) || port_->IsInitialized(dir);
}

int AudioDeviceController::StartInputOnly(std::string* error) {
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  if (port_->IsStarted(AudioDeviceDirection::kInput)) {
    if (error != nullptr) error->clear();
    return PU_OK;
  }
  // 先重解析当前选择（设备顺序变化/设备消失时回落到系统默认）；输入未在运行时
  // RefreshDirection 只改选择，绝不会启动采集。
  std::string refresh_error;
  const int refresh_rc =
      RefreshDirection(AudioDeviceDirection::kInput, &refresh_error);
  if (refresh_rc != PU_OK) {
    if (error != nullptr) *error = refresh_error;
    return refresh_rc;
  }
  // 上一轮语音会话（工厂的 WebRtcVoiceEngine）会在自己的线程上异步 Terminate ADM
  // 并释放采集端点：语音刚退出就点「麦克风测试」时，InitRecording 可能瞬时返回 -1。
  // 这里做有限次退避重试（每轮重新刷新设备选择），既吃掉这个竞态，又不会掩盖真实
  // 设备错误——重试耗尽后仍按 PU_ERR_MEDIA 失败并带上最后一次的真实原因。
  constexpr int kStartAttempts = 5;
  std::string detail;
  for (int attempt = 0; attempt < kStartAttempts; ++attempt) {
    if (attempt > 0) {
      LogLine(2, "audio: input monitor start retry attempt=" + std::to_string(attempt + 1) +
                     " last_error=" + detail);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      std::string retry_refresh_error;
      if (RefreshDirection(AudioDeviceDirection::kInput, &retry_refresh_error) != PU_OK) {
        detail = retry_refresh_error;
        continue;
      }
    }
    if (ActivateCandidate(AudioDeviceDirection::kInput, input_.selected_index,
                          /*start=*/true, &detail) == PU_OK) {
      break;
    }
    if (port_->IsInitialized(AudioDeviceDirection::kInput)) {
      (void)port_->Stop(AudioDeviceDirection::kInput);
    }
  }
  if (!port_->IsStarted(AudioDeviceDirection::kInput)) {
    if (port_->IsInitialized(AudioDeviceDirection::kInput)) {
      (void)port_->Stop(AudioDeviceDirection::kInput);
    }
    if (error != nullptr) {
      *error = detail.empty() ? "ADM reported the input is not running after start" : detail;
    }
    return PU_ERR_MEDIA;
  }
  LogLine(3, "audio: independent input monitor started");
  if (error != nullptr) error->clear();
  return PU_OK;
}

int AudioDeviceController::StopInputOnly(std::string* error) {
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  if (!port_->IsInitialized(AudioDeviceDirection::kInput) &&
      !port_->IsStarted(AudioDeviceDirection::kInput)) {
    if (error != nullptr) error->clear();
    return PU_OK;
  }
  if (port_->Stop(AudioDeviceDirection::kInput) != 0) {
    if (error != nullptr) *error = "ADM refused to stop the input";
    return PU_ERR_MEDIA;
  }
  LogLine(3, "audio: independent input monitor stopped");
  if (error != nullptr) error->clear();
  return PU_OK;
}

int AudioDeviceController::InputRunning() const {
  if (port_ == nullptr) return -1;
  return port_->IsStarted(AudioDeviceDirection::kInput) ? 1 : 0;
}

bool AudioDeviceController::MuteRequired(AudioDeviceDirection dir) const {
  return dir == AudioDeviceDirection::kInput ? microphone_muted_ : speaker_muted_;
}

std::string AudioDeviceController::StableKeyOf(const AudioDeviceEntry& entry) {
  if (!entry.guid.empty()) return entry.guid;
  // 没有稳定 ID 时只能用名字兜底；同名设备无法再区分（Windows 真实端点都有
  // IMMDevice::GetId，这个分支只防御异常 ADM 实现）。
  if (!entry.name.empty()) return "name:" + entry.name;
  return std::string();
}

std::string AudioDeviceController::BuildSignature(
    const std::vector<AudioDeviceEntry>& entries) {
  std::string signature;
  for (const AudioDeviceEntry& entry : entries) {
    signature += std::to_string(entry.index);
    signature += ':';
    signature += entry.name;
    signature += ':';
    signature += entry.guid;
    signature += ';';
  }
  return signature;
}

int AudioDeviceController::FindEntryByIndex(const std::vector<AudioDeviceEntry>& entries,
                                            int32_t index) const {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].index == index) return static_cast<int>(i);
  }
  return -1;
}

int AudioDeviceController::FindEntryByStableId(const std::vector<AudioDeviceEntry>& entries,
                                               const std::string& stable_id) const {
  if (stable_id.empty()) return -1;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (StableKeyOf(entries[i]) == stable_id) return static_cast<int>(i);
  }
  return -1;
}

std::string AudioDeviceController::DefaultId(AudioDeviceDirection dir) const {
  if (probe_ == nullptr) return std::string();
  return probe_->DefaultEndpointId(dir);
}

std::string AudioDeviceController::ResolveNameForDefault(
    const std::vector<AudioDeviceEntry>& entries, const std::string& default_id) const {
  if (default_id.empty()) return std::string();
  for (const AudioDeviceEntry& entry : entries) {
    if (!entry.guid.empty() && entry.guid == default_id) return entry.name;
  }
  return std::string();
}

int AudioDeviceController::EnumerateDevices(AudioDeviceDirection dir,
                                            std::vector<AudioDeviceEntry>* out,
                                            std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "audio devices: out is null";
    return PU_ERR_INVALID_ARG;
  }
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  std::vector<AudioDeviceEntry> entries;
  const int count = port_->Enumerate(dir, &entries);
  if (count < 0) {
    if (error != nullptr) {
      *error = std::string("audio devices: ADM enumeration failed for ") + DirectionName(dir);
    }
    return PU_ERR_STATE;
  }
  const std::string default_id = DefaultId(dir);
  for (AudioDeviceEntry& entry : entries) {
    entry.is_default = !default_id.empty() && entry.guid == default_id;
  }
  *out = std::move(entries);
  if (error != nullptr) error->clear();
  return static_cast<int>(out->size());
}

int AudioDeviceController::ApplyIdle(AudioDeviceDirection dir, int32_t index,
                                     const std::string& stable_id, const std::string& name,
                                     std::string* error) {
  DirState& st = State(dir);
  const int rc = port_->SetDevice(dir, index);
  if (rc != 0) {
    if (error != nullptr) {
      *error = std::string("ADM rejected the ") + DirectionName(dir) + " device selection";
    }
    return PU_ERR_STATE;
  }
  st.selected_index = index < 0 ? -1 : index;
  st.selected_stable_id = index < 0 ? std::string() : stable_id;
  st.name = name;
  if (index < 0) st.default_id = DefaultId(dir);
  st.fallback = false;
  st.fault = false;
  st.fault_was_started = false;
  st.fault_was_initialized = false;
  LogLine(3, std::string("audio: ") + DirectionName(dir) + " selection -> " +
                 (index < 0 ? std::string("system default") : name));
  return PU_OK;
}

int AudioDeviceController::ApplyBeforeJoin(std::string* error) {
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  for (AudioDeviceDirection dir :
       {AudioDeviceDirection::kInput, AudioDeviceDirection::kOutput}) {
    DirState& st = State(dir);
    // join 前若 ADM 还留着上一轮的 initialized 状态（例如离开流程尚未收干净），
    // ADM 会拒绝 SetDevice；先停掉，选择写入后由音频启动重新 Init。
    if (port_->IsInitialized(dir)) (void)port_->Stop(dir);
    std::vector<AudioDeviceEntry> entries;
    const int count = port_->Enumerate(dir, &entries);
    if (count < 0) {
      if (error != nullptr) {
        *error = std::string("voice: could not enumerate ") + DirectionName(dir) + " devices";
      }
      return PU_ERR_STATE;
    }
    const std::string default_id = DefaultId(dir);
    int32_t target = st.selected_index;
    std::string stable_id = st.selected_stable_id;
    std::string name;
    if (target >= 0) {
      // 正常按稳定 ID；只有 ADM 没给出 guid 时才退回序号（不常见的异常 ADM）。
      const int index = stable_id.empty() ? FindEntryByIndex(entries, target)
                                          : FindEntryByStableId(entries, stable_id);
      if (index >= 0) {
        target = entries[index].index;
        stable_id = StableKeyOf(entries[index]);
        name = entries[index].name;
      } else {
        // 上次选的设备已经拔掉：不要按旧序号猜别人的设备。
        st.fallback = true;
        ++generation_;
        LogLine(1, std::string("audio: selected ") + DirectionName(dir) +
                       " device disappeared before join; falling back to system default");
        target = -1;
        stable_id.clear();
        name = ResolveNameForDefault(entries, default_id);
      }
    } else {
      name = ResolveNameForDefault(entries, default_id);
    }

    int rc = port_->SetDevice(dir, target);
    if (rc != 0 && target >= 0) {
      // 显式设备在枚举之后、Set 之前又变了：回落默认，不静默失败。
      LogLine(1, std::string("audio: ADM rejected ") + DirectionName(dir) +
                     " device before join; falling back to system default");
      st.fallback = true;
      ++generation_;
      target = -1;
      stable_id.clear();
      name = ResolveNameForDefault(entries, default_id);
      rc = port_->SetDevice(dir, -1);
    }
    if (rc != 0) {
      if (error != nullptr) {
        *error = std::string("voice: could not select ") + DirectionName(dir) +
                 " device (system default also failed)";
      }
      st.fault = true;
      st.fault_was_started = false;
      st.fault_was_initialized = false;
      return PU_ERR_MEDIA;
    }
    st.selected_index = target < 0 ? -1 : target;
    st.selected_stable_id = target < 0 ? std::string() : stable_id;
    st.name = name;
    if (target < 0) st.default_id = default_id;
    // 选择已经真正写进 ADM；join 前的旧故障原因不再成立。选择写入不等于音频已经
    // 启动，pu_audio_status 的 fault 会在后续启动失败时再由切换路径置位。
    st.fault = false;
    st.fault_was_started = false;
    st.fault_was_initialized = false;
    LogLine(3, std::string("voice: ") + DirectionName(dir) + " device = " +
                   (target < 0 ? std::string("system default") : name));
  }
  return PU_OK;
}

int AudioDeviceController::SetDevice(AudioDeviceDirection dir, int32_t index,
                                     std::string* error) {
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  if (index < -1) {
    if (error != nullptr) *error = "audio device index must be >= -1";
    return PU_ERR_INVALID_ARG;
  }
  std::vector<AudioDeviceEntry> entries;
  const int count = port_->Enumerate(dir, &entries);
  if (count < 0) {
    if (error != nullptr) {
      *error = std::string("audio: could not enumerate ") + DirectionName(dir) + " devices";
    }
    return PU_ERR_STATE;
  }
  DirState& st = State(dir);
  int32_t target = -1;
  std::string stable_id;
  std::string name;
  if (index >= 0) {
    const int position = FindEntryByIndex(entries, index);
    if (position < 0) {
      if (error != nullptr) {
        *error = std::string("audio: ") + DirectionName(dir) + " device index is unavailable";
      }
      return PU_ERR_STATE;
    }
    target = entries[position].index;
    stable_id = StableKeyOf(entries[position]);
    name = entries[position].name;
  } else {
    name = ResolveNameForDefault(entries, DefaultId(dir));
  }

  const bool active = IsActive(dir);
  if (active && !st.fault && st.selected_index == target &&
      (target < 0 || st.selected_stable_id == stable_id) && port_->IsStarted(dir)) {
    // 同一设备重复选择：不做一次没必要的停/启（会把正在进行的通话音频切断）。
    // 但如果这个方向被要求静音，仍要读回确认：设备可能在外部被重置成未静音，
    // 不能因为「选择没变」就跳过安全确认。
    if (MuteRequired(dir)) {
      std::string gate_detail;
      if (ApplyMuteGate(dir, &gate_detail) != PU_OK) {
        FailDirection(dir, gate_detail);
        if (error != nullptr) *error = gate_detail;
        return PU_ERR_MEDIA;
      }
    }
    st.fallback = false;
    if (error != nullptr) error->clear();
    return PU_OK;
  }

  int rc = PU_OK;
  if (st.fault && !active && (st.fault_was_started || st.fault_was_initialized)) {
    // 故障后用户重新选择：真正按故障前形态重启，不能只写选择就清 fault。
    rc = RecoverFaultedDirection(dir, target, stable_id, name, error);
  } else if (active) {
    rc = SwitchActive(dir, target, stable_id, name, error);
  } else {
    rc = ApplyIdle(dir, target, stable_id, name, error);
  }
  if (rc == PU_OK) {
    st.fallback = false;
    st.fault = false;
    st.fault_was_started = false;
    st.fault_was_initialized = false;
  }
  return rc;
}

int AudioDeviceController::SwitchActive(AudioDeviceDirection dir, int32_t index,
                                        const std::string& stable_id,
                                        const std::string& name, std::string* error) {
  DirState& st = State(dir);
  const bool was_started = port_->IsStarted(dir);
  const bool was_initialized = port_->IsInitialized(dir);

  // 1) 停：ADM 只在 !initialized 时接受 SetDevice。
  if (was_initialized) {
    const int stop_rc = port_->Stop(dir);
    if (stop_rc != 0) {
      std::string detail =
          std::string("could not stop ") + DirectionName(dir) + " device before switching";
      // Stop 失败时旧设备仍在跑。若该方向应静音，至少确认静音仍在；确认不了就
      // 明确置 fault，绝不把它当成「界面 muted 且一切正常」。
      if (MuteRequired(dir)) {
        std::string gate_detail;
        if (ApplyMuteGate(dir, &gate_detail) != PU_OK) {
          detail += "; could not confirm mute on the still-running device: " + gate_detail;
          st.fault = true;
          st.fault_was_started = was_started;
          st.fault_was_initialized = was_initialized;
        }
      }
      LogLine(1, "audio: " + detail);
      if (error != nullptr) *error = detail;
      return PU_ERR_MEDIA;
    }
  }

  // 2) Set -> Init ->（静音门禁）-> Start ->（Start 后读回）全部在同一个候选激活
  //    事务里；任何一步失败都会先把该方向停掉，再交给恢复/回退路径。
  std::string detail;
  if (ActivateCandidate(dir, index, was_started, &detail) != PU_OK) {
    LogLine(1, "audio: " + detail);
    return RecoverAfterFailure(dir, was_started, was_initialized, detail, error);
  }

  st.selected_index = index < 0 ? -1 : index;
  st.selected_stable_id = index < 0 ? std::string() : stable_id;
  st.name = name;
  if (index < 0) st.default_id = DefaultId(dir);
  st.fault = false;
  st.fault_was_started = false;
  st.fault_was_initialized = false;
  LogLine(3, std::string("audio: switched ") + DirectionName(dir) + " device in call -> " +
                 (index < 0 ? std::string("system default") : name));
  if (error != nullptr) error->clear();
  return PU_OK;
}

int AudioDeviceController::ActivateCandidate(AudioDeviceDirection dir, int32_t index,
                                             bool start, std::string* detail) {
  if (port_ == nullptr) {
    if (detail != nullptr) *detail = "audio device module is not created";
    return PU_ERR_STATE;
  }
  // 半切换/上次失败可能留下 initialized 状态；ADM 只在未初始化时接受 SetDevice。
  if (port_->IsInitialized(dir)) {
    if (port_->Stop(dir) != 0) {
      if (detail != nullptr) {
        *detail = std::string("could not stop ") + DirectionName(dir) + " device";
      }
      return PU_ERR_MEDIA;
    }
  }
  if (port_->SetDevice(dir, index) != 0) {
    if (detail != nullptr) {
      *detail = std::string("ADM rejected the ") + DirectionName(dir) +
                " device selection during the call";
    }
    return PU_ERR_MEDIA;
  }
  if (port_->Init(dir) != 0 || !port_->IsInitialized(dir)) {
    if (detail != nullptr) {
      *detail = std::string("could not initialize ") + DirectionName(dir) + " device";
    }
    if (port_->IsInitialized(dir)) (void)port_->Stop(dir);
    return PU_ERR_MEDIA;
  }

  // 安全启动门禁：静音方向必须在 Start 之前成功 SetMuted 并读回为 true。
  // 这一步失败时该方向保持停止；绝不允许先 Start 再补 mute。
  if (ApplyMuteGate(dir, detail) != PU_OK) {
    if (port_->IsInitialized(dir)) (void)port_->Stop(dir);
    return PU_ERR_MEDIA;
  }
  if (!start) return PU_OK;

  if (port_->Start(dir) != 0 || !port_->IsStarted(dir)) {
    if (detail != nullptr) {
      *detail = std::string("could not start ") + DirectionName(dir) + " device";
    }
    if (port_->IsInitialized(dir)) (void)port_->Stop(dir);
    return PU_ERR_MEDIA;
  }
  // Start 之后再次读回：如果平台在 Start 时重置了静音，必须立刻停下并报错，
  // 不能继续让这个方向在「界面说 muted」时运行。
  if (VerifyMuteAfterStart(dir, detail) != PU_OK) {
    if (port_->IsInitialized(dir)) (void)port_->Stop(dir);
    return PU_ERR_MEDIA;
  }
  return PU_OK;
}

int AudioDeviceController::ApplyMuteState(AudioDeviceDirection dir, bool muted,
                                          std::string* detail) {
  if (port_ == nullptr) {
    if (detail != nullptr) *detail = "audio device module is not created";
    return PU_ERR_STATE;
  }
  if (port_->SetMuted(dir, muted) != 0) {
    if (detail != nullptr) {
      *detail = std::string("could not ") + (muted ? "mute " : "unmute ") +
                DirectionName(dir) + " device";
    }
    return PU_ERR_MEDIA;
  }
  bool actual = false;
  if (port_->GetMuted(dir, &actual) != 0) {
    if (detail != nullptr) {
      *detail = std::string("could not verify ") + DirectionName(dir) +
                " mute state after setting it";
    }
    return PU_ERR_MEDIA;
  }
  if (actual != muted) {
    if (detail != nullptr) {
      *detail = std::string(DirectionName(dir)) + " device reported " +
                (actual ? "muted" : "unmuted") + " after the " +
                (muted ? "mute" : "unmute") + " request";
    }
    return PU_ERR_MEDIA;
  }
  return PU_OK;
}

int AudioDeviceController::ApplyMuteGate(AudioDeviceDirection dir, std::string* detail) {
  if (!MuteRequired(dir)) return PU_OK;
  return ApplyMuteState(dir, /*muted=*/true, detail);
}

int AudioDeviceController::VerifyMuteAfterStart(AudioDeviceDirection dir,
                                                std::string* detail) {
  if (!MuteRequired(dir)) return PU_OK;
  if (port_ == nullptr) {
    if (detail != nullptr) *detail = "audio device module is not created";
    return PU_ERR_STATE;
  }
  bool actual = false;
  if (port_->GetMuted(dir, &actual) != 0) {
    if (detail != nullptr) {
      *detail = std::string("could not verify ") + DirectionName(dir) +
                " mute state after start";
    }
    return PU_ERR_MEDIA;
  }
  if (!actual) {
    if (detail != nullptr) {
      *detail = std::string(DirectionName(dir)) + " device started unmuted";
    }
    return PU_ERR_MEDIA;
  }
  return PU_OK;
}

void AudioDeviceController::FailDirection(AudioDeviceDirection dir,
                                          const std::string& detail) {
  if (port_ != nullptr) {
    const bool was_started = port_->IsStarted(dir);
    const bool was_initialized = port_->IsInitialized(dir);
    if (was_initialized || was_started) (void)port_->Stop(dir);
    DirState& st = State(dir);
    st.fault = true;
    st.fault_was_started = was_started;
    st.fault_was_initialized = was_initialized;
  }
  LogLine(1, "audio: " + detail);
}

int AudioDeviceController::RecoverFaultedDirection(AudioDeviceDirection dir, int32_t index,
                                                   const std::string& stable_id,
                                                   const std::string& name,
                                                   std::string* error) {
  DirState& st = State(dir);
  const bool start = st.fault_was_started;
  std::string detail;
  if (ActivateCandidate(dir, index, start, &detail) != PU_OK) {
    st.fault = true;
    st.fault_was_started = start;
    st.fault_was_initialized = true;
    const std::string message =
        std::string("could not recover ") + DirectionName(dir) + " audio: " + detail;
    LogLine(1, "audio: " + message);
    if (error != nullptr) *error = message;
    return PU_ERR_MEDIA;
  }
  st.selected_index = index < 0 ? -1 : index;
  st.selected_stable_id = index < 0 ? std::string() : stable_id;
  st.name = name;
  if (index < 0) st.default_id = DefaultId(dir);
  st.fault = false;
  st.fault_was_started = false;
  st.fault_was_initialized = false;
  LogLine(3, std::string("audio: recovered ") + DirectionName(dir) + " device -> " +
                 (index < 0 ? std::string("system default") : name));
  if (error != nullptr) error->clear();
  return PU_OK;
}

int AudioDeviceController::RecoverAfterFailure(AudioDeviceDirection dir, bool old_was_started,
                                               bool old_was_initialized,
                                               const std::string& detail,
                                               std::string* error) {
  DirState& st = State(dir);
  // 半切换状态下 ADM 可能已经 initialized；恢复前统一停掉。
  if (port_->IsInitialized(dir)) (void)port_->Stop(dir);

  bool old_available = true;
  int32_t restore_index = st.selected_index;
  if (restore_index >= 0 && !st.selected_stable_id.empty()) {
    std::vector<AudioDeviceEntry> entries;
    if (port_->Enumerate(dir, &entries) >= 0) {
      const int position = FindEntryByStableId(entries, st.selected_stable_id);
      if (position >= 0) {
        restore_index = entries[position].index;
      } else {
        old_available = false;  // 设备已经不在了，按旧序号恢复只会指向别人
      }
    } else {
      old_available = false;
    }
  }

  std::string restore_detail;
  if (old_available) {
    // 恢复旧设备也必须走同一个安全启动门禁：旧设备同样要在 Start 前静音/读回。
    if (ActivateCandidate(dir, restore_index, old_was_started, &restore_detail) == PU_OK) {
      st.fault = false;
      st.fault_was_started = false;
      st.fault_was_initialized = false;
      const std::string message = detail + "; restored the previous device";
      LogLine(1, "audio: " + message);
      if (error != nullptr) *error = message;
      return PU_ERR_MEDIA;
    }
  }

  // 旧选择本身就是系统默认：恢复与回退是同一个动作，不再重复尝试。
  if (st.selected_index < 0) {
    st.fault = true;
    st.fault_was_started = old_was_started;
    st.fault_was_initialized = old_was_initialized;
    const std::string message =
        detail + "; system default endpoint could not be activated" +
        (restore_detail.empty() ? std::string() : " (" + restore_detail + ")") + "; " +
        std::string(DirectionName(dir)) + " audio is stopped";
    LogLine(1, "audio: " + message);
    if (error != nullptr) *error = message;
    return PU_ERR_MEDIA;
  }

  // 显式设备恢复不了：明确回落到系统默认，并报告 fallback。
  st.selected_index = -1;
  st.selected_stable_id.clear();
  st.default_id = DefaultId(dir);
  ++generation_;
  std::string default_detail;
  if (ActivateCandidate(dir, -1, old_was_started, &default_detail) == PU_OK) {
    std::vector<AudioDeviceEntry> entries;
    if (port_->Enumerate(dir, &entries) >= 0) {
      st.name = ResolveNameForDefault(entries, st.default_id);
    } else {
      st.name.clear();
    }
    st.fallback = true;
    st.fault = false;
    st.fault_was_started = false;
    st.fault_was_initialized = false;
    const std::string message =
        detail + "; previous device could not be restored, fell back to system default";
    LogLine(1, "audio: " + message);
    if (error != nullptr) *error = message;
    return PU_ERR_MEDIA;
  }

  st.name.clear();
  st.fallback = false;
  st.fault = true;
  st.fault_was_started = old_was_started;
  st.fault_was_initialized = old_was_initialized;
  const std::string message =
      detail + "; previous device could not be restored and the system default also failed" +
      (default_detail.empty() ? std::string() : " (" + default_detail + ")") + "; " +
      std::string(DirectionName(dir)) + " audio is stopped";
  LogLine(1, "audio: " + message);
  if (error != nullptr) *error = message;
  return PU_ERR_MEDIA;
}

int AudioDeviceController::Refresh(std::string* error) {
  if (port_ == nullptr) {
    if (error != nullptr) *error = "audio device module is not created";
    return PU_ERR_STATE;
  }
  std::string first_error;
  int first_rc = PU_OK;
  for (AudioDeviceDirection dir :
       {AudioDeviceDirection::kInput, AudioDeviceDirection::kOutput}) {
    std::string direction_error;
    const int rc = RefreshDirection(dir, &direction_error);
    if (rc != PU_OK && first_rc == PU_OK) {
      first_rc = rc;
      first_error = direction_error;
    }
  }
  if (error != nullptr) *error = first_error;
  return first_rc;
}

int AudioDeviceController::RefreshDirection(AudioDeviceDirection dir, std::string* error) {
  DirState& st = State(dir);
  std::vector<AudioDeviceEntry> entries;
  const int count = port_->Enumerate(dir, &entries);
  if (count < 0) {
    if (error != nullptr) {
      *error = std::string("audio: could not enumerate ") + DirectionName(dir) + " devices";
    }
    return PU_ERR_STATE;
  }
  st.count = static_cast<uint32_t>(entries.size());
  const std::string signature = BuildSignature(entries);
  if (signature != st.signature) {
    st.signature = signature;
    ++generation_;
  }
  const std::string default_id = DefaultId(dir);

  if (st.selected_index >= 0) {
    const int position = st.selected_stable_id.empty()
                             ? FindEntryByIndex(entries, st.selected_index)
                             : FindEntryByStableId(entries, st.selected_stable_id);
    if (position >= 0) {
      // 设备顺序可能变了：按稳定 ID 重解析序号，绝不沿用旧序号指向别人。
      if (entries[position].index != st.selected_index) {
        LogLine(3, std::string("audio: ") + DirectionName(dir) + " device index re-mapped " +
                       std::to_string(st.selected_index) + " -> " +
                       std::to_string(entries[position].index) + " (same endpoint)");
      }
      st.selected_index = entries[position].index;
      st.name = entries[position].name;
      return PU_OK;
    }

    // 选中设备消失：必须是真回退，而不是只改状态/名字。
    LogLine(1, std::string("audio: selected ") + DirectionName(dir) +
                   " device disappeared; re-selecting the system default");
    ++generation_;
    if (IsActive(dir)) {
      const int rc = SwitchActive(dir, -1, std::string(),
                                  ResolveNameForDefault(entries, default_id), error);
      if (rc == PU_OK) st.fallback = true;
      // 失败时状态由 RecoverAfterFailure 决定：恢复旧设备 / 回落默认 / 置 fault，
      // 这里不能无条件改写选择，否则 status 会和实际运行设备不一致。
      return rc;
    }
    st.selected_index = -1;
    st.selected_stable_id.clear();
    st.default_id = default_id;
    st.name = ResolveNameForDefault(entries, default_id);
    st.fallback = true;
    return PU_OK;
  }

  // 跟随系统默认：默认端点变了就必须真正重新 Set(kDefaultDevice)+Init+Start。
  // 数量/名称都没变也能发现，因为用的是端点稳定 ID。
  if (!default_id.empty() && default_id != st.default_id) {
    const bool first_observation = st.default_id.empty();
    if (first_observation) {
      st.default_id = default_id;
    } else {
      ++generation_;
      if (IsActive(dir)) {
        LogLine(3, std::string("audio: system default ") + DirectionName(dir) +
                       " device changed; re-selecting it in the live call");
        // 不要在这里先把 default_id 写成新值：切换失败并恢复旧设备时，旧端点仍是
        // 实际在跑的设备；保留旧 default_id 让下一次 Refresh 重试，而不是把新默认
        // 当成「已观察」后永久丢掉重试。成功/回退默认时由对应路径提交新值。
        return SwitchActive(dir, -1, std::string(),
                            ResolveNameForDefault(entries, default_id), error);
      }
      st.default_id = default_id;
    }
  }
  const std::string resolved = ResolveNameForDefault(entries, default_id);
  if (!resolved.empty()) st.name = resolved;
  if (error != nullptr) error->clear();
  return PU_OK;
}

int AudioDeviceController::ReapplyMutes(std::string* error) {
  if (port_ == nullptr) return PU_ERR_STATE;
  int rc = PU_OK;
  for (AudioDeviceDirection dir :
       {AudioDeviceDirection::kInput, AudioDeviceDirection::kOutput}) {
    if (!MuteRequired(dir)) continue;
    // 未运行的方向没有可泄漏的音频；镜像已记住 true，下一次受控启动会在 Start
    // 之前施加并读回。这里不主动写一个尚未 Init 的设备，避免把「设备未打开」
    // 误报成静音失败。
    if (!IsActive(dir)) continue;
    std::string detail;
    if (ApplyMuteGate(dir, &detail) != PU_OK) {
      FailDirection(dir, detail);
      if (rc == PU_OK) {
        rc = PU_ERR_MEDIA;
        if (error != nullptr) *error = detail;
      }
    }
  }
  return rc;
}

int AudioDeviceController::SetMuteState(bool microphone_muted, bool speaker_muted,
                                        std::string* error) {
  microphone_muted_ = microphone_muted;
  speaker_muted_ = speaker_muted;
  return ReapplyMutes(error);
}

int AudioDeviceController::SetMuteStateFor(AudioDeviceDirection dir, bool muted,
                                           bool microphone_muted, bool speaker_muted,
                                           std::string* error) {
  microphone_muted_ = microphone_muted;
  speaker_muted_ = speaker_muted;
  // 这里必须同时处理 false：用户显式取消静音/闭音时，ADM 必须真的被写成 false
  // 并读回；否则「界面已取消但设备仍静音」或反向的假状态都会留在这个方向。
  std::string detail;
  const bool active = IsActive(dir);
  if (ApplyMuteState(dir, muted, &detail) != PU_OK) {
    // 取消静音失败时绝不能把镜像记成 false：界面可能仍显示 muted，而设备状态
    // 未能确认；把该方向保持为 true，下一次受控启动仍会在 Start 前静音（宁可
    // 多静音，也不能在「界面说 muted」时放出未静音音频）。
    if (dir == AudioDeviceDirection::kInput) {
      microphone_muted_ = true;
    } else {
      speaker_muted_ = true;
    }
    if (active) {
      FailDirection(dir, detail);
    } else {
      // 没有在跑的方向没有可泄漏的音频；不把一次空闲状态的写失败渲染成
      // 「该方向音频故障」，只把错误交给调用方。
      LogLine(1, "audio: " + detail);
    }
    if (error != nullptr) *error = detail;
    return PU_ERR_MEDIA;
  }
  return PU_OK;
}

AudioDeviceController::Snapshot AudioDeviceController::GetSnapshot(
    uint32_t caller_generation) const {
  Snapshot snapshot;
  snapshot.input_index = input_.selected_index;
  snapshot.output_index = output_.selected_index;
  snapshot.input_count = input_.count;
  snapshot.output_count = output_.count;
  snapshot.generation = generation_;
  snapshot.changed = caller_generation != 0 && caller_generation != generation_ ? 1u : 0u;
  snapshot.fallback = (input_.fallback ? kInputBit : 0u) | (output_.fallback ? kOutputBit : 0u);
  snapshot.fault = (input_.fault ? kInputBit : 0u) | (output_.fault ? kOutputBit : 0u);
  snapshot.input_name = input_.name;
  snapshot.output_name = output_.name;
  return snapshot;
}

}  // namespace partyup
