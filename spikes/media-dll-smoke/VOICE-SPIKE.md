# 语音 spike（A：1 对 1 直连）

验证「真 WASAPI ADM + libwebrtc 内置 Opus 编解码 + 双 PeerConnection」这条链路。

- 程序：`spikes/media-dll-smoke/voice-spike.cpp`
- 构建/运行：`pwsh -File spikes/media-dll-smoke/build-voice-spike.ps1`
- ADM 能力探针：`voice-adm-probe.cpp` + `build-voice-adm-probe.ps1`

## 三个必须记住的坑（重建环境最容易再踩）

1. **真 ADM 必须额外链接 `msdmo.lib`**
   `AudioDeviceWindowsCore::InitRecordingDMO` 引用 `MoInitMediaType` / `MoFreeMediaType`，
   只连 `mfplat.lib` 会 `LNK2019`。虽然函数名像 Media Foundation，实际导出在 `msdmo.lib`。

2. **必须定义 `/DWEBRTC_WIN`**
   否则 `rtc_base/byte_order.h` 直接 `#error "Missing byte order functions for this arch."`
   （它的 `#if` 分支只在 `WEBRTC_WIN`/`WEBRTC_POSIX` 下成立）。

3. **用 `webrtc::CreateAudioDeviceModule(env, AudioDeviceModule::kPlatformDefaultAudio)`**
   `modules/audio_device/include/audio_device_factory.h` 里的
   `CreateWindowsCoreAudioAudioDeviceModule` **不在 webrtc.lib 里**（符号缺失，链接期才报）。
   通用工厂能拿到同一个实现（会拉入 `audio_device_core_win.obj`）。

## 构型

- 两个工厂，各注入**自己的**真 ADM（A 采集 / B 播放），`audio_mixer=AudioMixerImpl::Create()`、
  `audio_processing_builder=BuiltinAudioProcessingBuilder`，`EnableMedia(deps)` 打开媒体。
- A：`CreateAudioSource(AudioOptions())` → `CreateAudioTrack("audio0")` → `pc1->AddTrack`。
- B：`OnTrack` 收到远端音频轨后 `AddSink(&AudioCounterSink)`，在 `OnData` 里数 PCM 帧。
- 所有 PC 调用投到 signaling 线程（`signaling->BlockingCall`）。

## 两个 API 陷阱（m152）

- `SessionDescriptionInterface::ToString()` **不再写 SDP**，要用 `api/webrtc_sdp.h` 的 `SdpSerialize`。
- `CreateSessionDescriptionObserver` 在回调后会被 libwebrtc `Release()`，**回调里必须当场拷出 SDP**
  （本 spike 写进 `g_sdp_out`）；回调结束后再读 observer 成员是 UB，会读到 0 字节。

## 实测（2026-11-03，本机 RTX 4060 Laptop / Realtek 声卡）

```
A: audio track added
[sdp] type=0 len=1324        # offer
[pc2/B] remote audio track arrived: audio0
[sdp] type=2 len=1211        # answer
[pc1/A]/[pc2/B] ICE state 2 (connected)
t+8s: B decoded frames=852 (106.5% of expected, 10ms/帧理论 100/s)
```

- **B 端解码 PCM 帧数 = 852 / 8s ≈ 106.5 帧每秒**（10 ms/帧理论 100/s）→ 解码链路通，无空档。
- **端到端延迟 = 不可用**：`absolute_capture_timestamp_ms` 全程 `std::nullopt`
  （该字段依赖 RTCP SR 的绝对时间映射；本构型下 8 秒内始终没有），所以延迟这一项本 spike 没测出来。
- **丢包率 = 未直接测量**（RTCStatsReport 需要 `api/stats/attribute.h` 的 `Attribute` 模板，
  当前静态库头组合编译不过，故未用统计接口）；从「解码帧率 ≥ 理论帧率、无 >200ms 空档」推断没有明显丢包。
