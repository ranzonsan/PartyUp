<#
.SYNOPSIS
  构建并运行 wgc-texture-smoke：同一批 300 帧上对比 WgcCapture 的 I420 路径与纹理路径。

.DESCRIPTION
  直接编生产代码 src/media-core/src/wgc_screen.cpp，并链接 libwebrtc 静态库
  （VideoFrame / libyuv 符号都在里面）。产物落 spikes/media-dll-smoke/out/。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\build-wgc-texture-smoke.ps1' -Frames 300
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage\wgc-texture'),
  [int]$Frames = 300,
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

$src = Join-Path $Root 'spikes\media-dll-smoke\wgc-texture-smoke.cpp'
$wgc = Join-Path $Root 'src\media-core\src\wgc_screen.cpp'
$include = Join-Path $Root 'third_party\libwebrtc\include'
$libDir = Join-Path $Root 'third_party\libwebrtc\release'
$webrtcLib = Join-Path $libDir 'webrtc.lib'
foreach ($p in @($vcvars, $src, $wgc, $include, $webrtcLib)) {
  if (-not (Test-Path $p)) { throw "missing required path: $p" }
}

New-Item -ItemType Directory -Force -Path $OutDir, $StageDir | Out-Null
$exe = Join-Path $OutDir 'wgc-texture-smoke.exe'
$log = Join-Path $OutDir 'wgc-texture-smoke.log'

# 编译中间文件放本机盘：UNC 下 cl 写 .obj 会 Permission denied。
$objDir = Join-Path $StageDir 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

# /MT 必须与 libwebrtc 静态库的 runtime 一致；libyuv 走 vcpkg 的静态库文件。
$libyuvLib = Join-Path $libDir 'libyuv.lib'
$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE',
  "/I`"$include`"",
  "/I`"$include\third_party\abseil-cpp`"",
  "/I`"$include\third_party\libyuv\include`"",
  "/I`"$Root\src\media-core\src`""
) -join ' '
$libs = @(
  "/LIBPATH:`"$libDir`"", 'webrtc.lib',
  'd3d11.lib', 'dxgi.lib', 'windowscodecs.lib', 'runtimeobject.lib',
  'ole32.lib', 'oleaut32.lib', 'user32.lib', 'gdi32.lib', 'shcore.lib',
  'ws2_32.lib', 'secur32.lib', 'crypt32.lib', 'iphlpapi.lib',
  'mfplat.lib', 'mf.lib', 'mfuuid.lib', 'wmcodecdspuuid.lib',
  'strmiids.lib', 'dwmapi.lib', 'version.lib', 'winmm.lib', 'avrt.lib'
) -join ' '
if (Test-Path $libyuvLib) { $libs += ' libyuv.lib' }

Write-Host "== cl: $src + wgc_screen.cpp"
$cmd = "cl $common `"$src`" `"$wgc`" /Fe:`"$exe`" /link $libs"
Push-Location $objDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $cmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building wgc-texture-smoke.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}
Write-Host "== built: $exe"
if ($BuildOnly) { return }

Write-Host "== run: $exe $Frames `"$OutDir`""
Push-Location $StageDir
try {
  & $exe "$Frames" "$OutDir" 2>&1 | Tee-Object -FilePath $log
  $exit = $LASTEXITCODE
} finally {
  Pop-Location
}
Write-Host "== log: $log"
if ($exit -ne 0) { throw "smoke exited $exit" }
Write-Host '== wgc texture smoke done' -ForegroundColor Green
