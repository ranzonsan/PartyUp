/* PartyUp 媒体核心 · 通话中的音频设备控制（内部头，不导出 ABI）
 *
 * 为什么单独一层：pu_audio_set_device / pu_audio_status 需要在**同一条语音通话**
 * 上真正切换输入/输出设备、跟随系统默认、检测热插拔并回退。libwebrtc 的 Windows
 * ADM（AudioDeviceWindowsCore）不允许在初始化状态下改设备：
 *   SetRecordingDevice/SetPlayoutDevice 在 _recIsInitialized/_playIsInitialized
 *   为真时直接返回 -1（见 audio_device_core_win.cc）；而 StopRecording/StopPlayout
 *   会把对应标志清回 false。所以一次真实切换必须是
 *     Stop -> SetDevice -> Init -> Start
 * 的事务；任何一步失败都要把**实际设备**恢复回去，PU_OK 只能在整个序列成功后返回。
 *
 * 本层只依赖一个窄端口（AudioDevicePort）和一个系统默认端点探针
 * （AudioDefaultEndpointProbe）：
 *   * 生产实现是 audio_device_adm_win.cpp：端口直接转发到通话用的**同一个**
 *     webrtc::AudioDeviceModule，探针用成熟 Windows CoreAudio（IMMDeviceEnumerator
 *     + GetDefaultAudioEndpoint(eConsole)），与 ADM 的 kDefaultDevice 精确同一角色；
 *   * 测试实现是 spikes/media-dll-smoke/audio_switch_regression.cpp 里的脚本化
 *     fake ADM，不打开任何真实麦克风/扬声器。
 * 设备序号（ADM index）只在**当前枚举快照**内有效；选择在内部按 ADM 给出的稳定
 * 端点 ID 保存，设备顺序变化不会把旧选择悄悄指向别的设备。
 *
 * 线程：全部方法只在「控制线程」（媒体核心的 signaling 线程）上调用；本类不加锁。
 */
#ifndef PARTYUP_MEDIA_SRC_AUDIO_DEVICE_CONTROL_H_
#define PARTYUP_MEDIA_SRC_AUDIO_DEVICE_CONTROL_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "partyup_media.h"

namespace partyup {

/* 端点方向。数值与 pu_audio_* 的 kind 一致：0=输入（录音），1=输出（播放）。 */
enum class AudioDeviceDirection { kInput = 0, kOutput = 1 };

/* 一次枚举快照里的一个设备。index 是 ADM 枚举序号（同一快照内有效）；
 * guid 是 ADM 的端点稳定 ID（Windows 下等于 IMMDevice::GetId 的字符串），
 * 设备重排后同一个 guid 仍指向同一块硬件。 */
struct AudioDeviceEntry {
  int32_t index = 0;
  std::string name;
  std::string guid;
  bool is_default = false;  // 该设备是否是当前系统默认端点（按稳定 ID 匹配）
};

/* 生产控制逻辑看到的 ADM 端口。实现方负责线程归属；本接口不打开/关闭设备，
 * 只按 ADM 自己的状态机调用。返回 0 表示 ADM 成功，非 0 表示失败。 */
class AudioDevicePort {
 public:
  virtual ~AudioDevicePort() = default;

  /* 枚举当前设备。返回设备数量（>=0），失败返回负数。out 被整体覆盖。 */
  virtual int Enumerate(AudioDeviceDirection dir,
                        std::vector<AudioDeviceEntry>* out) = 0;
  /* 选择设备。index >= 0 是 ADM 枚举序号；index < 0 表示系统默认
   * （Windows 下必须走 AudioDeviceModule::kDefaultDevice，不能映射成枚举 0）。 */
  virtual int SetDevice(AudioDeviceDirection dir, int32_t index) = 0;
  virtual bool IsInitialized(AudioDeviceDirection dir) const = 0;
  virtual bool IsStarted(AudioDeviceDirection dir) const = 0;
  virtual int Stop(AudioDeviceDirection dir) = 0;
  virtual int Init(AudioDeviceDirection dir) = 0;
  virtual int Start(AudioDeviceDirection dir) = 0;
  /* 静音状态：录音=麦克风静音，播放=扬声器静音（deafen）。设备重启后由控制层
   * 重新施加，避免切换设备把人声静音状态丢掉。 */
  virtual int SetMuted(AudioDeviceDirection dir, bool muted) = 0;
  /* 读回实际静音状态（Windows ADM：MicrophoneMute/SpeakerMute）。返回 0 表示查询
   * 成功，非 0 表示查询失败。查询失败不能被当成 false/true：需要静音的方向必须
   * 安全失败（见 AudioDeviceController 的启动门禁），不能伪造静音成功。 */
  virtual int GetMuted(AudioDeviceDirection dir, bool* muted) = 0;
};

/* 系统默认端点的稳定 ID。必须是 ADM 的 kDefaultDevice 实际解析的那个角色
 * （Windows：GetDefaultAudioEndpoint(eCapture/eRender, eConsole)）。返回空串
 * 表示本机/本线程查不到，控制层会退化为「只按枚举变化判断」。 */
class AudioDefaultEndpointProbe {
 public:
  virtual ~AudioDefaultEndpointProbe() = default;
  virtual std::string DefaultEndpointId(AudioDeviceDirection dir) = 0;
};

/* 设备选择 + 热插拔/默认跟随 + 事务化热切的生产控制逻辑。 */
class AudioDeviceController {
 public:
  struct Snapshot {
    int32_t input_index = -1;   // 当前解析到的 ADM 序号；-1 = 跟随系统默认
    int32_t output_index = -1;
    uint32_t input_count = 0;
    uint32_t output_count = 0;
    uint32_t generation = 0;    // 设备列表/顺序或系统默认端点变化时递增
    uint32_t changed = 0;       // 相对调用方带进来的 generation 是否有变化
    uint32_t fallback = 0;      // bit0 输入发生过回退，bit1 输出发生过回退
    uint32_t fault = 0;         // bit0 输入切换/回退失败后未运行，bit1 输出同理
    std::string input_name;
    std::string output_name;
  };

  AudioDeviceController() = default;
  ~AudioDeviceController() = default;
  AudioDeviceController(const AudioDeviceController&) = delete;
  AudioDeviceController& operator=(const AudioDeviceController&) = delete;

  /* 绑定端口与探针（在 ADM 创建之后、同一条 signaling 线程上调用）。
   * 传 nullptr 表示不可用；之后所有操作返回 PU_ERR_STATE。 */
  void Init(AudioDevicePort* port, AudioDefaultEndpointProbe* probe);
  bool Ready() const { return port_ != nullptr; }

  /* 可选日志出口（level 语义与媒体核心 Log 一致）。 */
  void SetLogSink(std::function<void(int, const std::string&)> sink);
  void LogLine(int level, const std::string& message) const;

  /* 枚举（只读）。is_default 按系统默认端点稳定 ID 标记；查不到默认时不标记。 */
  int EnumerateDevices(AudioDeviceDirection dir, std::vector<AudioDeviceEntry>* out,
                       std::string* error);

  /* voice.join 之前调用：把已保存的选择（按稳定 ID 重新解析）真正写进 ADM。
   * 设备已消失时回落系统默认并在 fallback 里报告；两种都失败返回错误。 */
  int ApplyBeforeJoin(std::string* error);

  /* pu_audio_set_device 的后端。index=-1 跟随系统默认。
   * 通话中（该方向已初始化/已启动）走 Stop->Set->Init->Start 事务；成功才返回
   * PU_OK。失败时尽力把实际设备与选择恢复原状；恢复不了就明确报错并回落默认，
   * 默认也起不来则置 fault，绝不假装成功。 */
  int SetDevice(AudioDeviceDirection dir, int32_t index, std::string* error);

  /* pu_audio_status 的后端：推进真实重新选择/回退。
   *   * 显式选择的设备从列表消失 -> 真正切回系统默认（通话中会停/启设备）；
   *   * 跟随默认时系统默认端点变化（数量/名称都没变也能发现，按端点 ID 判断）
   *     -> 真正重新 Set(kDefaultDevice)+Init+Start，而不是只改名字/代次；
   *   * 设备顺序变化 -> 按稳定 ID 重映射序号，不把选择指向别人。
   * 列表/默认/回退发生变化时递增 generation。 */
  int Refresh(std::string* error);

  /* 与 pu_voice_set_mute / pu_voice_set_deafen 同步的镜像；设备重启后自动重新
   * 施加 true 的方向（false 不主动写，避免把用户在系统里改过的状态又改回来）。
   * 只对**正在运行/已初始化**的方向写 ADM：未运行的方向没有可泄漏的音频，等下一次
   * 受控启动时由门禁在 Start 之前施加。写/读回失败时返回错误，并把该方向停下来
   * 置 fault（不会让它在「界面仍说 muted」时继续跑）。 */
  int SetMuteState(bool microphone_muted, bool speaker_muted,
                   std::string* error = nullptr);
  /* 只对 dir 执行真实的静音/取消静音 + 读回（另一个方向只更新镜像，不动 ADM）。
   * pu_voice_set_mute / pu_voice_set_deafen 用这个入口：用户只改一个方向时，不能
   * 因为另一个方向的重新施加失败而把本次操作误报成失败，也不能留下「界面 muted
   * 但设备在跑」的窗口。失败时该方向会被停下并置 fault。 */
  int SetMuteStateFor(AudioDeviceDirection dir, bool muted, bool microphone_muted,
                      bool speaker_muted, std::string* error = nullptr);

  Snapshot GetSnapshot(uint32_t caller_generation) const;

  /* 测试/诊断用：该方向当前是否已初始化或已启动。 */
  bool IsActive(AudioDeviceDirection dir) const;

  /* AUD02：独立输入电平监测用的最小输入方向操作（只在控制/signaling 线程调用）。
   *
   * StartInputOnly 复用与通话相同的安全事务：RefreshDirection 重解析选择（设备
   * 消失时回落系统默认，不按旧序号猜）→ Stop(如仍初始化) → SetDevice → Init →
   * 静音门禁（SetMuted + 读回）→ Start → Start 后读回。它只碰输入方向，绝不启动
   * 扬声器，也不解除已有的手动/系统静音；失败返回 PU_ERR_MEDIA 并保持输入停止。
   * 已启动时幂等成功，不重复开设备。
   *
   * StopInputOnly 幂等：未初始化/未启动时直接成功。InputRunning 返回 1=正在采集，
   * 0=没有采集，-1=端口不可用（无法确认）。 */
  int StartInputOnly(std::string* error);
  int StopInputOnly(std::string* error);
  int InputRunning() const;

 private:
  struct DirState {
    int32_t selected_index = -1;      // 当前解析出的 ADM 序号；-1 = 系统默认
    std::string selected_stable_id;   // 稳定端点 ID；跟随默认时为空
    std::string name;                 // 展示名（来自枚举或默认端点）
    uint32_t count = 0;
    std::string signature;            // index:name:guid;...，用于检测列表/顺序变化
    std::string default_id;           // 上次观察到的系统默认端点 ID
    bool fallback = false;            // 发生过回退（成功后由 SetDevice 清除）
    bool fault = false;               // 切换/回退失败且该方向未运行
    /* fault 后重新选择同一/新设备时，恢复到故障前的运行形态（启动过就重新 Start，
     * 只初始化过就只 Init）。两个都为 false 表示故障发生在 join 前，没有在跑的
     * 音频需要恢复，重新选择只需写 ADM。 */
    bool fault_was_started = false;
    bool fault_was_initialized = false;
  };

  DirState& State(AudioDeviceDirection dir);
  const DirState& State(AudioDeviceDirection dir) const;

  std::string DefaultId(AudioDeviceDirection dir) const;
  std::string ResolveNameForDefault(const std::vector<AudioDeviceEntry>& entries,
                                    const std::string& default_id) const;
  int FindEntryByIndex(const std::vector<AudioDeviceEntry>& entries, int32_t index) const;
  int FindEntryByStableId(const std::vector<AudioDeviceEntry>& entries,
                          const std::string& stable_id) const;
  static std::string StableKeyOf(const AudioDeviceEntry& entry);
  static std::string BuildSignature(const std::vector<AudioDeviceEntry>& entries);

  int RefreshDirection(AudioDeviceDirection dir, std::string* error);
  /* 未通话时直接写 ADM（不需要停/启）。 */
  int ApplyIdle(AudioDeviceDirection dir, int32_t index, const std::string& stable_id,
                const std::string& name, std::string* error);
  /* 通话中的事务化切换。失败时内部完成恢复/回退，并返回错误。 */
  int SwitchActive(AudioDeviceDirection dir, int32_t index, const std::string& stable_id,
                   const std::string& name, std::string* error);
  /* SwitchActive 失败后的恢复：先恢复旧设备，再尝试系统默认，最后置 fault。 */
  int RecoverAfterFailure(AudioDeviceDirection dir, bool old_was_started,
                          bool old_was_initialized, const std::string& detail,
                          std::string* error);
  /* fault 后用户重新选择设备：按故障前的运行形态真正重新 Set->Init->(门禁)->Start，
   * 成功才清 fault；失败保持 fault，绝不只改名字/选择就谎报恢复。 */
  int RecoverFaultedDirection(AudioDeviceDirection dir, int32_t index,
                              const std::string& stable_id, const std::string& name,
                              std::string* error);
  /* 该方向是否被要求静音/闭音（输入=麦克风静音，输出=deafen）。 */
  bool MuteRequired(AudioDeviceDirection dir) const;
  /* 在设备已 Init、尚未 Start 时施加并**读回确认**静音；失败返回错误。
   * 这是安全启动门禁：Start 之前必须先通过它，绝不允许先 Start 再补 mute。 */
  int ApplyMuteGate(AudioDeviceDirection dir, std::string* detail);
  /* 设置并读回一个方向的静音/取消静音；读回失败或与请求不一致都算失败。
   * 调用方决定失败后是否停方向/置 fault（启动门禁和用户静音路径都会这样做）。 */
  int ApplyMuteState(AudioDeviceDirection dir, bool muted, std::string* detail);
  /* Start 之后再读回一次；仍确认静音才返回成功，否则调用方必须 Stop 并报错。 */
  int VerifyMuteAfterStart(AudioDeviceDirection dir, std::string* detail);
  /* 一次候选设备的完整激活：Stop(如仍初始化)->Set->Init->静音门禁->Start(可选)
   * ->Start 后读回。任何一步失败都会把该方向停掉并返回错误，供恢复/回退路径继续
   * 尝试下一个候选；它不修改 selected_* 状态。 */
  int ActivateCandidate(AudioDeviceDirection dir, int32_t index, bool start,
                        std::string* detail);
  /* 对当前正在运行的方向重新施加镜像里的静音（只处理 active 方向）。 */
  int ReapplyMutes(std::string* error);
  /* 停掉该方向并置 fault；用于无法确认静音/无法启动的安全失败。 */
  void FailDirection(AudioDeviceDirection dir, const std::string& detail);

  AudioDevicePort* port_ = nullptr;
  AudioDefaultEndpointProbe* probe_ = nullptr;
  std::function<void(int, const std::string&)> log_;
  DirState input_;
  DirState output_;
  uint32_t generation_ = 0;
  bool microphone_muted_ = false;
  bool speaker_muted_ = false;
};

}  // namespace partyup

#endif  // PARTYUP_MEDIA_SRC_AUDIO_DEVICE_CONTROL_H_
