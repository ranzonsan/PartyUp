param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'),
  [int]$Port = 18086,
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$EvidenceDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\evidence'),
  [int]$HoldSeconds = 6,
  [int]$SecondaryX = -2560,
  [int]$SecondaryY = 0,
  [switch]$KeepClients
)

# ADM lifecycle control (content-independent): drive ONE product client and, between
# voice sessions, start the settings "mic test" (pu_audio_monitor_start, which shares the
# factory-level platform ADM with the voice engine). Native log records:
#   audio monitor: engine start rc=<rc>
#   audio monitor: engine start after ADM rebuild rc=<rc>   (only when 1st start failed)
# Judge = rc values + rebuild occurrence + UI level samples (0..100, from frames/peak).
# Windows are forced onto the secondary screen; console output goes to a file.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinRect {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@

$dataDir = Join-Path $Root 'data'
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$base = "http://127.0.0.1:$Port"
$logPath = Join-Path $EvidenceDir 'admctl-client.log'
$errPath = Join-Path $EvidenceDir 'admctl-client.err.log'
$nativePath = Join-Path $EvidenceDir 'admctl-native.log'
$checks = [ordered]@{}
function Record([string]$n, [string]$v) { $checks[$n] = $v; [Console]::WriteLine("$n=$v") }
function Log([string]$m) { [Console]::WriteLine("[" + (Get-Date -Format 'HH:mm:ss.fff') + "] " + $m) }

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
    $t = Find-ById (Get-Window $procId) $id
    if (-not $t) { return "NOT_FOUND:$id" }
    $pattern = $null
    if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
      $pattern.Invoke(); return "CLICKED:$id"
    }
    return "NO_INVOKE:$id"
  } catch { return "FAILED:$id" }
}
function Wait-ById([int]$procId, [string]$id, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Find-ById (Get-Window $procId) $id) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return $false
}
function Wait-ForWindow([int]$procId, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    $w = Get-Window $procId
    if ($w) { return $w }
    Start-Sleep -Milliseconds 300
  }
  return $null
}
function Count-Log([string]$p, [string]$needle) {
  if (-not (Test-Path $p)) { return 0 }
  return @(Select-String -Path $p -SimpleMatch $needle -ErrorAction SilentlyContinue).Count
}
function Wait-ForLog([string]$p, [string]$needle, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ((Count-Log $p $needle) -gt 0) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return $false
}
function Get-LineCount([string]$p) {
  if (-not (Test-Path $p)) { return 0 }
  return @(Get-Content -Path $p -ErrorAction SilentlyContinue).Count
}
function Get-NewNativeLines([string]$p, [int]$from, [string]$pattern) {
  if (-not (Test-Path $p)) { return @() }
  $all = @(Get-Content -Path $p -ErrorAction SilentlyContinue)
  if ($from -ge $all.Count) { return @() }
  return @($all[$from..($all.Count - 1)] | Where-Object { $_ -match $pattern })
}
function Get-LevelValue([int]$procId) {
  try {
    $t = Find-ById (Get-Window $procId) 'MicTestLevel'
    if (-not $t) { return -1 }
    $pattern = $null
    if ($t.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$pattern)) {
      return [int]$pattern.Current.Value
    }
    return -2
  } catch { return -3 }
}
function Get-RectText([IntPtr]$hwnd) {
  $r = New-Object WinRect+RECT
  [WinRect]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  return ("left=" + $r.Left + " top=" + $r.Top + " right=" + $r.Right + " bottom=" + $r.Bottom)
}
function Place-OnSecondary([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return 'NO_WINDOW' }
  $r = New-Object WinRect+RECT
  [WinRect]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left
  $h = $r.Bottom - $r.Top
  if ($w -le 0 -or $h -le 0) { return 'BAD_RECT' }
  $SWP_NOZORDER = 0x0004
  $SWP_NOACTIVATE = 0x0010
  [WinRect]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $SecondaryX, $SecondaryY, $w, $h,
    $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null
  Start-Sleep -Milliseconds 400
  return (Get-RectText $p.MainWindowHandle)
}

function Open-Settings([int]$procId) {
  Record 'SETTINGS_USERCARD' (Invoke-ById $procId 'UserCard')
  $found = $false
  for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Milliseconds 300
    if (Find-ById (Get-Window $procId) 'OpenSettings') { $found = $true; break }
  }
  if (-not $found) {
    Record 'SETTINGS_OPEN' 'MENU_ITEM_NOT_FOUND'
    return $false
  }
  Record 'SETTINGS_MENUITEM' (Invoke-ById $procId 'OpenSettings')
  $ok = Wait-ById $procId 'MicTestStart' 15
  Record 'SETTINGS_PANE' $(if ($ok) { 'PASS' } else { 'FAIL' })
  return $ok
}
function Close-Settings([int]$procId) {
  Invoke-ById $procId 'CloseSettings' | Out-Null
  Start-Sleep -Milliseconds 600
}
function Run-MonitorTest([int]$procId, [string]$tag) {
  $nativeBefore = Get-LineCount $nativePath
  $logBefore = Get-LineCount $logPath
  if (-not (Open-Settings $procId)) { return }
  Record ("M_" + $tag + "_START_CLICK") (Invoke-ById $procId 'MicTestStart')
  Start-Sleep -Milliseconds 500
  $levels = @()
  for ($i = 0; $i -lt 8; $i++) {
    Start-Sleep -Milliseconds 500
    $levels += (Get-LevelValue $procId)
  }
  Record ("M_" + $tag + "_LEVELS") ($levels -join ',')
  Record ("M_" + $tag + "_LEVEL_MAX") ([string](($levels | Measure-Object -Maximum).Maximum))
  Invoke-ById $procId 'MicTestStop' | Out-Null
  Start-Sleep -Milliseconds 400
  $monLines = Get-NewNativeLines $nativePath $nativeBefore 'audio monitor:|ADM (InitRecording|StartRecording|initialized)|could not initialize|ADM rebuild|ADM instances live'
  Record ("M_" + $tag + "_NATIVE") (($monLines | ForEach-Object { $_.Trim() }) -join ' || ')
  $clientLines = Get-NewNativeLines $logPath $logBefore 'audio: monitor'
  Record ("M_" + $tag + "_CLIENT_LOG") (($clientLines | ForEach-Object { $_.Trim() }) -join ' || ')
  Close-Settings $procId
}
function Kill-Previous {
  $procs = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
    Where-Object { $_.CommandLine -like "*$Root*" }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  $clients = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
    Where-Object { ($_.ExecutablePath -like "$Root*") -or ($ClientExe -and $_.ExecutablePath -ieq $ClientExe) }
  foreach ($c in $clients) { Stop-Process -Id $c.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
}

$app = $null
$server = $null
try {
  Kill-Previous
  if (-not (Test-Path $ClientExe)) { throw ("client exe missing: " + $ClientExe) }
  if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $serverOut = Join-Path $EvidenceDir 'admctl-server.out'
  Remove-Item -Force -ErrorAction SilentlyContinue $serverOut
  $server = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port",
      '--stun-listen', '127.0.0.1:34786', '--name', 'PartyUp-admctl'
    ) -RedirectStandardOutput $serverOut -RedirectStandardError (Join-Path $EvidenceDir 'admctl-server.err') `
    -PassThru -WindowStyle Hidden
  $ready = $false; $bootstrap = ''
  for ($i = 0; $i -lt 160; $i++) {
    Start-Sleep -Milliseconds 250
    if ($server.HasExited) { throw 'server exited early' }
    if (Test-Path $serverOut) {
      $lines = Get-Content $serverOut -ErrorAction SilentlyContinue
      if ($lines -contains 'READY') { $ready = $true }
      $b = $lines | Where-Object { $_ -like 'BOOTSTRAP_TOKEN=*' } | Select-Object -First 1
      if ($b) { $bootstrap = $b -replace '^BOOTSTRAP_TOKEN=', '' }
      if ($ready) { break }
    }
  }
  if (-not $ready) { throw 'server not ready' }
  $admin = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' `
    -Body (@{ token = $bootstrap; username = 'alice'; password = 'alice-pass-123'; display_name = 'Alice' } | ConvertTo-Json)
  $aliceToken = $admin.token
  Invoke-RestMethod -Method Post -Uri "$base/api/admin/channels" -ContentType 'application/json' `
    -Headers @{ Authorization = "Bearer $aliceToken" } -Body (@{ name = 'voice' } | ConvertTo-Json) | Out-Null
  Record 'SERVER_READY' 'PASS'

  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, $errPath, $nativePath
  $env:PARTYUP_MEDIA_LOG = $nativePath
  $env:PARTYUP_VOICE_PEAK_THRESHOLD = '4'
  $app = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', $base, '--token', $aliceToken, '--channel', 'voice', '--screen', '2') `
    -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru
  Record 'PID' ([string]$app.Id)
  if (-not (Wait-ForWindow $app.Id 120)) { throw 'window did not appear' }
  Record 'RECT_BEFORE_PLACE' (Get-RectText $app.MainWindowHandle)
  Record 'RECT_PLACED' (Place-OnSecondary $app.Id)
  Record 'LOGIN' $(if (Wait-ForLog $logPath 'gui: connected as' 90) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 1
  Record 'RECT_AFTER_LOGIN' (Get-RectText $app.MainWindowHandle)

  Run-MonitorTest $app.Id 'C0_PREVOICE'
  for ($r = 1; $r -le 3; $r++) {
    $joinedBefore = Count-Log $logPath 'voice: joined channel='
    Record ("S" + $r + "_JOIN_CLICK") (Invoke-ById $app.Id 'JoinCall')
    $deadline = (Get-Date).AddSeconds(90)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
      if ((Count-Log $logPath 'voice: joined channel=') -gt $joinedBefore) { $ok = $true; break }
      Start-Sleep -Milliseconds 300
    }
    Record ("S" + $r + "_JOINED") $(if ($ok) { 'PASS' } else { 'FAIL' })
    Start-Sleep -Seconds $HoldSeconds
    Record ("S" + $r + "_LEAVE_CLICK") (Invoke-ById $app.Id 'Leave')
    Record ("S" + $r + "_LEFT") $(if (Wait-ForLog $logPath 'voice: left' 40) { 'PASS' } else { 'FAIL' })
    Start-Sleep -Seconds 2
    Run-MonitorTest $app.Id ('C' + $r + '_POSTVOICE')
  }
  Record 'EVIDENCE_DIR' $EvidenceDir
} catch {
  Record 'EXCEPTION' ($_.Exception.Message + ' @ ' + ($_.InvocationInfo.PositionMessage -replace "`r?`n", ' '))
} finally {
  $checks['capturedAt'] = (Get-Date).ToString('o')
  $checks | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'admctl-evidence.json')
  if (-not $KeepClients) {
    if ($app) { try { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue } catch { } }
    if ($server) { try { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } catch { } }
  }
  Log 'ADMCTL_DONE'
}
