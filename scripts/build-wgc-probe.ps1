<#
.SYNOPSIS
  构建（并可选运行）Windows Graphics Capture 能力探针 wgc_probe.exe。

.DESCRIPTION
  * 只链接系统库：d3d11.lib dxgi.lib windowscodecs.lib runtimeobject.lib
    ole32.lib user32.lib shcore.lib。不依赖 libwebrtc，也不链接 windowsapp.lib。
  * 编译选项与 scripts/build-media-dll.ps1 一致：MSVC + /MT /std:c++20 /utf-8。
  * 编译工作目录固定在 Windows 本机磁盘（$StageDir\build），不在 UNC 下跑 cmd/cl；
    只有读源码（main.cpp）走 UNC。
  * -Run 时把 exe 复制到 $StageDir 并从本机盘运行，PNG/日志再拷回仓库
    <OutDir>\<run>，方便从 WSL 用 read_image 直接看。

.PARAMETER Root
  仓库根目录。缺省为本脚本所在目录的上级（脚本经 UNC 调用时即 UNC 路径）。

.PARAMETER OutDir
  仓库侧产物目录，缺省 <Root>\spikes\wgc-probe\out。exe 与每次运行的日志/PNG 放这里。

.PARAMETER StageDir
  本机暂存与运行目录，缺省 %LOCALAPPDATA%\PartyUp\wgc-probe。必须是 Windows 本地盘。

.PARAMETER Run
  构建后从本机盘运行探针。

.PARAMETER Runs
  -Run 时连跑几次（缺省 1）。每次独立进程、独立输出目录。

.PARAMETER Frames
  首帧之后连续抓多少帧做计时（缺省 10）。

.PARAMETER Stimulus
  打开激励窗口。默认不打开：实测本机静止桌面下 WGC 仍按 ~21 ms 节奏产帧。
  若某台机器在静止桌面下等不到连续帧，可用此开关加一个 260x140、
  置顶穿透、每 33 ms 变一次不透明度的小窗来驱动 DWM 产帧。

.PARAMETER TimeoutMs
  首帧超时（缺省 5000）。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\scripts\build-wgc-probe.ps1' -Run -Runs 3

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\scripts\build-wgc-probe.ps1' -Run -Stimulus
#>
param(
  [string]$Root,
  [string]$OutDir,
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\wgc-probe'),
  [switch]$Run,
  [int]$Runs = 1,
  [int]$Frames = 10,
  [switch]$Stimulus,
  [int]$TimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
  if ($PSScriptRoot) {
    $Root = Split-Path $PSScriptRoot -Parent
  } else {
    $Root = (Get-Location).Path
  }
}
if (-not $OutDir) { $OutDir = Join-Path $Root 'spikes\wgc-probe\out' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $Root 'spikes\wgc-probe\main.cpp'

foreach ($required in @($vcvars, $src)) {
  if (-not (Test-Path $required)) { throw "missing required path: $required" }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exeOut = Join-Path $OutDir 'wgc_probe.exe'

# 编译中间文件放本机盘：cmd.exe 不支持 UNC 作为工作目录。
$workRoot = Join-Path $StageDir 'build'
$objDir = Join-Path $workRoot 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$common = @(
  '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE'
) -join ' '

# 只允许系统库；缺哪个链接器会报 unresolved external，按提示补（同时更新文档）。
$sysLibs = @(
  'd3d11.lib', 'dxgi.lib', 'windowscodecs.lib', 'runtimeobject.lib',
  'ole32.lib', 'user32.lib', 'shcore.lib'
) -join ' '

Write-Host "== vcvars : $vcvars"
Write-Host "== root   : $Root"
Write-Host "== out    : $OutDir"
Write-Host "== stage  : $StageDir"

$probeCmd = "cl $common `"$src`" /Fe:`"$exeOut`" /link $sysLibs"

Push-Location $objDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $probeCmd"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building wgc_probe.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

$exeInfo = Get-Item $exeOut
Write-Host ("== wgc_probe.exe : {0:N0} bytes ({1:N2} MB)" -f $exeInfo.Length, ($exeInfo.Length / 1MB))

if (-not $Run) {
  Write-Host "== done (build only)" -ForegroundColor Green
  return
}

if ($Runs -lt 1) { $Runs = 1 }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
$stageExe = Join-Path $StageDir 'wgc_probe.exe'
Copy-Item -Force $exeOut $stageExe

$failures = @()
for ($i = 1; $i -le $Runs; $i++) {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $runName = 'run{0}-{1}' -f $i, $stamp
  $stageRun = Join-Path $StageDir ('shots\' + $runName)
  $repoRun = Join-Path $OutDir $runName
  New-Item -ItemType Directory -Force -Path $stageRun, $repoRun | Out-Null

  $probeArgs = @(
    '--out', $stageRun,
    '--label', "run$i",
    '--frames', $Frames,
    '--timeout-ms', $TimeoutMs
  )
  if ($Stimulus) { $probeArgs += '--stimulus' }

  $logPath = Join-Path $stageRun 'probe.log'
  Write-Host ("== run {0}/{1} from local disk: {2}" -f $i, $Runs, $StageDir) -ForegroundColor Cyan
  Push-Location $StageDir
  $prevOutEncoding = [Console]::OutputEncoding
  try {
    # 探针按 UTF-8 输出（窗口标题/显示器友好名含中文）；不显式设成 UTF-8 时
    # PowerShell 会按控制台 ANSI 代码页解码，Tee 出来的日志就是乱码。
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    & $stageExe @probeArgs 2>&1 | Tee-Object -FilePath $logPath
    $exitCode = $LASTEXITCODE
  } finally {
    [Console]::OutputEncoding = $prevOutEncoding
    Pop-Location
  }

  Copy-Item -Force $logPath (Join-Path $repoRun 'probe.log')
  $pngs = @(Get-ChildItem -Path $stageRun -Filter '*.png' -File -ErrorAction SilentlyContinue)
  foreach ($png in $pngs) {
    Copy-Item -Force $png.FullName (Join-Path $repoRun $png.Name)
  }

  $resultLines = @()
  if (Test-Path $logPath) {
    $resultLines = @(Get-Content $logPath | Where-Object {
      $_ -match '^(resolution|cold_first_frame_ms|frames_measured|frame_interval_avg_ms|frame_interval_max_ms|readback_avg_ms|readback_max_ms|copy_map_avg_ms|copy_map_max_ms|pack_avg_ms|pack_max_ms|png_path|png_bytes)='
    })
  }

  Write-Host ("== run {0} exit={1}" -f $i, $exitCode)
  foreach ($line in $resultLines) { Write-Host ("==   " + $line) }
  foreach ($png in $pngs) {
    $repoPng = Join-Path $repoRun $png.Name
    Write-Host ("==   png={0} ({1:N0} bytes)" -f $repoPng, $png.Length)
  }

  if ($exitCode -ne 0) {
    $failures += $runName
  }
}

if ($failures.Count -gt 0) {
  throw ("wgc probe failed in {0}/{1} run(s): {2}" -f $failures.Count, $Runs, ($failures -join ', '))
}

Write-Host "== done" -ForegroundColor Green
