// PartyUp AUD02 · 独立输入电平 Start/Stop/Status 的 fake 端口回归。
//
// 这个测试直接编译并驱动**生产 Module** src/media-core/src/audio_monitor.cpp，
// 用脚本化 AudioMonitorPort 替换 ADM 的启动/停止/读回与时钟：
//   * 不打开任何真实麦克风/扬声器，不创建平台 ADM/PeerConnection，不联网；
//   * 不链接 libwebrtc / partyup_media.dll，可独立安全运行（Linux/MSVC 均可）。
//
// 覆盖的行为（与 docs/design/v1.1-aud02-contract.md 对应）：
//   * 语音正在加入/已加入或残留会话时拒绝 Start，且端口零动作；
//   * 显式 Start 才启动输入；重复 Start 幂等，不重复开设备；
//   * 全部 16 bit PCM（含全零）都计帧；mono/stereo 形状合法，非法形状/旧代次丢弃；
//   * peak/RMS/帧数/新鲜时间来自真实 PCM；过期/停止后 valid=false，旧电平清零；
//   * Stop 幂等、不依赖 status 缓冲；Stop/读回失败进入 Faulted 并如实报告 running
//     （仍运行/未知），只有显式重试成功才清故障；
//   * Stop 不得停掉已经由 voice 接管的输入（不动别人的录音）；
//   * 设备事务把输入停掉后 Reconcile 只报故障，绝不自动重新开采集；
//   * PCM 线程与 Stop/重启并发时旧代次样本不会污染新会话；析构会停掉输入。
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "audio_monitor.h"
#include "partyup_media.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool ok, const char* expression, const char* test, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  std::printf("FAIL %s:%d [%s] %s\n", __FILE__, line, test, expression);
}

#define CHECK(cond) CheckImpl((cond), #cond, __func__, __LINE__)

class FakeMonitorPort final : public partyup::AudioMonitorPort {
 public:
  bool started = false;
  int start_rc = 0;
  int stop_rc = 0;
  int running_override = -1;  // >=0 时覆盖读回（模拟 ADM 状态与返回值不一致）
  int64_t now_ms = 1000;
  int start_calls = 0;
  int stop_calls = 0;
  int running_calls = 0;
  std::string start_error = "fake: input start failed";
  std::string stop_error = "fake: input stop failed";

  int StartInput(std::string* error) override {
    ++start_calls;
    if (start_rc != 0) {
      if (error != nullptr) *error = start_error;
      return start_rc;
    }
    started = true;
    return 0;
  }

  int StopInput(std::string* error) override {
    ++stop_calls;
    if (stop_rc != 0) {
      if (error != nullptr) *error = stop_error;
      return stop_rc;
    }
    started = false;
    return 0;
  }

  int InputRunning() override {
    ++running_calls;
    if (running_override >= 0) return running_override;
    if (running_override == -2) return -1;  // 显式模拟「无法确认」
    return started ? 1 : 0;
  }

  int64_t NowMs() override { return now_ms; }
};

pu_audio_monitor_info NewInfo() {
  pu_audio_monitor_info info{};
  info.struct_size = static_cast<uint32_t>(sizeof(info));
  return info;
}

void TestVoiceInUseRefusesStartWithoutPortAction() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  monitor.SetVoiceOwnsCapture(true);

  std::string error;
  CHECK(monitor.Start(&error) == PU_ERR_STATE);
  CHECK(!error.empty());
  CHECK(port.start_calls == 0);
  CHECK(port.stop_calls == 0);
  CHECK(port.running_calls == 0);

  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(info.running == 0);
  CHECK(info.valid == 0);
  CHECK(info.frames == 0);
}

void InjectPcm(partyup::AudioMonitor* monitor,
               const int16_t* samples,
               size_t frames,
               size_t channels = 1,
               size_t bytes_per_sample = 2,
               uint32_t sample_rate = 48000) {
  monitor->OnCapturePcm(monitor->CaptureEpoch(), samples, frames, bytes_per_sample,
                        channels, sample_rate);
}

void TestStartPcmAndStop() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;

  CHECK(monitor.Start(&error) == PU_OK);
  CHECK(error.empty());
  CHECK(port.start_calls == 1);
  CHECK(port.started);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_ACTIVE);
  CHECK(info.running == 1);
  CHECK(info.valid == 0);  // 还没有 PCM：不能把 0 当有效静音
  CHECK(info.frames == 0);

  // 重复 Start 幂等：不重复开设备。
  CHECK(monitor.Start(&error) == PU_OK);
  CHECK(port.start_calls == 1);

  // 合成 16 bit mono PCM：peak 与真实 RMS 都必须是实测值。
  int16_t alternating[480];
  for (size_t i = 0; i < 480; ++i) alternating[i] = (i % 2 == 0) ? 1000 : -1000;
  port.now_ms = 2000;
  InjectPcm(&monitor, alternating, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_ACTIVE);
  CHECK(info.running == 1);
  CHECK(info.valid == 1);
  CHECK(info.peak == 1000);
  CHECK(info.rms == 1000);
  CHECK(info.frames == 1);
  CHECK(info.last_frame_ms == 2000);

  // 全零 PCM 是**有效测量**（真实静音），不能按非零阈值过滤。
  int16_t zeros[480] = {};
  port.now_ms = 2010;
  InjectPcm(&monitor, zeros, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.valid == 1);
  CHECK(info.peak == 0);
  CHECK(info.rms == 0);
  CHECK(info.frames == 2);
  CHECK(info.last_frame_ms == 2010);

  // 常见 stereo 形状：frames 是帧数，样本数是 frames*channels。
  int16_t stereo[480];
  for (size_t i = 0; i < 480; ++i) stereo[i] = (i % 2 == 0) ? 2000 : -2000;
  port.now_ms = 2020;
  InjectPcm(&monitor, stereo, 240, /*channels=*/2);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.valid == 1);
  CHECK(info.peak == 2000);
  CHECK(info.rms == 2000);
  CHECK(info.frames == 3);

  // 非法形状整帧丢弃，不能猜格式。
  port.now_ms = 2030;
  InjectPcm(&monitor, alternating, 480, 1, /*bytes_per_sample=*/4);
  InjectPcm(&monitor, alternating, 480, /*channels=*/3);
  InjectPcm(&monitor, alternating, /*frames=*/0);
  InjectPcm(&monitor, alternating, 480, 1, 2, /*sample_rate=*/0);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 3);

  // 超过 1 秒没有新 PCM：valid=0（旧峰值不再当有效电平）。
  port.now_ms += 1001;
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.valid == 0);
  CHECK(info.frames == 3);

  // 新帧把窗口刷新回来。
  port.now_ms += 1;
  InjectPcm(&monitor, alternating, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.valid == 1);

  // Stop：清电平和帧数，幂等，且不重复调端口。
  CHECK(monitor.Stop(&error) == PU_OK);
  CHECK(port.stop_calls == 1);
  CHECK(!port.started);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(info.running == 0);
  CHECK(info.valid == 0);
  CHECK(info.peak == 0);
  CHECK(info.rms == 0);
  CHECK(info.frames == 0);
  CHECK(monitor.Stop(&error) == PU_OK);
  CHECK(port.stop_calls == 1);

  // 停止后旧代次的迟到 PCM 不能复活旧电平。
  InjectPcm(&monitor, alternating, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 0);
  CHECK(info.valid == 0);

  // 重启：新代次计数；上一代的 token 仍然无效。
  const uint64_t old_epoch = monitor.CaptureEpoch();
  CHECK(monitor.Start(&error) == PU_OK);
  CHECK(port.start_calls == 2);
  CHECK(monitor.CaptureEpoch() != old_epoch);
  monitor.OnCapturePcm(old_epoch, alternating, 480, 2, 1, 48000);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 0);
  InjectPcm(&monitor, alternating, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 1);
  CHECK(info.valid == 1);
}

void TestStartFailureIsFaultedAndRetryClears() {
  FakeMonitorPort port;
  port.start_rc = -1;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;

  CHECK(monitor.Start(&error) == PU_ERR_MEDIA);
  CHECK(!error.empty());
  CHECK(error.find("fake: input start failed") != std::string::npos);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == 0);
  CHECK(info.valid == 0);

  // 显式重试成功才清故障。
  port.start_rc = 0;
  CHECK(monitor.Start(&error) == PU_OK);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_ACTIVE);
  CHECK(info.running == 1);

  // 作用域结束：析构必须停掉仍在跑的输入。
  const int stops_before = port.stop_calls;
  (void)stops_before;
}

void TestDestructorStopsActiveInput() {
  FakeMonitorPort port;
  {
    partyup::AudioMonitor monitor;
    monitor.Bind(&port);
    std::string error;
    CHECK(monitor.Start(&error) == PU_OK);
    CHECK(port.started);
  }
  CHECK(port.stop_calls == 1);
  CHECK(!port.started);
}

void TestStopFailureReportsRunningAndRetryClears() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;
  CHECK(monitor.Start(&error) == PU_OK);

  // Stop 调用失败、ADM 仍报告在采集：必须 Faulted + running=1，不能假报已停。
  port.stop_rc = -1;
  CHECK(monitor.Stop(&error) == PU_ERR_MEDIA);
  CHECK(!error.empty());
  CHECK(error.find("fake: input stop failed") != std::string::npos);
  CHECK(port.started);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == 1);
  CHECK(info.valid == 0);
  CHECK(info.frames == 0);

  // 显式重试成功才清故障。
  port.stop_rc = 0;
  CHECK(monitor.Stop(&error) == PU_OK);
  CHECK(!port.started);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(info.running == 0);

  // Stop 返回失败但读回确认已经停止：仍报 Faulted（停命令失败是事实），
  // running 如实为 0，不能把失败吞成 PU_OK。
  CHECK(monitor.Start(&error) == PU_OK);
  port.stop_rc = -1;
  port.running_override = 0;
  CHECK(monitor.Stop(&error) == PU_ERR_MEDIA);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == 0);
  port.stop_rc = 0;
  port.running_override = -1;
  CHECK(monitor.Stop(&error) == PU_OK);

  // Stop 成功但读回无法确认（-1）：不得宣称已停。
  CHECK(monitor.Start(&error) == PU_OK);
  port.running_override = -2;
  port.started = true;
  CHECK(monitor.Stop(&error) == PU_ERR_MEDIA);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == -1);
  port.running_override = -1;
  port.started = false;
  CHECK(monitor.Stop(&error) == PU_OK);
}

void TestVoiceTakeoverNeverStopsVoiceRecording() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;

  // 空闲时 voice 接管：Stop 幂等成功且绝不触碰端口。
  monitor.SetVoiceOwnsCapture(true);
  CHECK(monitor.Stop(&error) == PU_OK);
  CHECK(port.stop_calls == 0);
  CHECK(monitor.Start(&error) == PU_ERR_STATE);
  CHECK(port.start_calls == 0);

  // monitor 自己在跑时被 voice 接管（正常顺序由引擎保证不会发生）：
  // 拒绝停掉别人的录音，并如实进入 Faulted + running=1。
  monitor.SetVoiceOwnsCapture(false);
  CHECK(monitor.Start(&error) == PU_OK);
  monitor.SetVoiceOwnsCapture(true);
  CHECK(monitor.Stop(&error) == PU_ERR_STATE);
  CHECK(port.stop_calls == 0);
  CHECK(port.started);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == 1);

  monitor.SetVoiceOwnsCapture(false);
  CHECK(monitor.Stop(&error) == PU_OK);
  CHECK(port.stop_calls == 1);
  CHECK(!port.started);
}

void TestReconcileStopsReportingButNeverRestartsCapture() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;
  CHECK(monitor.Start(&error) == PU_OK);
  const int starts_before = port.start_calls;

  // 设备事务把输入停掉（例如 pu_audio_status 的 Refresh 切换失败）。
  port.started = false;
  monitor.ReconcileInputRunning(false);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.running == 0);
  CHECK(info.valid == 0);
  CHECK(info.frames == 0);
  CHECK(port.start_calls == starts_before);  // 绝不自动重新开采集

  // 旧代次 PCM 被丢弃，故障轮询不会把电平“复活”。
  InjectPcm(&monitor, nullptr, 0);
  monitor.ReconcileInputRunning(false);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
  CHECK(info.frames == 0);

  // Idle 时忽略外部 running，不能把 voice 的采集算成 monitor 的。
  FakeMonitorPort idle_port;
  partyup::AudioMonitor idle_monitor;
  idle_monitor.Bind(&idle_port);
  idle_monitor.ReconcileInputRunning(true);
  pu_audio_monitor_info idle_info = NewInfo();
  idle_monitor.Status(&idle_info);
  CHECK(idle_info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(idle_info.running == 0);
  CHECK(idle_port.start_calls == 0);
}

void TestUnboundMonitorRefusesStart() {
  partyup::AudioMonitor monitor;
  std::string error;
  CHECK(monitor.Start(&error) == PU_ERR_STATE);
  CHECK(!error.empty());
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(info.running == 0);
}

void TestConcurrentPcmDuringStopIsDiscarded() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;
  CHECK(monitor.Start(&error) == PU_OK);

  std::atomic<bool> quit{false};
  int16_t samples[480];
  for (size_t i = 0; i < 480; ++i) samples[i] = (i % 2 == 0) ? 4000 : -4000;
  std::thread injector([&] {
    while (!quit.load(std::memory_order_relaxed)) {
      monitor.OnCapturePcm(monitor.CaptureEpoch(), samples, 480, 2, 1, 48000);
      std::this_thread::yield();
    }
  });
  // 让注入线程真的跑起来，然后在并发中 Stop。
  for (int i = 0; i < 1000; ++i) std::this_thread::yield();
  CHECK(monitor.Stop(&error) == PU_OK);
  quit.store(true, std::memory_order_relaxed);
  injector.join();

  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_IDLE);
  CHECK(info.running == 0);
  CHECK(info.valid == 0);
  CHECK(info.frames == 0);
  CHECK(info.peak == 0);
  CHECK(info.rms == 0);
}

void TestDeviceRestartClearsLevelsAndOldEpoch() {
  FakeMonitorPort port;
  partyup::AudioMonitor monitor;
  monitor.Bind(&port);
  std::string error;
  CHECK(monitor.Start(&error) == PU_OK);
  int16_t samples[480];
  for (size_t i = 0; i < 480; ++i) samples[i] = 3000;
  InjectPcm(&monitor, samples, 480);
  pu_audio_monitor_info info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 1);
  CHECK(info.valid == 1);

  // 设备事务成功重启输入：仍是 Active，但旧电平/帧数清零、代次推进。
  const uint64_t old_epoch = monitor.CaptureEpoch();
  monitor.OnInputRestarted();
  CHECK(monitor.CaptureEpoch() != old_epoch);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.state == PU_AUDIO_MONITOR_ACTIVE);
  CHECK(info.running == 1);
  CHECK(info.valid == 0);
  CHECK(info.frames == 0);
  CHECK(info.peak == 0);
  CHECK(info.rms == 0);

  // 旧代次的迟到 PCM 不再计入；新代次重新统计。
  monitor.OnCapturePcm(old_epoch, samples, 480, 2, 1, 48000);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 0);
  InjectPcm(&monitor, samples, 480);
  info = NewInfo();
  monitor.Status(&info);
  CHECK(info.frames == 1);
  CHECK(info.valid == 1);
  CHECK(info.peak == 3000);
}

void TestFaultedPortDetachesBeforeOwnerTeardown() {
  FakeMonitorPort port;
  int stops_at_detach = 0;
  {
    partyup::AudioMonitor monitor;
    monitor.Bind(&port);
    std::string error;
    CHECK(monitor.Start(&error) == PU_OK);
    port.stop_rc = -1;
    CHECK(monitor.Stop(&error) == PU_ERR_MEDIA);
    stops_at_detach = port.stop_calls;
    // Owner has terminated the ADM. Detach must also work in Faulted state;
    // deleting the monitor must not issue another call into that dead port.
    const auto old_epoch = monitor.CaptureEpoch();
    monitor.Bind(nullptr);
    int16_t samples[2] = {1000, -1000};
    monitor.OnCapturePcm(old_epoch, samples, 2, 2, 1, 48000);
    auto info = NewInfo();
    monitor.Status(&info);
    CHECK(info.state == PU_AUDIO_MONITOR_FAULTED);
    CHECK(info.running == 1);  // Detach is not a false claim that recording stopped.
    CHECK(info.valid == 0);
    CHECK(info.frames == 0);
    CHECK(monitor.Start(&error) == PU_ERR_STATE);
  }
  CHECK(port.stop_calls == stops_at_detach);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("== PartyUp AUD02 audio monitor fake-port regression (no real audio I/O)\n");

  TestVoiceInUseRefusesStartWithoutPortAction();
  TestStartPcmAndStop();
  TestStartFailureIsFaultedAndRetryClears();
  TestDestructorStopsActiveInput();
  TestStopFailureReportsRunningAndRetryClears();
  TestVoiceTakeoverNeverStopsVoiceRecording();
  TestReconcileStopsReportingButNeverRestartsCapture();
  TestUnboundMonitorRefusesStart();
  TestConcurrentPcmDuringStopIsDiscarded();
  TestDeviceRestartClearsLevelsAndOldEpoch();
  TestFaultedPortDetachesBeforeOwnerTeardown();

  std::printf("== checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures != 0) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: PASS (fake port only; NOT a real WASAPI capture result)\n");
  return 0;
}
