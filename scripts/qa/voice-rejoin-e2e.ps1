param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'),
  [int]$Port = 18086,
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$ClientExeA = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client-a\PartyUp.Client.exe'),
  [string]$ClientExeB = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client-b\PartyUp.Client.exe'),
  [string]$EvidenceDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\evidence'),
  [double]$ToneSeconds = 2.5,
  [double]$ToneAmplitude = 0.12,
  [switch]$KeepClients
)

# Real-media E2E for the "join voice -> leave -> rejoin voice" risk:
#   A and B are two independent GUI client processes (own ADMs, own media core).
#   A joins, B joins, then A leaves+rejoins while B stays, then B leaves+rejoins.
# Deterministic audio probe: play a short tone through the default render endpoint;
# both mics hear it, so both receive-side PCM counters must grow during playback.
# Judge: B's rx counter growth after A's rejoin (A->B direction alive?) and A's rx
# growth after B's rejoin (B->A alive?), compared with the first-session control.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$dataDir = Join-Path $Root 'data'
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$base = "http://127.0.0.1:$Port"
$logA = Join-Path $EvidenceDir 'client-a.log'
$logB = Join-Path $EvidenceDir 'client-b.log'
$errA = Join-Path $EvidenceDir 'client-a.err.log'
$errB = Join-Path $EvidenceDir 'client-b.err.log'
$nativeA = Join-Path $EvidenceDir 'a-native.log'
$nativeB = Join-Path $EvidenceDir 'b-native.log'
$wavPath = Join-Path $EvidenceDir 'probe-tone.wav'
$checks = [ordered]@{}
function Record([string]$name, [string]$value) { $checks[$name] = $value; Write-Output "$name=$value" }
function Log([string]$m) { [Console]::WriteLine("[" + (Get-Date -Format 'HH:mm:ss.fff') + "] " + $m) }

function New-ToneWav([string]$path, [double]$seconds, [double]$freq, [double]$amp) {
  $rate = 48000
  $n = [int]($rate * $seconds)
  $fs = [System.IO.File]::Create($path)
  $bw = New-Object System.IO.BinaryWriter($fs)
  $ascii = [System.Text.Encoding]::ASCII
  $dataLen = $n * 2
  $bw.Write($ascii.GetBytes('RIFF'))
  $bw.Write([int](36 + $dataLen))
  $bw.Write($ascii.GetBytes('WAVEfmt '))
  $bw.Write([int]16)
  $bw.Write([int16]1)
  $bw.Write([int16]1)
  $bw.Write([int]$rate)
  $bw.Write([int]($rate * 2))
  $bw.Write([int16]2)
  $bw.Write([int16]16)
  $bw.Write($ascii.GetBytes('data'))
  $bw.Write([int]$dataLen)
  for ($i = 0; $i -lt $n; $i++) {
    $v = [int16]([math]::Sin(2 * [math]::PI * $freq * $i / $rate) * 32767 * $amp)
    $bw.Write($v)
  }
  $bw.Flush()
  $bw.Close()
  $fs.Close()
}
function Play-Tone {
  $player = New-Object System.Media.SoundPlayer($wavPath)
  $player.Play()
  Start-Sleep -Seconds ([int]([math]::Ceiling($ToneSeconds)) + 1)
}

function Get-Window([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  return [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
}
function Find-ById($window, [string]$id) {
  if (-not $window) { return $null }
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  } catch { return $null }
}
function Invoke-ById([int]$procId, [string]$id) {
  try {
    $w = Get-Window $procId
    $t = Find-ById $w $id
    if (-not $t) { return "NOT_FOUND:$id" }
    $pattern = $null
    if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
      $pattern.Invoke()
      return "CLICKED:$id"
    }
    return "NO_INVOKE:$id"
  } catch { return "INVOKE_FAILED:$id" }
}
function Wait-ById([int]$procId, [string]$id, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Find-ById (Get-Window $procId) $id) { return $true }
    Start-Sleep -Milliseconds 400
  }
  return $false
}
function Wait-ForWindow([int]$procId, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    $w = Get-Window $procId
    if ($w) { return $w }
    Start-Sleep -Milliseconds 400
  }
  return $null
}
function Count-Log([string]$path, [string]$needle) {
  if (-not (Test-Path $path)) { return 0 }
  return @(Select-String -Path $path -SimpleMatch $needle -ErrorAction SilentlyContinue).Count
}
function Wait-ForLog([string]$path, [string]$needle, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ((Count-Log $path $needle) -gt 0) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return $false
}
function Get-LastPcmRx([string]$path) {
  if (-not (Test-Path $path)) { return -1 }
  $m = Select-String -Path $path -Pattern 'voice: stats pcmRx=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
  if ($m) { return [int]$m.Matches[0].Groups[1].Value }
  return -1
}
function Get-PcmRxLine([string]$path) {
  if (-not (Test-Path $path)) { return '' }
  $m = Select-String -Path $path -Pattern 'voice: stats pcmRx=' -ErrorAction SilentlyContinue | Select-Object -Last 1
  if ($m) { return $m.Line.Trim() }
  return ''
}
function Get-TrackLines([string]$path, [int]$take) {
  if (-not (Test-Path $path)) { return @() }
  return @(Select-String -Path $path -Pattern 'voice: track id=' -ErrorAction SilentlyContinue |
           Select-Object -Last $take | ForEach-Object { $_.Line.Trim() })
}
function Start-App([string]$exe, [string]$dir, [string[]]$argsList, [string]$outLog, [string]$errLog, [string]$nativeLog) {
  Remove-Item -Force -ErrorAction SilentlyContinue $outLog, $errLog, $nativeLog
  $env:PARTYUP_MEDIA_LOG = $nativeLog
  $env:PARTYUP_VOICE_PEAK_THRESHOLD = '4'
  return Start-Process -FilePath $exe -WorkingDirectory $dir -ArgumentList $argsList `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
}
function Stop-App($proc) {
  if (-not $proc) { return }
  try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
}
function Kill-Previous {
  $procs = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
    Where-Object { $_.CommandLine -like "*$Root*" }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  $clients = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
    Where-Object { $_.ExecutablePath -like "$Root*" }
  foreach ($c in $clients) { Stop-Process -Id $c.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
}

$appA = $null
$appB = $null
$server = $null
try {
  Kill-Previous
  New-ToneWav $wavPath $ToneSeconds 440 $ToneAmplitude
  Record 'TONE_WAV' $(if (Test-Path $wavPath) { ([int](Get-Item $wavPath).Length).ToString() } else { 'MISSING' })
  if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $serverOut = Join-Path $EvidenceDir 'server.out'
  $serverErr = Join-Path $EvidenceDir 'server.err'
  Remove-Item -Force -ErrorAction SilentlyContinue $serverOut, $serverErr
  $server = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port",
      '--stun-listen', '127.0.0.1:34786', '--name', 'PartyUp-rejoin'
    ) -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru -WindowStyle Hidden
  Record 'SERVER_PID' ([string]$server.Id)
  $ready = $false
  $bootstrap = ''
  for ($i = 0; $i -lt 160; $i++) {
    Start-Sleep -Milliseconds 250
    if ($server.HasExited) { throw "server exited early; see $serverErr" }
    if (Test-Path $serverOut) {
      $lines = Get-Content $serverOut -ErrorAction SilentlyContinue
      if ($lines -contains 'READY') { $ready = $true }
      $b = $lines | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
      if ($b) { $bootstrap = $b -replace '^BOOTSTRAP_TOKEN=', '' }
      if ($ready) { break }
    }
  }
  if (-not $ready) { throw "server did not print READY" }
  Record 'SERVER_READY' 'PASS'

  $adminBody = @{ token = $bootstrap; username = 'alice'; password = 'alice-pass-123'; display_name = 'Alice' } | ConvertTo-Json
  $admin = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' -Body $adminBody
  $aliceToken = $admin.token
  $aliceId = $admin.member.id
  Record 'BOOTSTRAP' 'PASS'

  $chBody = @{ name = 'voice' } | ConvertTo-Json
  $ch = Invoke-RestMethod -Method Post -Uri "$base/api/admin/channels" -ContentType 'application/json' `
        -Headers @{ Authorization = "Bearer $aliceToken" } -Body $chBody
  Record 'CHANNEL_CREATED' ([string]$ch.id)

  $inviteOut = & $ServerExe invite --data $dataDir --by $aliceId --server-url $base
  $inviteCode = ($inviteOut | Where-Object { $_ -like 'INVITE_CODE=*' } | Select-Object -First 1) -replace '^INVITE_CODE=', ''
  if (-not $inviteCode) { throw "no invite code: $inviteOut" }
  $bobBody = @{ username = 'bob'; password = 'bob-pass-1234'; display_name = 'Bob' } | ConvertTo-Json
  $bob = Invoke-RestMethod -Method Post -Uri "$base/api/invite/$inviteCode" -ContentType 'application/json' -Body $bobBody
  $bobToken = $bob.token
  Record 'REGISTER_BOB' 'PASS'

  $argsA = @('--server', $base, '--token', $aliceToken, '--channel', 'voice', '--voice-join', '--screen', '2')
  $appA = Start-App $ClientExeA (Split-Path $ClientExeA) $argsA $logA $errA $nativeA
  Record 'PID_A' ([string]$appA.Id)
  if (-not (Wait-ForWindow $appA.Id 120)) { throw 'client A window did not appear' }
  Record 'WINDOW_A' 'PASS'
  Record 'LOGIN_A' $(if (Wait-ForLog $logA 'gui: connected as' 90) { 'PASS' } else { 'FAIL' })
  Record 'VOICE_JOIN_A_1' $(if (Wait-ForLog $logA 'voice: joined channel=' 90) { 'PASS' } else { 'FAIL' })

  $argsB = @('--server', $base, '--token', $bobToken, '--channel', 'voice', '--voice-join', '--screen', '2')
  $appB = Start-App $ClientExeB (Split-Path $ClientExeB) $argsB $logB $errB $nativeB
  Record 'PID_B' ([string]$appB.Id)
  if (-not (Wait-ForWindow $appB.Id 120)) { throw 'client B window did not appear' }
  Record 'WINDOW_B' 'PASS'
  Record 'LOGIN_B' $(if (Wait-ForLog $logB 'gui: connected as' 90) { 'PASS' } else { 'FAIL' })
  Record 'VOICE_JOIN_B_1' $(if (Wait-ForLog $logB 'voice: joined channel=' 90) { 'PASS' } else { 'FAIL' })

  # ---- Control: both in first session, play tone, both directions must grow ----
  Start-Sleep -Seconds 4
  $a0 = Get-LastPcmRx $logA
  $b0 = Get-LastPcmRx $logB
  Record 'CTL_A_PCMRX_BEFORE' ([string]$a0)
  Record 'CTL_B_PCMRX_BEFORE' ([string]$b0)
  Play-Tone
  Start-Sleep -Seconds 2
  $a1 = Get-LastPcmRx $logA
  $b1 = Get-LastPcmRx $logB
  Record 'CTL_A_PCMRX_AFTER' ([string]$a1)
  Record 'CTL_B_PCMRX_AFTER' ([string]$b1)
  Record 'CTL_A_RX_GROWTH' ([string]($a1 - $a0))
  Record 'CTL_B_RX_GROWTH' ([string]($b1 - $b0))
  Record 'CTL_A_STATS_LINE' (Get-PcmRxLine $logA)
  Record 'CTL_B_STATS_LINE' (Get-PcmRxLine $logB)
  Record 'CTL_B_TRACK_LINES' ((Get-TrackLines $logB 3) -join ' || ')

  # ---- A leaves and rejoins ----
  Record 'A_LEAVE_CLICK' (Invoke-ById $appA.Id 'Leave')
  Record 'A_LEFT' $(if (Wait-ForLog $logA 'voice: left' 40) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 2
  $joinedBefore = Count-Log $logA 'voice: joined channel='
  Record 'A_JOIN_BUTTON_VISIBLE' $(if (Wait-ById $appA.Id 'JoinCall' 30) { 'PASS' } else { 'FAIL' })
  Record 'A_REJOIN_CLICK' (Invoke-ById $appA.Id 'JoinCall')
  $rejoined = $false
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    if ((Count-Log $logA 'voice: joined channel=') -gt $joinedBefore) { $rejoined = $true; break }
    Start-Sleep -Milliseconds 300
  }
  Record 'VOICE_JOIN_A_2' $(if ($rejoined) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 4
  $a2 = Get-LastPcmRx $logA
  $b2 = Get-LastPcmRx $logB
  Record 'S2_A_PCMRX_BEFORE' ([string]$a2)
  Record 'S2_B_PCMRX_BEFORE' ([string]$b2)
  Play-Tone
  Start-Sleep -Seconds 2
  $a2b = Get-LastPcmRx $logA
  $b2b = Get-LastPcmRx $logB
  Record 'S2_A_PCMRX_AFTER' ([string]$a2b)
  Record 'S2_B_PCMRX_AFTER' ([string]$b2b)
  Record 'S2_A_RX_GROWTH' ([string]($a2b - $a2))
  Record 'S2_B_RX_GROWTH' ([string]($b2b - $b2))
  Record 'S2_A_STATS_LINE' (Get-PcmRxLine $logA)
  Record 'S2_B_STATS_LINE' (Get-PcmRxLine $logB)
  Record 'S2_B_TRACK_LINES' ((Get-TrackLines $logB 3) -join ' || ')

  # ---- B leaves and rejoins ----
  Record 'B_LEAVE_CLICK' (Invoke-ById $appB.Id 'Leave')
  Record 'B_LEFT' $(if (Wait-ForLog $logB 'voice: left' 40) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 2
  $bJoinedBefore = Count-Log $logB 'voice: joined channel='
  Record 'B_JOIN_BUTTON_VISIBLE' $(if (Wait-ById $appB.Id 'JoinCall' 30) { 'PASS' } else { 'FAIL' })
  Record 'B_REJOIN_CLICK' (Invoke-ById $appB.Id 'JoinCall')
  $bRejoined = $false
  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    if ((Count-Log $logB 'voice: joined channel=') -gt $bJoinedBefore) { $bRejoined = $true; break }
    Start-Sleep -Milliseconds 300
  }
  Record 'VOICE_JOIN_B_2' $(if ($bRejoined) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 4
  $a3 = Get-LastPcmRx $logA
  $b3 = Get-LastPcmRx $logB
  Record 'S3_A_PCMRX_BEFORE' ([string]$a3)
  Record 'S3_B_PCMRX_BEFORE' ([string]$b3)
  Play-Tone
  Start-Sleep -Seconds 2
  $a3b = Get-LastPcmRx $logA
  $b3b = Get-LastPcmRx $logB
  Record 'S3_A_PCMRX_AFTER' ([string]$a3b)
  Record 'S3_B_PCMRX_AFTER' ([string]$b3b)
  Record 'S3_A_RX_GROWTH' ([string]($a3b - $a3))
  Record 'S3_B_RX_GROWTH' ([string]($b3b - $b3))
  Record 'S3_A_STATS_LINE' (Get-PcmRxLine $logA)
  Record 'S3_B_STATS_LINE' (Get-PcmRxLine $logB)
  Record 'S3_A_TRACK_LINES' ((Get-TrackLines $logA 3) -join ' || ')

  Record 'EVIDENCE_DIR' $EvidenceDir
} catch {
  Record 'EXCEPTION' ($_.Exception.Message + ' @ ' + ($_.InvocationInfo.PositionMessage -replace "`r?`n", ' '))
} finally {
  $checks['capturedAt'] = (Get-Date).ToString('o')
  $checks['base'] = $base
  $checks['dataDir'] = $dataDir
  $checks | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'voice-rejoin-evidence.json')
  if (-not $KeepClients) {
    Stop-App $appA
    Stop-App $appB
  }
  if ($server -and -not $KeepClients) {
    try { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } catch { }
  }
  Log 'E2E_DONE'
}
