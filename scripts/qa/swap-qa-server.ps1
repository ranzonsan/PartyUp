# 把 18081 的 QA 服务端换成修复后的二进制，并重新拉起两个客户端。
# 只动 PartyUpQA（18081）与 client-voicedll；不碰 PartyUpQA2（18082）与 8080 dev。
param(
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA",
  [string]$FixedExe = "$env:LOCALAPPDATA\PartyUpQA2\partyup-qa.exe",
  [string]$ClientDir = "$env:LOCALAPPDATA\PartyUpQA\client-voicedll",
  [int]$Port = 18081
)

$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Output ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

# 1) 停客户端（只停这两个测试目录里的）
Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "*PartyUpQA*" } |
  ForEach-Object { Log "stop client pid=$($_.Id) path=$($_.Path)"; Stop-Process -Id $_.Id -Force }
Start-Sleep -Milliseconds 600

# 2) 停旧服务端（只停数据目录在 $Root 下的那个）
Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
  Where-Object { $_.CommandLine -like "*$Root*" -and $_.CommandLine -notlike '*PartyUpQA2*' } |
  ForEach-Object { Log "stop server pid=$($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force }
Start-Sleep -Milliseconds 800

# 3) 换二进制
Copy-Item $FixedExe (Join-Path $Root 'partyup-qa.exe') -Force
$item = Get-Item (Join-Path $Root 'partyup-qa.exe')
Log ("binary swapped: {0} bytes, {1:HH:mm:ss}" -f $item.Length, $item.LastWriteTime)

# 4) 起新服务端（参数与旧实例一致）
$exe = Join-Path $Root 'partyup-qa.exe'
$out = Join-Path $Root 'server.out'
$err = Join-Path $Root 'server.err'
Remove-Item -Force -ErrorAction SilentlyContinue $out, $err
$p = Start-Process -FilePath $exe `
  -ArgumentList @('serve', '--data', (Join-Path $Root 'data'), '--http', "127.0.0.1:$Port", '--name', 'PartyUp QA') `
  -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
Log "server started pid=$($p.Id)"
$p.Id | Set-Content (Join-Path $Root 'server.pid')

# 5) 等端口可用
$ok = $false
for ($i = 0; $i -lt 40; $i++) {
  Start-Sleep -Milliseconds 500
  try {
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -UseBasicParsing -TimeoutSec 2
    if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 500) { $ok = $true; break }
  } catch { if ($_.Exception.Response) { $ok = $true; break } }
}
Log "server reachable=$ok"

# 6) 起两个客户端（自动登录 + 入会主频道），日志落到 handover-logs
$logDir = Join-Path $Root 'handover-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$aOut = Join-Path $logDir 'alice.log'
$aErr = Join-Path $logDir 'alice.log.err'
$bOut = Join-Path $logDir 'bob.log'
$bErr = Join-Path $logDir 'bob.log.err'
Remove-Item -Force -ErrorAction SilentlyContinue $aOut, $aErr, $bOut, $bErr
$alice = Start-Process -FilePath (Join-Path $ClientDir 'PartyUp.Client.exe') -WorkingDirectory $ClientDir `
  -ArgumentList @('--server', "http://127.0.0.1:$Port", '--token', 'KySd0Dn7i1aWYM6ZW7Yu_2yOKtUWa9Y4I0Vj4Gp9CoE', '--channel', '主频道', '--voice-join') `
  -RedirectStandardOutput $aOut -RedirectStandardError $aErr -PassThru
Start-Sleep -Milliseconds 900
$bob = Start-Process -FilePath (Join-Path $ClientDir 'PartyUp.Client.exe') -WorkingDirectory $ClientDir `
  -ArgumentList @('--server', "http://127.0.0.1:$Port", '--token', '-9yuhWSB86ye3sloprLyjbkrGvWIbINk9tmoOIj-K-I', '--channel', '主频道', '--voice-join') `
  -RedirectStandardOutput $bOut -RedirectStandardError $bErr -PassThru
Log "clients started alice=$($alice.Id) bob=$($bob.Id)"

# 7) 等入会：轮询客户端日志里的 "voice: joined"
$deadline = (Get-Date).AddSeconds(40)
$joined = @{ alice = $false; bob = $false }
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 700
  if (-not $joined.alice -and (Test-Path $aOut) -and (Select-String -Path $aOut -Pattern 'voice: joined' -Quiet -ErrorAction SilentlyContinue)) { $joined.alice = $true }
  if (-not $joined.bob -and (Test-Path $bOut) -and (Select-String -Path $bOut -Pattern 'voice: joined' -Quiet -ErrorAction SilentlyContinue)) { $joined.bob = $true }
  if ($joined.alice -and $joined.bob) { break }
}
Log ("VOICE_JOINED_ALICE=" + $joined.alice)
Log ("VOICE_JOINED_BOB=" + $joined.bob)
Log "clients alive=$((Get-Process -Id $alice.Id, $bob.Id -ErrorAction SilentlyContinue | Measure-Object).Count)"
Log ("ALICE_PID=" + $alice.Id)
Log ("BOB_PID=" + $bob.Id)
Log ("SERVER_PID=" + $p.Id)
