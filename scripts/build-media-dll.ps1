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
  [switch]$Symbols,
  [int]$StressIterations = 0,
  [string]$StressOrder = 'viewer-first',
  [int]$StressMinFrames = 5
)

$ErrorActionPreference = 'Stop'

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
# vendored 的 nlohmann/json 单头文件（信令信封解析用），见 src/media-core/vendor/nlohmann/README.md
$vendorDir = Join-Path $Root 'src\media-core\vendor'
$dllSrc = Join-Path $Root 'src\media-core\src\media_engine.cpp'
# v2.3：真实屏幕采集（Windows Graphics Capture）单独一个编译单元，见文件头注释。
$wgcSrc = Join-Path $Root 'src\media-core\src\wgc_screen.cpp'
# v2.4：硬件 H.264 编解码（Media Foundation MFT）单独一个编译单元，见文件头注释。
$hwSrc = Join-Path $Root 'src\media-core\src\hw_encoder.cpp'
$smokeSrc = Join-Path $Root 'spikes\media-dll-smoke\main.cpp'
$signalingSrc = Join-Path $Root 'spikes\media-dll-smoke\signaling.cpp'
$stressSrc = Join-Path $Root 'spikes\media-dll-smoke\teardown_stress.cpp'
$captureSrc = Join-Path $Root 'spikes\media-dll-smoke\screen_capture.cpp'

foreach ($required in @($vcvars, $include, $libDir, $dllSrc, $wgcSrc, $hwSrc, $smokeSrc, $signalingSrc, $stressSrc, $captureSrc, $vendorDir)) {
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
$exportsTxt = Join-Path $OutDir 'exports.txt'

# 编译中间文件与本机运行目录都放本机盘：cmd.exe 不支持 UNC 作为工作目录，
# 而运行阶段也明确要求不经 UNC。
$workRoot = Join-Path $StageDir 'build'
$objDll = Join-Path $workRoot 'obj-dll'
$objSmoke = Join-Path $workRoot 'obj-smoke'
$objSignaling = Join-Path $workRoot 'obj-signaling'
$objStress = Join-Path $workRoot 'obj-stress'
$objCapture = Join-Path $workRoot 'obj-capture'
New-Item -ItemType Directory -Force -Path $objDll, $objSmoke, $objSignaling, $objStress, $objCapture | Out-Null

# -Symbols：带调试信息构建，崩溃栈才能符号化到函数/行号（PDB 落在本机 $workRoot，
# 避免大 PDB 写 UNC 时偶发 LNK1201，也方便 dbghelp 就地加载）。
$debugCompile = ''
$debugLinkDll = ''
$debugLinkSmoke = ''
$debugLinkSignaling = ''
$debugLinkStress = ''
$debugLinkCapture = ''
if ($Symbols) {
  $debugCompile = ' /Zi'
  $debugLinkDll = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\partyup_media.pdb`""
  $debugLinkSmoke = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_smoke.pdb`""
  $debugLinkSignaling = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\media_signaling_smoke.pdb`""
  $debugLinkStress = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\teardown_stress.pdb`""
  $debugLinkCapture = " /DEBUG /INCREMENTAL:NO /PDB:`"$workRoot\screen_capture_smoke.pdb`""
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
$dllCmd = "cl $common$debugCompile /LD /DPU_BUILD_SHARED `"$dllSrc`" `"$wgcSrc`" `"$hwSrc`" /Fe:`"$dllOut`" " +
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
# 导出表断言：v2.12 起是 24 个（23 + pu_engine_set_ice_servers）。
# 少一个说明有函数被剥掉，多一个说明有人没走 ABI 评审就加导出。
$expectedExports = 24
if ($exportNames.Count -ne $expectedExports) {
  throw ("export count {0} != expected {1} (v2.12). names: {2}" -f $exportNames.Count, $expectedExports, ($exportNames -join ' '))
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
