param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$Server,
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [string]$User = 'alice',
  [string]$Password = 'alice-pass-123',
  [string]$WrongPassword = 'definitely-wrong-password',
  [string]$ExpectChannel = '开黑大厅',
  [string]$ExpectDisplayName = '爱丽丝',
  [string]$ExpectMessage = '大家好，这是真实消息',
  [string]$SecondChannel = '',
  [string]$SecondChannelMessage = '',
  [string]$RealtimeToken = '',
  [string]$RealtimeText = '',
  [switch]$SkipRegistration
)

# v1.1 重构外壳的真实 Windows 验收：真实登录 → 真实频道 → 真实历史消息 → 真实名册，
# 再验错误密码的就近报错，以及（可选）注册面板能否进入。
#
# 只做 UI 驱动与取证，不做断言美化：每一步的 PASS/FAIL 都写进 KEY=VALUE 与 evidence.json。
# 退出码 0 = 场景跑完（结论看 checks），非 0 = 驱动本身失败。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$clientExe = Join-Path $ClientDir 'PartyUp.Client.exe'
if (-not (Test-Path $clientExe)) { throw "client exe not found: $clientExe" }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

$checks = [ordered]@{}
$results = [ordered]@{}
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
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Wait-ById([int]$procId, [string]$id, [int]$timeoutSec) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Find-ById (Get-Window $procId) $id) { return $true }
    Start-Sleep -Milliseconds 400
  }
  return $false
}

function Set-Text($procId, [string]$id, [string]$value) {
  $w = Get-Window $procId
  $t = Find-ById $w $id
  if (-not $t) { return "NOT_FOUND:$id" }
  $vp = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) {
    $vp.SetValue($value)
    return "SET:$id"
  }
  return "NO_VALUEPATTERN:$id"
}

function Invoke-ById($procId, [string]$id) {
  $w = Get-Window $procId
  $t = Find-ById $w $id
  if (-not $t) { return "NOT_FOUND:$id" }
  $pattern = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
    $pattern.Invoke()
    return "CLICKED:$id"
  }
  return "NO_INVOKE:$id"
}

# 菜单项/弹出层可能不在主窗口的 UIA 子树里：退到桌面根再按 Name 找一次。
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
    $sel = $null
    if ($t.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
      $sel.Select()
      return "SELECTED:$name"
    }
    return "NO_INVOKE:$name"
  } catch {
    return "FAILED:$($_.Exception.Message)"
  }
}

# 逐属性 try/catch：UIA 树里任何异常都不能让整轮验收挂掉。
function Get-Texts($procId) {
  $list = New-Object System.Collections.Generic.List[string]
  $w = Get-Window $procId
  if (-not $w) { return $list }
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Text)
    $all = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    foreach ($t in $all) {
      try {
        $n = $t.Current.Name
        if ($n) { $list.Add($n) }
      } catch { }
    }
  } catch { }
  return $list
}

function Texts-Contain($procId, [string]$needle) {
  # 用 Ordinal Contains 而不是 PowerShell -like（消息里带 [ ] 时通配会误判）。
  $texts = Get-Texts $procId
  foreach ($t in $texts) {
    if ($t.Contains($needle, [System.StringComparison]::Ordinal)) { return $true }
  }
  return $false
}

function Get-IsEnabled($procId, [string]$id) {
  $w = Get-Window $procId
  $t = Find-ById $w $id
  if (-not $t) { return $null }
  try { return $t.Current.IsEnabled } catch { return $null }
}

function Wait-ForText($procId, [string]$needle, [int]$timeoutSec) {
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

# 左栏频道列表项：ListBoxItem 支持 SelectionItemPattern；Select() 在 Avalonia 的 ListBox
# 上会真正改变 SelectedItem（这一点与分页导航不同，已在本轮验收中确认）。
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

function Start-App([string[]]$extraArgs, [string]$logPath) {
  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, "$logPath.err"
  $argsList = @('--server', $Server) + $extraArgs
  $p = Start-Process -FilePath $clientExe -WorkingDirectory $ClientDir -ArgumentList $argsList `
    -RedirectStandardOutput $logPath -RedirectStandardError "$logPath.err" -PassThru
  return $p
}

function Stop-App($proc) {
  if (-not $proc) { return }
  try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
  Start-Sleep -Milliseconds 600
}

function Take-Shot([int]$procId, [string]$name) {
  $shot = Join-Path $EvidenceDir $name
  try {
    & (Join-Path $here '..\capture-window.ps1') -ProcessId $procId -OutPath $shot | Out-Null
  } catch {
    return "SHOT_FAIL:$($_.Exception.Message)"
  }
  if (Test-Path $shot) { return "SHOT_OK:$shot" }
  return 'SHOT_MISSING'
}

$logLogin = Join-Path $EvidenceDir 'client-login.log'
$logWrong = Join-Path $EvidenceDir 'client-wrong-password.log'
$app = $null
try {
  # ---------- 阶段 1：登录页 → 真实登录 → 真实频道 ----------
  $app = Start-App @() $logLogin
  $win = Wait-ForWindow $app.Id 120
  if (-not $win) { Record 'WINDOW' 'FAIL'; throw 'client window did not appear' }
  Record 'WINDOW' 'PASS'
  # 等 Avalonia 把首帧画完并完成窗口尺寸设置，否则首张截图可能抓到创建期的小窗口。
  Start-Sleep -Milliseconds 2500

  Record 'LOGIN_PAGE' $(if (Wait-ForText $app.Id '登录你的服务器' 20) { 'PASS' } else { 'FAIL' })
  Record 'SHOT_LOGIN' (Take-Shot $app.Id 'login.png')

  Record 'SET_SERVER' (Set-Text $app.Id 'ServerAddress' $Server)
  Record 'SET_USER' (Set-Text $app.Id 'LoginUser' $User)
  Record 'SET_PASSWORD' (Set-Text $app.Id 'LoginPassword' $Password)
  Start-Sleep -Milliseconds 300
  Record 'CLICK_LOGIN' (Invoke-ById $app.Id 'Login')

  # 真实登录成功信号：左栏出现用户卡片/频道入口，且频道名/展示名来自服务端。
  Record 'MAIN_SHELL' $(if (Wait-ById $app.Id 'UserCard' 45) { 'PASS' } else { 'FAIL' })
  Record 'CHANNEL_LISTED' $(if (Texts-Contain $app.Id $ExpectChannel) { 'PASS' } else { 'FAIL' })
  Record 'DISPLAY_NAME' $(if (Texts-Contain $app.Id $ExpectDisplayName) { 'PASS' } else { 'FAIL' })
  Record 'HISTORY_MESSAGE' $(if (Texts-Contain $app.Id $ExpectMessage) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 800
  Record 'SHOT_MAIN' (Take-Shot $app.Id 'main.png')

  # ---------- 阶段 1a2：切浏览频道（单击只浏览，不碰通话状态） ----------
  if ($SecondChannel -ne '' -and $SecondChannelMessage -ne '') {
    Record 'CHANNEL_LISTED_SECOND' $(if (Texts-Contain $app.Id $SecondChannel) { 'PASS' } else { 'FAIL' })
    Record 'BROWSE_SWITCH_CLICK' (Select-ChannelByName $app.Id $SecondChannel)
    Record 'BROWSE_SWITCHED' $(if (Wait-ForText $app.Id $SecondChannelMessage 20) { 'PASS' } else { 'FAIL' })
    # 切浏览不得把通话条变成「通话中」：本用例当前未加入语音，所以必须仍是「未加入」。
    Record 'BROWSE_DID_NOT_JOIN_CALL' $(if (Texts-Contain $app.Id '未加入频道语音') { 'PASS' } else { 'FAIL' })
    Start-Sleep -Milliseconds 600
    Record 'SHOT_BROWSE_SWITCH' (Take-Shot $app.Id 'browse-switch.png')
    # 切回第一个频道，后续步骤仍在原频道上进行。
    [void](Select-ChannelByName $app.Id $ExpectChannel)
    [void](Wait-ForText $app.Id $ExpectMessage 15)
  }

  # ---------- 阶段 1b：成员视图（全服名册三组） ----------
  # 新版左栏没有「返回成员列表」按钮：顶部频道按钮在列表/成员之间切换；点回当前频道不会发请求，
  # 所以选中后再点一次按钮（若还停在列表）即回到成员视图。
  Record 'MEMBERS_CLICK' (Select-ChannelByName $app.Id $ExpectChannel)
  if (-not (Wait-ForText $app.Id '频道中' 5)) { [void](Invoke-ById $app.Id 'ChooseChannel') }
  Record 'MEMBERS_GROUPS' $(if (Wait-ForText $app.Id '频道中' 12) { 'PASS' } else { 'FAIL' })
  Record 'MEMBERS_OFFLINE_GROUP' $(if (Texts-Contain $app.Id '离线') { 'PASS' } else { 'FAIL' })
  # bob 只注册过、从未连接：名册应把他放进离线组（MEM-01 的真实证据）。
  Record 'MEMBERS_REMOTE_USER' $(if (Texts-Contain $app.Id '鲍勃') { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 500
  Record 'SHOT_MEMBERS' (Take-Shot $app.Id 'members.png')

  # ---------- 阶段 1c：从 GUI 真实发一条消息（走 text.send，不是本地插入） ----------
  $guiBody = "来自重构外壳的真实消息 $([DateTime]::Now.ToString('HH:mm:ss'))"
  Record 'SET_MESSAGE' (Set-Text $app.Id 'MessageInput' $guiBody)
  Start-Sleep -Milliseconds 300
  Record 'CLICK_SEND' (Invoke-ById $app.Id 'SendMessage')
  # 消息经服务端回推（text.new）后才会出现在列表里；这里等回显而不是等本地乐观插入。
  Record 'SEND_ECHO' $(if (Wait-ForText $app.Id $guiBody 20) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 800
  Record 'SHOT_SENT' (Take-Shot $app.Id 'sent.png')

  # ---------- 阶段 1c2：另一真实进程发消息，验证实时推送（不是历史回放） ----------
  if ($RealtimeToken -ne '' -and $RealtimeText -ne '') {
    $senderLog = Join-Path $EvidenceDir 'realtime-sender.log'
    try {
      $sender = Start-Process -FilePath $clientExe -WorkingDirectory $ClientDir -PassThru `
        -ArgumentList @('--role', 'send', '--server', $Server, '--token', $RealtimeToken,
                        '--channel', $ExpectChannel, '--text', $RealtimeText) `
        -RedirectStandardOutput $senderLog -RedirectStandardError "$senderLog.err"
      $sender.WaitForExit(30000) | Out-Null
      Record 'REALTIME_SENDER_EXIT' $(if ($sender.HasExited) { "CODE=$($sender.ExitCode)" } else { 'TIMEOUT' })
    } catch {
      Record 'REALTIME_SENDER_EXIT' "FAILED:$($_.Exception.Message)"
    }
    Record 'REALTIME_ECHO' $(if (Wait-ForText $app.Id $RealtimeText 20) { 'PASS' } else { 'FAIL' })
    Start-Sleep -Milliseconds 600
    Record 'SHOT_REALTIME' (Take-Shot $app.Id 'realtime.png')
  }

  # ---------- 阶段 1d：设置抽屉（外观真实生效；未接能力禁用） ----------
  Record 'SETTINGS_OPEN_CARD' (Invoke-ById $app.Id 'UserCard')
  Start-Sleep -Milliseconds 500
  Record 'SETTINGS_CLICK_ITEM' (Invoke-ByNameDesktop '应用设置')
  Record 'SETTINGS_PANEL' $(if (Wait-ForText $app.Id '外观' 15) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 600
  Record 'SHOT_SETTINGS' (Take-Shot $app.Id 'settings.png')

  # ---------- 阶段 1e：退出登录 → 立即重新登录（B1 回归：忙态必须复位） ----------
  Record 'LOGOUT_OPEN_CARD' (Invoke-ById $app.Id 'UserCard')
  Start-Sleep -Milliseconds 400
  Record 'LOGOUT_CLICK' (Invoke-ByNameDesktop '退出登录')
  Record 'LOGOUT_RETURNS_LOGIN' $(if (Wait-ForText $app.Id '登录你的服务器' 15) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 400
  $loginEnabled = Get-IsEnabled $app.Id 'Login'
  Record 'LOGOUT_LOGIN_ENABLED' $(
    if ($loginEnabled -eq $null) { 'UNKNOWN' }
    elseif ($loginEnabled) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 300
  Record 'SHOT_LOGOUT' (Take-Shot $app.Id 'logout.png')

  [void](Set-Text $app.Id 'ServerAddress' $Server)
  [void](Set-Text $app.Id 'LoginUser' $User)
  [void](Set-Text $app.Id 'LoginPassword' $Password)
  Start-Sleep -Milliseconds 200
  Record 'RELOGIN_CLICK' (Invoke-ById $app.Id 'Login')
  Record 'RELOGIN_OK' $(if (Wait-ById $app.Id 'UserCard' 40) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 600
  Record 'SHOT_RELOGIN' (Take-Shot $app.Id 'relogin.png')

  # ---------- 阶段 2：错误密码 → 就近报错且可重试 ----------
  Stop-App $app
  $app = Start-App @() $logWrong
  $win = Wait-ForWindow $app.Id 120
  if (-not $win) { Record 'WRONG_WINDOW' 'FAIL' } else {
    Record 'WRONG_WINDOW' 'PASS'
    Start-Sleep -Milliseconds 800
    [void](Set-Text $app.Id 'ServerAddress' $Server)
    [void](Set-Text $app.Id 'LoginUser' $User)
    [void](Set-Text $app.Id 'LoginPassword' $WrongPassword)
    Start-Sleep -Milliseconds 200
    Record 'WRONG_CLICK_LOGIN' (Invoke-ById $app.Id 'Login')
    Record 'WRONG_ERROR_SHOWN' $(if (Wait-ForText $app.Id '用户名或密码不正确' 30) { 'PASS' } else { 'FAIL' })
    Record 'WRONG_STILL_LOGIN_PAGE' $(if (Texts-Contain $app.Id '登录你的服务器') { 'PASS' } else { 'FAIL' })
    Record 'SHOT_WRONG' (Take-Shot $app.Id 'wrong-password.png')

    # ---------- 阶段 3：注册面板入口（真实分步注册，不在本轮提交邀请码） ----------
    if (-not $SkipRegistration) {
      Record 'REG_CLICK' (Invoke-ById $app.Id 'BeginRegistration')
      Record 'REG_PANEL' $(if (Wait-ForText $app.Id '先找到你的服务器' 20) { 'PASS' } else { 'FAIL' })
      Start-Sleep -Milliseconds 500
      Record 'SHOT_REG' (Take-Shot $app.Id 'registration.png')
    }
  }
}
finally {
  Stop-App $app
}

# 证据：JSON + 原始日志尾部（日志里不含密码；邀请码只在注册流程用，本脚本不填）。
$evidence = [ordered]@{
  scenario         = 'shell-real-login'
  server           = $Server
  clientExe        = $clientExe
  clientExeSha256  = $(try { (Get-FileHash -Algorithm SHA256 $clientExe).Hash } catch { '' })
  capturedAt       = (Get-Date).ToString('o')
  checks           = $checks
  loginLogTail     = $(if (Test-Path $logLogin) { (Get-Content $logLogin -Tail 40) -join "`n" } else { '' })
  wrongLogTail     = $(if (Test-Path $logWrong) { (Get-Content $logWrong -Tail 40) -join "`n" } else { '' })
}
$evidence | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'evidence.json')
Write-Output "EVIDENCE=$EvidenceDir"

# 只有每一项都是成功形态才算通过；有失败就以非零退出（简报 §11：不能恒 exit 0）。
$bad = @($checks.Keys | Where-Object {
    $v = [string]$checks[$_]
    -not ($v -match '^(PASS|SET:|CLICKED:|SELECTED:|SHOT_OK:|CODE=0)')
  })
if ($bad.Count -gt 0) {
  Write-Output ("FAILED_CHECKS=" + ($bad -join ','))
  exit 1
}
Write-Output 'ALL_CHECKS_PASS'
exit 0
