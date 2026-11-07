param(
  [string]$ClientExe = '',
  [int]$Port = 18085,
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence-audio"
)

# B1 音频验收：真实设备枚举 + 麦克风电平测试。
# 只在隔离实例（默认 18085）上跑，不碰 18081/18082。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Log([string]$m) { Write-Output ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }
function Get-Window([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  return [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
}
function Find-ById($window, [string]$id) {
  if (-not $window) { return $null }
  $c = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $c)
}
function Invoke-ById([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return "NOT_FOUND:$id" }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) {
    $pat.Invoke(); return "CLICKED:$id"
  }
  return "NO_INVOKE:$id"
}
function Read-Range([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return $null }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$pat)) {
    return [double]$pat.Current.Value
  }
  return $null
}
function Dump-Texts([int]$procId, [string]$name) {
  $w = Get-Window $procId
  if (-not $w) { return @() }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Text)
  $texts = @()
  foreach ($t in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
    if ($t.Current.Name) { $texts += $t.Current.Name }
  }
  $texts | Set-Content -Encoding UTF8 (Join-Path $OutDir "$name.texts.txt")
  return $texts
}
function Capture([int]$procId, [string]$name) {
  $path = Join-Path $OutDir $name
  & "$PSScriptRoot\..\capture-window.ps1" -ProcessId $procId -OutPath $path | Out-Null
  if (Test-Path $path) { return "SHOT:$path" } else { return 'SHOT_FAIL' }
}

function Scroll-PanelToBottom([int]$procId, [int]$steps = 6) {
  $w = Get-Window $procId
  if (-not $w) { return 'NO_WINDOW' }
  $all = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($el in $all) {
    try {
      $sp = $null
      if (-not $el.TryGetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern, [ref]$sp)) { continue }
      if (-not $sp.Current.VerticallyScrollable) { continue }
      for ($i = 0; $i -lt $steps; $i++) {
        $sp.Scroll([System.Windows.Automation.ScrollAmount]::LargeIncrement,
          [System.Windows.Automation.ScrollAmount]::NoAmount)
        Start-Sleep -Milliseconds 120
      }
      return 'SCROLLED'
    } catch {
      continue
    }
  }

  # 回退：把焦点放到设置面板上的按钮，再按 PageDown
  $target = Find-ById $w 'MicTestStart'
  if ($target) {
    try { $target.SetFocus() } catch { }
  }
  for ($i = 0; $i -lt $steps; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait('{PGDN}')
    Start-Sleep -Milliseconds 150
  }
  return 'KEYSCROLLED'
}

function Scroll-Wheel([int]$procId, [int]$notches = 5) {
  # Avalonia 的 ScrollViewer 对 UIA ScrollPattern 支持不稳定，PageDown 又会落到按钮上，
  # 所以直接给窗口发 WM_MOUSEWHEEL（坐标取设置面板区域）。
  if (-not ('PartyUpProbe.Wheel' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PartyUpProbe {
  public static class Wheel {
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    public static void Scroll(IntPtr hWnd, int screenX, int screenY, int delta) {
      IntPtr lParam = (IntPtr)((screenY << 16) | (screenX & 0xFFFF));
      SendMessage(hWnd, 0x020A, (IntPtr)(delta << 16), lParam);
    }
  }
}
'@
  }
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return 'NO_WINDOW' }
  $rect = $p.MainWindowHandle
  $win = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
  $b = $win.Current.BoundingRectangle
  $x = [int]($b.Right - 400)
  $y = [int]($b.Top + ($b.Height / 2))
  for ($i = 0; $i -lt $notches; $i++) {
    [PartyUpProbe.Wheel]::Scroll($p.MainWindowHandle, $x, $y, -120)
    Start-Sleep -Milliseconds 150
  }
  return ("WHEELED x=$x y=$y")
}

$base = "http://127.0.0.1:$Port"

# 单实例会把「上一个残留的同路径客户端」当成已有实例，新进程会立刻退出——
# 上一轮探针失败时留下的进程必须先清掉，否则本轮的窗口根本不会出现。
Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -ieq $ClientExe } |
  ForEach-Object { Log ("killing stale client pid=" + $_.Id); Stop-Process -Id $_.Id -Force }
Start-Sleep -Milliseconds 600
$body = @{ username = 'tester'; password = 'tester-pass-123' } | ConvertTo-Json
$resp = Invoke-RestMethod -Uri "$base/api/login" -Method Post -Body $body -ContentType 'application/json'
$token = $resp.token
Log ("LOGIN_OK token_len=" + $token.Length)

$p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) -ArgumentList @(
  '--server', $base, '--token', $token, '--screen', '2'
) -PassThru
Log "client pid=$($p.Id)"
Start-Sleep -Seconds 8

Capture $p.Id 'audio-shell.png' | ForEach-Object { Log $_ }
Log (Invoke-ById $p.Id 'UserCard')
Start-Sleep -Milliseconds 900
Log (Invoke-ById $p.Id 'OpenSettings')
Start-Sleep -Seconds 3

Capture $p.Id 'audio-settings.png' | ForEach-Object { Log $_ }
$texts = Dump-Texts $p.Id 'audio-settings'
Log ("TEXTS=" + ($texts -join ' | '))

# 设备下拉里应该至少有一项（默认项）+ 真实设备名
$inCombo = Find-ById (Get-Window $p.Id) 'InputDeviceChoice'
if ($inCombo) {
  $sel = $null
  if ($inCombo.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern, [ref]$sel)) {
    Log ("INPUT_SELECTION=" + (($sel.Current.GetSelection() | ForEach-Object { $_.Current.Name }) -join ','))
  }
  $exp = $null
  if ($inCombo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$exp)) {
    $exp.Expand(); Start-Sleep -Milliseconds 700
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::ListItem)
    $items = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
    Log ("INPUT_DEVICE_ITEMS=" + $items.Count)
    foreach ($it in $items) { Log ("  item: " + $it.Current.Name) }
    try { $exp.Collapse() } catch { }
  }
} else { Log 'INPUT_COMBO_NOT_FOUND' }

# 麦克风测试：开始 → 采样 4 秒电平 → 停止
Log (Invoke-ById $p.Id 'MicTestStart')
Log ("SCROLL=" + (Scroll-Wheel $p.Id 6))
Start-Sleep -Milliseconds 400
$max = -1.0
$last = -1.0
$shotTaken = $false
$deadline = (Get-Date).AddSeconds(8)
while ((Get-Date) -lt $deadline) {
  $v = Read-Range $p.Id 'MicTestLevel'
  if ($null -ne $v) { $last = $v; if ($v -gt $max) { $max = $v } }
  # 抓一张电平条真的在动时的截图（静止时截到空条说明不了问题）
  if (-not $shotTaken -and $null -ne $v -and $v -gt 3) {
    Capture $p.Id 'audio-mictest-live.png' | ForEach-Object { Log $_ }
    Log ("LIVE_LEVEL_AT_CAPTURE=" + $v)
    $shotTaken = $true
  }
  Start-Sleep -Milliseconds 200
}
Log ("MIC_LEVEL_MAX=" + $max)
Log ("MIC_LEVEL_LAST=" + $last)
Capture $p.Id 'audio-mictest.png' | ForEach-Object { Log $_ }
$texts2 = Dump-Texts $p.Id 'audio-mictest'
Log ("TEXTS_MICTEST=" + (($texts2 | Where-Object { $_ -match '电平|Level|采集|Capturing|没有|No level' }) -join ' | '))
Log (Invoke-ById $p.Id 'MicTestStop')

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Log 'DONE'
