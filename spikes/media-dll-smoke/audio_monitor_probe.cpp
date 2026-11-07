// PartyUp AUD02 · 官方 ADM observer 的 fake-only 预检（先证明 SDK 行为，不接生产）。
//
// 目的：回答一个窄问题——把官方 audio_device_data_observer.h 的
//   CreateAudioDeviceWithDataObserver(scoped_refptr<AudioDeviceModule> impl,
//                                     unique_ptr<AudioDeviceDataObserver> observer)
// 包在一个 fixture ADM 外面后，ADM 实际收到的 AudioTransport 回调是否：
//   1) 先/后通知 observer（OnCaptureData，参数与样本原样）；
//   2) 仍转发给用户注册的原 AudioTransport（参数、样本、newMicLevel、返回值）；
//   3) 不产生 OnRenderData / NeedMorePlayData / PullRenderData 等其它路径。
//
// 本文件**只做 fake-only 预检**：
//   * 不调用 CreateAudioDeviceModule(kPlatformDefaultAudio)，也不包含
//     create_audio_device_module.h / audio_device_factory.h；
//   * 不初始化 COM、不枚举设备、不打开真实麦克风/扬声器、不采屏、不建 PC、
//     不联网、不加载 partyup_media.dll；
//   * 传给 observer wrapper 的唯一 impl 是 fixture FakeAdm（见下），因此所有
//     ADM 调用都会落在本文件的计数上；若 SDK 偷偷建了平台 ADM，这些计数不会
//     增加，下面的断言会 red；
//   * 不实现生产 monitor，不改 ABI / caps / client / UI / ViewModels。
//
// 签名来源（逐字对照）：
//   third_party/libwebrtc/include/modules/audio_device/include/audio_device_data_observer.h:25-47
//   third_party/libwebrtc/include/api/audio/audio_device_defines.h:35-91  (AudioTransport)
//   third_party/libwebrtc/include/modules/audio_device/include/audio_device_default.h:25-142
//   third_party/libwebrtc/include/api/audio/audio_device.h:24-179         (AudioDeviceModule)
// 库定义符号（Lead 已用成熟 nm 在 release/webrtc.lib 确认，但未 link/回调验证）：
//   CreateAudioDeviceWithDataObserver —— 本 probe 直接链接同一 webrtc.lib，
//   链接/回调行为在 Lead 执行时才会第一次被真正验证。
//
// 注意：本 probe 通过 *不等于* 真实 WASAPI 无 PC 采集通过；它只证明 fake 回调
// 路径与 SDK linkage。真实设备行为仍待单独验证，不据此新增任何 caps。
//
// 不猜测官方语义：newMicLevel / 返回值只与下游 transport 实际写入/返回的值
// 比较（转发链是否吞掉或改写），不预设某个固定值；若观察到不一致，打印实际
// 值并以非零退出，交 Lead 判断。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/ref_count.h"
#include "api/scoped_refptr.h"
#include "modules/audio_device/include/audio_device_data_observer.h"
#include "modules/audio_device/include/audio_device_default.h"

namespace {

// 48 kHz / 16-bit / mono / 10 ms。mono 可避免 “num_samples 是帧数还是标量数”
// 的歧义，也让样本字节数只依赖一个维度。
constexpr size_t kFramesPerInjection = 480;
constexpr size_t kBytesPerSample = 2;
constexpr size_t kChannels = 1;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kTotalDelayMs = 20;
constexpr uint32_t kCurrentMicLevel = 31;
constexpr uint32_t kDownstreamMicLevel = kCurrentMicLevel + 3;
constexpr int32_t kDownstreamReturnCode = 0x1234;  // 转发标记，不是真实错误码
constexpr size_t kMaxPcmBytes = 4096;

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool ok, const char* expression, const char* test, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  std::printf("FAIL %s:%d [%s] %s\n", __FILE__, line, test, expression);
}

#define CHECK(cond) CheckImpl((cond), #cond, __func__, __LINE__)

// 生命周期账本：证明 wrapper 重建/销毁后 fixture 资源回到 0。
int g_fake_created = 0;
int g_fake_destroyed = 0;
int g_fake_addrefs = 0;
int g_fake_releases = 0;
int g_observer_created = 0;
int g_observer_destroyed = 0;

size_t CopyBounded(unsigned char* destination,
                   const void* source,
                   size_t num_samples,
                   size_t bytes_per_sample,
                   size_t channels) {
  if (source == nullptr) return 0;
  size_t bytes = num_samples * bytes_per_sample * channels;
  if (bytes > kMaxPcmBytes) bytes = kMaxPcmBytes;
  std::memcpy(destination, source, bytes);
  return bytes;
}

void FillSyntheticPcm(int16_t* samples, size_t frames, int cycle) {
  for (size_t i = 0; i < frames; ++i) {
    int value = 0;
    if (cycle == 1) {
      value = (static_cast<int>(i) * 37 + 1234) % 30000 - 15000;
    } else {
      value = (i % 2 == 0) ? 12000 : -12000;
    }
    samples[i] = static_cast<int16_t>(value);
  }
}

// 记录 observer 实际收到的内容；不解析 PCM，不模拟 webrtc 状态。
class FakeObserver final : public webrtc::AudioDeviceDataObserver {
 public:
  FakeObserver() { ++g_observer_created; }
  ~FakeObserver() override { ++g_observer_destroyed; }

  void OnCaptureData(const void* audio_samples,
                     size_t num_samples,
                     size_t bytes_per_sample,
                     size_t num_channels,
                     uint32_t samples_per_sec) override {
    ++capture_calls;
    capture_num_samples = num_samples;
    capture_bytes_per_sample = bytes_per_sample;
    capture_num_channels = num_channels;
    capture_samples_per_sec = samples_per_sec;
    capture_data_size = CopyBounded(capture_data, audio_samples, num_samples,
                                    bytes_per_sample, num_channels);
  }

  void OnRenderData(const void* audio_samples,
                    size_t num_samples,
                    size_t bytes_per_sample,
                    size_t num_channels,
                    uint32_t samples_per_sec) override {
    ++render_calls;
    render_num_samples = num_samples;
    render_bytes_per_sample = bytes_per_sample;
    render_num_channels = num_channels;
    render_samples_per_sec = samples_per_sec;
  }

  int capture_calls = 0;
  int render_calls = 0;
  size_t capture_num_samples = 0;
  size_t capture_bytes_per_sample = 0;
  size_t capture_num_channels = 0;
  uint32_t capture_samples_per_sec = 0;
  size_t capture_data_size = 0;
  unsigned char capture_data[kMaxPcmBytes] = {};

  size_t render_num_samples = 0;
  size_t render_bytes_per_sample = 0;
  size_t render_num_channels = 0;
  uint32_t render_samples_per_sec = 0;
};

// 下游 transport：只记录并写回可区分的 newMicLevel / 返回值，不解析 PCM。
class FakeAudioTransport final : public webrtc::AudioTransport {
 public:
  int32_t RecordedDataIsAvailable(const void* audioSamples,
                                  size_t nSamples,
                                  size_t nBytesPerSample,
                                  size_t nChannels,
                                  uint32_t samplesPerSec,
                                  uint32_t totalDelayMS,
                                  int32_t clockDrift,
                                  uint32_t currentMicLevel,
                                  bool keyPressed,
                                  uint32_t& newMicLevel) override {
    ++recorded_calls;
    last_n_samples = nSamples;
    last_bytes_per_sample = nBytesPerSample;
    last_num_channels = nChannels;
    last_samples_per_sec = samplesPerSec;
    last_total_delay_ms = totalDelayMS;
    last_clock_drift = clockDrift;
    last_current_mic_level = currentMicLevel;
    last_key_pressed = keyPressed;
    last_data_size = CopyBounded(last_data, audioSamples, nSamples,
                                 nBytesPerSample, nChannels);
    last_written_mic_level = currentMicLevel + (kDownstreamMicLevel - kCurrentMicLevel);
    newMicLevel = last_written_mic_level;
    return kDownstreamReturnCode;
  }

  int32_t NeedMorePlayData(size_t nSamples,
                           size_t nBytesPerSample,
                           size_t nChannels,
                           uint32_t samplesPerSec,
                           void* audioSamples,
                           size_t& nSamplesOut,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    ++need_more_play_calls;
    if (audioSamples != nullptr) {
      std::memset(audioSamples, 0, nSamples * nBytesPerSample * nChannels);
    }
    nSamplesOut = nSamples;
    if (elapsed_time_ms != nullptr) *elapsed_time_ms = 0;
    if (ntp_time_ms != nullptr) *ntp_time_ms = 0;
    return 0;
  }

  void PullRenderData(int bits_per_sample,
                      int sample_rate,
                      size_t number_of_channels,
                      size_t number_of_frames,
                      void* audio_data,
                      int64_t* elapsed_time_ms,
                      int64_t* ntp_time_ms) override {
    ++pull_render_calls;
    if (elapsed_time_ms != nullptr) *elapsed_time_ms = 0;
    if (ntp_time_ms != nullptr) *ntp_time_ms = 0;
  }

  int recorded_calls = 0;
  int need_more_play_calls = 0;
  int pull_render_calls = 0;
  size_t last_n_samples = 0;
  size_t last_bytes_per_sample = 0;
  size_t last_num_channels = 0;
  uint32_t last_samples_per_sec = 0;
  uint32_t last_total_delay_ms = 0;
  int32_t last_clock_drift = 0;
  uint32_t last_current_mic_level = 0;
  bool last_key_pressed = false;
  size_t last_data_size = 0;
  unsigned char last_data[kMaxPcmBytes] = {};
  uint32_t last_written_mic_level = 0;
};

// 最小 fixture ADM：继承官方 AudioDeviceModuleDefault，只为必要签名提供可观测
// 实现。它不持有任何平台设备，也不调用任何 Windows 音频 API。
class FakeAdm final
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
 public:
  FakeAdm() { ++g_fake_created; }
  ~FakeAdm() override { ++g_fake_destroyed; }

  void AddRef() const override {
    ++g_fake_addrefs;
    ++ref_count_;
  }

  webrtc::RefCountReleaseStatus Release() const override {
    ++g_fake_releases;
    if (ref_count_ <= 0) {
      std::printf("FATAL: FakeAdm::Release() underflow (ref_count=%d)\n", ref_count_);
      std::abort();
    }
    --ref_count_;
    if (ref_count_ == 0) {
      delete this;
      return webrtc::RefCountReleaseStatus::kDroppedLastRef;
    }
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

  int32_t RegisterAudioCallback(webrtc::AudioTransport* audio_callback) override {
    ++register_calls;
    registered_audio_transport = audio_callback;
    return 0;
  }

  int32_t InitRecording() override {
    ++init_recording_calls;
    recording_initialized = true;
    return 0;
  }

  int32_t StartRecording() override {
    ++start_recording_calls;
    if (!recording_initialized) return -1;
    recording = true;
    return 0;
  }

  int32_t StopRecording() override {
    ++stop_recording_calls;
    recording = false;
    return 0;
  }

  bool Recording() const override { return recording; }

  webrtc::AudioTransport* registered_audio_transport = nullptr;
  int register_calls = 0;
  int init_recording_calls = 0;
  int start_recording_calls = 0;
  int stop_recording_calls = 0;

 private:
  mutable int ref_count_ = 0;
  bool recording_initialized = false;
  bool recording = false;
};

struct InjectionObservation {
  int32_t return_code = 0;
  uint32_t new_mic_level = 0;
};

InjectionObservation InjectPcm(FakeAdm* fake,
                               const int16_t* samples,
                               size_t frames) {
  InjectionObservation result;
  uint32_t new_mic_level = kCurrentMicLevel;
  webrtc::AudioTransport* callback = fake->registered_audio_transport;
  if (callback == nullptr) {
    std::printf("FAIL: fake ADM has no registered AudioTransport "
                "(wrapper did not register a proxy)\n");
    ++g_checks;
    ++g_failures;
    result.return_code = -999;
    result.new_mic_level = new_mic_level;
    return result;
  }
  result.return_code = callback->RecordedDataIsAvailable(
      samples, frames, kBytesPerSample, kChannels, kSampleRate, kTotalDelayMs,
      0, kCurrentMicLevel, false, new_mic_level);
  result.new_mic_level = new_mic_level;
  return result;
}

InjectionObservation InjectPcmWithTimestamp(FakeAdm* fake,
                                            const int16_t* samples,
                                            size_t frames,
                                            int64_t estimated_capture_time_ns) {
  InjectionObservation result;
  uint32_t new_mic_level = kCurrentMicLevel;
  webrtc::AudioTransport* callback = fake->registered_audio_transport;
  if (callback == nullptr) {
    std::printf("FAIL: fake ADM has no registered AudioTransport "
                "(wrapper did not register a proxy)\n");
    ++g_checks;
    ++g_failures;
    result.return_code = -999;
    result.new_mic_level = new_mic_level;
    return result;
  }
  result.return_code = callback->RecordedDataIsAvailable(
      samples, frames, kBytesPerSample, kChannels, kSampleRate, kTotalDelayMs,
      0, kCurrentMicLevel, false, new_mic_level,
      std::optional<int64_t>(estimated_capture_time_ns));
  result.new_mic_level = new_mic_level;
  return result;
}

void VerifyInjection(const FakeAudioTransport& downstream,
                     const FakeObserver& observer,
                     const int16_t* samples,
                     size_t frames,
                     int expected_calls,
                     const InjectionObservation& observed,
                     const char* label) {
  const size_t expected_bytes = frames * kBytesPerSample * kChannels;

  // observer：被调用、次数、参数、样本内容都与注入一致。
  CHECK(observer.capture_calls == expected_calls);
  CHECK(observer.capture_num_samples == frames);
  CHECK(observer.capture_bytes_per_sample == kBytesPerSample);
  CHECK(observer.capture_num_channels == kChannels);
  CHECK(observer.capture_samples_per_sec == kSampleRate);
  CHECK(observer.capture_data_size == expected_bytes);
  CHECK(std::memcmp(observer.capture_data, samples, expected_bytes) == 0);

  // 下游：仍被转发，且参数/样本原样。
  CHECK(downstream.recorded_calls == expected_calls);
  CHECK(downstream.last_n_samples == frames);
  CHECK(downstream.last_bytes_per_sample == kBytesPerSample);
  CHECK(downstream.last_num_channels == kChannels);
  CHECK(downstream.last_samples_per_sec == kSampleRate);
  CHECK(downstream.last_total_delay_ms == kTotalDelayMs);
  CHECK(downstream.last_clock_drift == 0);
  CHECK(downstream.last_current_mic_level == kCurrentMicLevel);
  CHECK(!downstream.last_key_pressed);
  CHECK(downstream.last_data_size == expected_bytes);
  CHECK(std::memcmp(downstream.last_data, samples, expected_bytes) == 0);

  // 转发语义：只和下游实际写入/返回的值比较，不猜固定值；不一致即 red。
  CHECK(observed.return_code == kDownstreamReturnCode);
  CHECK(observed.new_mic_level == downstream.last_written_mic_level);
  CHECK(observed.new_mic_level == kDownstreamMicLevel);

  // 本 probe 只走 capture；wrapper 不得产生 render / 其它录音路径。
  CHECK(observer.render_calls == 0);
  CHECK(downstream.need_more_play_calls == 0);
  CHECK(downstream.pull_render_calls == 0);

  std::printf(
      "  [%s] observer.calls=%d downstream.calls=%d rc=0x%X "
      "mic=%u(downstream=%u) render=%d\n",
      label, observer.capture_calls, downstream.recorded_calls,
      static_cast<unsigned>(observed.return_code), observed.new_mic_level,
      downstream.last_written_mic_level, observer.render_calls);
}

void RunWrapperCycle(int cycle) {
  std::printf("-- cycle %d: fixture FakeAdm + official observer wrapper\n", cycle);

  // fixture 先于 wrapper 构造，wrapper 释放前显式 Stop；downstream 在栈上，
  // 生命周期覆盖整个 cycle。
  FakeAudioTransport downstream;
  auto observer = std::make_unique<FakeObserver>();
  FakeObserver* observer_raw = observer.get();

  FakeAdm* fake_raw = new FakeAdm();
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> fake(fake_raw);
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> wrapped =
      webrtc::CreateAudioDeviceWithDataObserver(std::move(fake),
                                                std::move(observer));

  CHECK(wrapped.get() != nullptr);
  if (!wrapped) {
    std::printf("FAIL: CreateAudioDeviceWithDataObserver returned null\n");
    return;
  }

  // 先完成 ADM 初始化，再接收 transport；不能跳过生产工厂所执行的生命周期。
  CHECK(wrapped->Init() == 0);

  // 用户注册自己的 transport；wrapper 必须把「自己」注册给 fake，而不是把
  // 下游直接注册给 fake，否则 ADM 的 PCM 会绕过 observer。
  const int32_t register_rc = wrapped->RegisterAudioCallback(&downstream);
  CHECK(register_rc == 0);
  CHECK(fake_raw->register_calls == 1);
  CHECK(fake_raw->registered_audio_transport != nullptr);
  CHECK(fake_raw->registered_audio_transport != &downstream);

  // Start/Stop/Status seam 的预检：forwarding 与状态可观测。
  CHECK(!wrapped->Recording());
  CHECK(wrapped->InitRecording() == 0);
  CHECK(fake_raw->init_recording_calls == 1);
  CHECK(wrapped->StartRecording() == 0);
  CHECK(fake_raw->start_recording_calls == 1);
  CHECK(wrapped->Recording());

  int16_t synthetic[kFramesPerInjection];
  FillSyntheticPcm(synthetic, kFramesPerInjection, cycle);
  const InjectionObservation first =
      InjectPcm(fake_raw, synthetic, kFramesPerInjection);
  VerifyInjection(downstream, *observer_raw, synthetic, kFramesPerInjection, 1,
                  first, "synthetic-16bit");

  int16_t zeros[kFramesPerInjection] = {};
  const InjectionObservation second =
      InjectPcm(fake_raw, zeros, kFramesPerInjection);
  VerifyInjection(downstream, *observer_raw, zeros, kFramesPerInjection, 2,
                  second, "all-zero");

  int16_t alternating[kFramesPerInjection];
  FillSyntheticPcm(alternating, kFramesPerInjection, 2);
  const InjectionObservation third = InjectPcmWithTimestamp(
      fake_raw, alternating, kFramesPerInjection, /*estimated_capture_time_ns=*/123456789);
  VerifyInjection(downstream, *observer_raw, alternating, kFramesPerInjection, 3,
                  third, "synthetic+timestamp");

  // 销毁前 Stop：之后不得再有任何 capture 回调。
  const int capture_calls_before_stop = observer_raw->capture_calls;
  CHECK(wrapped->StopRecording() == 0);
  CHECK(fake_raw->stop_recording_calls == 1);
  CHECK(!wrapped->Recording());
  CHECK(observer_raw->capture_calls == capture_calls_before_stop);

  // 重建/销毁：wrapper 释放时 observer 与 fake 都必须真正销毁，引用计数归零。
  wrapped = nullptr;
  std::printf("-- cycle %d destroyed: fake.live=%d observer.live=%d "
              "fake.addref=%d fake.release=%d\n",
              cycle, g_fake_created - g_fake_destroyed,
              g_observer_created - g_observer_destroyed, g_fake_addrefs,
              g_fake_releases);
  CHECK(g_fake_created - g_fake_destroyed == 0);
  CHECK(g_observer_created - g_observer_destroyed == 0);
  CHECK(g_fake_addrefs == g_fake_releases);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf(
      "== PartyUp AUD02 fake-only official observer probe "
      "(no platform ADM, no real audio I/O)\n");
  std::printf(
      "-- impl is fixture FakeAdm only; this TU contains no "
      "CreateAudioDeviceModule/factory call\n");

  RunWrapperCycle(1);
  RunWrapperCycle(2);

  // 两轮重建后，fixture 资源必须全部有界回收。
  CHECK(g_fake_created == 2);
  CHECK(g_fake_destroyed == 2);
  CHECK(g_observer_created == 2);
  CHECK(g_observer_destroyed == 2);
  CHECK(g_fake_addrefs == g_fake_releases);

  std::printf("== checks=%d failures=%d\n", g_checks, g_failures);
  if (g_failures != 0) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf(
      "RESULT: PASS (fake callback/SDK linkage only; NOT real WASAPI/PC-free "
      "capture evidence)\n");
  return 0;
}
