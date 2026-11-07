<#
.SYNOPSIS
  构建 PartyUp 媒体核心 C ABI 动态库（partyup_media.dll + 导入库）并（可选）跑冒烟测试。

.DESCRIPTION
  * 工具链与 scripts/build-media-spike.ps1 完全一致：MSVC + /MT /std:c++20 /utf-8，
    libwebrtc 静态库（含 abseil 第二个 include 根）与同一组系统库。
  * 产物默认放 <repo>/src/media-core/out/：
        partyup_media.dll        导出库（只有 pu_*）
        partyup_media.lib        导入库
        media_smoke.exe          单引擎生命周期冒烟（v1 的 6 个函数 + v2 的 offer 事件）
        media_signaling_smoke.exe 双引擎信令冒烟（offer/answer/DTLS/远端帧）
        teardown_stress.exe      teardown 压测（建/连/拆循环，崩溃时写符号化栈 + dump）
        exports.txt              dumpbin /exports 结果
  * 编译工作目录固定在 Windows 本机磁盘（$StageDir\build），不在 UNC 下跑 cmd/cl；
    只有读写源码与头文件走 UNC。

.PARAMETER Root
  仓库根目录。缺省为本脚本所在目录的上级（脚本经 UNC 调用时即 UNC 路径）。

.PARAMETER OutDir
  产物目录。缺省 <Root>\src\media-core\out。

.PARAMETER StageDir
  本机暂存目录，用于编译中间文件与 -Run 时运行 exe。
  缺省 %LOCALAPPDATA%\PartyUp\media-stage。必须是 Windows 本地盘路径。

.PARAMETER Run
  构建后把 dll + exe 复制到 $StageDir 并从那里运行冒烟测试（不从 UNC 运行）。

.PARAMETER Symbols
  生成 PDB（/Zi /DEBUG /INCREMENTAL:NO）并复制到运行目录，供 teardown 压测的崩溃栈符号化。

.PARAMETER StressIterations
  > 0 时构建并运行 teardown 压测（teardown_stress.exe）这么多轮。每次迭代一个日志
  文件；崩溃时写 crash-*.log（含符号化调用栈）与 crash-*.dmp。跑完把日志复制到
  <OutDir>\\run-logs\\stress-<时间戳>\\。

.PARAMETER StressOrder
  压测的收尾顺序：viewer-first（默认）| media | signaling | abrupt | sharer-first | alternate。

.PARAMETER StressMinFrames
  压测进入收尾前要求观看方解出的帧数（默认 5）。

.PARAMETER RunWatch
  只运行合成源显式观看回归（media_watch_regression.exe）。它不采全桌面、不开麦克风，
  用两个同进程引擎验证 watch-before-track / track-before-watch / 帧归属 / unwatch。
  与 -Run 分开，避免为了验证 watch 而触发 screen_capture_smoke 的真实桌面采集。

.PARAMETER RunAudioSwitch
  只运行音频设备控制回归（audio_switch_regression.exe）：用 fake ADM + 受控默认端点
  驱动生产代码 src/media-core/src/audio_device_control.cpp，不打开真实麦克风/扬声器、
  不采屏。覆盖通话中切输入/输出、-1 默认、设备重排、拔掉/回退、默认端点变化、
  Set/Stop/Init/Start 失败与回滚、静音保持，以及 v2.15 安全启动门禁：输入/输出在
  muted/deafened 时 Start 之前必须 SetMuted+readback；SetMuted/readback 失败、
  设备重开清 mute、Start 重置 mute、恢复旧设备/回退默认都断言无未静音 Start。
  它验证控制逻辑与失败路径，不代替真实 WASAPI 设备验证（真机仍未验）。

.PARAMETER RunDirectIsolation
  只运行 direct 严格接收隔离回归（media_direct_isolation.exe）：两个同进程引擎各
  自发布 synthetic 屏幕并互相观看（4 条独立 PC），验证停看只关闭对应接收/发送
  传输、另一方向继续出帧、采集源未重启、重看拿到新会话身份、旧会话信令被拒绝。
  不采全桌面、不开麦克风、不接服务端（信令由测试按会话 token 直接路由，等价于
  服务端按 session/nonce 转发）。

.PARAMETER RunAudioMonitorProbe
  只运行官方音频设备 observer 的 fake-only 预检（audio_monitor_probe.exe）：直接
  链接同一 webrtc.lib，用 fixture FakeAdm（继承官方 AudioDeviceModuleDefault）+
  Fake AudioTransport 验证 CreateAudioDeviceWithDataObserver 把注入的 16-bit 合成
  PCM / 全零 PCM 原样交给 observer 并转发给原 transport。不调用
  CreateAudioDeviceModule(kPlatformDefaultAudio)，不打开真实麦克风/扬声器、不采屏。
  成功仅表示 fake 回调/SDK linkage，不代表真实 WASAPI 无 PC 采集通过，不新增 caps。

.PARAMETER RunAudioMonitor
  只运行 AUD02 独立输入电平的 fake 端口回归（audio_monitor_regression.exe）：直接
  编译生产 Module src/media-core/src/audio_monitor.cpp + 脚本化 AudioMonitorPort，
  验证语音占用拒绝 Start、真实 PCM 峰值/RMS/帧数/新鲜度、Stop/读回失败如实报
  Faulted+running、不碰 voice 录音、设备事务后不自动重开采集。不链接
  libwebrtc/DLL，不打开真实麦克风/扬声器、不建 PC；不能代替真实 WASAPI 采集验收。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\scripts\build-media-dll.ps1' -Run

.EXAMPLE
  # 带符号跑 200 轮收尾压测（复现/验证 teardown 竞态）
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\scripts\build-media-dll.ps1' `
    -Symbols -StressIterations 200 -StressOrder viewer-first
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage'),
  [switch]$Run,
  [switch]$RunWatch,
  [switch]$RunAudioSwitch,
  [switch]$RunDirectIsolation,
  [switch]$RunAudioMonitorProbe,
  [switch]$RunAudioMonitor,
  [switch]$Symbols,
  [int]$StressIterations = 0,
  [string]$StressOrder = 'viewer-first',
  [int]$StressMinFrames = 5
)

$ErrorActionPreference = 'Stop'

# 本批 fake-only 预检不得与 -Run 组合：-Run 会跑真实桌面捕获（WGC）。
if ($RunAudioMonitorProbe -and $Run) {
  throw '-RunAudioMonitorProbe must not be combined with -Run (real WGC desktop capture)'
}
if ($RunAudioMonitor -and $Run) {
  throw '-RunAudioMonitor must not be combined with -Run (real WGC desktop capture)'
}

if (-not $Root) {
  if ($PSScriptRoot) {
    $Root = Split-Path $PSScriptRoot -Parent
  } else {
    $Root = (Get-Location).Path
  }
}
if (-not $OutDir) { $OutDir = Join-Path $Root 'src\media-core\out' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$include = Join-Path $Root 'third_party\libwebrtc\include'
$libDir = Join-Path $Root 'third_party\libwebrtc\release'
$hdrDir = Join-Path $Root 'src\media-core\include'
# 媒体核心内部头（audio_device_control.h 等）与实现同目录，DLL 与回归测试都要能 include。
$srcDir = Join-Path $Root 'src\media-core\src'
# vendored 的 nlohmann/json 单头文件（信令信封解析用），见 src/media-core/vendor/nlohmann/README.md
$vendorDir = Join-Path $Root 'src\media-core\vendor'
$dllSrc = Join-Path $Root 'src\media-core\src\media_engine.cpp'
# v2.3：真实屏幕采集（Windows Graphics Capture）单独一个编译单元，见文件头注释。
$wgcSrc = Join-Path $Root 'src\media-core\src\wgc_screen.cpp'
# v2.4：硬件 H.264 编解码（Media Foundation MFT）单独一个编译单元，见文件头注释。
$hwSrc = Join-Path $Root 'src\media-core\src\hw_encoder.cpp'
# v2.15：通话中音频设备热切/默认跟随/热插拔回退。控制逻辑与 ADM 端口分开，
# 前者被 fake ADM 回归直接编译运行，后者只在 DLL 里转发到真实 webrtc ADM。
$audioControlSrc = Join-Path $Root 'src\media-core\src\audio_device_control.cpp'
$audioAdmSrc = Join-Path $Root 'src\media-core\src\audio_device_adm_win.cpp'
$smokeSrc = Join-Path $Root 'spikes\media-dll-smoke\main.cpp'
$signalingSrc = Join-Path $Root 'spikes\media-dll-smoke\signaling.cpp'
$stressSrc = Join-Path $Root 'spikes\media-dll-smoke\teardown_stress.cpp'
$captureSrc = Join-Path $Root 'spikes\media-dll-smoke\screen_capture.cpp'
# v2.14：只采 synthetic、不碰真实桌面的显式观看回归（watch-before/after-track、帧归属）。
$watchSrc = Join-Path $Root 'spikes\media-dll-smoke\watch_regression.cpp'
# v2.15：fake ADM 驱动生产控制逻辑；不打开任何真实麦克风/扬声器。
$audioSwitchSrc = Join-Path $Root 'spikes\media-dll-smoke\audio_switch_regression.cpp'
# v2.17（B2b direct）：双引擎、4 条 PC 的严格接收隔离回归（只采 synthetic）。
$directIsoSrc = Join-Path $Root 'spikes\media-dll-smoke\direct_isolation.cpp'
# AUD02：官方 SDK observer 的 fake-only 预检（不创建平台 ADM、不采真实设备）。
$audioMonitorProbeSrc = Join-Path $Root 'spikes\media-dll-smoke\audio_monitor_probe.cpp'

$audioMonitorSrc = Join-Path $Root 'src\media-core\src\audio_monitor.cpp'
$audioMonitorRegressionSrc = Join-Path $Root 'spikes\media-dll-smoke\audio_monitor_regression.cpp'
# 真机输入电平探针：只走生产 DLL 的 C ABI（会真实打开麦克风），用于验证官方 observer
# 在真实硬件上是否收到 PCM，以及 voice/monitor 互斥语义。不是无人值守回归。
$audioMonitorRealSrc = Join-Path $Root 'src\media-core\tools\audio_monitor_real.cpp'
# 真机「再次入会」探针：只用可计数判据（采集 PCM 块数），验证多次 join/leave 后
# 语音采集仍活着。同样会真实打开麦克风，不进无人值守回归。
$audioVoiceRejoinSrc = Join-Path $Root 'src\media-core\tools\audio_voice_rejoin_real.cpp'
foreach ($required in @($vcvars, $include, $libDir, $srcDir, $dllSrc, $wgcSrc, $hwSrc, $audioControlSrc, $audioAdmSrc, $audioMonitorSrc, $smokeSrc, $signalingSrc, $stressSrc, $captureSrc, $watchSrc, $audioSwitchSrc, $directIsoSrc, $audioMonitorProbeSrc, $audioMonitorRegressionSrc, $audioMonitorRealSrc, $audioVoiceRejoinSrc, $vendorDir)) {
  if (-not (Test-Path $required)) { throw "missing required path: $required" }
}
if (-not (Test-Path (Join-Path $include 'api\peer_connection_interface.h'))) {
  throw "libwebrtc headers not found under $include (download third_party/libwebrtc first)"
}
if (-not (Test-Path (Join-Path $vendorDir 'nlohmann\json.hpp'))) {
  throw "vendor/nlohmann/json.hpp not found (it is required by the signaling envelope parser)"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$dllOut = Join-Path $OutDir 'partyup_media.dll'
$dllLib = Join-Path $OutDir 'partyup_media.lib'
$smokeOut = Join-Path $OutDir 'media_smoke.exe'
$signalingOut = Join-Path $OutDir 'media_signaling_smoke.exe'
$stressOut = Join-Path $OutDir 'teardown_stress.exe'
$captureOut = Join-Path $OutDir 'screen_capture_smoke.exe'
$watchOut = Join-Path $OutDir 'media_watch_regression.exe'
$audioSwitchOut = Join-Path $OutDir 'audio_switch_regression.exe'
$directIsoOut = Join-Path $OutDir 'media_direct_isolation.exe'
$audioMonitorProbeOut = Join-Path $OutDir 'audio_monitor_probe.exe'
$audioMonitorRegressionOut = Join-Path $OutDir 'audio_monitor_regression.exe'
$audioMonitorRealOut = Join-Path $OutDir 'audio_monitor_real.exe'
$audioVoiceRejoinOut = Join-Path $OutDir 'audio_voice_rejoin_real.exe'
$exportsTxt = Join-Path $OutDir 'exports.txt'

# 编译中间文件与本机运行目录都放本机盘：cmd.exe 不支持 UNC 作为工作目录，
# 而运行阶段也明确要求不经 UNC。
$workRoot = Join-Path $StageDir 'build'
$objDll = Join-Path $workRoot 'obj-dll'
$objSmoke = Join-Path $workRoot 'obj-smoke'
$objSignaling = Join-Path $workRoot 'obj-signaling'
$objStress = Join-Path $workRoot 'obj-stress'
$objCapture = Join-Path $workRoot 'obj-capture'
$objWatch = Join-Path $workRoot 'obj-watch'
$objAudioSwitch = Join-Path $workRoot 'obj-audio-switch'
$objDirectIso = Join-Path $workRoot 'obj-direct-iso'
$objAudioMonitorProbe = Join-Path $workRoot 'obj-audio-monitor-probe'
$objAudioMonitorRegression = Join-Path $workRoot 'obj-audio-monitor-regression'
$objAudioMonitorReal = Join-Path $workRoot 'obj-audio-monitor-real'
$objAudioVoiceRejoin = Join-Path $workRoot 'obj-audio-voice-rejoin'
New-Item -ItemType Directory -Force -Path $objDll, $objSmoke, $objSignaling, $objStress, $objCapture, $objWatch, $objAudioSwitch, $objDirectIso, $objAudioMonitorProbe, $objAudioMonitorRegression, $objAudioMonitorReal, $objAudioVoiceRejoin | Out-Null

# -Symbols：带调试信息构建，崩溃栈才能符号化到函数/行号（PDB 落在本机 $workRoot，
# 避免大 PDB 写 UNC 时偶发 LNK1201，也方便 dbghelp 就地加载）。
$debugCompile = ''
$debugLinkDll = ''
$debugLinkSmoke = ''
$debugLinkSignaling = ''
$debugLinkStress = ''
$debugLinkCapture = ''
$debugLinkWatch = ''
$debugLinkAudioSwitch = ''
$debugLinkDirectIso = ''
$debugLinkAudioMonitorProbe = ''
$debugLinkAudioMonitorRegression = ''
$debugLinkAudioMonitorReal = ''
$debugLinkAudioVoiceRejoin = ''
if ($Symbols) {
  $debugCompile = ' /Zi'
  $debugLinkDll = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\partyup_media.pdb`""
  $debugLinkSmoke = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_smoke.pdb`""
  $debugLinkSignaling = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_signaling_smoke.pdb`""
  $debugLinkStress = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\teardown_stress.pdb`""
  $debugLinkCapture = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\screen_capture_smoke.pdb`""
  $debugLinkWatch = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_watch_regression.pdb`""
  $debugLinkAudioSwitch = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\audio_switch_regression.pdb`""
  $debugLinkDirectIso = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_direct_isolation.pdb`""
  $debugLinkAudioMonitorProbe = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\audio_monitor_probe.pdb`""
  $debugLinkAudioMonitorRegression = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\audio_monitor_regression.pdb`""
  $debugLinkAudioMonitorReal = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\audio_monitor_real.pdb`""
  $debugLinkAudioVoiceRejoin = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\audio_voice_rejoin_real.pdb`""
}

$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DWEBRTC_WIN', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
  "/I`"$include`"",
  "/I`"$include\third_party\abseil-cpp`"",
  # libyuv 的公开头文件（ARGBToI420 / I420ToARGB）随 libwebrtc 一起 vendored，
  # 其内部 include 形如 "libyuv/basic_types.h"，所以根目录是 ...\libyuv\include。
  "/I`"$include\third_party\libyuv\include`"",
  "/I`"$hdrDir`"",
  # 媒体核心内部头（audio_device_control.h 等）：DLL 与 fake ADM 回归都要 include。
  "/I`"$srcDir`"",
  "/I`"$vendorDir`""
) -join ' '

# 与 spike 完全相同的系统库清单；缺哪个链接器会报 unresolved external，按提示补。
$sysLibs = @(
  'ws2_32.lib', 'secur32.lib', 'crypt32.lib', 'iphlpapi.lib', 'winmm.lib', 'shlwapi.lib',
  'dmoguids.lib', 'wmcodecdspuuid.lib', 'msdmo.lib', 'strmiids.lib', 'advapi32.lib', 'ole32.lib',
  # v2.3 真实屏幕采集新增：WGC 自身（d3d11/dxgi/runtimeobject=RoGetActivationFactory）、
  # 首帧回写 PNG（windowscodecs=WIC）、DPI/显示器枚举（user32/shcore）。
  'd3d11.lib', 'dxgi.lib', 'runtimeobject.lib', 'windowscodecs.lib', 'user32.lib', 'shcore.lib',
  # v2.4 硬件 H.264 编解码新增：Media Foundation 平台库（MFTEnumEx / MFCreateSample /
  # MFCreateDXGIDeviceManager / MFStartup）+ MF 的 GUID（MFVideoFormat_* / MFSampleExtension_*）+ 
  # ICodecAPI 的 CODECAPI_* GUID（strmiids）+ VariantInit/VariantClear（oleaut32）。
  'mfplat.lib', 'mfuuid.lib', 'mf.lib', 'strmiids.lib', 'oleaut32.lib'
) -join ' '

Write-Host "== vcvars : $vcvars"
Write-Host "== root   : $Root"
Write-Host "== out    : $OutDir"
Write-Host "== stage  : $StageDir"

# ---- 1. 动态库 ----
$dllCmd = "cl $common$debugCompile /LD /DPU_BUILD_SHARED `"$dllSrc`" `"$wgcSrc`" `"$hwSrc`" `"$audioControlSrc`" `"$audioAdmSrc`" `"$audioMonitorSrc`" /Fe:`"$dllOut`" " +
          "/link$debugLinkDll /LIBPATH:`"$libDir`" webrtc.lib $sysLibs /IMPLIB:`"$dllLib`""

Push-Location $objDll
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $dllCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building partyup_media.dll (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2. 冒烟测试 exe（链接导入库） ----
$smokeCmd = "cl $common$debugCompile `"$smokeSrc`" /Fe:`"$smokeOut`" /link$debugLinkSmoke `"$dllLib`""

Push-Location $objSmoke
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $smokeCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_smoke.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2b. 双实例信令冒烟 exe（v2：两个 engine 同进程互相喂 SIGNAL_OUT） ----
$signalingCmd = "cl $common$debugCompile `"$signalingSrc`" /Fe:`"$signalingOut`" /link$debugLinkSignaling `"$dllLib`""

Push-Location $objSignaling
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $signalingCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_signaling_smoke.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2c. teardown 压测 exe（收尾竞态专用循环；dbghelp 用于崩溃栈符号化） ----
$stressCmd = "cl $common$debugCompile `"$stressSrc`" /Fe:`"$stressOut`" " +
             "/link$debugLinkStress `"$dllLib`" dbghelp.lib"

Push-Location $objStress
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $stressCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building teardown_stress.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2d. 真实屏幕采集冒烟 exe（抓 10 秒真桌面，报文 fps/分段耗时/码率） ----
$captureCmd = "cl $common$debugCompile `"$captureSrc`" /Fe:`"$captureOut`" /link$debugLinkCapture `"$dllLib`""

Push-Location $objCapture
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $captureCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building screen_capture_smoke.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2e. 显式观看回归 exe（只用 synthetic，不采全桌面、不开麦克风） ----
$watchCmd = "cl $common$debugCompile `"$watchSrc`" /Fe:`"$watchOut`" /link$debugLinkWatch `"$dllLib`""

Push-Location $objWatch
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $watchCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_watch_regression.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2f. 音频设备控制回归 exe（fake ADM；不打开真实麦克风/扬声器，不链 DLL/webrtc） ----
# 直接编译生产控制逻辑 audio_device_control.cpp + 测试 main，fake 只替换端口 I/O。
$audioSwitchCmd = "cl $common$debugCompile `"$audioSwitchSrc`" `"$audioControlSrc`" /Fe:`"$audioSwitchOut`" /link$debugLinkAudioSwitch"

Push-Location $objAudioSwitch
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $audioSwitchCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building audio_switch_regression.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2g. direct 严格接收隔离回归 exe（双引擎、4 条 PC；只用 synthetic） ----
$directIsoCmd = "cl $common$debugCompile `"$directIsoSrc`" /Fe:`"$directIsoOut`" /link$debugLinkDirectIso `"$dllLib`""

Push-Location $objDirectIso
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $directIsoCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_direct_isolation.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2h. AUD02 官方 ADM observer fake-only 预检 exe ----
# 直接链接同一 webrtc.lib；只用 fixture FakeAdm + FakeAudioTransport，不调用
# CreateAudioDeviceModule(kPlatformDefaultAudio)，不访问任何真实录音/播放设备。
$audioMonitorProbeCmd = "cl $common$debugCompile `"$audioMonitorProbeSrc`" /Fe:`"$audioMonitorProbeOut`" " +
                        "/link$debugLinkAudioMonitorProbe /LIBPATH:`"$libDir`" webrtc.lib $sysLibs"

Push-Location $objAudioMonitorProbe
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $audioMonitorProbeCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building audio_monitor_probe.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2i. AUD02 独立输入电平的 fake 端口回归 exe ----
# 直接编译生产 Module audio_monitor.cpp + 测试 main；不链 DLL/webrtc，设备 I/O 全部由
# fake AudioMonitorPort 替换。成功只代表 Module 状态机/统计正确，不代表真实采集通过。
$audioMonitorRegressionCmd = "cl $common$debugCompile `"$audioMonitorRegressionSrc`" `"$audioMonitorSrc`" /Fe:`"$audioMonitorRegressionOut`" /link$debugLinkAudioMonitorRegression"

Push-Location $objAudioMonitorRegression
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $audioMonitorRegressionCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building audio_monitor_regression.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2j. AUD02 真机输入电平探针 exe ----
# 链接生产 DLL 的导入库，走与客户端相同的 pu_audio_monitor_* 路径；会真实打开麦克风，
# 不进无人值守回归，只在需要真机证据时手动运行（把本 exe 与 partyup_media.dll 放到
# 同一个本机盘目录后直接执行；从 UNC 运行会加载错库）。
$audioMonitorRealCmd = "cl $common$debugCompile `"$audioMonitorRealSrc`" /Fe:`"$audioMonitorRealOut`" /link$debugLinkAudioMonitorReal `"$dllLib`""

Push-Location $objAudioMonitorReal
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $audioMonitorRealCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building audio_monitor_real.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2k. 「再次入会」真机探针 exe ----
# 只用可计数判据（采集回调次数/字节数、ADM 状态）验证多次 join/leave 后语音采集仍活着。
# 会真实打开麦克风；不进无人值守回归。
$audioVoiceRejoinCmd = "cl $common$debugCompile `"$audioVoiceRejoinSrc`" /Fe:`"$audioVoiceRejoinOut`" /link$debugLinkAudioVoiceRejoin `"$dllLib`""

Push-Location $objAudioVoiceRejoin
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $audioVoiceRejoinCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building audio_voice_rejoin_real.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 3. 导出符号检查：必须只有 pu_* ----
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
Push-Location $StageDir
try {
  $dumpbinOut = cmd /c "call `"$vcvars`" >nul 2>&1 && dumpbin /nologo /exports `"$dllOut`""
  if ($LASTEXITCODE -ne 0) { throw "dumpbin failed (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}
$dumpbinOut | Set-Content -Encoding utf8 $exportsTxt

$exportNames = @()
foreach ($line in $dumpbinOut) {
  # 末尾可能是 "name = alias"（/DEBUG 构建会出现），只取第一个 token 即可。
  if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') {
    $exportNames += $Matches[1]
  }
}
$badExports = @($exportNames | Where-Object { $_ -notmatch '^pu_' })
if ($exportNames.Count -eq 0) { throw "dumpbin reported no exports — DLL is broken" }
if ($badExports.Count -ne 0) {
  throw "non pu_* exports found: $($badExports -join ', ')"
}
# 导出表断言：v2.18 起是 43 个。v2.16 的 35 个 =
#   30 + pu_screen_receive_open/close + pu_signal_in_session
#      + pu_screen_receive_stats + pu_screen_pause_send；
# v2.17（B2b direct）再加 5 个 per-viewer 发布会话/RTP 计数导出：
#   pu_screen_publish_open/signal_in/close/stats + pu_screen_receive_rtp_stats；
# v2.18（AUD02）再加 3 个独立输入电平导出：
#   pu_audio_monitor_start/stop/status。
# 少一个说明有函数被剥掉，多一个说明有人没走 ABI 评审就加导出。
$expectedExports = 43
if ($exportNames.Count -ne $expectedExports) {
  throw ("export count {0} != expected {1} (v2.18). names: {2}" -f $exportNames.Count, $expectedExports, ($exportNames -join ' '))
}

$dllInfo = Get-Item $dllOut
$libInfo = Get-Item $dllLib
Write-Host ("== partyup_media.dll : {0:N0} bytes ({1:N2} MB)" -f $dllInfo.Length, ($dllInfo.Length / 1MB))
Write-Host ("== partyup_media.lib : {0:N0} bytes ({1:N2} MB)" -f $libInfo.Length, ($libInfo.Length / 1MB))
Write-Host ("== exports ({0})     : {1}" -f $exportNames.Count, ($exportNames -join ' '))

# ---- 4. 可选：在本机盘运行两个冒烟测试 ----
if ($Run) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  Copy-Item -Force $dllOut (Join-Path $StageDir 'partyup_media.dll')
  Copy-Item -Force $smokeOut (Join-Path $StageDir 'media_smoke.exe')
  Copy-Item -Force $signalingOut (Join-Path $StageDir 'media_signaling_smoke.exe')
  Copy-Item -Force $captureOut (Join-Path $StageDir 'screen_capture_smoke.exe')
  Write-Host "== [1/3] smoke (single engine lifecycle) from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_smoke.exe')
    $exitCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($exitCode -ne 0) { throw "media_smoke failed: exit=$exitCode" }
  Write-Host "== media_smoke exit=0"

  Write-Host "== [2/3] signaling smoke (two engines, offer/answer/DTLS/remote frames)"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_signaling_smoke.exe')
    $sigExitCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($sigExitCode -ne 0) { throw "media_signaling_smoke failed: exit=$sigExitCode" }
  Write-Host "== media_signaling_smoke exit=0"

  Write-Host "== [3/3] screen capture smoke (real WGC desktop, 10 s)"
  # pu_frame_acquire 首帧（远端 BGRA）→ WIC PNG，直接写进仓库 out/。
  # 若 UNC 写入被拒，exe 会回落到 $StageDir\frame-acquire.png，这里再拷回来。
  $repoCaptureOut = Join-Path $Root 'spikes\media-dll-smoke\out'
  New-Item -ItemType Directory -Force -Path $repoCaptureOut | Out-Null
  $repoFramePng = Join-Path $repoCaptureOut 'frame-acquire.png'
  Remove-Item -Force -ErrorAction SilentlyContinue $repoFramePng
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'screen_capture_smoke.exe') --seconds 10 --frame-png $repoFramePng
    $captureExitCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if (-not (Test-Path $repoFramePng)) {
    $stageFramePng = Join-Path $StageDir 'frame-acquire.png'
    if (Test-Path $stageFramePng) {
      Copy-Item -Force $stageFramePng $repoFramePng
      Write-Host "== frame png copied from stage fallback: $repoFramePng"
    }
  }
  if (Test-Path $repoFramePng) {
    $framePngInfo = Get-Item $repoFramePng
    Write-Host ("== frame png: {0} ({1:N0} bytes)" -f $repoFramePng, $framePngInfo.Length)
  } else {
    Write-Host "== WARN: frame-acquire.png was not produced"
  }
  if ($captureExitCode -ne 0) { throw "screen_capture_smoke failed: exit=$captureExitCode" }
  Write-Host "== screen_capture_smoke exit=0"
  # 首帧 PNG 与引擎日志带回仓库，方便从 WSL 用 read_image / 文本工具直接看。
  $stagePng = Join-Path $StageDir 'capture-first-frame.png'
  if (Test-Path $stagePng) {
    Copy-Item -Force $stagePng (Join-Path $OutDir 'capture-first-frame.png')
    Write-Host "== first-frame png: $(Join-Path $OutDir 'capture-first-frame.png')"
  } else {
    Write-Host "== WARN: capture-first-frame.png was not produced"
  }
  $engineLog = Join-Path $env:TEMP 'partyup-screen-capture-smoke.log'
  if (Test-Path $engineLog) {
    Copy-Item -Force $engineLog (Join-Path $OutDir 'screen-capture-engine.log')
    Write-Host "== engine log: $(Join-Path $OutDir 'screen-capture-engine.log')"
  }
}

# ---- 4b. 可选：只跑合成源显式观看回归（不触发真实桌面采集） ----
if ($RunWatch) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  Copy-Item -Force $dllOut (Join-Path $StageDir 'partyup_media.dll')
  Copy-Item -Force $watchOut (Join-Path $StageDir 'media_watch_regression.exe')
  Write-Host "== [watch] synthetic-only explicit watch regression from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_watch_regression.exe')
    $watchExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($watchExit -ne 0) { throw "media_watch_regression failed: exit=$watchExit" }
  Write-Host "== media_watch_regression exit=0"
}

# ---- 4c. 可选：只跑 fake ADM 音频设备控制回归（不打开任何真实音频设备） ----
if ($RunAudioSwitch) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  Copy-Item -Force $audioSwitchOut (Join-Path $StageDir 'audio_switch_regression.exe')
  Write-Host "== [audio] fake-ADM device control regression from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'audio_switch_regression.exe')
    $audioSwitchExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($audioSwitchExit -ne 0) { throw "audio_switch_regression failed: exit=$audioSwitchExit" }
  Write-Host "== audio_switch_regression exit=0"
}

# ---- 4d. 可选：direct 严格接收隔离回归（双引擎 4 PC；只用 synthetic） ----
if ($RunDirectIsolation) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  Copy-Item -Force $dllOut (Join-Path $StageDir 'partyup_media.dll')
  Copy-Item -Force $directIsoOut (Join-Path $StageDir 'media_direct_isolation.exe')
  Write-Host "== [direct] synthetic-only direct isolation regression from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_direct_isolation.exe')
    $directIsoExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($directIsoExit -ne 0) { throw "media_direct_isolation failed: exit=$directIsoExit" }
  Write-Host "== media_direct_isolation exit=0"
}

# ---- 4e. 可选：官方 ADM observer 的 fake-only 预检（不打开任何真实音频设备） ----
if ($RunAudioMonitorProbe) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  # probe 静态链接 webrtc.lib，不加载 partyup_media.dll；只拷 exe。
  Copy-Item -Force $audioMonitorProbeOut (Join-Path $StageDir 'audio_monitor_probe.exe')
  Write-Host "== [audio-monitor] official ADM observer fake-only probe from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'audio_monitor_probe.exe')
    $audioMonitorProbeExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($audioMonitorProbeExit -ne 0) { throw "audio_monitor_probe failed: exit=$audioMonitorProbeExit" }
  Write-Host "== audio_monitor_probe exit=0"
}

# ---- 4f. 可选：AUD02 独立输入电平的 fake 端口回归（不打开任何真实音频设备） ----
if ($RunAudioMonitor) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  # 回归只链生产 Module，不加载 partyup_media.dll；只拷 exe。
  Copy-Item -Force $audioMonitorRegressionOut (Join-Path $StageDir 'audio_monitor_regression.exe')
  Write-Host "== [audio-monitor] fake-port input-level regression from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'audio_monitor_regression.exe')
    $audioMonitorExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($audioMonitorExit -ne 0) { throw "audio_monitor_regression failed: exit=$audioMonitorExit" }
  Write-Host "== audio_monitor_regression exit=0"
}

# ---- 5. 可选：teardown 压测（复现/验证收尾竞态） ----
if ($StressIterations -gt 0) {
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  $stageLogs = Join-Path $StageDir ("logs\stress-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
  New-Item -ItemType Directory -Force -Path $stageLogs | Out-Null
  Copy-Item -Force $dllOut (Join-Path $StageDir 'partyup_media.dll')
  Copy-Item -Force $stressOut (Join-Path $StageDir 'teardown_stress.exe')
  if ($Symbols) {
    Get-ChildItem -Path $workRoot -Filter '*.pdb' | Copy-Item -Force -Destination $StageDir
  }
  Write-Host ("== teardown stress: {0} iteration(s), order={1}, logs={2}" -f `
      $StressIterations, $StressOrder, $stageLogs)
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'teardown_stress.exe') --iterations $StressIterations `
      --order $StressOrder --min-frames $StressMinFrames --logs $stageLogs
    $stressExit = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  # 无论崩没崩都把日志/转储留档到 out/run-logs（仓库内，方便事后分析）。
  $archiveLogs = Join-Path $OutDir ("run-logs\stress-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
  New-Item -ItemType Directory -Force -Path (Split-Path $archiveLogs -Parent) | Out-Null
  Copy-Item -Recurse -Force $stageLogs $archiveLogs
  Write-Host "== stress logs archived: $archiveLogs"
  if ($stressExit -ne 0) {
    throw "teardown stress failed: exit=$stressExit (see $archiveLogs)"
  }
  Write-Host "== teardown_stress exit=0"
}

Write-Host "== done" -ForegroundColor Green
