param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$ServerExe,
  [string]$Root = "$env:TEMP\pu-gui",
  [string]$HttpAddr = '127.0.0.1:18994',
  [int]$Members = 6,
  [string]$ChannelName = 'scale',
  [switch]$Reuse
)

# GUI 验收（C1/C2）的服务端 + 频道准备：
#   1. 干净数据目录、复制 Windows 版服务端、起服务
#   2. bootstrap 管理员 + 造 Members-1 个邀请成员（都是真实账号，GUI 用真实 token 登录）
#   3. 用管理员 GUI 建一个频道（GUI 没有命令行建频道入口，只能 UIA 建）
# 输出 KEY=VALUE 供 WSL 侧解析：M0_TOKEN..M5_TOKEN / CHANNEL / ROOT / PID 等。
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Reuse) {
  Get-Process PartyUp.Client, partyup -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 500
  if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
}
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$logs = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$server = Join-Path $Root 'partyup.exe'
Copy-Item -Path $ServerExe -Destination $server -Force
$data = Join-Path $Root 'data'
if (-not (Test-Path $data)) { New-Item -ItemType Directory -Force -Path $data | Out-Null }

$outLog = Join-Path $Root 'server.log'
$errLog = Join-Path $Root 'server.err'

$serverProc = $null
$existing = Get-CimInstance Win32_Process -Filter "Name='partyup.exe'" |
  Where-Object { $_.CommandLine -like "*$Root*" } | Select-Object -First 1
if ($existing) {
  $serverProc = Get-Process -Id $existing.ProcessId
} else {
  Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $errLog
  $serverProc = Start-Process -FilePath $server `
    -ArgumentList @('serve', '--data', $data, '--http', $HttpAddr, '--log-level', 'info') `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
}

$token = ''
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 250
  if (Test-Path $outLog) {
    $tokenLine = (Get-Content $outLog) | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
    if ($tokenLine) { $token = $tokenLine -replace '^BOOTSTRAP_TOKEN=', ''; break }
  }
}
if (-not $token) { throw 'server did not print BOOTSTRAP_TOKEN' }

$admin = @{ token = $token; username = 'user0'; password = 'password-123' } | ConvertTo-Json
$tokens = New-Object System.Collections.Generic.List[string]
$tokens.Add((Invoke-RestMethod -Method Post -Uri "http://$HttpAddr/api/bootstrap" -ContentType 'application/json' -Body $admin).token)

for ($i = 1; $i -lt $Members; $i++) {
  $line = (& $server invite --data $data) | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1
  if (-not $line) { throw "invite CLI failed (member $i)" }
  $code = $line -replace '^INVITE_CODE=', ''
  $body = @{ username = "user$i"; password = 'password-123' } | ConvertTo-Json
  $resp = Invoke-RestMethod -Method Post -Uri "http://$HttpAddr/api/invite/$code" -ContentType 'application/json' -Body $body
  $tokens.Add($resp.token)
}

# 建频道：先起一个管理员 GUI，等窗口出来，再用 UIA 建频道，然后关掉它（稍后 Launcher 会重新拉起全部成员）。
$clientExe = Join-Path $ClientDir 'PartyUp.Client.exe'
if (-not (Test-Path $clientExe)) { throw "client exe not found: $clientExe" }

$alreadyExists = $false
if (Test-Path $outLog) {
  # 复用场景：频道可能已经建过（客户端启动时日志会记录；这里不查服务端，直接重跑 UIA 的幂等性由服务端决定）
}
$adminLog = Join-Path $logs 'bootstrap-admin.log'
$argList = @('--server', "http://$HttpAddr", '--token', $tokens[0])
$adminProc = Start-Process -FilePath $clientExe -ArgumentList $argList -NoNewWindow `
  -RedirectStandardOutput $adminLog -RedirectStandardError "$adminLog.err" -PassThru

$ready = $false
for ($i = 0; $i -lt 80; $i++) {
  Start-Sleep -Milliseconds 250
  if ($adminProc.HasExited) { break }
  $adminProc.Refresh()
  if ($adminProc.MainWindowHandle -ne 0) { $ready = $true; break }
}
if (-not $ready) { throw "admin GUI did not show a window (see $adminLog)" }
Start-Sleep -Milliseconds 1200

$created = & (Join-Path $here 'uia-create-channel.ps1') -ProcessId $adminProc.Id -ChannelName $ChannelName
"CHANNEL_CREATE=$created"
Start-Sleep -Milliseconds 800
$adminProc | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

"ROOT=$Root"
"CHANNEL=$ChannelName"
"SERVER_PID=$($serverProc.Id)"
"SERVER_LOG=$outLog"
"SERVER_ERR=$errLog"
"CLIENT_DIR=$ClientDir"
"LOGS=$logs"
for ($i = 0; $i -lt $tokens.Count; $i++) { "M${i}_TOKEN=$($tokens[$i])" }
