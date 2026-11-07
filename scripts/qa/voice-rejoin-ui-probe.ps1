param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'),
  [int]$Port = 18086,
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$EvidenceDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\evidence'),
  [int]$SecondaryX = -2560,
  [int]$SecondaryY = 0
)

# UI probe: (1) force the client window onto the secondary screen, (2) find a UIA path
# to the Settings pane / mic-test panel. Writes a bounded element dump to a file.
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
$logPath = Join-Path $EvidenceDir 'uiprobe-client.log'
$errPath = Join-Path $EvidenceDir 'uiprobe-client.err.log'
$out = Join-Path $EvidenceDir 'uiprobe-tree.txt'
Set-Content -Path $out -Value ("# uiprobe " + (Get-Date -Format o)) -Encoding UTF8
function Dump([string]$s) { Add-Content -Path $out -Value $s -Encoding UTF8 }
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
function Get-Rect([IntPtr]$hwnd) {
  $r = New-Object WinRect+RECT
  [WinRect]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  return ("left=" + $r.Left + " top=" + $r.Top + " right=" + $r.Right + " bottom=" + $r.Bottom +
          " w=" + ($r.Right - $r.Left) + " h=" + ($r.Bottom - $r.Top))
}
function Get-RectObj([IntPtr]$hwnd) {
  $r = New-Object WinRect+RECT
  [WinRect]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  return $r
}
function Place-OnSecondary([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return 'NO_WINDOW' }
  $r = Get-RectObj $p.MainWindowHandle
  $w = $r.Right - $r.Left
  $h = $r.Bottom - $r.Top
  if ($w -le 0 -or $h -le 0) { return 'BAD_RECT' }
  $SWP_NOZORDER = 0x0004
  $SWP_NOACTIVATE = 0x0010
  [WinRect]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $SecondaryX, $SecondaryY, $w, $h,
    $SWP_NOZORDER -bor $SWP_NOACTIVATE) | Out-Null
  Start-Sleep -Milliseconds 400
  return (Get-Rect $p.MainWindowHandle)
}
function Dump-ProcessWindows([int]$procId, [string]$label) {
  Dump ("=== " + $label + " (pid=" + $procId + ") ===")
  $root = [System.Windows.Automation.AutomationElement]::RootElement
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $child = $walker.GetFirstChild($root)
  while ($child) {
    try {
      $cpid = $child.Current.ProcessId
      if ($cpid -eq $procId) {
        Dump ("WINDOW name=" + $child.Current.Name + " class=" + $child.Current.ClassName +
              " type=" + $child.Current.ControlType.ProgrammaticName)
        $desc = $child.FindAll([System.Windows.Automation.TreeScope]::Descendants,
          [System.Windows.Automation.Condition]::TrueCondition)
        Dump ("  DESCENDANTS=" + $desc.Count)
        $n = [Math]::Min($desc.Count, 500)
        for ($i = 0; $i -lt $n; $i++) {
          $e = $desc.Item($i)
          try {
            $aid = $e.Current.AutomationId
            $nm = $e.Current.Name
            if ($aid -ne '' -or $nm -ne '') {
              Dump ("  AID=" + $aid + " | NAME=" + $nm + " | TYPE=" + $e.Current.ControlType.ProgrammaticName)
            }
          } catch { }
        }
      }
    } catch { }
    try { $child = $walker.GetNextSibling($child) } catch { $child = $null }
  }
}

$app = $null
$server = $null
try {
  $procs = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
    Where-Object { $_.ExecutablePath -like "$Root*" }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  $srv = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
    Where-Object { $_.CommandLine -like "*$Root*" }
  foreach ($p in $srv) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 600
  if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $serverOut = Join-Path $EvidenceDir 'uiprobe-server.out'
  Remove-Item -Force -ErrorAction SilentlyContinue $serverOut
  $server = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port",
      '--stun-listen', '127.0.0.1:34786', '--name', 'PartyUp-uiprobe'
    ) -RedirectStandardOutput $serverOut -RedirectStandardError (Join-Path $EvidenceDir 'uiprobe-server.err') `
    -PassThru -WindowStyle Hidden
  $ready = $false; $bootstrap = ''
  for ($i = 0; $i -lt 160; $i++) {
    Start-Sleep -Milliseconds 250
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
  Invoke-RestMethod -Method Post -Uri "$base/api/admin/channels" -ContentType 'application/json' `
    -Headers @{ Authorization = "Bearer $($admin.token)" } -Body (@{ name = 'voice' } | ConvertTo-Json) | Out-Null

  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, $errPath
  $env:PARTYUP_VOICE_PEAK_THRESHOLD = '4'
  $app = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', $base, '--token', $admin.token, '--channel', 'voice', '--screen', '2') `
    -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru
  $deadline = (Get-Date).AddSeconds(120)
  while ((Get-Date) -lt $deadline) {
    if (Get-Window $app.Id) { break }
    Start-Sleep -Milliseconds 250
  }
  if (-not (Get-Window $app.Id)) { throw 'client window did not appear' }
  Log ("RECT_BEFORE " + (Get-Rect $app.MainWindowHandle))
  Log ("RECT_PLACED " + (Place-OnSecondary $app.Id))
  Dump ("RECT_BEFORE " + (Get-Rect $app.MainWindowHandle))

  $deadline = (Get-Date).AddSeconds(90)
  while ((Get-Date) -lt $deadline) {
    if (Test-Path $logPath) {
      if (@(Select-String -Path $logPath -SimpleMatch 'gui: connected as' -ErrorAction SilentlyContinue).Count -gt 0) { break }
    }
    Start-Sleep -Milliseconds 300
  }
  Start-Sleep -Seconds 1
  Log ("RECT_AFTER_LOGIN " + (Get-Rect $app.MainWindowHandle))
  Dump ("RECT_AFTER_LOGIN " + (Get-Rect $app.MainWindowHandle))
  Dump-ProcessWindows $app.Id 'AFTER_LOGIN'

  Log ("USER_CARD=" + (Invoke-ById $app.Id 'UserCard'))
  Start-Sleep -Milliseconds 1500
  Dump-ProcessWindows $app.Id 'AFTER_USER_CARD_CLICK'
  Log 'UIPROBE_DONE'
} catch {
  Log ('EXCEPTION: ' + $_.Exception.Message)
} finally {
  if ($app) { try { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue } catch { } }
  if ($server) { try { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue } catch { } }
  Log ('TREE_FILE=' + $out)
}
