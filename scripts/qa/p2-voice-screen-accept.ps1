param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$Server,
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [Parameter(Mandatory = $true)][string]$AliceToken,
  [Parameter(Mandatory = $true)][string]$AliceId,
  [Parameter(Mandatory = $true)][string]$BobToken,
  [Parameter(Mandatory = $true)][string]$BobId,
  [string]$MainChannel = '主频道',
  [string]$AltChannel = '备用频道',
  [string]$AliceDisplay = '爱丽丝',
  [string]$BobDisplay = '鲍勃',
  # 服务端日志（server.err）。提供时才能断言「先看后发」真机路径的
  # publisher_codec_known=false；缺省记 NO_SERVER_LOG。
  [string]$ServerLog = '',
  [switch]$KeepClients
)

# v1.1 外壳的 P2 真机验收：真实麦克风入会 + 真实屏幕共享发布/观看/停止 + 共享中转移。
# 前提：服务端必须跑在 Windows 侧（WSL 服务端的 UDP 媒体到不了 Windows 客户端），
# 且本机用户已授权麦克风与桌面采集。所有断言写进 KEY=VALUE 与 p2-evidence.json。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$clientExe = Join-Path $ClientDir 'PartyUp.Client.exe'
if (-not (Test-Path $clientExe)) { throw "client exe not found: $clientExe" }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

$checks = [ordered]@{}
function Record([string]$name, [string]$value) {
  $checks[$name] = $value
  Write-Output "$name=$value"
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

function Wait-ById([int]$procId, [string]$id, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Find-ById (Get-Window $procId) $id) { return $true }
    Start-Sleep -Milliseconds 400
  }
  return $false
}

function Find-ByIdDesktop([string]$id) {
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    return [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
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
  } catch { return "INVOKE_FAILED:$id`:$($_.Exception.GetType().Name)" }
}

function Invoke-ByNameDesktop([string]$name) {
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $t = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
    if (-not $t) { return "NOT_FOUND:$name" }
    $pattern = $null
    if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
      $pattern.Invoke()
      return "CLICKED:$name"
    }
    return "NO_INVOKE:$name"
  } catch { return "FAILED:$($_.Exception.Message)" }
}

function Get-Texts([int]$procId) {
  $list = New-Object System.Collections.Generic.List[string]
  $w = Get-Window $procId
  if (-not $w) { return $list }
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Text)
    $all = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    foreach ($t in $all) {
      try { $n = $t.Current.Name; if ($n) { $list.Add($n) } } catch { }
    }
  } catch { }
  return $list
}

function Texts-Contain([int]$procId, [string]$needle) {
  foreach ($t in (Get-Texts $procId)) {
    if ($t.Contains($needle, [System.StringComparison]::Ordinal)) { return $true }
  }
  return $false
}

function Wait-ForText([int]$procId, [string]$needle, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Texts-Contain $procId $needle) { return $true }
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

function Wait-ForLog([string]$path, [string]$needle, [int]$timeoutSec) {
  if (-not (Test-Path $path)) { return $false }
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    try {
      if (Select-String -Path $path -SimpleMatch -Pattern $needle -Quiet) { return $true }
    } catch { }
    Start-Sleep -Milliseconds 300
  }
  return $false
}

function Count-Log([string]$path, [string]$needle) {
  if (-not (Test-Path $path)) { return 0 }
  try { return @(Select-String -Path $path -SimpleMatch -Pattern $needle).Count } catch { return 0 }
}

# 观看舞台上的帧计数：「画面：1920x1080 · 123 帧」；取所有匹配里的最大值。
function Get-MaxFrames([int]$procId) {
  $max = -1
  foreach ($t in (Get-Texts $procId)) {
    $m = [regex]::Match($t, '·\s*(\d+)\s*帧')
    if ($m.Success) {
      $v = [int]$m.Groups[1].Value
      if ($v -gt $max) { $max = $v }
    }
  }
  return $max
}

# 瞬时读 UI 文本可能在会话切换/重绘的那一帧读到「等待首帧」（返回 -1），造成假失败。
# FRAMES_AT_PEAK 改为窗口内多次采样的最大值：只修测量方式，判据仍是「必须 ≥1 帧」。
function Get-MaxFramesOver([int]$procId, [int]$seconds) {
  $peak = -1
  $deadline = (Get-Date).AddSeconds($seconds)
  while ($true) {
    $v = Get-MaxFrames $procId
    if ($v -gt $peak) { $peak = $v }
    if ((Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds 200
  }
  return $peak
}

function Wait-ForFrames([int]$procId, [int]$minFrames, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ((Get-MaxFrames $procId) -ge $minFrames) { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Wait-ForNoFrames([int]$procId, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if ((Get-MaxFrames $procId) -lt 0) { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Select-ChannelByName([int]$procId, [string]$name) {
  # 新版左栏默认停在成员视图：先点频道按钮展开列表，再选中目标频道（选中后列表自动收起）。
  [void](Invoke-ById $procId 'ChooseChannel')
  Start-Sleep -Milliseconds 500
  $w = Get-Window $procId
  if (-not $w) { return 'NO_WINDOW' }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::ListItem)
  $items = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
  foreach ($it in $items) {
    try {
      if ($it.Current.Name -like "*$name*") {
        $sel = $null
        if ($it.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
          $sel.Select()
          return "SELECTED:$name"
        }
        return "NO_SELECTIONITEM:$name"
      }
    } catch { }
  }
  return "NOT_FOUND:$name"
}

# 共享源 ComboBox：先 Expand，再在桌面根上按名字找 ListItem（popup 不在主窗口子树里）。
function Set-ShareSource([int]$procId, [string]$needle) {
  $w = Get-Window $procId
  $combo = Find-ById $w 'ShareSource'
  if (-not $combo) { return 'COMBO_NOT_FOUND' }
  $exp = $null
  if ($combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$exp)) {
    $exp.Expand()
    Start-Sleep -Milliseconds 600
  } else { return 'NO_EXPAND' }
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::ListItem)
    $items = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
      [System.Windows.Automation.TreeScope]::Descendants, $cond)
    $last = $null
    foreach ($it in $items) {
      try {
        $last = $it
        # 条目本体可能没有 Name（模板里只有 TextBlock），退一步读子节点文本。
        $name = $it.Current.Name
        if ([string]::IsNullOrWhiteSpace($name)) {
          $tc = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Text)
          $sub = $it.FindAll([System.Windows.Automation.TreeScope]::Descendants, $tc)
          $name = ($sub | ForEach-Object { $_.Current.Name }) -join ' '
        }
        if ($name -like "*$needle*") {
          $sel = $null
          if ($it.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
            $sel.Select()
            Start-Sleep -Milliseconds 400
            try { $exp.Collapse() } catch { }
            return "SELECTED:$name"
          }
        }
      } catch { }
    }
    # 兜底：合成源固定在列表末尾（引擎固定追加），选中最后一项。
    if ($needle -eq 'Synthetic' -and $last) {
      $sel = $null
      if ($last.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
        $sel.Select()
        Start-Sleep -Milliseconds 400
        try { $exp.Collapse() } catch { }
        return "SELECTED_LAST"
      }
    }
  } catch { }
  try { $exp.Collapse() } catch { }
  return "NOT_FOUND:$needle"
}

function Start-App([string[]]$extraArgs, [string]$logPath) {
  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, "$logPath.err"
  $argsList = @('--server', $Server) + $extraArgs
  return Start-Process -FilePath $clientExe -WorkingDirectory $ClientDir -ArgumentList $argsList `
    -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err" -PassThru
}

function Stop-App($proc) {
  if (-not $proc) { return }
  try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
  Start-Sleep -Milliseconds 600
}

function Take-Shot([int]$procId, [string]$name) {
  $shot = Join-Path $EvidenceDir $name
  try { & (Join-Path $here '..\capture-window.ps1') -ProcessId $procId -OutPath $shot | Out-Null }
  catch { return "SHOT_FAIL:$($_.Exception.Message)" }
  if (Test-Path $shot) { return "SHOT_OK:$shot" }
  return 'SHOT_MISSING'
}

$logA = Join-Path $EvidenceDir 'client-alice.log'
$logB = Join-Path $EvidenceDir 'client-bob.log'
$appA = $null
$appB = $null
try {
  # ---------- 阶段 1：alice 真实入会（真实麦克风） ----------
  $appA = Start-App @('--token', $AliceToken, '--channel', $MainChannel, '--voice-join') $logA
  if (-not (Wait-ForWindow $appA.Id 120)) { Record 'WINDOW_A' 'FAIL'; throw 'alice window did not appear' }
  Record 'WINDOW_A' 'PASS'
  Start-Sleep -Milliseconds 2500
  Record 'LOGIN_A' $(if (Wait-ById $appA.Id 'UserCard' 45) { 'PASS' } else { 'FAIL' })
  Record 'VOICE_JOIN_A' $(if (Wait-ForLog $logA 'voice: joined channel=' 60) { 'PASS' } else { 'FAIL' })
  Record 'VOICE_MODE_A' $(if (Wait-ForLog $logA 'voice: joined channel=' 5) { (Select-String -Path $logA -SimpleMatch 'voice: joined channel=' | Select-Object -Last 1).Line } else { '' })
  Start-Sleep -Milliseconds 800
  Record 'SHOT_A_VOICE' (Take-Shot $appA.Id 'a-voice.png')

  # ---------- 阶段 2：alice 打开共享设置弹窗、选合成源并真实发布 ----------
  Record 'OPEN_SHARE_DIALOG' (Invoke-ById $appA.Id 'Publish')
  $dialogUp = Wait-ById $appA.Id 'ShareSetupStart' 15
  Record 'SHARE_DIALOG_SHOWN' $(if ($dialogUp) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 500
  Record 'SET_SHARE_SOURCE' (Set-ShareSource $appA.Id 'Synthetic')
  Start-Sleep -Milliseconds 400
  Record 'CLICK_PUBLISH' (Invoke-ById $appA.Id 'ShareSetupStart')
  Record 'SHARE_A_STARTED' $(if (Wait-ForLog $logA 'share: started' 30) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 1200
  Record 'SHOT_A_SHARING' (Take-Shot $appA.Id 'a-sharing.png')

  # ---------- 阶段 3：bob 入会并观看 ----------
  $appB = Start-App @('--token', $BobToken, '--channel', $MainChannel, '--voice-join') $logB
  if (-not (Wait-ForWindow $appB.Id 120)) { Record 'WINDOW_B' 'FAIL'; throw 'bob window did not appear' }
  Record 'WINDOW_B' 'PASS'
  Start-Sleep -Milliseconds 2500
  Record 'LOGIN_B' $(if (Wait-ById $appB.Id 'UserCard' 45) { 'PASS' } else { 'FAIL' })
  Record 'VOICE_JOIN_B' $(if (Wait-ForLog $logB 'voice: joined channel=' 60) { 'PASS' } else { 'FAIL' })
  Record 'SHOT_B_VOICE' (Take-Shot $appB.Id 'b-voice.png')

  # 观看按钮的 AutomationId 就是共享者 member id（成员卡片模板）。
  $btn = $null
  $deadline = (Get-Date).AddSeconds(40)
  while ((Get-Date) -lt $deadline) {
    $btn = Find-ById (Get-Window $appB.Id) $AliceId
    if ($btn) { break }
    Start-Sleep -Milliseconds 500
  }
  Record 'WATCH_BUTTON_VISIBLE' $(if ($btn) { 'PASS' } else { 'NOT_FOUND' })
  Record 'CLICK_WATCH' $(if ($btn) { (Invoke-ById $appB.Id $AliceId) } else { 'SKIPPED' })
  Record 'WATCH_ACCEPTED' $(if (Wait-ForLog $logB 'gui: watching' 45) { 'PASS' } else { 'FAIL' })
  Record 'FIRST_FRAME' $(if (Wait-ForFrames $appB.Id 1 45) { 'PASS' } else { 'FAIL' })
  Record 'FRAMES_AT_PEAK' $(Get-MaxFramesOver $appB.Id 3)
  Start-Sleep -Milliseconds 800
  Record 'SHOT_B_WATCHING' (Take-Shot $appB.Id 'b-watching.png')

  # ---------- 阶段 4：bob 停止观看（只停接收，alice 仍在共享） ----------
  $framesBeforeStop = Get-MaxFrames $appB.Id
  Record 'CLICK_STOP_WATCH' (Invoke-ById $appB.Id 'StopWatch')
  Record 'WATCH_UI_CLEARED' $(if (Wait-ForNoFrames $appB.Id 20) { 'PASS' } else { 'FAIL' })
  $shareStoppedOnA = Count-Log $logA 'share: stopped'
  Record 'A_STILL_SHARING_AFTER_B_STOP' $(if ($shareStoppedOnA -eq 0) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 600
  Record 'SHOT_B_STOP_WATCH' (Take-Shot $appB.Id 'b-stop-watch.png')

  # ---------- 阶段 5：alice 停止共享 → bob 的观看入口消失 ----------
  Record 'CLICK_STOP_PUBLISH' (Invoke-ById $appA.Id 'StopPublish')
  Record 'SHARE_A_STOPPED' $(if (Wait-ForLog $logA 'share: stopped' 20) { 'PASS' } else { 'FAIL' })
  $gone = $false
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    if (-not (Find-ById (Get-Window $appB.Id) $AliceId)) { $gone = $true; break }
    Start-Sleep -Milliseconds 500
  }
  Record 'SHARE_REMOVED_ON_B' $(if ($gone) { 'PASS' } else { 'FAIL' })

  # ---------- 阶段 6：共享中转移频道 —— 必须确认，且转移前先停共享 ----------
  Record 'CLICK_PUBLISH_AGAIN' (Invoke-ById $appA.Id 'Publish')
  Start-Sleep -Milliseconds 600
  Record 'CLICK_PUBLISH_AGAIN_START' (Invoke-ById $appA.Id 'ShareSetupStart')
  Record 'SHARE_A_RESTARTED' $(if (Wait-ForLog $logA 'share: started' 30) { 'PASS' } else { 'FAIL' })

  # ---------- 阶段 6b：先看后发（服务端 codec 未知）也必须出帧 ----------
  # 共享刚重启、发布轨尚未 attach 时立刻点观看：服务端此刻拿不到发布者 codec
  # （日志 publisher_codec_known=false），只能先建占位轨、等发布轨 attach 时换轨并
  # 请求关键帧。这是本缺陷的第二条真机时序，必须证明它能出帧，不能用「脚本点得慢」掩盖。
  # 若本次仍晚于 attach（known=true），最多重试 2 次：只重试时序，不放宽任何断言。
  $codecUnknownFramesOK = $false
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    if ($attempt -gt 1) {
      [void](Invoke-ById $appB.Id 'StopWatch')
      Start-Sleep -Milliseconds 600
      $stoppedBefore = Count-Log $logA 'share: stopped'
      [void](Invoke-ById $appA.Id 'StopPublish')
      $dlStop = (Get-Date).AddSeconds(20)
      while ((Get-Date) -lt $dlStop) {
        if ((Count-Log $logA 'share: stopped') -gt $stoppedBefore) { break }
        Start-Sleep -Milliseconds 300
      }
      [void](Invoke-ById $appA.Id 'Publish')
      Start-Sleep -Milliseconds 600
      [void](Invoke-ById $appA.Id 'ShareSetupStart')
      $startedBefore = Count-Log $logA 'share: started'
      $dlStart = (Get-Date).AddSeconds(30)
      while ((Get-Date) -lt $dlStart) {
        if ((Count-Log $logA 'share: started') -gt $startedBefore) { break }
        Start-Sleep -Milliseconds 200
      }
    }
    # 服务端日志基线：只认本次尝试新出现的 publisher_codec_known=false。
    $unknownBefore = 0
    if ($ServerLog -and (Test-Path $ServerLog)) {
      $unknownBefore = @(Select-String -Path $ServerLog -SimpleMatch 'publisher_codec_known=false').Count
    }
    # 100ms 轮询：共享状态一出现就点观看，尽量落在发布轨 attach 之前。
    $rwBtn = $null
    $dlBtn = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $dlBtn) {
      $rwBtn = Find-ById (Get-Window $appB.Id) $AliceId
      if ($rwBtn) { break }
      Start-Sleep -Milliseconds 100
    }
    Record "REWATCH_BUTTON_$attempt" $(if ($rwBtn) { 'PASS' } else { 'NOT_FOUND' })
    if (-not $rwBtn) { continue }
    $watchBefore = Count-Log $logB 'gui: watching'
    # 帧证据用**日志行增量**而不是 UI 文本：UI 文本可能还残留上一次会话的计数，
    # 只有「点击之后客户端又打出新的 share: remote frame 行」才能证明新会话出帧。
    $frameLinesBefore = Count-Log $logB 'share: remote frame'
    Record "REWATCH_CLICK_$attempt" (Invoke-ById $appB.Id $AliceId)
    $accepted = $false
    $dlAccept = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $dlAccept) {
      if ((Count-Log $logB 'gui: watching') -gt $watchBefore) { $accepted = $true; break }
      Start-Sleep -Milliseconds 200
    }
    Record "REWATCH_ACCEPTED_$attempt" $(if ($accepted) { 'PASS' } else { 'FAIL' })
    # 新会话出帧 = 点击后客户端日志里新增 share: remote frame 行（旧会话已解绑）。
    $newFrame = $false
    $dlFrame = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $dlFrame) {
      if ((Count-Log $logB 'share: remote frame') -gt $frameLinesBefore) { $newFrame = $true; break }
      Start-Sleep -Milliseconds 200
    }
    $framesOK = $newFrame
    Record "REWATCH_NEW_FRAME_$attempt" $(if ($newFrame) { 'PASS' } else { 'FAIL' })
    Record "REWATCH_FIRST_FRAME_$attempt" $(if (Wait-ForFrames $appB.Id 1 20) { 'PASS' } else { 'FAIL' })
    Record "REWATCH_FRAMES_AT_PEAK_$attempt" (Get-MaxFramesOver $appB.Id 2)
    if ($ServerLog -and (Test-Path $ServerLog)) {
      $unknownNow = @(Select-String -Path $ServerLog -SimpleMatch 'publisher_codec_known=false').Count
      Record "REWATCH_SERVER_CODEC_UNKNOWN_$attempt" ([string]($unknownNow - $unknownBefore))
      if ($unknownNow -gt $unknownBefore) {
        $lastLine = [string](@(Select-String -Path $ServerLog -SimpleMatch 'publisher_codec_known=false')[-1].Line)
        Record "REWATCH_UNKNOWN_SESSION_LINE_$attempt" ('SET:' + $lastLine.Trim())
        $sm = [regex]::Match($lastLine, 'session=([0-9a-f]+)')
        if ($sm.Success) { Record "REWATCH_UNKNOWN_SESSION_$attempt" ('SET:' + $sm.Groups[1].Value) }
        if ($framesOK) { $codecUnknownFramesOK = $true }
        break
      }
    } else {
      Record "REWATCH_SERVER_CODEC_UNKNOWN_$attempt" 'NO_SERVER_LOG'
      break
    }
  }
  Record 'CODEC_UNKNOWN_PATH_WITH_FRAMES' $(if ($codecUnknownFramesOK) { 'PASS' } else { 'FAIL' })
  Record 'SELECT_ALT_CHANNEL' (Select-ChannelByName $appA.Id $AltChannel)
  Start-Sleep -Milliseconds 800
  Record 'CLICK_JOIN_ALT' (Invoke-ById $appA.Id 'JoinCall')
  Record 'TRANSFER_CONFIRM_SHOWN' $(if (Wait-ForText $appA.Id '会停止当前共享' 20) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 600
  Record 'SHOT_A_TRANSFER_CONFIRM' (Take-Shot $appA.Id 'a-transfer-confirm.png')
  Record 'CLICK_CONFIRM_TRANSFER' (Invoke-ByNameDesktop '确认转移')
  Record 'TRANSFER_STOPPED_SHARE' $(if (Wait-ForLog $logA 'sharing stopped before leaving call' 25) { 'PASS' } else { 'FAIL' })
  Record 'TRANSFER_JOINED_ALT' $(if (Wait-ForLog $logA 'voice: joined channel=' 45) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 1200
  Record 'SHOT_A_AFTER_TRANSFER' (Take-Shot $appA.Id 'a-after-transfer.png')
  # bob 仍在主频道：alice 转移后该频道的共享必须已经消失。
  $goneAfterTransfer = $true
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    if (Find-ById (Get-Window $appB.Id) $AliceId) { Start-Sleep -Milliseconds 500; continue }
    $goneAfterTransfer = $false
    break
  }
  Record 'OLD_CHANNEL_SHARE_CLEARED' $(if ($goneAfterTransfer) { 'PASS' } else { 'FAIL' })

  # ---------- 阶段 7：收尾（默认把两个客户端留下给用户自由测试） ----------
  Record 'SHOT_FINAL_A' (Take-Shot $appA.Id 'a-final.png')
  Record 'SHOT_FINAL_B' (Take-Shot $appB.Id 'b-final.png')
  Record 'PID_A' ([string]$appA.Id)
  Record 'PID_B' ([string]$appB.Id)
  if (-not $KeepClients) {
    [void](Invoke-ById $appA.Id 'Leave')
    [void](Invoke-ById $appB.Id 'Leave')
    Start-Sleep -Milliseconds 800
  }
} catch {
  # 驱动自身异常也要留证据：否则只看到「缺了某几项」而不知道断在哪。
  Record 'EXCEPTION' ($_.Exception.Message + ' @ ' + ($_.InvocationInfo.PositionMessage -replace "`r?`n", ' '))
} finally {
  $evidence = [ordered]@{
    scenario    = 'p2-voice-screen-real'
    server      = $Server
    clientExe   = $clientExe
    clientHash  = $(try { (Get-FileHash -Algorithm SHA256 $clientExe).Hash } catch { '' })
    capturedAt  = (Get-Date).ToString('o')
    mainChannel = $MainChannel
    altChannel  = $AltChannel
    aliceId     = $AliceId
    bobId       = $BobId
    checks      = $checks
    aliceLog    = $logA
    bobLog      = $logB
  }
  $evidence | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'p2-evidence.json')
  Write-Output "EVIDENCE=$EvidenceDir"
  $bad = @($checks.Keys | Where-Object {
      $v = [string]$checks[$_]
      -not ($v -match '^(PASS|CLICKED:|SELECTED:|SET:|SHOT_OK:|PID_|WINDOW_)') -and $v -notmatch '^\d+$' -and $v -ne ''
    })
  if ($bad.Count -gt 0) {
    Write-Output ("FAILED_CHECKS=" + ($bad -join ','))
    exit 1
  }
  Write-Output 'ALL_CHECKS_PASS'
  exit 0
}
