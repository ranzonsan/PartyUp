param(
  # 隔离 QA 实例 3：端口 18084 + %LOCALAPPDATA%\PartyUpQA3，绝不触碰 8080 / 18081 / 18082。
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA3",
  [int]$Port = 18084,
  [string]$ServerExe = "",                  # 默认 $Root\partyup-qa.exe
  [string]$SessionTTL = "720h",
  [string]$LoginRateLimit = "10/15m",
  [string]$DataSubdir = "data",             # 允许为 TTL 验收起独立数据目录（如 data-ttl）
  [switch]$Fresh,                           # 清空该数据目录（只删 $Root 下的子目录）
  # 监听地址：默认只绑回环；角色/bootstap 验收需要模拟「非回环连接」时传 0.0.0.0:18084。
  [string]$ListenAddr = "",
  # 只启动 + 打印 bootstrap token，不做任何账号初始化（bootstrap 安全边界验收用）。
  [switch]$NoBootstrap,
  [string]$AdminUser = 'alice',
  [string]$AdminPass = 'alice-pass-123',
  [string]$GuestUser = 'bob',
  [string]$GuestPass = 'bob-pass-1234'
)

# 认证链路（logout / TTL / 单端登录 / 限速）的隔离实例启动器。
#
# 边界（硬性）：
#   - 只操作 CommandLine 里带 $Root 的 partyup-qa.exe；不按进程名全杀。
#   - 监听 127.0.0.1:18084；STUN 用 127.0.0.1:34784，避免与 8080/18081/18082 实例抢 :3478。
#   - 数据目录 = $Root\<DataSubdir>，-Fresh 也只删这个子目录。
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$data = Join-Path $Root $DataSubdir
$exe = if ($ServerExe) { $ServerExe } else { Join-Path $Root 'partyup-qa.exe' }
if (-not (Test-Path $exe)) { throw "server exe not found: $exe（先 build 到 $Root\partyup-qa.exe）" }

# 停掉同 Root 的旧 18084 实例（运行中的 exe 会锁文件，必须在复制/启动前停）。
$existing = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
  Where-Object { $_.CommandLine -like "*$Root*" }
foreach ($proc in $existing) {
  Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
}

if ($Fresh -and (Test-Path $data)) { Remove-Item -Recurse -Force $data }
New-Item -ItemType Directory -Force -Path $data | Out-Null

$outLog = Join-Path $Root 'server.out'
$errLog = Join-Path $Root 'server.err'
Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $errLog

# 注意：Start-Process -ArgumentList 不做引号处理，所以 --name 用一个不含空格的词。
if (-not $ListenAddr) { $ListenAddr = "127.0.0.1:$Port" }
$serverArgs = @(
  'serve', '--data', $data, '--http', $ListenAddr,
  '--stun-listen', '127.0.0.1:34784',
  '--session-ttl', $SessionTTL,
  '--login-rate-limit', $LoginRateLimit,
  '--name', 'PartyUp-QA3'
)
$server = Start-Process -FilePath $exe -ArgumentList $serverArgs `
  -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru -WindowStyle Hidden
Set-Content -Path (Join-Path $Root 'server.pid') -Value $server.Id -Encoding ascii

$base = "http://127.0.0.1:$Port"
# 等 READY：stdout 是机器可读的 KEY=VALUE 块。
$ready = $false
$bootstrap = ''
$invite = ''
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep -Milliseconds 250
  if ($server.HasExited) { throw "QA3 server exited early; see $errLog" }
  if (Test-Path $outLog) {
    $lines = Get-Content $outLog -ErrorAction SilentlyContinue
    if ($lines -contains 'READY') { $ready = $true }
    $b = $lines | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
    if ($b) { $bootstrap = $b -replace '^BOOTSTRAP_TOKEN=', '' }
    $v = $lines | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1
    if ($v) { $invite = $v -replace '^INVITE_CODE=', '' }
    if ($ready) { break }
  }
}
if (-not $ready) { throw "QA3 server did not print READY; see $outLog / $errLog" }

# 健康检查
$healthy = $false
for ($i = 0; $i -lt 40; $i++) {
  try {
    $h = Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec 2
    if ($h.ok) { $healthy = $true; break }
  } catch { Start-Sleep -Milliseconds 250 }
}
if (-not $healthy) { throw "QA3 health check failed: $base/api/health" }

Write-Output "QA3_BOOTSTRAP_TOKEN=$bootstrap"
if ($NoBootstrap) {
  # bootstrap 安全边界验收：只启动实例并交出 token，由调用方从不同来源尝试 bootstrap。
  Write-Output "QA3_ROOT=$Root"
  Write-Output "QA3_BASE=$base"
  Write-Output "QA3_PID=$($server.Id)"
  Write-Output "QA3_DATA=$data"
  Write-Output "QA3_READY"
  return
}

# 首次数据：bootstrap 管理员；复用数据：固定密码登录。
$tokensPath = Join-Path $Root 'tokens.json'
$aliceToken = $null
$aliceId = $null
try {
  $body = @{ token = $bootstrap; username = $AdminUser; password = $AdminPass; display_name = '爱丽丝' } | ConvertTo-Json
  $admin = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' -Body $body
  $aliceToken = $admin.token
  $aliceId = $admin.member.id
} catch {
  $body = @{ username = $AdminUser; password = $AdminPass } | ConvertTo-Json
  $admin = Invoke-RestMethod -Method Post -Uri "$base/api/login" -ContentType 'application/json' -Body $body
  $aliceToken = $admin.token
  $aliceId = $admin.member.id
}

# bob：首次用邀请码注册；复用数据则登录。
$bobToken = $null
$bobId = $null
try {
  $inviteOut = & $exe invite --data $data --by $aliceId --server-url $base
  $inviteCode = ($inviteOut | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1) -replace '^INVITE_CODE=', ''
  if (-not $inviteCode) { throw "invite CLI did not print INVITE_CODE: $inviteOut" }
  $body = @{ username = $GuestUser; password = $GuestPass; display_name = '鲍勃' } | ConvertTo-Json
  $guest = Invoke-RestMethod -Method Post -Uri "$base/api/invite/$inviteCode" -ContentType 'application/json' -Body $body
  $bobToken = $guest.token
  $bobId = $guest.member.id
} catch {
  $body = @{ username = $GuestUser; password = $GuestPass } | ConvertTo-Json
  $guest = Invoke-RestMethod -Method Post -Uri "$base/api/login" -ContentType 'application/json' -Body $body
  $bobToken = $guest.token
  $bobId = $guest.member.id
}

$state = [ordered]@{
  root        = $Root
  port        = $Port
  base        = $base
  dataDir     = $data
  serverPid   = $server.Id
  sessionTtl  = $SessionTTL
  rateLimit   = $LoginRateLimit
  fresh       = [bool]$Fresh
  aliceId     = $aliceId
  aliceToken  = $aliceToken
  bobId       = $bobId
  bobToken    = $bobToken
  aliceName   = $AdminUser
  alicePass   = $AdminPass
  bobName     = $GuestUser
  bobPass     = $GuestPass
}
$state | ConvertTo-Json | Set-Content -Encoding UTF8 $tokensPath

Write-Output "QA3_ROOT=$Root"
Write-Output "QA3_BASE=$base"
Write-Output "QA3_PID=$($server.Id)"
Write-Output "QA3_DATA=$data"
Write-Output "QA3_SESSION_TTL=$SessionTTL"
Write-Output "QA3_LOGIN_RATE_LIMIT=$LoginRateLimit"
Write-Output "QA3_ALICE_TOKEN=$aliceToken"
Write-Output "QA3_ALICE_ID=$aliceId"
Write-Output "QA3_BOB_TOKEN=$bobToken"
Write-Output "QA3_BOB_ID=$bobId"
Write-Output "QA3_BOOTSTRAP_TOKEN=$bootstrap"
Write-Output "QA3_READY"
