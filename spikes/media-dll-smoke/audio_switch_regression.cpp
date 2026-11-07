// PartyUp 媒体核心 · 通话中音频设备控制的 fake ADM 回归（v2.15）。
//
// 目的：**不打开任何真实麦克风/扬声器**，用脚本化 ADM 端口 + 受控默认端点驱动
// 生产实现 src/media-core/src/audio_device_control.cpp 的 AudioDeviceController，
// 覆盖：
//   * 通话中切输入/输出：必须是 Stop -> SetDevice -> Init ->（静音门禁）->
//     Start 的真实事务，PU_OK 只在新设备实际启动且静音状态已确认后返回；
//   * index=-1 走 ADM 的 kDefaultDevice（记录为 set:-1），绝不映射成枚举 0；
//   * 设备顺序变化时按稳定端点 ID 重解析，原选择不会指向别的设备；
//   * 选中设备被拔掉：通话中真正回退系统默认，fallback 位置位；
//   * 系统默认端点变化（设备数量/名称都不变，仅端点 ID 变化）：真正重新选择并重启；
//   * Set/Stop/Init/Start 失败：恢复旧设备；恢复失败回落默认；两种都失败置 fault，
//     绝不返回 PU_OK 假装成功；
//   * 设备重启后保持麦克风静音 / 闭音状态；fake 每次 Init 清 mute、记录每次成功
//     Start 瞬间的 mute 值，因此能抓到「先 Start 后 mute」的泄漏窗口，而不只看最终
//     bool；SetMuted / readback 失败、Start 时重置 mute、恢复旧设备、回退默认都
//     断言没有任何一次未静音的 Start；
//   * fault 后重新选择设备才真正恢复；未真实恢复前 status 不得清 fault；
//   * join 前选择、join 前设备消失回落、默认端点选择失败。
//
// 这个测试链接的是**生产控制逻辑**（同一份 audio_device_control.cpp），不是把算法
// 在测试里重写一遍；fake 只替换 ADM 的 I/O 与失败注入。真实 WASAPI 设备行为仍未
// 验证（真机未验），本回归不能代替硬件验收。
//
// 不依赖 partyup_media.dll / libwebrtc：没有声音设备访问，可独立安全运行。
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio_device_control.h"
#include "partyup_media.h"

namespace {

using Direction = partyup::AudioDeviceDirection;
constexpr Direction kInput = Direction::kInput;
constexpr Direction kOutput = Direction::kOutput;
constexpr int kIn = 0;
constexpr int kOut = 1;

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool ok, const char* expression, const char* test, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  std::printf("FAIL %s:%d [%s] %s\n", __FILE__, line, test, expression);
}

#define CHECK(cond) CheckImpl((cond), #cond, __func__, __LINE__)

int DirIndex(Direction dir) { return dir == kInput ? kIn : kOut; }
const char* DirName(Direction dir) { return dir == kInput ? "in" : "out"; }

// 脚本化 ADM：行为按 libwebrtc AudioDeviceWindowsCore 的真实约束建模：
//   * initialized 状态下 SetDevice 返回 -1（必须先 Stop）；
//   * Init 在已 initialized 时返回 0（不会切设备），所以控制层必须真的 Stop 再 Set；
//   * Init 按“当前选择”解析设备：index>=0 取集合项，index=-1 按系统默认端点 ID；
//   * 设备集合顺序可以变化，index 只是位置。
class FakeDevicePort final : public partyup::AudioDevicePort {
 public:
  struct Device {
    std::string name;
    std::string guid;
  };

  std::vector<Device> devices[2];
  std::string default_id[2];
  bool initialized[2] = {false, false};
  bool started[2] = {false, false};
  int32_t selected[2] = {-1, -1};
  std::string active_guid[2];
  int32_t active_index[2] = {-1};
  bool mic_muted = false;
  bool speaker_muted = false;
  /* 模拟某些平台在 Start 时把端点静音重置掉：Start 成功记录瞬间值后立刻清 mute。
   * 控制层必须在 Start 之后再次 readback，发现重置就 Stop 并置 fault。 */
  bool reset_mute_on_start[2] = {false, false};
  /* 每次**成功** Start 时设备瞬间的静音值。只为「当前端点」记录，不受之后的
   * mute 调用影响；测试用它证明没有「先 Start 后 mute」的泄漏窗口。 */
  std::vector<bool> start_muted[2];
  std::vector<std::string> started_guid[2];

  std::vector<std::string> calls;
  std::map<std::string, int> calls_by_op;
  std::map<std::string, std::set<int>> fail_at;  // op -> 第 N 次调用失败（1 起）
  std::map<std::string, int> fail_from;          // op -> 从第 N 次起一直失败

  void ClearCalls() {
    calls.clear();
    calls_by_op.clear();
  }

  void ClearStartLog() {
    start_muted[kIn].clear();
    start_muted[kOut].clear();
    started_guid[kIn].clear();
    started_guid[kOut].clear();
  }

  bool Muted(Direction dir) const { return dir == kInput ? mic_muted : speaker_muted; }

  void SetMutedValue(Direction dir, bool muted) {
    if (dir == kInput) {
      mic_muted = muted;
    } else {
      speaker_muted = muted;
    }
  }

  bool AllStartsMuted(Direction dir) const {
    for (bool muted : start_muted[DirIndex(dir)]) {
      if (!muted) return false;
    }
    return true;
  }

  int Count(const std::string& exact) const {
    int n = 0;
    for (const std::string& call : calls) {
      if (call == exact) ++n;
    }
    return n;
  }

  int CountPrefix(const std::string& prefix) const {
    int n = 0;
    for (const std::string& call : calls) {
      if (call.rfind(prefix, 0) == 0) ++n;
    }
    return n;
  }

  bool InOrder(const std::vector<std::string>& sequence) const {
    size_t position = 0;
    for (const std::string& call : calls) {
      if (position < sequence.size() && call == sequence[position]) ++position;
    }
    return position == sequence.size();
  }

  int Enumerate(Direction dir, std::vector<partyup::AudioDeviceEntry>* out) override {
    const int d = DirIndex(dir);
    calls.push_back(std::string("enum-") + DirName(dir));
    out->clear();
    for (size_t i = 0; i < devices[d].size(); ++i) {
      partyup::AudioDeviceEntry entry;
      entry.index = static_cast<int32_t>(i);
      entry.name = devices[d][i].name;
      entry.guid = devices[d][i].guid;
      out->push_back(entry);
    }
    return static_cast<int>(out->size());
  }

  int SetDevice(Direction dir, int32_t index) override {
    const int d = DirIndex(dir);
    const std::string name = DirName(dir);
    calls.push_back(std::string("set-") + name + ":" + std::to_string(index));
    if (ShouldFail(std::string("set-") + name)) return -1;
    if (initialized[d]) return -1;  // 真实 ADM：初始化时拒绝换设备
    if (index >= 0 && index >= static_cast<int32_t>(devices[d].size())) return -1;
    selected[d] = index;
    return 0;
  }

  bool IsInitialized(Direction dir) const override { return initialized[DirIndex(dir)]; }
  bool IsStarted(Direction dir) const override { return started[DirIndex(dir)]; }

  int Stop(Direction dir) override {
    const int d = DirIndex(dir);
    const std::string name = DirName(dir);
    calls.push_back(std::string("stop-") + name);
    if (ShouldFail(std::string("stop-") + name)) return -1;  // 失败时保持原状态
    initialized[d] = false;
    started[d] = false;
    return 0;
  }

  int Init(Direction dir) override {
    const int d = DirIndex(dir);
    const std::string name = DirName(dir);
    calls.push_back(std::string("init-") + name);
    if (ShouldFail(std::string("init-") + name)) return -1;
    if (started[d]) return -1;
    if (initialized[d]) return 0;  // 真实 ADM：已初始化时直接成功，不会换设备
    int32_t index = selected[d];
    if (index < 0) {
      index = -1;
      for (size_t i = 0; i < devices[d].size(); ++i) {
        if (devices[d][i].guid == default_id[d]) {
          index = static_cast<int32_t>(i);
          break;
        }
      }
      if (index < 0) return -1;  // 没有可用的系统默认端点
    } else if (index >= static_cast<int32_t>(devices[d].size())) {
      return -1;  // 集合变化后旧序号失效
    }
    initialized[d] = true;
    active_index[d] = index;
    active_guid[d] = devices[d][index].guid;
    // 模拟真实端点重新打开后的复位：新设备默认未静音。控制层必须在 Start 之前
    // 重新 SetMuted + 读回，否则这里的 false 会被下面的 Start 记录成泄漏窗口。
    SetMutedValue(dir, false);
    return 0;
  }

  int Start(Direction dir) override {
    const int d = DirIndex(dir);
    const std::string name = DirName(dir);
    calls.push_back(std::string("start-") + name);
    if (ShouldFail(std::string("start-") + name)) return -1;
    if (!initialized[d]) return -1;
    // 只有真正成功启动才记录瞬间静音值；这里是「音频开始输出/采集」的那一瞬。
    start_muted[d].push_back(Muted(dir));
    started_guid[d].push_back(active_guid[d]);
    started[d] = true;
    if (reset_mute_on_start[d]) SetMutedValue(dir, false);
    return 0;
  }

  int SetMuted(Direction dir, bool muted) override {
    const std::string name = DirName(dir);
    calls.push_back(std::string("mute-") + name + ":" + (muted ? "1" : "0"));
    if (ShouldFail(std::string("mute-") + name)) return -1;
    SetMutedValue(dir, muted);
    return 0;
  }

  int GetMuted(Direction dir, bool* muted) override {
    const std::string name = DirName(dir);
    calls.push_back(std::string("getmute-") + name);
    if (ShouldFail(std::string("getmute-") + name)) return -1;
    if (muted == nullptr) return -1;
    *muted = Muted(dir);
    return 0;
  }

 private:
  bool ShouldFail(const std::string& op) {
    const int count = ++calls_by_op[op];
    const auto from = fail_from.find(op);
    if (from != fail_from.end() && count >= from->second) return true;
    const auto at = fail_at.find(op);
    if (at == fail_at.end()) return false;
    return at->second.count(count) != 0;
  }
};

class FakeDefaultEndpointProbe final : public partyup::AudioDefaultEndpointProbe {
 public:
  std::string ids[2];

  std::string DefaultEndpointId(Direction dir) override { return ids[DirIndex(dir)]; }
};

struct Harness {
  FakeDevicePort port;
  FakeDefaultEndpointProbe probe;
  partyup::AudioDeviceController controller;
  std::vector<std::string> logs;

  Harness() {
    port.devices[kIn] = {{"Mic A", "guid-mic-a"},
                         {"Mic B", "guid-mic-b"},
                         {"Mic C", "guid-mic-c"}};
    port.devices[kOut] = {{"Speaker A", "spk-a"}, {"Speaker B", "spk-b"}};
    port.default_id[kIn] = "guid-mic-a";
    port.default_id[kOut] = "spk-a";
    probe.ids[kIn] = port.default_id[kIn];
    probe.ids[kOut] = port.default_id[kOut];
    controller.Init(&port, &probe);
    controller.SetLogSink([this](int, const std::string& message) { logs.push_back(message); });
  }

  bool SelectBeforeJoin(Direction dir, int32_t index) {
    std::string error;
    if (controller.SetDevice(dir, index, &error) == PU_OK) return true;
    std::printf("FAIL: SetDevice(%s,%d) before join: %s\n", DirName(dir),
                static_cast<int>(index), error.c_str());
    return false;
  }

  // 模拟一次真实语音通话：先把选择写进 ADM，再由音频管线 Init/Start。
  bool Join() {
    std::string error;
    if (controller.ApplyBeforeJoin(&error) != PU_OK) {
      std::printf("FAIL: ApplyBeforeJoin: %s\n", error.c_str());
      return false;
    }
    for (Direction dir : {kInput, kOutput}) {
      if (port.Init(dir) != 0) {
        std::printf("FAIL: ADM Init(%s) rejected the selected device\n", DirName(dir));
        return false;
      }
      if (port.Start(dir) != 0) {
        std::printf("FAIL: ADM Start(%s) failed\n", DirName(dir));
        return false;
      }
    }
    return true;
  }

  bool JoinWithInput(int32_t index) {
    return SelectBeforeJoin(kInput, index) && Join();
  }

  bool JoinWithOutput(int32_t index) {
    return SelectBeforeJoin(kOutput, index) && Join();
  }

  bool MuteBoth() {
    std::string error;
    if (controller.SetMuteState(true, true, &error) == PU_OK) return true;
    std::printf("FAIL: SetMuteState(true,true): %s\n", error.c_str());
    return false;
  }

  partyup::AudioDeviceController::Snapshot Snap(uint32_t caller_generation = 0) {
    return controller.GetSnapshot(caller_generation);
  }
};

// ---------------------------------------------------------------------------
// 1) 通话中切输入：真实 Stop->Set->Init->Start，PU_OK 时新设备已在跑。
// ---------------------------------------------------------------------------
void InCallInputSwitch() {
  Harness h;
  CHECK(h.Join());
  CHECK(h.port.active_guid[kIn] == "guid-mic-a");
  h.port.ClearCalls();

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) == PU_OK);
  CHECK(error.empty());
  CHECK(h.port.selected[kIn] == 2);
  CHECK(h.port.active_guid[kIn] == "guid-mic-c");
  CHECK(h.port.active_index[kIn] == 2);
  CHECK(h.port.started[kIn]);
  CHECK(h.port.initialized[kIn]);
  CHECK(h.port.InOrder({"stop-in", "set-in:2", "init-in", "start-in"}));
  CHECK(h.port.CountPrefix("stop-out") == 0);
  CHECK(h.port.CountPrefix("set-out") == 0);

  const auto snapshot = h.Snap();
  CHECK(snapshot.input_index == 2);
  CHECK(snapshot.input_name == "Mic C");
  CHECK(snapshot.fault == 0);
  CHECK(snapshot.fallback == 0);
}

// ---------------------------------------------------------------------------
// 2) 通话中切输出：同一事务，且不碰输入设备。
// ---------------------------------------------------------------------------
void InCallOutputSwitch() {
  Harness h;
  CHECK(h.Join());
  CHECK(h.port.active_guid[kOut] == "spk-a");
  h.port.ClearCalls();

  std::string error;
  CHECK(h.controller.SetDevice(kOutput, 1, &error) == PU_OK);
  CHECK(h.port.active_guid[kOut] == "spk-b");
  CHECK(h.port.InOrder({"stop-out", "set-out:1", "init-out", "start-out"}));
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.Snap().output_index == 1);
  CHECK(h.Snap().output_name == "Speaker B");
}

// ---------------------------------------------------------------------------
// 3) -1 必须是真正的系统默认（kDefaultDevice = 当前默认端点），不是枚举 0。
//    这里故意让 index 0 与系统默认不是同一块设备。
// ---------------------------------------------------------------------------
void SystemDefaultUsesRoleNotIndexZero() {
  Harness h;
  h.port.default_id[kIn] = "guid-mic-b";
  h.probe.ids[kIn] = "guid-mic-b";
  CHECK(h.JoinWithInput(2));  // 先显式选 Mic C
  CHECK(h.port.active_guid[kIn] == "guid-mic-c");
  h.port.ClearCalls();

  std::string error;
  CHECK(h.controller.SetDevice(kInput, -1, &error) == PU_OK);
  // 记录里必须是 set-in:-1；映射成枚举 0 会变成 set-in:0 并打开 Mic A。
  CHECK(h.port.Count("set-in:-1") == 1);
  CHECK(h.port.Count("set-in:0") == 0);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.selected[kIn] == -1);
  const auto snapshot = h.Snap();
  CHECK(snapshot.input_index == -1);
  CHECK(snapshot.input_name == "Mic B");
}

// ---------------------------------------------------------------------------
// 4) 设备重排：选择按稳定端点 ID 跟随，序号更新但不指向别人；不制造音频中断。
// ---------------------------------------------------------------------------
void DeviceReorderKeepsSelection() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);  // 建立签名基线
  const auto before = h.Snap();

  std::swap(h.port.devices[kIn][0], h.port.devices[kIn][1]);  // B, A, C
  h.port.ClearCalls();

  CHECK(h.controller.Refresh(&error) == PU_OK);
  const auto after = h.Snap(before.generation);
  CHECK(after.input_index == 0);
  CHECK(after.input_name == "Mic B");
  CHECK(after.changed);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.port.CountPrefix("start-in") == 0);
}

// ---------------------------------------------------------------------------
// 5) 选中设备被拔掉：通话中真正回退系统默认（Stop->Set(-1)->Init->Start）。
// ---------------------------------------------------------------------------
void UnplugSelectedFallsBackLive() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);
  const auto before = h.Snap();

  h.port.devices[kIn].erase(h.port.devices[kIn].begin() + 1);  // 拔掉 Mic B
  h.port.ClearCalls();

  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK(h.port.active_guid[kIn] == "guid-mic-a");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.InOrder({"stop-in", "set-in:-1", "init-in", "start-in"}));
  const auto after = h.Snap(before.generation);
  CHECK(after.input_index == -1);
  CHECK(after.input_name == "Mic A");
  CHECK((after.fallback & 0x1u) != 0);
  CHECK(after.fault == 0);
  CHECK(after.changed);
}

// ---------------------------------------------------------------------------
// 6) 系统默认端点变化，但设备数量与名称都不变：必须按端点 ID 发现并真正重启。
// ---------------------------------------------------------------------------
void DefaultEndpointChangeWithSameNames() {
  Harness h;
  h.port.devices[kOut] = {{"Speakers", "spk-a"}, {"Speakers", "spk-b"}};
  h.port.default_id[kOut] = "spk-a";
  h.probe.ids[kOut] = "spk-a";
  CHECK(h.Join());
  CHECK(h.port.active_guid[kOut] == "spk-a");
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);
  const auto before = h.Snap();

  // 只有默认端点 ID 变了；数量、名称、顺序完全不变。
  h.probe.ids[kOut] = "spk-b";
  h.port.default_id[kOut] = "spk-b";
  h.port.ClearCalls();

  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK(h.port.active_guid[kOut] == "spk-b");
  CHECK(h.port.started[kOut]);
  CHECK(h.port.InOrder({"stop-out", "set-out:-1", "init-out", "start-out"}));
  CHECK(h.port.CountPrefix("stop-in") == 0);
  const auto after = h.Snap(before.generation);
  CHECK(after.output_index == -1);
  CHECK(after.output_name == "Speakers");
  CHECK(after.changed);
  CHECK((after.fallback & 0x2u) == 0);
  CHECK(after.fault == 0);
}

// ---------------------------------------------------------------------------
// 7) 未在通话中时默认端点变化：只更新状态/代次，不做设备启停（下次 join 生效）。
// ---------------------------------------------------------------------------
void DefaultChangeIdleNoDeviceOps() {
  Harness h;
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);
  const auto before = h.Snap();

  h.probe.ids[kIn] = "guid-mic-c";
  h.port.default_id[kIn] = "guid-mic-c";
  h.port.ClearCalls();

  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK(h.port.CountPrefix("stop-") == 0);
  CHECK(h.port.CountPrefix("set-") == 0);
  CHECK(h.port.CountPrefix("init-") == 0);
  CHECK(h.port.CountPrefix("start-") == 0);
  const auto after = h.Snap(before.generation);
  CHECK(after.input_index == -1);
  CHECK(after.input_name == "Mic C");
  CHECK(after.changed);
}

// ---------------------------------------------------------------------------
// 8) 显式选择的设备不受系统默认变化影响（不要偷偷把用户选择改成默认）。
// ---------------------------------------------------------------------------
void ExplicitSelectionIgnoresDefaultChange() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);
  const auto before = h.Snap();

  h.probe.ids[kIn] = "guid-mic-c";
  h.port.default_id[kIn] = "guid-mic-c";
  h.port.ClearCalls();

  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  const auto after = h.Snap(before.generation);
  CHECK(after.input_index == 1);
  CHECK(after.input_name == "Mic B");
  CHECK(!after.changed);
}

// ---------------------------------------------------------------------------
// 9) SetDevice 被 ADM 拒绝：把实际设备恢复成旧设备，不返回 PU_OK。
// ---------------------------------------------------------------------------
void SetRejectedRollsBack() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  h.port.ClearCalls();
  h.port.fail_at["set-in"] = {1};  // 切 Mic C 的那次 Set 失败

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(!error.empty());
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.InOrder({"stop-in", "set-in:2", "set-in:1", "init-in", "start-in"}));
  CHECK(h.Snap().input_index == 1);
  CHECK(error.find("restored") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 10) Init 失败：恢复旧设备并重新启动。
// ---------------------------------------------------------------------------
void InitFailureRollsBack() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();
  h.port.fail_at["init-in"] = {1};

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.InOrder({"stop-in", "set-in:2", "init-in", "set-in:1", "init-in", "start-in"}));
  CHECK(h.Snap().input_index == 1);
  CHECK(h.Snap().fault == 0);
}

// ---------------------------------------------------------------------------
// 11) Start 失败：恢复旧设备并重新启动。
// ---------------------------------------------------------------------------
void StartFailureRollsBack() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();
  h.port.fail_at["start-in"] = {1};

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.Snap().input_index == 1);
  CHECK(h.Snap().fault == 0);
}

// ---------------------------------------------------------------------------
// 12) Stop 失败：不能假装已经切过去，状态与选择都不变。
// ---------------------------------------------------------------------------
void StopFailureDoesNotFakeSuccess() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();
  h.port.fail_at["stop-in"] = {1};

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(error.find("stop") != std::string::npos);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.Count("set-in:2") == 0);  // 没停成功就绝不 Set
  const auto snapshot = h.Snap();
  CHECK(snapshot.input_index == 1);
  CHECK(snapshot.fault == 0);
}

// ---------------------------------------------------------------------------
// 13) 新设备与旧设备都起不来：明确回落系统默认（fallback 置位，错误可读）。
// ---------------------------------------------------------------------------
void RestoreFailureFallsBackToDefault() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  h.port.ClearCalls();
  h.port.fail_at["start-in"] = {1, 2};  // 新设备起不来 + 恢复旧设备也起不来

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(h.port.active_guid[kIn] == "guid-mic-a");
  CHECK(h.port.started[kIn]);
  const auto snapshot = h.Snap();
  CHECK(snapshot.input_index == -1);
  CHECK(snapshot.input_name == "Mic A");
  CHECK((snapshot.fallback & 0x1u) != 0);
  CHECK(snapshot.fault == 0);
  CHECK(error.find("fell back to system default") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 14) 恢复默认也失败：置 fault、方向停止运行、错误明确；不返回 PU_OK。
// ---------------------------------------------------------------------------
void RestoreAndDefaultFailureReportsFault() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();
  h.port.fail_from["start-in"] = 1;  // 所有 Start 都失败

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(!h.port.started[kIn]);
  const auto snapshot = h.Snap();
  CHECK(snapshot.input_index == -1);
  CHECK((snapshot.fault & 0x1u) != 0);
  CHECK((snapshot.fallback & 0x1u) == 0);
  CHECK(error.find("also failed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 15) 设备重启不能把麦克风静音/闭音丢掉。
// ---------------------------------------------------------------------------
void MutePreservedAcrossSwitch() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.controller.SetMuteState(true, true);
  CHECK(h.port.mic_muted);
  CHECK(h.port.speaker_muted);
  h.port.ClearCalls();
  h.port.ClearStartLog();

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) == PU_OK);
  // Safety contract: the new endpoint must be muted before it can capture audio.
  CHECK(h.port.InOrder({"stop-in", "set-in:2", "init-in", "mute-in:1", "start-in"}));
  CHECK(h.port.active_guid[kIn] == "guid-mic-c");
  CHECK(h.port.mic_muted);
  CHECK(h.port.speaker_muted);
  CHECK(h.port.Count("mute-in:1") >= 1);
  CHECK(h.port.Count("mute-in:0") == 0);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);  // 只重启了被切换的输入方向

  // 输出方向同样必须在 Start 之前重施闭音并读回；输入方向不被顺手重启。
  h.port.ClearCalls();
  h.port.ClearStartLog();
  CHECK(h.controller.SetDevice(kOutput, 1, &error) == PU_OK);
  CHECK(h.port.InOrder({"stop-out", "set-out:1", "init-out", "mute-out:1", "start-out"}));
  CHECK(h.port.active_guid[kOut] == "spk-b");
  CHECK(h.port.speaker_muted);
  CHECK(h.port.Count("mute-out:0") == 0);
  CHECK(h.port.AllStartsMuted(kOutput));
  CHECK(h.port.start_muted[kOut].size() == 1);
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.port.CountPrefix("start-in") == 0);
}

// ---------------------------------------------------------------------------
// 15a) 启动前静音失败（输入）：新端点必须保持停止，旧端点恢复时同样先静音再
//      Start；不能出现任何一次未静音的 Start。
// ---------------------------------------------------------------------------
void MuteSetFailureOnInputRestoresOldMuted() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["mute-in"] = {1};  // 新设备的第一次静音失败

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(error.find("restored") != std::string::npos);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.mic_muted);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);
  CHECK(h.port.started_guid[kIn][0] == "guid-mic-b");
  CHECK(h.Snap().input_index == 1);
  CHECK(h.Snap().fault == 0);
}

// ---------------------------------------------------------------------------
// 15b) 静音 readback 失败（输入）：静音写入返回成功但查询失败，同样不得 Start；
//      恢复旧设备也必须重新读回确认。
// ---------------------------------------------------------------------------
void MuteReadbackFailureOnInputRestoresOldMuted() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["getmute-in"] = {1};  // 新设备静音后的第一次 readback 失败

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(error.find("restored") != std::string::npos);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.mic_muted);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);
  CHECK(h.port.started_guid[kIn][0] == "guid-mic-b");
  CHECK(h.Snap().fault == 0);
}

// ---------------------------------------------------------------------------
// 15c) 静音写入持续失败（输入）：新设备恢复旧设备、回退默认全部失败时，
//      该方向必须停止并置 fault，且没有任何一次成功 Start。
// ---------------------------------------------------------------------------
void MuteFailureAllPathsStopsInput() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_from["mute-in"] = 1;

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(!h.port.started[kIn]);
  CHECK(!h.port.initialized[kIn]);
  CHECK(h.port.start_muted[kIn].empty());
  CHECK((h.Snap().fault & 0x1u) != 0);
  CHECK((h.Snap().fallback & 0x1u) == 0);
  CHECK(error.find("also failed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 15d) 静音 readback 持续失败（输入）：与 15c 同样必须安全停止，不得把查询失败
//      当成「未静音但可以继续跑」。
// ---------------------------------------------------------------------------
void MuteReadbackFailureAllPathsStopsInput() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_from["getmute-in"] = 1;

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(!h.port.started[kIn]);
  CHECK(h.port.start_muted[kIn].empty());
  CHECK((h.Snap().fault & 0x1u) != 0);
  CHECK(error.find("also failed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 15e) 启动前闭音失败（输出）：deafen 方向同样必须先静音再 Start，恢复旧设备
//      也要走同一门禁。
// ---------------------------------------------------------------------------
void MuteSetFailureOnOutputRestoresOldMuted() {
  Harness h;
  CHECK(h.Join());  // 输出默认 Speaker A
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["mute-out"] = {1};

  std::string error;
  CHECK(h.controller.SetDevice(kOutput, 1, &error) != PU_OK);
  CHECK(error.find("restored") != std::string::npos);
  CHECK(h.port.active_guid[kOut] == "spk-a");
  CHECK(h.port.started[kOut]);
  CHECK(h.port.speaker_muted);
  CHECK(h.port.AllStartsMuted(kOutput));
  CHECK(h.port.start_muted[kOut].size() == 1);
  CHECK(h.port.started_guid[kOut][0] == "spk-a");
  CHECK(h.Snap().fault == 0);
}

// ---------------------------------------------------------------------------
// 15f) 闭音 readback 持续失败（输出）：必须停止输出方向并置 fault，不得在
//      「界面仍 deafened」时继续播放。
// ---------------------------------------------------------------------------
void MuteReadbackFailureAllPathsStopsOutput() {
  Harness h;
  CHECK(h.Join());
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_from["getmute-out"] = 1;

  std::string error;
  CHECK(h.controller.SetDevice(kOutput, 1, &error) != PU_OK);
  CHECK(!h.port.started[kOut]);
  CHECK(h.port.start_muted[kOut].empty());
  CHECK((h.Snap().fault & 0x2u) != 0);
  CHECK(error.find("audio is stopped") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 15g) 设备状态被外部重置（mute 丢了）但控制层镜像仍是 true：重复选择同一设备
//      不能只返回 PU_OK，必须先重施并读回静音；且不做无谓的停/启。
// ---------------------------------------------------------------------------
void DeviceResetOnSameSelectionReappliesMute() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.SetMutedValue(kInput, false);  // 模拟设备/系统把静音重置掉
  h.port.ClearCalls();
  h.port.ClearStartLog();

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 1, &error) == PU_OK);
  CHECK(h.port.mic_muted);
  CHECK(h.port.Count("mute-in:1") >= 1);
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.port.CountPrefix("start-in") == 0);
  CHECK(h.port.start_muted[kIn].empty());
}

// ---------------------------------------------------------------------------
// 15h) 平台在 Start 时重置静音：Start 瞬间虽然已静音，但之后读回失败必须立刻
//      Stop 并置 fault，不能带着未静音设备继续运行。
// ---------------------------------------------------------------------------
void StartResetMuteStopsDirection() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.reset_mute_on_start[kIn] = true;

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(!h.port.started[kIn]);
  CHECK(!h.port.initialized[kIn]);
  CHECK((h.Snap().fault & 0x1u) != 0);
  CHECK(!h.port.start_muted[kIn].empty());
  CHECK(h.port.AllStartsMuted(kInput));  // Start 那一瞬仍是静音；重置发生在之后
  CHECK(error.find("started unmuted") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 15i) 回退系统默认成功时也必须先静音再 Start（旧恢复分支曾直接返回/先启后静音）。
// ---------------------------------------------------------------------------
void MuteGateCoversFallbackDefault() {
  Harness h;
  CHECK(h.JoinWithInput(1));  // Mic B
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["start-in"] = {1, 2};  // 新设备 + 恢复旧设备都起不来

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK(error.find("fell back to system default") != std::string::npos);
  CHECK(h.port.active_guid[kIn] == "guid-mic-a");
  CHECK(h.port.started[kIn]);
  CHECK(h.port.mic_muted);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);
  CHECK(h.port.started_guid[kIn][0] == "guid-mic-a");
  const auto snapshot = h.Snap();
  CHECK((snapshot.fallback & 0x1u) != 0);
  CHECK(snapshot.fault == 0);
}

// ---------------------------------------------------------------------------
// 15j) fault 后重新选择设备：必须按故障前的运行形态真正重启（并过静音门禁），
//      成功后清 fault；不能只写选择就谎报恢复。
// ---------------------------------------------------------------------------
void FaultedDirectionRecoversOnReselect() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_from["mute-in"] = 1;

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK((h.Snap().fault & 0x1u) != 0);
  CHECK(!h.port.started[kIn]);

  h.port.fail_from.erase("mute-in");
  h.port.ClearCalls();
  h.port.ClearStartLog();
  CHECK(h.controller.SetDevice(kInput, 0, &error) == PU_OK);
  CHECK(h.port.started[kIn]);
  CHECK(h.port.mic_muted);
  CHECK(h.Snap().fault == 0);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);
}

// ---------------------------------------------------------------------------
// 15k) fault 可见性：恢复条件未满足时 Refresh 不能把 fault 清掉或偷偷启动；
//      只有真正重新选择并成功启动才恢复健康。
// ---------------------------------------------------------------------------
void FaultStaysVisibleUntilRealRecovery() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_from["mute-in"] = 1;
  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) != PU_OK);
  CHECK((h.Snap().fault & 0x1u) != 0);
  CHECK(!h.port.started[kIn]);

  h.port.fail_from.erase("mute-in");
  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK((h.Snap().fault & 0x1u) != 0);  // 没有真实恢复前，fault 不能被伪装成健康
  CHECK(!h.port.started[kIn]);
}

// ---------------------------------------------------------------------------
// 15k2) 取消静音失败：不能把镜像记成 false 后还让界面保持 muted；该方向必须
//       停下，且下一次重新选择设备时仍然先静音再 Start。
// ---------------------------------------------------------------------------
void FailedUnmuteKeepsFutureStartsMuted() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  CHECK(h.MuteBoth());
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["getmute-in"] = {1};  // SetMuted(false) 成功但 readback 失败

  std::string error;
  CHECK(h.controller.SetMuteStateFor(kInput, false, false, true, &error) != PU_OK);
  CHECK(!error.empty());
  CHECK(!h.port.started[kIn]);
  CHECK(!h.port.initialized[kIn]);
  CHECK((h.Snap().fault & 0x1u) != 0);

  h.port.fail_at.erase("getmute-in");
  h.port.ClearCalls();
  h.port.ClearStartLog();
  CHECK(h.controller.SetDevice(kInput, 2, &error) == PU_OK);
  CHECK(h.port.started[kIn]);
  CHECK(h.port.mic_muted);
  CHECK(h.port.AllStartsMuted(kInput));
  CHECK(h.port.start_muted[kIn].size() == 1);
}

// ---------------------------------------------------------------------------
// 15k3) 用户静音写入失败（方向正在运行）：必须返回错误、停掉该方向并置 fault；
//       不能把「界面 muted」当成已经生效。
// ---------------------------------------------------------------------------
void ActiveMuteSetFailureStopsDirection() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["mute-in"] = {1};

  std::string error;
  CHECK(h.controller.SetMuteStateFor(kInput, true, true, false, &error) != PU_OK);
  CHECK(!error.empty());
  CHECK(!h.port.started[kIn]);
  CHECK(!h.port.initialized[kIn]);
  CHECK((h.Snap().fault & 0x1u) != 0);
}

// ---------------------------------------------------------------------------
// 15k4) 未运行的方向静音失败：返回错误，但不要把「空闲设备的写失败」渲染成
//       音频故障（没有可泄漏的音频）；镜像仍保守地记为需要静音。
// ---------------------------------------------------------------------------
void IdleMuteFailureReportsErrorWithoutFault() {
  Harness h;
  h.port.fail_at["mute-in"] = {1};
  std::string error;
  CHECK(h.controller.SetMuteStateFor(kInput, true, true, false, &error) != PU_OK);
  CHECK(!error.empty());
  CHECK((h.Snap().fault & 0x1u) == 0);
  CHECK(!h.port.started[kIn]);
}

// ---------------------------------------------------------------------------
// 15l) 默认端点切换失败在 Stop 阶段：不能先把 default_id 写成新值而丢掉下一次
//      Refresh 的重试；下一次轮询必须真正切到新默认端点。
// ---------------------------------------------------------------------------
void DefaultChangeStopFailureRetriesNextRefresh() {
  Harness h;
  CHECK(h.Join());
  std::string error;
  CHECK(h.controller.Refresh(&error) == PU_OK);  // 建立默认端点基线
  const auto before = h.Snap();

  h.probe.ids[kOut] = "spk-b";
  h.port.default_id[kOut] = "spk-b";
  h.port.ClearCalls();
  h.port.ClearStartLog();
  h.port.fail_at["stop-out"] = {1};

  CHECK(h.controller.Refresh(&error) != PU_OK);
  CHECK(error.find("could not stop") != std::string::npos);
  CHECK(h.port.active_guid[kOut] == "spk-a");  // 旧默认仍在跑
  CHECK(h.port.started[kOut]);

  h.port.fail_at.erase("stop-out");  // 允许下一次 Refresh 真正重试
  h.port.ClearCalls();
  CHECK(h.controller.Refresh(&error) == PU_OK);
  CHECK(h.port.active_guid[kOut] == "spk-b");
  CHECK(h.port.started[kOut]);
  const auto after = h.Snap(before.generation);
  CHECK(after.output_index == -1);
  CHECK(after.changed);
  CHECK(after.fault == 0);
}

// ---------------------------------------------------------------------------
// 16) 重复选择同一设备：不做一次没必要的停/启（避免通话音频被切断）。
// ---------------------------------------------------------------------------
void SameDeviceReselectNoGlitch() {
  Harness h;
  CHECK(h.JoinWithInput(1));
  h.port.ClearCalls();

  std::string error;
  CHECK(h.controller.SetDevice(kInput, 1, &error) == PU_OK);
  CHECK(h.port.CountPrefix("stop-in") == 0);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.port.CountPrefix("start-in") == 0);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
}

// ---------------------------------------------------------------------------
// 17) 未在通话中：选择立即写进 ADM（不是只缓存），不需要停/启。
// ---------------------------------------------------------------------------
void IdleSetAppliesImmediately() {
  Harness h;
  std::string error;
  CHECK(h.controller.SetDevice(kInput, 2, &error) == PU_OK);
  CHECK(h.port.selected[kIn] == 2);
  CHECK(h.port.CountPrefix("init-in") == 0);
  CHECK(h.port.CountPrefix("start-in") == 0);
  CHECK(h.Snap().input_index == 2);
  CHECK(h.Snap().input_name == "Mic C");

  h.port.ClearCalls();
  CHECK(h.controller.SetDevice(kInput, -1, &error) == PU_OK);
  CHECK(h.port.Count("set-in:-1") == 1);
  CHECK(h.port.selected[kIn] == -1);
  CHECK(h.Snap().input_name == "Mic A");
}

// ---------------------------------------------------------------------------
// 18) join 前选中的设备已经消失：join 时回落默认并报告 fallback，不按旧序号猜。
// ---------------------------------------------------------------------------
void ApplyBeforeJoinFallsBackWhenDeviceGone() {
  Harness h;
  CHECK(h.SelectBeforeJoin(kInput, 1));  // Mic B（还未 join）
  h.port.devices[kIn].erase(h.port.devices[kIn].begin() + 1);
  h.port.ClearCalls();

  std::string error;
  CHECK(h.controller.ApplyBeforeJoin(&error) == PU_OK);
  CHECK(h.port.selected[kIn] == -1);
  CHECK((h.Snap().fallback & 0x1u) != 0);
  CHECK(h.port.Init(kInput) == 0);
  CHECK(h.port.Start(kInput) == 0);
  CHECK(h.port.active_guid[kIn] == "guid-mic-a");
}

// ---------------------------------------------------------------------------
// 19) join 前连系统默认都选不上：明确失败并置 fault，不继续用未生效的设备。
// ---------------------------------------------------------------------------
void ApplyBeforeJoinFailureIsReported() {
  Harness h;
  h.port.fail_at["set-in"] = {1};
  std::string error;
  CHECK(h.controller.ApplyBeforeJoin(&error) != PU_OK);
  CHECK(!error.empty());
  CHECK((h.Snap().fault & 0x1u) != 0);
}

// ---------------------------------------------------------------------------
// 20) 枚举的 is_default 按系统默认端点 ID 标记（不是 index==0）。
// ---------------------------------------------------------------------------
void EnumerationMarksSystemDefault() {
  Harness h;
  h.port.default_id[kIn] = "guid-mic-b";
  h.probe.ids[kIn] = "guid-mic-b";
  std::vector<partyup::AudioDeviceEntry> entries;
  std::string error;
  CHECK(h.controller.EnumerateDevices(kInput, &entries, &error) >= 0);
  CHECK(entries.size() == 3);
  CHECK(!entries[0].is_default);
  CHECK(entries[1].is_default);
  CHECK(!entries[2].is_default);
  CHECK(entries[1].guid == "guid-mic-b");
}

// ---------------------------------------------------------------------------
// 21) 只初始化、还没启动的方向：换设备后保持「已初始化未启动」，不要顺手启动它。
// ---------------------------------------------------------------------------
void SwitchWhileInitializedNotStartedKeepsStopped() {
  Harness h;
  std::string error;
  CHECK(h.controller.ApplyBeforeJoin(&error) == PU_OK);
  CHECK(h.port.Init(kInput) == 0);
  CHECK(!h.port.started[kIn]);
  h.port.ClearCalls();

  CHECK(h.controller.SetDevice(kInput, 1, &error) == PU_OK);
  CHECK(h.port.initialized[kIn]);
  CHECK(!h.port.started[kIn]);
  CHECK(h.port.active_guid[kIn] == "guid-mic-b");
  CHECK(h.port.CountPrefix("start-in") == 0);
}

// ---------------------------------------------------------------------------
// 22) 非法索引：明确拒绝，不产生任何设备操作。
// ---------------------------------------------------------------------------
void UnknownIndexRejected() {
  Harness h;
  h.port.ClearCalls();
  std::string error;
  CHECK(h.controller.SetDevice(kInput, 42, &error) == PU_ERR_STATE);
  CHECK(h.port.CountPrefix("set-in") == 0);
  CHECK(h.port.CountPrefix("stop-in") == 0);
}

}  // namespace

int main() {
  std::printf("== PartyUp audio device controller regression (fake ADM, no real audio I/O)\n");
  InCallInputSwitch();
  InCallOutputSwitch();
  SystemDefaultUsesRoleNotIndexZero();
  DeviceReorderKeepsSelection();
  UnplugSelectedFallsBackLive();
  DefaultEndpointChangeWithSameNames();
  DefaultChangeIdleNoDeviceOps();
  ExplicitSelectionIgnoresDefaultChange();
  SetRejectedRollsBack();
  InitFailureRollsBack();
  StartFailureRollsBack();
  StopFailureDoesNotFakeSuccess();
  RestoreFailureFallsBackToDefault();
  RestoreAndDefaultFailureReportsFault();
  MutePreservedAcrossSwitch();
  MuteSetFailureOnInputRestoresOldMuted();
  MuteReadbackFailureOnInputRestoresOldMuted();
  MuteFailureAllPathsStopsInput();
  MuteReadbackFailureAllPathsStopsInput();
  MuteSetFailureOnOutputRestoresOldMuted();
  MuteReadbackFailureAllPathsStopsOutput();
  DeviceResetOnSameSelectionReappliesMute();
  StartResetMuteStopsDirection();
  MuteGateCoversFallbackDefault();
  FaultedDirectionRecoversOnReselect();
  FaultStaysVisibleUntilRealRecovery();
  FailedUnmuteKeepsFutureStartsMuted();
  ActiveMuteSetFailureStopsDirection();
  IdleMuteFailureReportsErrorWithoutFault();
  DefaultChangeStopFailureRetriesNextRefresh();
  SameDeviceReselectNoGlitch();
  IdleSetAppliesImmediately();
  ApplyBeforeJoinFallsBackWhenDeviceGone();
  ApplyBeforeJoinFailureIsReported();
  EnumerationMarksSystemDefault();
  SwitchWhileInitializedNotStartedKeepsStopped();
  UnknownIndexRejected();

  std::printf("== checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures != 0) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: PASS\n");
  return 0;
}
