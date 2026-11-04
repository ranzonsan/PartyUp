param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$Server,
  [Parameter(Mandatory = $true)][string]$Channel,
  [Parameter(Mandatory = $true)][string]$TokenA,
  [Parameter(Mandatory = $true)][string]$TokenB,
  [string]$Logs = "$env:TEMP\pu-gui\run\e2e"
)

# 打包版客户端的端到端走查（最多 2 个客户端）：
#   启动 → 自动登录 → 进频道 → GUI 发文字（看到自己的消息）→ 进语音（PCM 统计）
#   → 开始共享（预览在动，截图对比）→ 第二个客户端看到画面 → 停止共享 → 退出语音 → 关闭。
# 不跑任何性能/资源测量：只看「打包版能不能走完真实流程」。
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$clientExe = Join-Path $ClientDir 'PartyUp.Client.exe'
New-Item -ItemType Directory -Force -Path $Logs | Out-Null

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@

# 按 base64 传中文控件名：bash 经 -File 传中文给 PowerShell 会变乱码。
function N([string]$b64) { [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String($b64)) }
$BtnSend = N '5Y+R6YCB'          # 发送
$BtnStartShare = N '5byA5aeL5YWx5Lqr'  # 开始共享
$BtnStopShare = N '5YGc5q2i'     # 停止

function Get-Window([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  return [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
}

function Invoke-ByName([int]$procId, [string]$name) {
  $w = Get-Window $procId
  if (-not $w) { return "NO_WINDOW" }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $name)
  $t = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  if (-not $t) { return "NOT_FOUND:$name" }
  $pattern = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
    $pattern.Invoke(); return "CLICKED:$name"
  }
  return "NO_INVOKE:$name"
}

function Invoke-ByAutomationId([int]$procId, [string]$id) {
  $w = Get-Window $procId
  if (-not $w) { return "NO_WINDOW" }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  $t = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  if (-not $t) { return "NOT_FOUND:$id" }
  $pattern = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
    $pattern.Invoke(); return "CLICKED:$id"
  }
  return "NO_INVOKE:$id"
}

function Set-TextValue([int]$procId, [string]$automationId, [string]$value) {
  $w = Get-Window $procId
  if (-not $w) { return "NO_WINDOW" }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
  $t = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  if (-not $t) { return "NOT_FOUND:$automationId" }
  $vp = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) {
    $vp.SetValue($value); return "SET:$value"
  }
  return "NO_VALUEPATTERN:$automationId"
}

function Wait-Log([string]$path, [string]$pattern, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Test-Path $path) {
      $txt = Get-Content -Raw -ErrorAction SilentlyContinue $path
      if ($txt -and $txt -match $pattern) { return $true }
    }
    Start-Sleep -Milliseconds 400
  }
  return $false
}

function Start-Client([string]$token, [string]$logPath) {
  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, "$logPath.err"
  $p = Start-Process -FilePath $clientExe `
    -ArgumentList @('--server', $Server, '--token', $token, '--channel', $Channel, '--voice-join') `
    -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err" -PassThru
  # 窗口一出来就最小化，别占用户桌面。
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) {
      [W32]::ShowWindow($p.MainWindowHandle, 6) | Out-Null  # SW_MINIMIZE
      break
    }
  }
  return $p
}

$logA = Join-Path $Logs 'client-a.log'
$logB = Join-Path $Logs 'client-b.log'
$clientA = $null
$clientB = $null
$results = [ordered]@{}

try {
  # ---------- A：启动 → 登录 → 进频道 → 进语音 ----------
  $clientA = Start-Client $TokenA $logA
  $results.A_JOIN = if (Wait-Log $logA 'voice auto-join transport=\S+ ok=True' 90) { 'PASS' } else { 'FAIL' }
  $results.A_CHANNEL = if (Wait-Log $logA 'opened #' 30) { 'PASS' } else { 'FAIL' }

  # ---------- 发文字（GUI 输入 + 点「发送」），确认看到自己的消息 ----------
  $body = 'e2e-打包版-文字-1'
  $results.A_SET_TEXT = Set-TextValue $clientA.Id 'MessageInput' $body
  Start-Sleep -Milliseconds 500
  $results.A_CLICK_SEND = Invoke-ByName $clientA.Id $BtnSend
  $results.A_TEXT_ECHO = if (Wait-Log $logA ([regex]::Escape($body)) 20) { 'PASS' } else { 'FAIL' }

  # ---------- B：第二个客户端进同一频道语音 ----------
  $clientB = Start-Client $TokenB $logB
  $results.B_JOIN = if (Wait-Log $logB 'voice auto-join transport=\S+ ok=True' 90) { 'PASS' } else { 'FAIL' }
  $results.B_SEES_TEXT = if (Wait-Log $logA ([regex]::Escape("user1") + '.{0,40}' + [regex]::Escape($body)) 20) { 'PASS' } else { 'UNKNOWN' }
  Start-Sleep -Seconds 5

  # PCM：A 侧应看到远端音轨的 pcmFrames 统计（B 在语音里发麦克风）。
  $txtA = Get-Content -Raw $logA
  $stats = [regex]::Matches($txtA, 'voice: stats pcmRx=(\d+)')
  $frames = [regex]::Matches($txtA, 'pcmFrames=(\d+)')
  $maxPcmRx = 0
  foreach ($m in $stats) { if ([int]$m.Groups[1].Value -gt $maxPcmRx) { $maxPcmRx = [int]$m.Groups[1].Value } }
  $maxPcmFrames = 0
  foreach ($m in $frames) { if ([int]$m.Groups[1].Value -gt $maxPcmFrames) { $maxPcmFrames = [int]$m.Groups[1].Value } }
  $results.A_PCM = "pcmRx_max=$maxPcmRx track_pcmFrames_max=$maxPcmFrames"

  # ---------- 开始共享（A）----------
  $results.A_CLICK_SHARE = Invoke-ByName $clientA.Id $BtnStartShare
  $results.A_SHARE_STARTED = if (Wait-Log $logA 'share: started' 30) { 'PASS' } else { 'FAIL' }

  # 预览是否在动：同一窗口间隔 1.2s 两张截图，比较 SHA256。
  $shot1 = Join-Path $Logs 'a-preview-1.png'
  $shot2 = Join-Path $Logs 'a-preview-2.png'
  & (Join-Path $here 'capture-window.ps1') -OutPath $shot1 -ProcessId $clientA.Id | Out-Null
  Start-Sleep -Milliseconds 1200
  & (Join-Path $here 'capture-window.ps1') -OutPath $shot2 -ProcessId $clientA.Id | Out-Null
  if ((Test-Path $shot1) -and (Test-Path $shot2)) {
    $h1 = (Get-FileHash $shot1 -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $shot2 -Algorithm SHA256).Hash
    $results.A_PREVIEW_MOVING = if ($h1 -ne $h2) { 'PASS (两帧不同)' } else { 'SUSPECT (两帧相同)' }
  } else {
    $results.A_PREVIEW_MOVING = 'NO_SCREENSHOT'
  }

  # ---------- B 看到画面 ----------
  $results.B_FIRST_FRAME = if (Wait-Log $logB '首帧上屏' 30) { 'PASS' } else { 'FAIL' }

  # ---------- 停止共享 + 退出语音 + 关闭 ----------
  $results.A_CLICK_STOP_SHARE = Invoke-ByName $clientA.Id $BtnStopShare
  $results.A_SHARE_STOPPED = if (Wait-Log $logA 'share: stopped' 20) { 'PASS' } else { 'FAIL' }
  $results.A_LEAVE = Invoke-ByAutomationId $clientA.Id 'VoiceToggleButton'
  $results.A_LEFT = if (Wait-Log $logA 'voice: left' 20) { 'PASS' } else { 'FAIL' }
  $results.B_LEAVE = Invoke-ByAutomationId $clientB.Id 'VoiceToggleButton'
  $results.B_LEFT = if (Wait-Log $logB 'voice: left' 20) { 'PASS' } else { 'FAIL' }
}
finally {
  foreach ($p in @($clientA, $clientB)) {
    if ($p -and -not $p.HasExited) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
  }
  Start-Sleep -Milliseconds 500
  "CLIENTS_REMAINING=$((Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Measure-Object).Count)"
}

foreach ($k in $results.Keys) { "$k=$($results[$k])" }
"LOGS=$Logs"
