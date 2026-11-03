<#
.SYNOPSIS
  构建（并运行）voice-adm-probe：确认本机能否创建「真实 WASAPI ADM」。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\build-voice-adm-probe.ps1'
#>
param(
  [string]$Root,
  [string]$OutDir,
  [switch]$TryWinFactory,
  [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
  if ($PSScriptRoot) { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }
  else { $Root = (Get-Location).Path }
}
if (-not $OutDir) { $OutDir = Join-Path $Root 'spikes\media-dll-smoke\out' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$src = Join-Path $Root 'spikes\media-dll-smoke\voice-adm-probe.cpp'
$include = Join-Path $Root 'third_party\libwebrtc\include'
$webrtcLib = Join-Path $Root 'third_party\libwebrtc\release\webrtc.lib'
foreach ($p in @($vcvars, $src, $include, $webrtcLib)) {
  if (-not (Test-Path $p)) { throw "missing required path: $p" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exe = Join-Path $OutDir 'voice-adm-probe.exe'
# cl 写 .obj 需要一个 Windows 本地盘的 cwd（UNC 下会 Permission denied）。
$objDir = Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage\voice-adm-probe'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE',
  "/I`"$include`"",
  "/I`"$include\third_party\abseil-cpp`"",
  "/I`"$include\third_party\libyuv\include`""
)
if ($TryWinFactory) { $common += '/DTRY_WIN_FACTORY' }

$cmd = "cl $($common -join ' ') `"$src`" /Fe:`"$exe`" /link `"$webrtcLib`" " +
  'winmm.lib d3d11.lib dxgi.lib ole32.lib oleaut32.lib user32.lib gdi32.lib ' +
  'shcore.lib ws2_32.lib advapi32.lib secur32.lib crypt32.lib iphlpapi.lib ' +
  'dmoguids.lib msdmo.lib wmcodecdspuuid.lib strmiids.lib mfplat.lib mf.lib mfuuid.lib ' +
  'amstrmid.lib uuid.lib winmm.lib'

Push-Location $objDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $cmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building voice-adm-probe.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

Write-Host "== built: $exe"
if (-not $BuildOnly) {
  Write-Host "== running: $exe"
  & $exe
  Write-Host ("== exit=" + $LASTEXITCODE)
}
