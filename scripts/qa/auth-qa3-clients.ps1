param(
  # 认证链路真机验收的客户端：把 client-voicedll 整份复制到 client-auth 再运行；
  # 杀进程只杀路径含 client-auth 的 PartyUp.Client.exe（绝不碰用户其它实例）。
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA3",
  [string]$ClientSrc = "$env:LOCALAPPDATA\PartyUpQA\client-voicedll",
  [string]$ClientDir = "",
  [string]$Server = "http://127.0.0.1:18084",
  # Tag 决定日志名与窗口用途：alice / alice2 / bob …
  [Parameter(Mandatory = $true)][string]$Tag,
  # 要登录的账号（从 tokens.json 取 token）；默认 tag 里的 alice/bob 前缀。
  [string]$Account = "",
  # 显式指定 token（例如「第二次登录」拿到的新 token）；给出时忽略 Account。
  [string]$Token = "",
  [switch]$RestartAll,        # 先停掉所有 client-auth 客户端
  [switch]$Copy,              # 重新复制客户端目录（首次必须）
  [int]$WaitWindowMs = 8000
)

$ErrorActionPreference = 'Stop'
if (-not $ClientDir) { $ClientDir = Join-Path $Root 'client-auth' }
$logDir = Join-Path $ClientDir 'logs'
New-Item -ItemType Directory -Force -Path $Root, $ClientDir, $logDir | Out-Null

function Stop-AuthClients {
  $procs = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
    Where-Object { $_.ExecutablePath -like "*client-auth*" -or $_.CommandLine -like "*client-auth*" }
  foreach ($p in $procs) {
    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
  }
  if ($procs) { Start-Sleep -Milliseconds 600 }
}

if ($RestartAll) { Stop-AuthClients }

if ($Copy -or -not (Test-Path (Join-Path $ClientDir 'PartyUp.Client.exe'))) {
  if (-not (Test-Path (Join-Path $ClientSrc 'PartyUp.Client.exe'))) {
    throw "client source not found: $ClientSrc"
  }
  # robocopy 退出码 0-7 都是成功。用 /E 而不是 /MIR：/MIR 会把刚建好的 logs 目录删掉，
  # 随后 Start-Process 重定向到不存在的目录会直接失败。
  & robocopy $ClientSrc $ClientDir /E /NFL /NDL /NJH /NJS | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy client failed: exit=$LASTEXITCODE" }
}
# 复制之后再确保日志目录存在（首次 / 重新复制 / 目录被清都能工作）。
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$state = Get-Content (Join-Path $Root 'tokens.json') -Raw | ConvertFrom-Json
if (-not $Account) { $Account = if ($Tag -like 'bob*') { 'bob' } else { 'alice' } }
if (-not $Token) {
  $Token = switch ($Account) {
    'alice' { $state.aliceToken }
    'bob'   { $state.bobToken }
    default { throw "unknown account: $Account" }
  }
}
if (-not $Token) { throw "no token for $Account in tokens.json" }

$exe = Join-Path $ClientDir 'PartyUp.Client.exe'
$tagLog = Join-Path $logDir "$Tag.log"
$tagErr = Join-Path $logDir "$Tag.err"
Remove-Item -Force -ErrorAction SilentlyContinue $tagLog, $tagErr

# 不要传 --channel：未显式指定时客户端登录后进第一个频道，避免默认 "interop"
# 不存在时的报错噪音。
$clientArgs = @('--server', $Server, '--token', $Token)
$p = Start-Process -FilePath $exe -WorkingDirectory $ClientDir `
  -ArgumentList $clientArgs `
  -RedirectStandardOutput $tagLog -RedirectStandardError $tagErr -PassThru
$deadline = (Get-Date).AddMilliseconds($WaitWindowMs)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 250
  $p.Refresh()
  if ($p.HasExited) { break }
  if ($p.MainWindowHandle -ne 0) { break }
}
Start-Sleep -Milliseconds 1500
Set-Content -Path (Join-Path $logDir "$Tag.pid") -Value $p.Id -Encoding ascii

Write-Output "CLIENT_TAG=$Tag"
Write-Output "CLIENT_ACCOUNT=$Account"
Write-Output "CLIENT_PID=$($p.Id)"
Write-Output "CLIENT_LOG=$tagLog"
Write-Output "CLIENT_READY"
