param(
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA",
  [int]$Port = 18081,
  [string]$ServerExe = "$env:LOCALAPPDATA\PartyUpDev\partyup-dev.exe",
  # 隔离实例用：与已在跑的服务端错开 STUN UDP 端口（缺省 :3478，与旧行为一致）。
  [string]$StunListen = '',
  [string]$AdminUser = 'alice',
  [string]$AdminPass = 'alice-pass-123',
  [string]$AdminDisplay = '爱丽丝',
  [string]$GuestUser = 'bob',
  [string]$GuestPass = 'bob-pass-1234',
  [string]$GuestDisplay = '鲍勃',
  [switch]$KeepData
)

# 真实 P2（语音/屏幕共享）验收用的 Windows 侧隔离服务端：
# 必须在 Windows 上跑——Windows 客户端 ↔ WSL 服务端的 UDP 媒体不通（既定环境事实）。
# 数据目录独立，端口独立，绝不触碰 %LOCALAPPDATA%\PartyUpDev 的 dev 数据。
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$data = Join-Path $Root 'data'
$exe = Join-Path $Root 'partyup-qa.exe'

if (-not (Test-Path $ServerExe)) { throw "server exe not found: $ServerExe" }

# 停掉同一 Root 旧实例（不能停 dev 8080 的进程）。
# 必须在 Copy-Item 之前：运行中的 partyup-qa.exe 会锁住目标文件，Windows 上无法覆盖
# （重跑同一 Root 时的实际踩坑）。
$existing = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
  Where-Object { $_.CommandLine -like "*$Root*" }
foreach ($proc in $existing) {
  Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 400
}
Copy-Item $ServerExe $exe -Force
if (-not $KeepData -and (Test-Path $data)) { Remove-Item -Recurse -Force $data }

$outLog = Join-Path $Root 'server.out'
$errLog = Join-Path $Root 'server.err'
Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $errLog

$serverArgs = @('serve', '--data', $data, '--http', "127.0.0.1:$Port")
# 同机开第二个隔离实例时默认的 :3478 会与已在跑的 dev/QA 服务端冲突（UDP 绑定独占），
# 用 -StunListen 指定独立地址即可；缺省保持原行为（:3478）。
# 注意：--name 的值含空格且未加引号，Go 的 flag 解析会在第一个非 flag 参数处停止，
# 所以 --stun-listen 必须排在 --name 之前。
if ($StunListen) { $serverArgs += @('--stun-listen', $StunListen) }
$serverArgs += @('--name', 'PartyUp QA')
$server = Start-Process -FilePath $exe `
  -ArgumentList $serverArgs `
  -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru -WindowStyle Hidden
Set-Content -Path (Join-Path $Root 'server.pid') -Value $server.Id

$base = "http://127.0.0.1:$Port"
$bootstrap = ''
for ($i = 0; $i -lt 80; $i++) {
  Start-Sleep -Milliseconds 250
  if (Test-Path $outLog) {
    $line = (Get-Content $outLog) | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
    if ($line) { $bootstrap = $line -replace '^BOOTSTRAP_TOKEN=', ''; break }
  }
  if ($server.HasExited) { throw "QA server exited early; see $errLog" }
}
if (-not $bootstrap -and -not $KeepData) { throw "QA server did not print BOOTSTRAP_TOKEN; see $outLog" }
# -KeepData 复用已 bootstrap 的旧数据时服务端不会打印 BOOTSTRAP_TOKEN：跳过等待，
# 下面 bootstrap 调用会失败并回落到固定密码登录。

# 等待 /api/health 就绪
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
  try {
    $h = Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec 2
    if ($h.ok) { $ready = $true; break }
  } catch { Start-Sleep -Milliseconds 250 }
}
if (-not $ready) { throw "QA server health check failed" }

$adminTokens = $null
try {
  $body = @{ token = $bootstrap; username = $AdminUser; password = $AdminPass; display_name = $AdminDisplay } | ConvertTo-Json
  $adminTokens = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' -Body $body
} catch {
  # 复用旧数据时 bootstrap 会失败：用固定密码登录。
  $body = @{ username = $AdminUser; password = $AdminPass } | ConvertTo-Json
  $adminTokens = Invoke-RestMethod -Method Post -Uri "$base/api/login" -ContentType 'application/json' -Body $body
}
$aliceToken = $adminTokens.token
$aliceId = $adminTokens.member.id

# 邀请码 + 注册第二个真实账号
$inviteOut = & $exe invite --data $data --by $aliceId --server-url $base
$inviteCode = ($inviteOut | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1) -replace '^INVITE_CODE=', ''
if (-not $inviteCode) { throw "invite CLI did not print INVITE_CODE: $inviteOut" }
$body = @{ username = $GuestUser; password = $GuestPass; display_name = $GuestDisplay } | ConvertTo-Json
try {
  $guest = Invoke-RestMethod -Method Post -Uri "$base/api/invite/$inviteCode" -ContentType 'application/json' -Body $body
} catch {
  # 复用旧数据（-KeepData）：guest 账号已存在，邀请注册会 409 → 直接用固定密码登录。
  $body = @{ username = $GuestUser; password = $GuestPass } | ConvertTo-Json
  $guest = Invoke-RestMethod -Method Post -Uri "$base/api/login" -ContentType 'application/json' -Body $body
}
$bobToken = $guest.token
$bobId = $guest.member.id

$state = [ordered]@{
  root      = $Root
  port      = $Port
  base      = $base
  dataDir   = $data
  serverPid = $server.Id
  aliceId   = $aliceId
  aliceName = $AdminUser
  aliceDisplay = $AdminDisplay
  aliceToken = $aliceToken
  bobId     = $bobId
  bobName   = $GuestUser
  bobDisplay = $GuestDisplay
  bobToken  = $bobToken
  inviteCode = $inviteCode
}
$state | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Root 'tokens.json')

Write-Output "QA_ROOT=$Root"
Write-Output "QA_BASE=$base"
Write-Output "QA_PID=$($server.Id)"
Write-Output "QA_DATA=$data"
Write-Output "QA_ALICE_ID=$aliceId"
Write-Output "QA_ALICE_TOKEN=$aliceToken"
Write-Output "QA_BOB_ID=$bobId"
Write-Output "QA_BOB_TOKEN=$bobToken"
Write-Output "QA_INVITE=$inviteCode"
Write-Output 'QA_READY'
exit 0
