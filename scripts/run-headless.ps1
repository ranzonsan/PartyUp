# 无窗口运行 PartyUp 客户端的 console 模式（--selftest / --duo / --role send）。
#
# 为什么需要它：调试桥的 build-run / run 会在 Windows 桌面启动进程，且形态上等同于
# 「跑 GUI」；当桌面不允许弹窗时，改用「wdb build（只编译）+ 本脚本（无窗口启动）」。
# 这些模式在 Program.Main 里就返回，不会初始化 Avalonia，所以不会创建窗口。
#
# 在 WSL 里调用：
#   pwsh.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass `
#     -File scripts/run-headless.ps1 -Exe 'C:\...\PartyUp.Client.exe' `
#     -LogPath 'C:\Users\<you>\AppData\Local\Temp\out.log' `
#     -ArgumentsBase64 <base64(逗号分隔的参数)>
#
# 为什么参数用 base64：`-File` 的参数解析会把以 `--` 开头的值当成参数名（实测报
# "缺少参数 'Arguments' 的某个参数"）。base64 不含 `-`/`,`/空格，绕开解析与转义问题。
param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,

    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [string]$ArgumentsBase64 = '',

    [string]$ErrorPath = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Output "exe not found: $Exe"
    exit 4
}

$argList = @()
if ($ArgumentsBase64) {
    $joined = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String($ArgumentsBase64))
    $argList = $joined.Split(',')
}

$errPath = if ($ErrorPath) { $ErrorPath } else { "$LogPath.err" }

$process = Start-Process -FilePath $Exe `
    -ArgumentList $argList `
    -NoNewWindow `
    -Wait `
    -PassThru `
    -RedirectStandardOutput $LogPath `
    -RedirectStandardError $errPath

Write-Output "exit=$($process.ExitCode) log=$LogPath err=$errPath"
exit $process.ExitCode
