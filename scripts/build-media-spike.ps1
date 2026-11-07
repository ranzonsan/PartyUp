param(
    # 仓库根：默认取脚本所在目录（scripts\）的父目录，换机器无需改路径。
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Source = 'spikes\media-link\main.cpp',
    [string]$Exe = 'spikes\media-link\out\hello_webrtc.exe'
)

$ErrorActionPreference = 'Stop'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found (vswhere returned nothing)' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$include = Join-Path $Root 'third_party\libwebrtc\include'
$libDir  = Join-Path $Root 'third_party\libwebrtc\release'
$src     = Join-Path $Root $Source
$outExe  = Join-Path $Root $Exe
$outDir  = Split-Path $outExe

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# libwebrtc 静态库以 /MT 静态 CRT 编译（libcmt），必须用 /MT 匹配；
# 该库由 clang-cl 构建（MS ABI），MSVC 可直接链接。
$cl = @(
    'cl', '/nologo', '/MT', '/std:c++20', '/EHsc', '/O2', '/W3', '/utf-8',
    '/DWEBRTC_WIN', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
    "/I`"$include`"",
    "/I`"$include\third_party\abseil-cpp`"",
    "`"$src`"",
    "/Fe:`"$outExe`"",
    "/Fo:`"$outDir\\`"",
    '/link', "/LIBPATH:`"$libDir`"",
    'webrtc.lib',
    'ws2_32.lib', 'secur32.lib', 'crypt32.lib', 'iphlpapi.lib',
    'winmm.lib', 'shlwapi.lib', 'dmoguids.lib', 'wmcodecdspuuid.lib',
    'msdmo.lib', 'strmiids.lib', 'advapi32.lib', 'ole32.lib'
) -join ' '

Write-Host "== vcvars: $vcvars" -ForegroundColor Cyan
cmd /c "call `"$vcvars`" >nul 2>&1 && $cl"
if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }

Write-Host "== built: $outExe" -ForegroundColor Green
