<#
.SYNOPSIS
  编译并运行硬件 H.264 MFT 探针（spikes/media-dll-smoke/mft_*.cpp）。

.DESCRIPTION
  只是探测，不参与 partyup_media.dll 的生产构建。两种探针：
    -Capability（默认）：mft_probe.cpp，枚举编码/解码 MFT、属性、D3D11 握手
    -Encode           ：mft_encode_probe.cpp，真跑一遍 H.264 编码 + 解码验证
  产物落 <repo>/spikes/media-dll-smoke/out/，日志同名 .log。

  编译工作目录固定在 Windows 本机盘（$StageDir）：从 UNC 直接调 cmd/cl 时
  CMD 会把 cwd 回落到 C:\Windows，cl 写 .obj 会 Permission denied。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\probe-hardware-mft.ps1'

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\probe-hardware-mft.ps1' `
    -Encode -Texture
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage\probe'),
  [switch]$Encode,
  [switch]$Texture,
  # 隔离对照：同一批合成帧上跑硬件 H.264 MFT 与软件 VP8（libwebrtc），
  # 输出「每帧 CPU / 墙钟 / 平均字节」对照表。需要链接 webrtc.lib。
  [switch]$Ab,
  [int]$Frames = 300
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
  # 本脚本在 <repo>/spikes/media-dll-smoke/ 下：上两级就是仓库根。
  if ($PSScriptRoot) { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }
  else { $Root = (Get-Location).Path }
}
if (-not $OutDir) { $OutDir = Join-Path $Root 'spikes\media-dll-smoke\out' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$probe = if ($Encode -or $Ab) { 'mft_encode_probe' } else { 'mft_probe' }
$src = Join-Path $Root "spikes\media-dll-smoke\$probe.cpp"
if (-not (Test-Path $src)) { throw "missing probe source: $src" }

New-Item -ItemType Directory -Force -Path $OutDir, $StageDir | Out-Null
$exe = Join-Path $OutDir "$probe.exe"
$log = Join-Path $OutDir "$probe.log"

$common = @('/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/utf-8', '/DNDEBUG',
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') -join ' '
$libs = @('mfplat.lib', 'mfuuid.lib', 'ole32.lib', 'oleaut32.lib', 'd3d11.lib',
          'dxgi.lib') -join ' '

# 隔离对照要链 libwebrtc（软件 VP8 在 webrtc.lib 里，libyuv 符号也在里面）。
if ($Ab) {
  $include = Join-Path $Root 'third_party\libwebrtc\include'
  $libDir = Join-Path $Root 'third_party\libwebrtc\release'
  $common = @(
    $common, '/DWEBRTC_WIN',
    "/I`"$include`"",
    "/I`"$include\third_party\abseil-cpp`"",
    "/I`"$include\third_party\libyuv\include`""
  ) -join ' '
  $common = "/I`"$include`" /I`"$include\third_party\abseil-cpp`" " +
            "/I`"$include\third_party\libyuv\include`"" + ' ' + $common
  $libs = "$libs /LIBPATH:`"$libDir`" webrtc.lib ws2_32.lib secur32.lib crypt32.lib " +
          'iphlpapi.lib winmm.lib shlwapi.lib dmoguids.lib wmcodecdspuuid.lib msdmo.lib ' +
          'strmiids.lib advapi32.lib runtimeobject.lib windowscodecs.lib user32.lib shcore.lib ' +
          'mf.lib'
}

# .obj 落在 cwd（Push-Location 到本机 $StageDir），不要给 /Fo 带尾反斜杠的路径：
# 反斜杠会把引号转义掉，cl 会把 "/link ..." 当成输出文件名。
$cmd = "cl $common `"$src`" /Fe:`"$exe`" /link $libs"
Write-Host "== cl: $cmd"
Push-Location $StageDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $cmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building $probe.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

$probeArgs = @()
if ($Encode -and $Texture) { $probeArgs += '--texture' }
if ($Ab) { $probeArgs += @('--ab', '--frames', "$Frames") }
Write-Host "== run: $exe $probeArgs"
Push-Location $StageDir
try {
  & $exe @probeArgs 2>&1 | Tee-Object -FilePath $log
  $exit = $LASTEXITCODE
} finally {
  Pop-Location
}
Write-Host "== log: $log"
if ($exit -ne 0) { throw "$probe exited $exit" }
Write-Host '== probe done' -ForegroundColor Green
