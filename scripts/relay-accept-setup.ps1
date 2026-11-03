param(
  [Parameter(Mandatory = $true)][string]$ServerExe,
  [string]$Root = "$env:TEMP\pu-relay",
  [string]$HttpAddr = '127.0.0.1:18994',
  [string]$VoiceMode = 'relay'
)

# 中继语音验收的服务端准备：干净数据目录、复制 Windows 版服务端、起服务、建 3 个账号。
# 输出三行 KEY=VALUE 供 WSL 侧解析：ALICE_TOKEN / BOB_TOKEN / CAROL_TOKEN
$ErrorActionPreference = 'Stop'

Get-Process PartyUp.Client, partyup-sfu, partyup -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

New-Item -ItemType Directory -Force -Path $Root | Out-Null
$server = Join-Path $Root 'partyup.exe'
Copy-Item -Path $ServerExe -Destination $server -Force

$data = Join-Path $Root 'data'
if (Test-Path $data) { Remove-Item -Recurse -Force $data }
New-Item -ItemType Directory -Force -Path $data | Out-Null

$outLog = Join-Path $Root 'server.log'
$errLog = Join-Path $Root 'server.err'
Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $errLog

Start-Process -FilePath $server `
  -ArgumentList @('serve', '--data', $data, '--http', $HttpAddr, '--log-level', 'info', '--voice-mode', $VoiceMode) `
  -RedirectStandardOutput $outLog -RedirectStandardError $errLog | Out-Null

$token = ''
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 250
  if (Test-Path $outLog) {
    $lines = Get-Content $outLog
    $tokenLine = $lines | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
    if ($tokenLine) { $token = $tokenLine -replace '^BOOTSTRAP_TOKEN=', ''; break }
  }
}
if (-not $token) { throw 'server did not print BOOTSTRAP_TOKEN' }

$bootstrap = @{ token = $token; username = 'alice'; password = 'alice-password-123' } | ConvertTo-Json
$alice = Invoke-RestMethod -Method Post -Uri "http://$HttpAddr/api/bootstrap" -ContentType 'application/json' -Body $bootstrap

# 另外两个邀请码（CLI 直连同一个 SQLite；WAL 下可与 serve 并存）
$codes = New-Object System.Collections.Generic.List[string]
$inviteLine = (Get-Content $outLog) | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1
if ($inviteLine) { $codes.Add(($inviteLine -replace '^INVITE_CODE=', '')) }
for ($i = 0; $i -lt 2; $i++) {
  $line = (& $server invite --data $data) | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1
  if (-not $line) { throw "invite CLI failed (attempt $i)" }
  $codes.Add(($line -replace '^INVITE_CODE=', ''))
}

$bob = Invoke-RestMethod -Method Post -Uri "http://$HttpAddr/api/invite/$($codes[1])" `
  -ContentType 'application/json' -Body (@{ username = 'bob'; password = 'bob-password-123' } | ConvertTo-Json)
$carol = Invoke-RestMethod -Method Post -Uri "http://$HttpAddr/api/invite/$($codes[2])" `
  -ContentType 'application/json' -Body (@{ username = 'carol'; password = 'carol-password-123' } | ConvertTo-Json)

"ALICE_TOKEN=$($alice.token)"
"BOB_TOKEN=$($bob.token)"
"CAROL_TOKEN=$($carol.token)"
"VOICE_MODE=$VoiceMode"
"SERVER_LOG=$outLog"
