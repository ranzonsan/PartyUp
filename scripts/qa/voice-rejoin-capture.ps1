param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'),
  [int]$Port = 18086,
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$EvidenceDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\evidence'),
  [string]$Label = 'newdll',
  [string]$MediaDll = '',
  [int]$Rounds = 3,
  [int]$HoldSeconds = 8,
  [int]$SecondaryX = -2560,
  [int]$SecondaryY = 0,
  [switch]$KeepClients
)

# Voice rejoin capture test (single client, isolated 18086 server, secondary screen).
#
# Judge = client-side capture liveness line (new client build, content-independent):
#   voice: capture frames=<n> bytes=<n> lastCbMs=<n> adm(init=… rec=… recInit=… instance=…)
# per-round delta of `frames` must be non-zero and comparable (~100/s); `instance`
# should stay identical across rounds. pcmRx is printed for context only.
# Scenario: join -> hold -> leave -> join -> hold -> leave -> join (3 joins).
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
$logPath = Join-Path $EvidenceDir ("rejoin-" + $Label + ".log")
$errPath = Join-Path $EvidenceDir ("rejoin-" + $Label + ".err.log")
$nativePath = Join-Path $EvidenceDir ("rejoin-" + $Label + "-native.log")
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
function Get-NewLines([string]$p, [int]$from) {
  if (-not (Test-Path $p)) { return @() }
  $all = @(Get-Content -Path $p -ErrorAction SilentlyContinue)
  if ($from -ge $all.Count) { return @() }
  return @($all[$from..($all.Count - 1)])
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
# ---- capture liveness parsing ---------------------------------------------------
function Get-CaptureLines([string]$p) {
  if (-not (Test-Path $p)) { return @() }
  return @(Select-String -Path $p -Pattern 'voice: capture frames=' -ErrorAction SilentlyContinue |
           ForEach-Object { $_.Line.Trim() })
}
function Get-LastCapture([string]$p) {
  $lines = Get-CaptureLines $p
  if ($lines.Count -eq 0) { return $null }
  return $lines[-1]
}
function Parse-Capture([string]$line) {
  $h = @{ frames = -1; bytes = -1; lastCb = -1; init = -1; rec = -1; recInit = -1; instance = '' }
  if (-not $line) { return $h }
  $m = [regex]::Match($line, 'frames=(\d+)')
  if ($m.Success) { $h.frames = [int64]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'bytes=(\d+)')
  if ($m.Success) { $h.bytes = [int64]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'lastCbMs=(-?\d+)')
  if ($m.Success) { $h.lastCb = [int64]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'init=(\d+)')
  if ($m.Success) { $h.init = [int]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'rec=(\d+)')
  if ($m.Success) { $h.rec = [int]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'recInit=(\d+)')
  if ($m.Success) { $h.recInit = [int]$m.Groups[1].Value }
  $m = [regex]::Match($line, 'instance=([0-9A-Fa-fx]+)')
  if ($m.Success) { $h.instance = $m.Groups[1].Value }
  return $h
}
function Capture-Summary([string]$line) {
  $c = Parse-Capture $line
  return ("frames=" + $c.frames + " bytes=" + $c.bytes + " lastCbMs=" + $c.lastCb +
          " adm(init=" + $c.init + " rec=" + $c.rec + " recInit=" + $c.recInit + " instance=" + $c.instance + ")")
}
function Get-LastPcmRx([string]$p) {
  if (-not (Test-Path $p)) { return -1 }
  $m = Select-String -Path $p -Pattern 'voice: stats pcmRx=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
  if ($m) { return [int]$m.Matches[0].Groups[1].Value }
  return -1
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
  $dll = Join-Path (Split-Path $ClientExe) 'partyup_media.dll'
  Record 'DLL_PATH' $dll
  $script:dllBackup = ''
  if ($MediaDll) {
    if (-not (Test-Path $MediaDll)) { throw ("media dll missing: " + $MediaDll) }
    $script:dllBackup = Join-Path $env:TEMP ('partyup_media_backup_' + [guid]::NewGuid().ToString('N') + '.dll')
    if (Test-Path $dll) { Copy-Item $dll $script:dllBackup -Force }
    Copy-Item $MediaDll $dll -Force
    Record 'DLL_SWAPPED_TO' ((Get-FileHash -Algorithm SHA256 $MediaDll).Hash.Substring(0, 16) + '...')
  }
  if (Test-Path $dll) { Record 'DLL_SHA256' ((Get-FileHash -Algorithm SHA256 $dll).Hash.Substring(0, 16) + '...') }
  if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $serverOut = Join-Path $EvidenceDir ("rejoin-" + $Label + "-server.out")
  Remove-Item -Force -ErrorAction SilentlyContinue $serverOut
  $server = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port",
      '--stun-listen', '127.0.0.1:34786', '--name', 'PartyUp-rejoin'
    ) -RedirectStandardOutput $serverOut -RedirectStandardError (Join-Path $EvidenceDir ("rejoin-" + $Label + "-server.err")) `
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
    -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru -WindowStyle Hidden
  Record 'PID' ([string]$app.Id)
  if (-not (Wait-ForWindow $app.Id 120)) { throw 'window did not appear' }
  Record 'RECT_PLACED' (Place-OnSecondary $app.Id)
  Record 'LOGIN' $(if (Wait-ForLog $logPath 'gui: connected as' 90) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Seconds 1
  Record 'RECT_AFTER_LOGIN' (Get-RectText $app.MainWindowHandle)

  for ($r = 1; $r -le $Rounds; $r++) {
    $logFrom = Get-LineCount $logPath
    $joinedBefore = Count-Log $logPath 'voice: joined channel='
    Record ("R" + $r + "_JOIN_CLICK") (Invoke-ById $app.Id 'JoinCall')
    $deadline = (Get-Date).AddSeconds(90)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
      if ((Count-Log $logPath 'voice: joined channel=') -gt $joinedBefore) { $ok = $true; break }
      Start-Sleep -Milliseconds 300
    }
    Record ("R" + $r + "_JOINED") $(if ($ok) { 'PASS' } else { 'FAIL' })
    # Baseline = the FIRST capture-liveness line of this round (stats loop ticks ~2 s).
    $baseLine = $null
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
      $roundLines = @(Get-NewLines $logPath $logFrom | Where-Object { $_ -match 'voice: capture frames=' })
      if ($roundLines.Count -gt 0) { $baseLine = $roundLines[0].Trim(); break }
      Start-Sleep -Milliseconds 300
    }
    $baseCap = Parse-Capture $baseLine
    Record ("R" + $r + "_CAPTURE_BASELINE") (Capture-Summary $baseLine)
    Start-Sleep -Seconds $HoldSeconds
    $afterLine = Get-LastCapture $logPath
    $afterCap = Parse-Capture $afterLine
    Record ("R" + $r + "_CAPTURE_AFTER") (Capture-Summary $afterLine)
    $delta = $afterCap.frames - $baseCap.frames
    Record ("R" + $r + "_CAPTURE_DELTA") ([string]$delta)
    Record ("R" + $r + "_CAPTURE_DELTA_PER_S") ([string]([math]::Round(($delta / [double]$HoldSeconds), 1)))
    Record ("R" + $r + "_ADM_INSTANCE") $afterCap.instance
    Record ("R" + $r + "_ADM_FLAGS") ("init=" + $afterCap.init + " rec=" + $afterCap.rec + " recInit=" + $afterCap.recInit)
    Record ("R" + $r + "_PCMRX") ([string](Get-LastPcmRx $logPath))
    Record ("R" + $r + "_CAPTURE_RAW") ((Get-CaptureLines $logPath | Select-Object -Last 4 | ForEach-Object { $_ }) -join ' || ')
    Record ("R" + $r + "_LEAVE_CLICK") (Invoke-ById $app.Id 'Leave')
    Record ("R" + $r + "_LEFT") $(if (Wait-ForLog $logPath 'voice: left' 40) { 'PASS' } else { 'FAIL' })
    Start-Sleep -Seconds 2
    $afterLeaveLast = Get-LastCapture $logPath
    Record ("R" + $r + "_CAPTURE_AFTER_LEAVE") (Capture-Summary $afterLeaveLast)
    $clientVoice = Get-NewLines $logPath $logFrom | Where-Object { $_ -match 'voice: (joined|left|stats|slots|capture)' }
    Record ("R" + $r + "_CLIENT_LINES") (($clientVoice | ForEach-Object { $_.Trim() }) -join ' || ')
  }
  Record 'EVIDENCE_DIR' $EvidenceDir
} catch {
  Record 'EXCEPTION' ($_.Exception.Message + ' @ ' + ($_.InvocationInfo.PositionMessage -replace "`r?`n", ' '))
} finally {
  $checks['capturedAt'] = (Get-Date).ToString('o')
  $checks | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir ("rejoin-" + $Label + "-evidence.json"))
  if (-not $KeepClients) {
    if ($app) { try { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue } catch { } }
    if ($server) { try { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } catch { } }
    Start-Sleep -Seconds 1
  }
  # Restore the swapped DLL only AFTER the client stopped (a live client keeps it mapped).
  if ($script:dllBackup -and (Test-Path $script:dllBackup)) {
    $dllPath = Join-Path (Split-Path $ClientExe) 'partyup_media.dll'
    $restored = $false
    for ($attempt = 1; $attempt -le 5; $attempt++) {
      try {
        Copy-Item $script:dllBackup $dllPath -Force
        $restored = $true
        break
      } catch {
        Log ('DLL_RESTORE_RETRY ' + $attempt + ': ' + $_.Exception.Message)
        Start-Sleep -Seconds 2
      }
    }
    if ($restored) {
      Log ('DLL_RESTORED_SHA=' + (Get-FileHash -Algorithm SHA256 $dllPath).Hash.Substring(0, 16))
    } else {
      Log ('DLL_RESTORE_FAILED: backup=' + $script:dllBackup)
    }
  }
  Log 'REJOIN_DONE'
}
