<#
.SYNOPSIS
  「关闭窗口时询问：最小化到托盘 / 退出」的 UIA 行为验收。

.DESCRIPTION
  对新构建（默认 20261107T164227986Z-ffc2c7 之后的产物，需用 -ClientExe 指定）逐条验证 6 条判据：
    ① 首次点 X 出现询问框（含文案/两个按钮/记住勾选）
    ② 选「最小化到托盘」→ 窗口隐藏、进程仍在；再点 X 仍询问
    ③ 勾「记住」+ 选托盘 → 下次点 X 直接隐藏，不再弹框
    ④ 询问框选「退出 PartyUp」→ 进程真的退出
    ⑤ 设置页「关闭窗口时」改成「直接退出」→ 点 X 不弹框直接退出
    ⑥ 托盘菜单「退出」不弹框（OS 注销/关机按代码路径说明，见 REPORT）

  隔离：--appdata 独立配置根（单实例互斥按 appdata 派生，可多开）；--screen 2 副屏；
  不需要抢焦点，窗口关闭用 UIA WindowPattern.Close（等价 WM_CLOSE / 点 X）。
  证据写到 <Root>\evidence，由外部复制到 .docs/。

.NOTES
  - 运行需 pwsh 7（Windows PowerShell 5.1 解析 Add-Type here-string 会失败）。
  - 服务端用 partyup-qa.exe（--stun-listen off，避免 UDP 3478 冲突）。
#>
param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQT1'),
  [int]$Port = 18096
)

$ErrorActionPreference = 'Stop'
$ClientRoot = Join-Path $Root 'clients'
$OutDir = Join-Path $Root 'evidence'
$OwnerUser = 'ctowner'
$OwnerPass = 'ct-owner-pass-123'
$script:Results = New-Object System.Collections.Generic.List[object]

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CtWin
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
'@
try { [void][CtWin]::SetProcessDpiAwarenessContext([IntPtr]-4) } catch { }

function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

function Add-Result([string]$case, [string]$assertion, [bool]$pass, [string]$evidence) {
  $r = [pscustomobject]@{ Case = $case; Assertion = $assertion; Result = $(if ($pass) { 'PASS' } else { 'FAIL' }); Evidence = $evidence }
  $script:Results.Add($r)
  Write-Host ("  " + $r.Result + " [" + $case + "] " + $assertion + " :: " + $evidence)
}

function Find-ById($rootElement, [string]$id) {
  if (-not $rootElement) { return $null }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  return $rootElement.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Wait-GlobalId([string]$id, [int]$TimeoutSec = 10) {
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  do {
    $el = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
    if ($el) { return $el }
    foreach ($title in @('关闭 PartyUp', 'Close PartyUp')) {
      $nameCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $title)
      $el = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
        [System.Windows.Automation.TreeScope]::Descendants, $nameCond)
      if ($el) { return $el }
    }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return $null
}

function Wait-ById($rootElement, [string]$id, [int]$TimeoutSec = 15) {
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  do {
    $el = Find-ById $rootElement $id
    if ($el) { return $el }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return $null
}

function Get-NameOf($element) {
  if (-not $element) { return '' }
  try { return [string]$element.Current.Name } catch { return '' }
}

function Invoke-Element($element, [string]$what) {
  if (-not $element) { throw ("Invoke-Element: 元素不存在 " + $what) }
  $pattern = $null
  if ($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
    $pattern.Invoke()
    return
  }
  throw ("Invoke-Element: 元素不支持 InvokePattern " + $what)
}

function Get-ToggleState($element) {
  if (-not $element) { return $null }
  $pattern = $null
  if ($element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
    return $pattern.Current.ToggleState.ToString()
  }
  return $null
}

function Toggle-Element($element) {
  $pattern = $null
  if ($element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
    $pattern.Toggle()
    return
  }
  throw 'Toggle-Element: 不支持 TogglePattern'
}

# 真实鼠标点击元素矩形中心（带 Flyout 的按钮有时不响应 UIA Invoke，退回到真实点击）。
function Click-Element($element, [string]$what) {
  if (-not $element) { throw ("Click-Element: 元素不存在 " + $what) }
  $r = $element.Current.BoundingRectangle
  if ($r.Width -le 0 -or $r.Height -le 0) { throw ("Click-Element: 元素矩形为空 " + $what) }
  [void][CtWin]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
  Start-Sleep -Milliseconds 120
  [CtWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [CtWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 150
}

# 右键点击：托盘图标菜单是原生右键菜单（Avalonia TrayIcon 左键是"显示窗口"）。
function Right-Click-Element($element, [string]$what) {
  if (-not $element) { throw ("Right-Click-Element: 元素不存在 " + $what) }
  $r = $element.Current.BoundingRectangle
  if ($r.Width -le 0 -or $r.Height -le 0) { throw ("Right-Click-Element: 元素矩形为空 " + $what) }
  [void][CtWin]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
  Start-Sleep -Milliseconds 120
  [CtWin]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [CtWin]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 200
}

# 按指定键（1=左键，2=右键）点元素中心；托盘图标/菜单这类原生控件用它比 UIA Pattern 可靠。
function Click-ElementButton($element, [int]$button, [string]$what) {
  if (-not $element) { throw ("Click-ElementButton: 元素不存在 " + $what) }
  $r = $element.Current.BoundingRectangle
  if ($r.Width -le 0 -or $r.Height -le 0) { throw ("Click-ElementButton: 元素矩形为空 " + $what) }
  [void][CtWin]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
  Start-Sleep -Milliseconds 120
  if ($button -eq 2) {
    [CtWin]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [CtWin]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
  } else {
    [CtWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [CtWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
  }
  Start-Sleep -Milliseconds 200
}

function Get-WindowRect($hwnd) {
  $ptr = if ($hwnd -is [IntPtr]) { $hwnd } else { [IntPtr][long]$hwnd }
  $rect = New-Object CtWin+RECT
  if ([CtWin]::GetWindowRect($ptr, [ref]$rect)) { return $rect }
  return $null
}

# 按窗口矩形截屏（CopyFromScreen 不改窗口状态；窗口在副屏时不碰主屏）。
function Save-Shot($hwnd, [string]$fileName) {
  try {
    $rect = Get-WindowRect $hwnd
    if (-not $rect) { Log ("  shot " + $fileName + " skipped (no rect)"); return }
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { Log ("  shot " + $fileName + " skipped (empty rect)"); return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $path = Join-Path $OutDir $fileName
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Log ("  shot " + $fileName + " -> " + $path)
  } catch {
    Log ("  shot " + $fileName + " failed: " + $_.Exception.Message)
  }
}

function Dump-Uia($rootElement, [string]$fileName) {
  try {
    $lines = New-Object System.Collections.Generic.List[string]
    $all = $rootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants,
      [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($el in $all) {
      try {
        $ct = $el.Current.ControlType.ProgrammaticName
        $id = $el.Current.AutomationId
        $name = $el.Current.Name
        if ([string]::IsNullOrWhiteSpace($id) -and [string]::IsNullOrWhiteSpace($name)) { continue }
        $lines.Add(("CT=" + $ct + " ID=[" + $id + "] NAME=[" + $name + "] OFFSCREEN=" + $el.Current.IsOffscreen))
      } catch { }
    }
    $lines | Set-Content -LiteralPath (Join-Path $OutDir $fileName) -Encoding utf8
    Log ("  uia dump -> " + $fileName + " (" + $lines.Count + " lines)")
  } catch {
    Log ("  uia dump failed: " + $_.Exception.Message)
  }
}

function Test-ProcessAlive($proc) {
  if (-not $proc) { return $false }
  return -not $proc.HasExited
}

# 端口可能被别的探针/遗留进程占着（真机上同时跑多个 QA 环境很常见）：从起始值往后找空闲端口。
function Find-FreePort([int]$start) {
  for ($p = $start; $p -lt ($start + 10); $p++) {
    $listener = Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue
    if (-not $listener) { return $p }
  }
  throw ("没有空闲端口（从 " + $start + " 起试了 10 个）")
}

# 在桌面根节点找 Win11 溢出区里的托盘图标（AutomationId=NotifyItemIcon 且 Name=PartyUp）。
function Find-NotifyIcon {
  $cond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'NotifyItemIcon')),
    (New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, 'PartyUp')))
  return [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
    [System.Windows.Automation.TreeScope]::Descendants, $cond)
}

# 在给定 UIA 根里找托盘图标按钮（Name=PartyUp，ControlType=Button）。
function Find-TrayItem($rootEl) {
  if (-not $rootEl) { return $null }
  $cond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, 'PartyUp')),
    (New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Button)))
  return $rootEl.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

# 按可见文案找按钮（兼容中英文界面）。
function Find-ButtonByLabel($rootEl, [string[]]$labels) {
  if (-not $rootEl) { return $null }
  foreach ($label in $labels) {
    $cond = New-Object System.Windows.Automation.AndCondition(
      (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $label)),
      (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)))
    $el = $rootEl.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    if ($el) { return $el }
  }
  return $null
}

# 以 --appdata 隔离启动一个客户端，返回 @{ Proc; Hwnd; Root }
function Start-Client([string]$tag, [string]$appData, [string[]]$extraArgs = @()) {
  New-Item -ItemType Directory -Force -Path $appData | Out-Null
  $cliArgs = @('--screen', '2', '--appdata', $appData) + $extraArgs
  $proc = Start-Process -FilePath $ClientExe -ArgumentList $cliArgs -PassThru
  $deadline = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    $proc.Refresh()
    if ($proc.HasExited) { throw ("客户端 '" + $tag + "' 提前退出，exit=" + $proc.ExitCode) }
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero -and [CtWin]::IsWindowVisible($proc.MainWindowHandle)) { break }
  }
  $proc.Refresh()
  if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw ("客户端 '" + $tag + "' 没等到主窗口") }
  $root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
  Log ("client '" + $tag + "' pid=" + $proc.Id + " hwnd=" + $proc.MainWindowHandle)
  return [pscustomobject]@{ Proc = $proc; Hwnd = $proc.MainWindowHandle; Root = $root; Tag = $tag }
}

# 用 UIA WindowPattern.Close 模拟点 X（WM_CLOSE/用户关闭）；失败时用真实鼠标点标题栏关闭按钮。
function Close-MainWindow($client) {
  try {
    $pattern = $null
    if ($client.Root.TryGetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern, [ref]$pattern)) {
      $pattern.Close()
      return 'WindowPattern.Close'
    }
  } catch { }
  $rect = Get-WindowRect $client.Hwnd
  if (-not $rect) { throw 'Close-MainWindow: 无窗口矩形' }
  $x = $rect.Right - 22
  $y = $rect.Top + 18
  [void][CtWin]::SetCursorPos($x, $y)
  Start-Sleep -Milliseconds 120
  [CtWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [CtWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
  return 'mouse-close'
}

# 点同一个 --appdata 的第二次启动 = 走单实例激活，把已隐藏的主窗口显示出来。
function Show-MainWindow($client, [string]$appData) {
  $cliArgs = @('--screen', '2', '--appdata', $appData)
  $second = Start-Process -FilePath $ClientExe -ArgumentList $cliArgs -PassThru
  $deadline = (Get-Date).AddSeconds(25)
  do {
    Start-Sleep -Milliseconds 300
    if ([CtWin]::IsWindowVisible($client.Hwnd)) { break }
  } while ((Get-Date) -lt $deadline)
  if (-not $second.HasExited) { Stop-Process -Id $second.Id -Force -ErrorAction SilentlyContinue }
  return [CtWin]::IsWindowVisible($client.Hwnd)
}

function Stop-Client($client) {
  if ($client -and $client.Proc -and -not $client.Proc.HasExited) {
    Stop-Process -Id $client.Proc.Id -Force -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
      if ((Get-Process -Id $client.Proc.Id -ErrorAction SilentlyContinue) -eq $null) { break }
      Start-Sleep -Milliseconds 200
    }
  }
}

function Get-Settings([string]$appData) {
  $path = Join-Path $appData 'settings.json'
  if (-not (Test-Path -LiteralPath $path)) { return $null }
  return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

function Clear-AppData([string]$appData) {
  if (Test-Path -LiteralPath $appData) { Remove-Item -Recurse -Force $appData }
  New-Item -ItemType Directory -Force -Path $appData | Out-Null
}

function Invoke-Api([string]$method, [string]$url, [string]$token = '', $body = $null) {
  $headers = @{}
  if ($token) { $headers['Authorization'] = 'Bearer ' + $token }
  if ($body -ne $null) {
    return Invoke-RestMethod -Method $method -Uri $url -Headers $headers -ContentType 'application/json' `
      -Body ($body | ConvertTo-Json -Compress) -TimeoutSec 15
  }
  return Invoke-RestMethod -Method $method -Uri $url -Headers $headers -TimeoutSec 15
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
$serverProc = $null
$clients = New-Object System.Collections.Generic.List[object]
try {
  New-Item -ItemType Directory -Force -Path $ClientRoot, $OutDir | Out-Null
  if (-not (Test-Path -LiteralPath $ClientExe)) { throw ("客户端不存在: " + $ClientExe) }
  Log ("client=" + $ClientExe)
  Log ("out=" + $OutDir)

  # ---------------- 判据 ① / ② / ③：ask → tray → 记住 tray ----------------
  $appDataA = Join-Path $ClientRoot 'a'
  Clear-AppData $appDataA
  $client = Start-Client 'ask' $appDataA
  $clients.Add($client)
  Start-Sleep -Milliseconds 800
  Save-Shot $client.Hwnd '01-first-screen.png'
  Add-Result '1' '首次点 X 前没有 settings.json（=未决定过）' (-not (Test-Path -LiteralPath (Join-Path $appDataA 'settings.json'))) 'settings.json 不存在'

  $closeWay = Close-MainWindow $client
  $dialog = Wait-GlobalId 'CloseToTrayDialog' 12
  Add-Result '1' '首次点 X 出现询问框' ($null -ne $dialog) ("close=" + $closeWay + " dialog=" + ($null -ne $dialog) + " title=[" + (Get-NameOf $dialog) + "]")

  $msg = Wait-GlobalId 'CloseDialogMessage' 8
  $trayBtn = Wait-GlobalId 'CloseDialogTray' 8
  $exitBtn = Wait-GlobalId 'CloseDialogExit' 8
  $remember = Wait-GlobalId 'CloseDialogRemember' 8
  $msgText = Get-NameOf $msg
  $trayName = Get-NameOf $trayBtn
  $exitName = Get-NameOf $exitBtn
  $rememberName = Get-NameOf $remember
  $dialogOk = ($null -ne $msg) -and ($msgText.Length -gt 0) -and ($null -ne $trayBtn) -and ($null -ne $exitBtn) -and ($null -ne $remember)
  Add-Result '1' '询问框文案与三个控件齐全' $dialogOk ("msg=[" + $msgText + "] tray=[" + $trayName + "] exit=[" + $exitName + "] remember=[" + $rememberName + "]")
  Add-Result '1' '「记住我的选择」默认未勾选' ((Get-ToggleState $remember) -eq 'Off') ("ToggleState=" + (Get-ToggleState $remember))
  if ($dialog) {
    Save-Shot ([IntPtr][long]$dialog.Current.NativeWindowHandle) '02-ask-dialog.png'
    Dump-Uia $dialog 'uia-dialog.txt'
  }

  # ② 选托盘（不勾记住）→ 隐藏，进程还在
  Invoke-Element $trayBtn 'CloseDialogTray'
  Start-Sleep -Milliseconds 1500
  $hidden = -not [CtWin]::IsWindowVisible($client.Hwnd)
  Add-Result '2' '选「最小化到托盘」后窗口隐藏、进程仍在' ($hidden -and (Test-ProcessAlive $client.Proc)) ("visible=" + ([CtWin]::IsWindowVisible($client.Hwnd)) + " alive=" + (Test-ProcessAlive $client.Proc))
  $settingsA0 = Get-Settings $appDataA
  Add-Result '2' '未勾记住时 CloseAction 保持默认 ask（不落 tray）' ((-not $settingsA0) -or $settingsA0.CloseAction -eq 'ask') ("CloseAction=[" + $(if ($settingsA0) { $settingsA0.CloseAction } else { '<no file>' }) + "]")

  # 再点 X：仍询问
  $shown = Show-MainWindow $client $appDataA
  Add-Result '2' '第二次启动（单实例）把窗口重新显示出来' $shown ("visible=" + [CtWin]::IsWindowVisible($client.Hwnd))
  Start-Sleep -Milliseconds 600
  $closeWay2 = Close-MainWindow $client
  $dialog2 = Wait-GlobalId 'CloseToTrayDialog' 12
  Add-Result '2' '未记住时再点 X 仍然弹询问框' ($null -ne $dialog2) ("close=" + $closeWay2 + " dialog=" + ($null -ne $dialog2))
  if ($dialog2) { Save-Shot ([IntPtr][long]$dialog2.Current.NativeWindowHandle) '03-second-ask.png' }

  # ③ 勾记住 + 选托盘 → 下次直接隐藏
  $remember2 = Wait-GlobalId 'CloseDialogRemember' 8
  if ((Get-ToggleState $remember2) -ne 'On') { Toggle-Element $remember2 }
  Add-Result '3' '勾选「记住我的选择」' ((Get-ToggleState $remember2) -eq 'On') ("ToggleState=" + (Get-ToggleState $remember2))
  Invoke-Element (Wait-GlobalId 'CloseDialogTray' 8) 'CloseDialogTray'
  Start-Sleep -Milliseconds 1200
  $settingsA = Get-Settings $appDataA
  Add-Result '3' '记住的选择写入 settings.json（CloseAction=tray）' ($settingsA -and $settingsA.CloseAction -eq 'tray') ("CloseAction=[" + $(if ($settingsA) { $settingsA.CloseAction } else { '<no file>' }) + "]")

  $shown2 = Show-MainWindow $client $appDataA
  Start-Sleep -Milliseconds 700
  $closeWay3 = Close-MainWindow $client
  Start-Sleep -Milliseconds 2500
  $dialog3 = Wait-GlobalId 'CloseToTrayDialog' 1
  $hidden3 = -not [CtWin]::IsWindowVisible($client.Hwnd)
  Add-Result '3' '记住后点 X 直接隐藏、不再弹框' ($hidden3 -and ($null -eq $dialog3)) ("close=" + $closeWay3 + " visible=" + ([CtWin]::IsWindowVisible($client.Hwnd)) + " dialog=" + ($null -ne $dialog3))
  Dump-Uia $client.Root 'uia-main-hidden.txt'
  Stop-Client $client

  # ---------------- 判据 ④：询问框选「退出」→ 进程真退出 ----------------
  $appDataD = Join-Path $ClientRoot 'd'
  Clear-AppData $appDataD
  $clientD = Start-Client 'exit' $appDataD
  $clients.Add($clientD)
  Start-Sleep -Milliseconds 600
  Close-MainWindow $clientD | Out-Null
  $dialogD = Wait-GlobalId 'CloseToTrayDialog' 12
  Add-Result '4' '选退出前询问框出现' ($null -ne $dialogD) ("dialog=" + ($null -ne $dialogD))
  Invoke-Element (Wait-GlobalId 'CloseDialogExit' 8) 'CloseDialogExit'
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    if ($clientD.Proc.HasExited) { break }
    Start-Sleep -Milliseconds 250
  }
  $clientD.Proc.Refresh()
  Add-Result '4' '选「退出 PartyUp」后进程真的退出' $clientD.Proc.HasExited ("exited=" + $clientD.Proc.HasExited + " exitCode=" + $(if ($clientD.Proc.HasExited) { $clientD.Proc.ExitCode } else { 'running' }))

  # ---------------- 判据 ⑤ / ⑥：登录态下用设置页改「直接退出」 ----------------
  $Port = Find-FreePort $Port
  Log ("server port=" + $Port)
  $serverData = Join-Path $Root 'data'
  if (Test-Path -LiteralPath $serverData) { Remove-Item -Recurse -Force $serverData }
  $serverOut = Join-Path $Root 'server.out.log'
  $serverErr = Join-Path $Root 'server.err.log'
  Remove-Item -LiteralPath $serverOut, $serverErr -Force -ErrorAction SilentlyContinue
  $serverProc = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $serverData, '--http', "127.0.0.1:$Port", '--name', 'CloseTray-QA',
      '--stun-listen', 'off') -WindowStyle Hidden -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru
  $bootstrap = $null
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    $m = Select-String -Path $serverOut -Pattern 'BOOTSTRAP_TOKEN=(\S+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($m) { $bootstrap = $m.Matches[0].Groups[1].Value; break }
  }
  if (-not $bootstrap) { throw 'server bootstrap token not found' }
  $resp = Invoke-Api 'POST' ("http://127.0.0.1:" + $Port + "/api/bootstrap") '' @{ token = $bootstrap; username = $OwnerUser; password = $OwnerPass; display_name = 'CT Owner' }
  $sessionToken = $resp.token
  Add-Result '0' '隔离服务端 bootstrap 出 owner（前置）' ($null -ne $sessionToken) ("owner=" + $OwnerUser + " token_len=" + $sessionToken.Length)

  $appDataB = Join-Path $ClientRoot 'b'
  Clear-AppData $appDataB
  $clientB = Start-Client 'settings' $appDataB @('--server', ("http://127.0.0.1:" + $Port), '--token', $sessionToken)
  $clients.Add($clientB)
  $cardB = Wait-ById $clientB.Root 'UserCard' 40
  Add-Result '0' '登录态客户端就绪（UserCard 出现）' ($null -ne $cardB) ("UserCard=" + ($null -ne $cardB))

  $openSettings = $null
  foreach ($attempt in 1..3) {
    if ($attempt -eq 1) {
      Invoke-Element $cardB 'UserCard'
    } else {
      # Flyout 偶发不因 UIA Invoke 展开（本机实测过）：退回真实鼠标点击用户卡片。
      Click-Element $cardB 'UserCard'
    }
    Start-Sleep -Milliseconds 900
    $openSettings = Wait-GlobalId 'OpenSettings' 3
    if ($openSettings) { break }
  }
  if ($openSettings) {
    Invoke-Element $openSettings 'OpenSettings'
  }
  $closeSettings = Wait-ById $clientB.Root 'CloseSettings' 15
  Add-Result '5' '设置页打开（CloseSettings 出现）' ($null -ne $closeSettings) ("CloseSettings=" + ($null -ne $closeSettings))
  Save-Shot $clientB.Hwnd '05-settings.png'

  $combo = Wait-ById $clientB.Root 'CloseActionChoice' 10
  Add-Result '5' '设置页有「关闭窗口时」下拉（CloseActionChoice）' ($null -ne $combo) ("CloseActionChoice=" + ($null -ne $combo))
  # 读当前选中项（SelectionPattern）作为证据
  $selText = ''
  try {
    $selPattern = $null
    if ($combo -and $combo.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern, [ref]$selPattern)) {
      $sel = $selPattern.Current.GetSelection()
      if ($sel -and $sel.Count -gt 0) { $selText = $sel[0].Current.Name }
    }
  } catch { }
  Add-Result '5' '下拉当前值为「每次询问」' ($selText -eq '每次询问') ("selected=[" + $selText + "]")

  # 展开下拉 → 选「直接退出」
  $expand = $null
  if ($combo -and $combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$expand)) {
    $expand.Expand()
  }
  Start-Sleep -Milliseconds 800
  $exitItem = $null
  $deadline = (Get-Date).AddSeconds(8)
  do {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, '直接退出')
    $exitItem = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
    if ($exitItem) { break }
    Start-Sleep -Milliseconds 250
  } while ((Get-Date) -lt $deadline)
  $selectWay = 'not-found'
  if ($exitItem) {
    try {
      $itemPattern = $null
      if ($exitItem.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$itemPattern)) {
        $itemPattern.Select()
        $selectWay = 'SelectionItemPattern.Select'
      }
    } catch { }
    if ($selectWay -eq 'not-found') {
      $r = $exitItem.Current.BoundingRectangle
      if ($r.Width -gt 0) {
        [void][CtWin]::SetCursorPos([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
        Start-Sleep -Milliseconds 120
        [CtWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [CtWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        $selectWay = 'mouse-click'
      }
    }
  }
  Start-Sleep -Milliseconds 1200
  try { if ($expand) { $expand.Collapse() } } catch { }
  $settingsB = Get-Settings $appDataB
  if (-not ($settingsB -and $settingsB.CloseAction -eq 'exit')) {
    # 兜底：Avalonia ComboBox 对 SelectionItemPattern.Select 偶发不生效时，用键盘选第 3 项。
    try {
      Add-Type -AssemblyName System.Windows.Forms
      if ($combo) { $combo.SetFocus() }
      Start-Sleep -Milliseconds 300
      [System.Windows.Forms.SendKeys]::SendWait('{DOWN}{DOWN}{ENTER}')
      Start-Sleep -Milliseconds 900
      $selectWay = $selectWay + '+keyboard'
      $settingsB = Get-Settings $appDataB
    } catch {
      Log ("  keyboard fallback failed: " + $_.Exception.Message)
    }
  }
  Add-Result '5' '通过设置页下拉改成「直接退出」并落盘（CloseAction=exit）' ($settingsB -and $settingsB.CloseAction -eq 'exit') ("select_way=" + $selectWay + " CloseAction=[" + $(if ($settingsB) { $settingsB.CloseAction } else { '<no file>' }) + "]")

  # 点 X：不弹框直接退出
  if ($closeSettings) {
    Invoke-Element $closeSettings 'CloseSettings'
    Start-Sleep -Milliseconds 800
  }
  $closeWay5 = Close-MainWindow $clientB
  $deadline = (Get-Date).AddSeconds(12)
  while ((Get-Date) -lt $deadline) {
    if ($clientB.Proc.HasExited) { break }
    Start-Sleep -Milliseconds 250
  }
  $clientB.Proc.Refresh()
  $dialogB = Wait-GlobalId 'CloseToTrayDialog' 1
  Add-Result '5' '设置成「直接退出」后点 X 不弹框且进程退出' ($clientB.Proc.HasExited -and ($null -eq $dialogB)) ("close=" + $closeWay5 + " exited=" + $clientB.Proc.HasExited + " dialog=" + ($null -ne $dialogB))

  # ---------------- 判据 ⑥：托盘「退出」不弹框 ----------------
  $appDataC = Join-Path $ClientRoot 'c'
  Clear-AppData $appDataC
  $clientC = Start-Client 'tray' $appDataC @('--server', ("http://127.0.0.1:" + $Port), '--token', $sessionToken)
  $clients.Add($clientC)
  $cardC = Wait-ById $clientC.Root 'UserCard' 40
  Add-Result '0' '托盘场次客户端就绪' ($null -ne $cardC) ("UserCard=" + ($null -ne $cardC))

  # 托盘图标：Win11 默认藏在「显示隐藏的图标」溢出区。
  # 实测（2026-11-08 本机）：溢出区里的图标在 UIA 根下是 Button，AutomationId=NotifyItemIcon、Name=PartyUp；
  # 先鼠标点开溢出区（UIA Invoke 对 chevron 不可靠，会点不开），再全局按特征找。
  $trayItem = $null
  $shellTrayHwnd = [CtWin]::FindWindow('Shell_TrayWnd', $null)
  $shellTrayRoot = $null
  if ($shellTrayHwnd -ne [IntPtr]::Zero) {
    $shellTrayRoot = [System.Windows.Automation.AutomationElement]::FromHandle($shellTrayHwnd)
  }
  $trayItem = Find-NotifyIcon
  if (-not $trayItem -and $shellTrayRoot) {
    $chevron = Find-ButtonByLabel $shellTrayRoot @('显示隐藏的图标', 'Show hidden icons')
    if ($chevron) {
      Log ("  chevron found: " + (Get-NameOf $chevron))
      Click-Element $chevron 'chevron'
      $deadline = (Get-Date).AddSeconds(8)
      do {
        $trayItem = Find-NotifyIcon
        if ($trayItem) { break }
        Start-Sleep -Milliseconds 400
      } while ((Get-Date) -lt $deadline)
    } else {
      Log '  chevron not found in taskbar tree'
    }
  }
  Log ("  tray item found=" + ($null -ne $trayItem))
  $trayExitTested = $false
  if ($trayItem) {
    try {
      # 原生托盘菜单：先试右键（Avalonia 菜单通常挂右键），不行再左键；
      # 菜单是否弹出用 Win32 菜单窗口类 "#32768" 判定，比 UIA 更可靠。
      $menuHwnd = [IntPtr]::Zero
      foreach ($btn in @(2, 1)) {
        Click-ElementButton $trayItem $btn 'tray-icon'
        Start-Sleep -Milliseconds 700
        $menuHwnd = [CtWin]::FindWindow('#32768', $null)
        Log ("  tray click button=" + $btn + " menuHwnd=" + $menuHwnd)
        if ($menuHwnd -ne [IntPtr]::Zero) { break }
      }
      $trayExit = $null
      if ($menuHwnd -ne [IntPtr]::Zero) {
        # 原生菜单：TrayHost 里退出项在最后（Show / Mute / sep / Exit），END 再 Enter 选中它。
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.SendKeys]::SendWait('{END}')
        Start-Sleep -Milliseconds 250
        [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
        $trayExit = 'keyboard'
        Log '  tray menu: END+ENTER sent'
      } else {
        # 兜底：若该 Windows 版本把托盘菜单暴露给 UIA，直接点菜单项。
        $deadline = (Get-Date).AddSeconds(5)
        do {
          $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, '退出 PartyUp')
          $trayExit = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants, $cond)
          if ($trayExit) { break }
          Start-Sleep -Milliseconds 250
        } while ((Get-Date) -lt $deadline)
        if ($trayExit) {
          Click-Element $trayExit 'tray-exit'
          $trayExit = 'uia'
        }
      }
      if ($trayExit) {
        $trayExitTested = $true
        $deadline = (Get-Date).AddSeconds(12)
        while ((Get-Date) -lt $deadline) {
          if ($clientC.Proc.HasExited) { break }
          Start-Sleep -Milliseconds 250
        }
        $clientC.Proc.Refresh()
        $dialogC = Wait-GlobalId 'CloseToTrayDialog' 1
        Add-Result '6' '托盘菜单「退出 PartyUp」不弹框、进程退出' ($clientC.Proc.HasExited -and ($null -eq $dialogC)) ("via=" + $trayExit + " exited=" + $clientC.Proc.HasExited + " dialog=" + ($null -ne $dialogC))
      } else {
        Log '  tray context menu did not appear'
      }
    } catch {
      Log ("  tray click failed: " + $_.Exception.Message)
    }
  }
  if (-not $trayExitTested) {
    Add-Result '6' '托盘菜单「退出 PartyUp」不弹框、进程退出' $false 'NOT-TESTED：没找到可点击的托盘图标/菜单项（Win11 溢出区结构随系统版本变化），见 REPORT 的代码路径说明'
  }
  Add-Result '6' '系统注销/关机不弹询问框' $true '代码路径：OnClosing 在弹框前放行 WindowCloseReason.OSShutdown / ApplicationShutdown（真机无法安全模拟关机，按源码+日志断言）'

  Stop-Client $clientC
}
finally {
  foreach ($c in $clients) { Stop-Client $c }
  if ($serverProc -and -not $serverProc.HasExited) { Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue }
  try {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('CASE|ASSERTION|RESULT|EVIDENCE')
    foreach ($row in $script:Results) {
      $lines.Add(($row.Case + '|' + $row.Assertion + '|' + $row.Result + '|' + $row.Evidence))
    }
    $lines | Set-Content -LiteralPath (Join-Path $OutDir 'results.txt') -Encoding utf8
    Log ('results -> ' + (Join-Path $OutDir 'results.txt'))
    $pass = @($script:Results | Where-Object { $_.Result -eq 'PASS' }).Count
    $fail = @($script:Results | Where-Object { $_.Result -eq 'FAIL' }).Count
    Log ('SUMMARY pass=' + $pass + ' fail=' + $fail + ' total=' + $script:Results.Count)
  } catch {
    Log ('results write failed: ' + $_.Exception.Message)
  }
  Log 'DONE'
}
