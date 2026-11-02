<#
.SYNOPSIS
  编译并运行 capture-texture-probe：同一批 WGC 帧上对比
  「现状路径（staging 读回 + CPU pack）」与「目标路径（VideoProcessorBlt → NV12）」。

.DESCRIPTION
  纯新增探针，不参与 partyup_media.dll 的生产构建；只链系统库。
  产物落 spikes/media-dll-smoke/out/，日志同名 .log。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\build-capture-texture-probe.ps1' -Frames 300
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage\capture-probe'),
  [int]$Frames = 300,
  [switch]$NoStimulus
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

$src = Join-Path $Root 'spikes\media-dll-smoke\capture-texture-probe.cpp'
if (-not (Test-Path $src)) { throw "missing probe source: $src" }

New-Item -ItemType Directory -Force -Path $OutDir, $StageDir | Out-Null
$exe = Join-Path $OutDir 'capture-texture-probe.exe'
$log = Join-Path $OutDir 'capture-texture-probe.log'
$png = Join-Path $OutDir 'capture-texture-probe.png'

# 编译工作目录放本机盘：UNC 下 cl 写 .obj 会 Permission denied。
$objDir = Join-Path $StageDir 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE'
) -join ' '
$sysLibs = @(
  'd3d11.lib', 'dxgi.lib', 'windowscodecs.lib', 'runtimeobject.lib',
  'ole32.lib', 'user32.lib', 'shcore.lib', 'gdi32.lib'
) -join ' '

Write-Host "== cl: $src"
$cmd = "cl $common `"$src`" /Fe:`"$exe`" /link $sysLibs"
Push-Location $objDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $cmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building capture-texture-probe.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

$probeArgs = @('--frames', "$Frames", '--out', $OutDir, '--label', 'capture-texture')
if ($NoStimulus) { $probeArgs += '--no-stimulus' }
Write-Host "== run: $exe $probeArgs"
Push-Location $StageDir
try {
  & $exe @probeArgs 2>&1 | Tee-Object -FilePath $log
  $exit = $LASTEXITCODE
} finally {
  Pop-Location
}
Write-Host "== log: $log"
if ($exit -ne 0) { throw "probe exited $exit" }
Write-Host '== capture texture probe done' -ForegroundColor Green
