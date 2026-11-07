param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$Server,
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [Parameter(Mandatory = $true)][string]$InviteLink,
  [Parameter(Mandatory = $true)][string]$NewUser,
  [Parameter(Mandatory = $true)][string]$NewDisplayName,
  [Parameter(Mandatory = $true)][string]$NewPassword,
  [string]$BadInvite = 'ZZZZZZZZZZ'
)

# P1b 分步注册的真实 Windows 走查（REG-01/02/03/04 客户端接线）：
#   1) 带 code 的邀请链接 → 真实非消费预检 → 填账户 → 真实注册 → 自动登录进主界面
#   2) 另一实例：裸服务器地址 + 错邀请码 → 服务端 404 的就地错误文案
# 每个结论写 KEY=VALUE 与 evidence.json；退出码 0 表示场景跑完。
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

function Get-Texts($procId) {
  $list = New-Object System.Collections.Generic.List[string]
  $w = Get-Window $procId
  if (-not $w) { return $list }
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Text)
    foreach ($t in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
      try { if ($t.Current.Name) { $list.Add($t.Current.Name) } } catch { }
    }
  } catch { }
  return $list
}

function Texts-Contain($procId, [string]$needle) {
  foreach ($t in (Get-Texts $procId)) { if ($t -like "*$needle*") { return $true } }
  return $false
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

function Start-App([string]$logPath) {
  Remove-Item -Force -ErrorAction SilentlyContinue $logPath, "$logPath.err"
  return Start-Process -FilePath $clientExe -WorkingDirectory $ClientDir `
    -ArgumentList @('--server', $Server) `
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

$logHappy = Join-Path $EvidenceDir 'client-register.log'
$logBad = Join-Path $EvidenceDir 'client-register-bad-invite.log'
$app = $null
try {
  # ---------- 阶段 1：邀请链接 → 真实预检 → 真实注册 → 自动登录 ----------
  $app = Start-App $logHappy
  if (-not (Wait-ForWindow $app.Id 120)) { Record 'WINDOW' 'FAIL'; throw 'window did not appear' }
  Record 'WINDOW' 'PASS'
  Start-Sleep -Milliseconds 2000

  Record 'OPEN_REGISTRATION' (Invoke-ById $app.Id 'BeginRegistration')
  Record 'REG_STEP0' $(if (Wait-ForText $app.Id '先找到你的服务器' 15) { 'PASS' } else { 'FAIL' })

  # 带 code 的邀请链接：解析 + 非消费预检应直接推进到账户步骤。
  Record 'SET_INVITE_LINK' (Set-Text $app.Id 'RegistrationTarget' $InviteLink)
  Start-Sleep -Milliseconds 300
  Record 'CLICK_CONTINUE' (Invoke-ById $app.Id 'ContinueRegistrationTarget')
  Record 'REG_STEP2_AUTO_VERIFY' $(if (Wait-ForText $app.Id '创建你的账户' 25) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 500
  Record 'SHOT_REG_ACCOUNT' (Take-Shot $app.Id 'register-account.png')

  Record 'SET_NEW_USER' (Set-Text $app.Id 'RegistrationUsername' $NewUser)
  Record 'SET_NEW_DISPLAY' (Set-Text $app.Id 'RegistrationDisplayName' $NewDisplayName)
  Record 'SET_NEW_PASSWORD' (Set-Text $app.Id 'RegistrationPassword' $NewPassword)
  Record 'SET_CONFIRM_PASSWORD' (Set-Text $app.Id 'RegistrationConfirmPassword' $NewPassword)
  Start-Sleep -Milliseconds 400
  Record 'CLICK_CREATE' (Invoke-ById $app.Id 'CreateRegistrationAccount')

  # 真实注册成功 = 自动登录进主界面，且左栏显示新账号的展示名。
  Record 'REG_AUTO_LOGIN' $(if (Wait-ById $app.Id 'UserCard' 40) { 'PASS' } else { 'FAIL' })
  Record 'REG_DISPLAY_NAME' $(if (Texts-Contain $app.Id $NewDisplayName) { 'PASS' } else { 'FAIL' })
  Start-Sleep -Milliseconds 800
  Record 'SHOT_REG_LOGGED_IN' (Take-Shot $app.Id 'register-logged-in.png')
  Stop-App $app

  # ---------- 阶段 2：裸服务器地址 + 错邀请码 → 服务端 404 的就地错误 ----------
  $app = Start-App $logBad
  if (-not (Wait-ForWindow $app.Id 120)) { Record 'BAD_WINDOW' 'FAIL' } else {
    Record 'BAD_WINDOW' 'PASS'
    Start-Sleep -Milliseconds 2000
    [void](Invoke-ById $app.Id 'BeginRegistration')
    [void](Wait-ForText $app.Id '先找到你的服务器' 15)
    Record 'BAD_SET_SERVER' (Set-Text $app.Id 'RegistrationTarget' $Server)
    Start-Sleep -Milliseconds 300
    Record 'BAD_CLICK_CONTINUE' (Invoke-ById $app.Id 'ContinueRegistrationTarget')
    Record 'BAD_STEP1' $(if (Wait-ForText $app.Id '确认加入资格' 15) { 'PASS' } else { 'FAIL' })
    Record 'BAD_SET_INVITE' (Set-Text $app.Id 'RegistrationInviteCode' $BadInvite)
    Start-Sleep -Milliseconds 300
    Record 'BAD_CLICK_VERIFY' (Invoke-ById $app.Id 'VerifyRegistrationInvite')
    Record 'BAD_ERROR' $(if (Wait-ForText $app.Id '邀请码不存在' 20) { 'PASS' } else { 'FAIL' })
    Record 'BAD_STAYS_STEP1' $(if (Texts-Contain $app.Id '确认加入资格') { 'PASS' } else { 'FAIL' })
    Start-Sleep -Milliseconds 500
    Record 'SHOT_REG_BAD_INVITE' (Take-Shot $app.Id 'register-bad-invite.png')
  }
}
finally {
  Stop-App $app
}

$evidence = [ordered]@{
  scenario     = 'shell-registration'
  server       = $Server
  clientExe    = $clientExe
  capturedAt   = (Get-Date).ToString('o')
  checks       = $checks
  happyLogTail = $(if (Test-Path $logHappy) { (Get-Content $logHappy -Tail 40) -join "`n" } else { '' })
  badLogTail   = $(if (Test-Path $logBad) { (Get-Content $logBad -Tail 30) -join "`n" } else { '' })
}
$evidence | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $EvidenceDir 'registration-evidence.json')
Write-Output "EVIDENCE=$EvidenceDir"

# 有失败就以非零退出（简报 §11：不能恒 exit 0）。
$bad = @($checks.Keys | Where-Object {
    $v = [string]$checks[$_]
    -not ($v -match '^(PASS|SET:|CLICKED:|SELECTED:|SHOT_OK:)')
  })
if ($bad.Count -gt 0) {
  Write-Output ("FAILED_CHECKS=" + ($bad -join ','))
  exit 1
}
Write-Output 'ALL_CHECKS_PASS'
exit 0
