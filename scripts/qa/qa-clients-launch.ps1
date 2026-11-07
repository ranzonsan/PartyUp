param(
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA",
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [string]$Channel = '主频道',
  [switch]$VoiceJoin
)

# 把两个真实账号的客户端留在桌面上，供人工自由测试（不自动入会，避免语音失败弹错）。
$state = Get-Content (Join-Path $Root 'tokens.json') -Raw | ConvertFrom-Json
$exe = Join-Path $ClientDir 'PartyUp.Client.exe'
if (-not (Test-Path $exe)) { throw "client exe not found: $exe" }

Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$logDir = Join-Path $Root 'handover-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Launch([string]$tag, [string]$token) {
  $log = Join-Path $logDir "$tag.log"
  Remove-Item -Force -ErrorAction SilentlyContinue $log, "$log.err"
  $argsList = @('--server', $state.base, '--token', $token, '--channel', $Channel)
  if ($VoiceJoin) { $argsList += '--voice-join' }
  $p = Start-Process -FilePath $exe -WorkingDirectory $ClientDir `
    -ArgumentList $argsList `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
  for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { break }
  }
  Start-Sleep -Milliseconds 2000
  return $p
}

$a = Launch 'alice' $state.aliceToken
$b = Launch 'bob' $state.bobToken
Write-Output "HANDOVER_ALICE_PID=$($a.Id)"
Write-Output "HANDOVER_BOB_PID=$($b.Id)"
Write-Output "HANDOVER_SERVER=$($state.base)"
Write-Output "HANDOVER_LOGIN_ALICE=$($state.aliceName)/$(($state.aliceToken).Substring(0,4))... (token 自动登录)"
Write-Output "HANDOVER_LOGIN_BOB=$($state.bobName)"
Write-Output "HANDOVER_LOGS=$logDir"
Write-Output 'HANDOVER_READY'
