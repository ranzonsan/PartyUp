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

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\scripts\build-media-dll.ps1' -Run
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage'),
  [switch]$Run
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
$smokeSrc = Join-Path $Root 'spikes\media-dll-smoke\main.cpp'
$signalingSrc = Join-Path $Root 'spikes\media-dll-smoke\signaling.cpp'

foreach ($required in @($vcvars, $include, $libDir, $dllSrc, $smokeSrc, $signalingSrc, $vendorDir)) {
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
$exportsTxt = Join-Path $OutDir 'exports.txt'

# 编译中间文件与本机运行目录都放本机盘：cmd.exe 不支持 UNC 作为工作目录，
# 而运行阶段也明确要求不经 UNC。
$workRoot = Join-Path $StageDir 'build'
$objDll = Join-Path $workRoot 'obj-dll'
$objSmoke = Join-Path $workRoot 'obj-smoke'
$objSignaling = Join-Path $workRoot 'obj-signaling'
New-Item -ItemType Directory -Force -Path $objDll, $objSmoke, $objSignaling | Out-Null

$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DWEBRTC_WIN', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
  "/I`"$include`"",
  "/I`"$include\third_party\abseil-cpp`"",
  "/I`"$hdrDir`"",
  "/I`"$vendorDir`""
) -join ' '

# 与 spike 完全相同的系统库清单；缺哪个链接器会报 unresolved external，按提示补。
$sysLibs = @(
  'ws2_32.lib', 'secur32.lib', 'crypt32.lib', 'iphlpapi.lib', 'winmm.lib', 'shlwapi.lib',
  'dmoguids.lib', 'wmcodecdspuuid.lib', 'msdmo.lib', 'strmiids.lib', 'advapi32.lib', 'ole32.lib'
) -join ' '

Write-Host "== vcvars : $vcvars"
Write-Host "== root   : $Root"
Write-Host "== out    : $OutDir"
Write-Host "== stage  : $StageDir"

# ---- 1. 动态库 ----
$dllCmd = "cl $common /LD /DPU_BUILD_SHARED `"$dllSrc`" /Fe:`"$dllOut`" " +
          "/link /LIBPATH:`"$libDir`" webrtc.lib $sysLibs /IMPLIB:`"$dllLib`""

Push-Location $objDll
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $dllCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building partyup_media.dll (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2. 冒烟测试 exe（链接导入库） ----
$smokeCmd = "cl $common `"$smokeSrc`" /Fe:`"$smokeOut`" /link `"$dllLib`""

Push-Location $objSmoke
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $smokeCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_smoke.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# ---- 2b. 双实例信令冒烟 exe（v2：两个 engine 同进程互相喂 SIGNAL_OUT） ----
$signalingCmd = "cl $common `"$signalingSrc`" /Fe:`"$signalingOut`" /link `"$dllLib`""

Push-Location $objSignaling
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $signalingCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building media_signaling_smoke.exe (exit $LASTEXITCODE)" }
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
  if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$') {
    $exportNames += $Matches[1]
  }
}
$badExports = @($exportNames | Where-Object { $_ -notmatch '^pu_' })
if ($exportNames.Count -eq 0) { throw "dumpbin reported no exports — DLL is broken" }
if ($badExports.Count -ne 0) {
  throw "non pu_* exports found: $($badExports -join ', ')"
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
  Write-Host "== [1/2] smoke (single engine lifecycle) from local disk: $StageDir"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_smoke.exe')
    $exitCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($exitCode -ne 0) { throw "media_smoke failed: exit=$exitCode" }
  Write-Host "== media_smoke exit=0"

  Write-Host "== [2/2] signaling smoke (two engines, offer/answer/DTLS/remote frames)"
  Push-Location $StageDir
  try {
    & (Join-Path $StageDir 'media_signaling_smoke.exe')
    $sigExitCode = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  if ($sigExitCode -ne 0) { throw "media_signaling_smoke failed: exit=$sigExitCode" }
  Write-Host "== media_signaling_smoke exit=0"
}

Write-Host "== done" -ForegroundColor Green
