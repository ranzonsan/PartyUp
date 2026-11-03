// 语音 spike 第 0 步：确认本机到底能不能拿到「真实 WASAPI ADM」。
//
// 背景：媒体核心里注入的是 NullAudioDeviceModule（当初为了修 teardown 崩溃）。
// 接语音要把它换回真 ADM，所以先要回答两个问题：
//   1) 我们这份静态 webrtc.lib 里，哪个工厂能产出真 ADM（m152 有两条路：
//      api/audio/create_audio_device_module.h 的 CreateAudioDeviceModule，
//      以及 modules/audio_device/include/audio_device_factory.h 的
//      CreateWindowsCoreAudioAudioDeviceModule）；
//   2) 本机有没有麦克风 / 扬声器，Init/StartRecording/StartPlayout 返回什么。
//
// 用 /DTRY_WIN_FACTORY 打开第二条路（若该符号不在库里，会在链接期直接失败，
// 那本身就是结论）。
#include <cstdio>
#include <cstring>

#include <windows.h>

#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "modules/audio_device/include/audio_device.h"
#ifdef TRY_WIN_FACTORY
#include "modules/audio_device/include/audio_device_factory.h"
#endif
#include "rtc_base/win/scoped_com_initializer.h"

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("partyup voice · ADM probe\n");

  webrtc::ScopedCOMInitializer com(webrtc::ScopedCOMInitializer::kMTA);
  std::printf("  COM (MTA)        : %s\n", com.Succeeded() ? "ok" : "FAILED");

  webrtc::Environment env = webrtc::CreateEnvironment();

  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      webrtc::CreateAudioDeviceModule(env,
                                      webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  std::printf("  generic factory  : %s\n", adm ? "non-null" : "NULL");
#ifdef TRY_WIN_FACTORY
  if (!adm) {
    adm = webrtc::CreateWindowsCoreAudioAudioDeviceModule(env);
    std::printf("  windows factory  : %s\n", adm ? "non-null" : "NULL");
  }
#endif
  if (!adm) {
    std::printf("RESULT: 没有可用的真 ADM 工厂\n");
    return 2;
  }

  const int init_rc = adm->Init();
  std::printf("  Init()           : %d\n", init_rc);
  if (init_rc != 0) {
    std::printf("RESULT: ADM 存在但 Init 失败（WASAPI 不可用？）\n");
    return 3;
  }

  const int16_t recording_devices = adm->RecordingDevices();
  const int16_t playout_devices = adm->PlayoutDevices();
  std::printf("  recording devices: %d\n", recording_devices);
  std::printf("  playout devices  : %d\n", playout_devices);

  char name[webrtc::kAdmMaxDeviceNameSize] = {};
  char guid[webrtc::kAdmMaxGuidSize] = {};
  for (int16_t i = 0; i < recording_devices && i < 4; ++i) {
    std::memset(name, 0, sizeof(name));
    std::memset(guid, 0, sizeof(guid));
    adm->RecordingDeviceName(i, name, guid);
    std::printf("    [rec %d] %s\n", i, name);
  }
  for (int16_t i = 0; i < playout_devices && i < 4; ++i) {
    std::memset(name, 0, sizeof(name));
    std::memset(guid, 0, sizeof(guid));
    adm->PlayoutDeviceName(i, name, guid);
    std::printf("    [play %d] %s\n", i, name);
  }

  // 真采一下：StartRecording 成功即证明 WASAPI 采集起得来（不需要 AudioDeviceBuffer）。
  const int rec_rc = adm->InitRecording();
  const int rec_start = rec_rc == 0 ? adm->StartRecording() : -999;
  std::printf("  InitRecording()  : %d\n", rec_rc);
  std::printf("  StartRecording() : %d\n", rec_start);
  Sleep(300);
  std::printf("  Recording()      : %d\n", adm->Recording() ? 1 : 0);
  if (rec_start == 0) adm->StopRecording();

  const int play_init = adm->InitPlayout();
  const int play_start = play_init == 0 ? adm->StartPlayout() : -999;
  std::printf("  InitPlayout()    : %d\n", play_init);
  std::printf("  StartPlayout()   : %d\n", play_start);
  Sleep(200);
  std::printf("  Playing()        : %d\n", adm->Playing() ? 1 : 0);
  if (play_start == 0) adm->StopPlayout();

  adm->Terminate();

  const bool usable = init_rc == 0 && rec_start == 0;
  std::printf("RESULT: %s\n", usable ? "真 ADM 可用（可采可播）" : "真 ADM 不完整");
  return usable ? 0 : 4;
}
