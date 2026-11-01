param(
    [string]$Configuration = 'Debug',
    [string]$Project = 'spikes/frame-presentation/Spike.Frame.csproj'
)

$ErrorActionPreference = 'Stop'

# 新装的工具不在已运行 WSL 会话继承的 PATH 里，显式补齐
$env:Path = "C:\Program Files\CMake\bin;C:\Program Files\Go\bin;$env:Path"
$env:GOPROXY = 'https://goproxy.cn,direct'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'

Set-Location C:\src\PartyUp

Write-Host "== dotnet build $Project ($Configuration)" -ForegroundColor Cyan
dotnet build $Project -c $Configuration --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed with exit code $LASTEXITCODE" }
