param(
  [string]$ClientExe = '',
  [int]$Port = 18085,
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence-desktop"
)

# UI 端到端：设置页勾选框 → 真实注册表 / 真实 close-to-tray。
# desktop-integration 只验了服务层，这里验的是「点了勾选框之后系统里到底变了没有」。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
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
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) { $pat.Invoke(); return "CLICKED:$id" }
  return "NO_INVOKE:$id"
}
function Toggle-ById([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return "NOT_FOUND:$id" }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pat)) {
    $pat.Toggle(); return ("TOGGLED:$id => " + $pat.Current.ToggleState)
  }
  return "NO_TOGGLE:$id"
}
function Toggle-State([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return 'MISSING' }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pat)) { return [string]$pat.Current.ToggleState }
  return 'NO_PATTERN'
}
function Reg-Run() {
  $o = & reg.exe query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' /v PartyUp 2>&1
  return ($o -join ' | ')
}
function Visible([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p) { return 'PROCESS_GONE' }
  return ('' + (-not $p.MainWindowHandle.Equals([IntPtr]::Zero)))
}

$base = "http://127.0.0.1:$Port"
$body = @{ username = 'tester'; password = 'tester-pass-123' } | ConvertTo-Json
$token = (Invoke-RestMethod -Uri "$base/api/login" -Method Post -Body $body -ContentType 'application/json').token
Log 'LOGIN_OK'

$settingsFile = Join-Path $env:APPDATA 'PartyUp\settings.json'
New-Item -ItemType Directory -Force -Path (Split-Path $settingsFile) | Out-Null

$p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) -ArgumentList @('--server', $base, '--token', $token, '--screen', '2') -PassThru
Log "client pid=$($p.Id)"
Start-Sleep -Seconds 8
Log (Invoke-ById $p.Id 'UserCard')
Start-Sleep -Milliseconds 900
Log (Invoke-ById $p.Id 'OpenSettings')
Start-Sleep -Seconds 3

Log ("REG_INITIAL=" + (Reg-Run))
Log ("CHECKBOX_INITIAL=" + (Toggle-State $p.Id 'LaunchAtStartup'))

Log (Toggle-ById $p.Id 'LaunchAtStartup')
Start-Sleep -Seconds 2
Log ("CHECKBOX_AFTER_ON=" + (Toggle-State $p.Id 'LaunchAtStartup'))
Log ("REG_AFTER_ON=" + (Reg-Run))

Log (Toggle-ById $p.Id 'LaunchAtStartup')
Start-Sleep -Seconds 2
Log ("CHECKBOX_AFTER_OFF=" + (Toggle-State $p.Id 'LaunchAtStartup'))
Log ("REG_AFTER_OFF=" + (Reg-Run))

# close-to-tray：勾上 → 关窗口 → 进程应存活且窗口不可见
Log (Toggle-ById $p.Id 'CloseToTray')
Start-Sleep -Milliseconds 800
Log ("CLOSETOTRAY_STATE=" + (Toggle-State $p.Id 'CloseToTray'))
$proc = Get-Process -Id $p.Id
[void]$proc.CloseMainWindow()
Start-Sleep -Seconds 2
Log ("ALIVE_AFTER_WMCLOSE=" + ($null -ne (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)))
Log ("VISIBLE_AFTER_WMCLOSE=" + (Visible $p.Id))

$shot = Join-Path $OutDir 'startup-ui-settings.png'
& "$PSScriptRoot\..\capture-window.ps1" -ProcessId $p.Id -OutPath $shot 2>$null | Out-Null
Log ("SHOT=" + (Test-Path $shot))

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# 清场：注册表项、设置文件都不能留
if (Test-Path $settingsFile) { Remove-Item -Force $settingsFile; Log 'settings_deleted=True' }
$leftover = Reg-Run
if ($leftover -notmatch 'Unable|找不到|exit') { & reg.exe delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' /v PartyUp /f | Out-Null; Log 'reg_cleanup=deleted' }
Log ("REG_FINAL=" + (Reg-Run))
Log 'DONE'
