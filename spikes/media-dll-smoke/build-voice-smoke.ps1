<#
.SYNOPSIS
  构建并运行 voice_smoke.exe（链接 partyup_media.dll 的导入库）。

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\build-voice-smoke.ps1'
#>
param(
  [string]$Root,
  [string]$Name = 'voice_smoke',
  [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
  if ($PSScriptRoot) { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }
  else { $Root = (Get-Location).Path }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$include = Join-Path $Root 'third_party\libwebrtc\include'
$hdrDir = Join-Path $Root 'src\media-core\include'
$dllLib = Join-Path $Root 'src\media-core\out\partyup_media.lib'
$dllDll = Join-Path $Root 'src\media-core\out\partyup_media.dll'
$src = Join-Path $Root "spikes\media-dll-smoke\$Name.cpp"
$outExe = Join-Path $Root "spikes\media-dll-smoke\out\$Name.exe"
$outDir = Split-Path $outExe
foreach ($p in @($vcvars, $include, $hdrDir, $dllLib, $dllDll, $src)) {
  if (-not (Test-Path $p)) { throw "missing required path: $p" }
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$objDir = Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage\obj-voice-smoke'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$cl = @(
  'cl', '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8', '/DNDEBUG',
  '/DWEBRTC_WIN', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
  "/I`"$include`"",
  "/I`"$include\third_party\abseil-cpp`"",
  "/I`"$hdrDir`"",
  "`"$src`"",
  "/Fe:`"$outExe`"",
  "/Fo:`"$objDir\\`"",
  '/link', "`"$dllLib`""
) -join ' '

Push-Location $objDir
try {
  cmd /c "call `"$vcvars`" >nul 2>&1 && $cl"
  if ($LASTEXITCODE -ne 0) { throw "cl failed building $Name.exe (exit $LASTEXITCODE)" }
} finally {
  Pop-Location
}

Write-Host "== built: $outExe"
if (-not $BuildOnly) {
  # 独立 stage 目录：600 轮压测可能正占着 media-stage 里的 partyup_media.dll（进程加载即锁文件）。
  $stage = Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage-voice'
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  Copy-Item -Force $outExe $stage
  Copy-Item -Force $dllDll $stage
  Push-Location $stage
  try { & (Join-Path $stage "$Name.exe") } finally { Pop-Location }
  Write-Host ("== exit=" + $LASTEXITCODE)
}
