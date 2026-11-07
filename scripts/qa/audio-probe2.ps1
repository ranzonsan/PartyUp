param(
  [string]$ClientExe = '',
  [int]$Port = 18085,
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence-audio2",
  [string]$Pick = 'first'
)

# 追问：麦克风测试「正在采集但收不到电平」是不是因为选择停留在「跟随系统默认」？
# 本探针显式选一个真实输入设备，再开始测试，比较两种选择下的电平与状态。
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
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) { $pat.Invoke(); return "CLICKED:$id" }
  return "NO_INVOKE:$id"
}
function Read-Range([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return $null }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$pat)) { return [double]$pat.Current.Value }
  return $null
}
function Dump-Texts([int]$procId, [string]$name) {
  $w = Get-Window $procId
  if (-not $w) { return @() }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
  $texts = @()
  foreach ($t in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) { if ($t.Current.Name) { $texts += $t.Current.Name } }
  $texts | Set-Content -Encoding UTF8 (Join-Path $OutDir "$name.texts.txt")
  return $texts
}

$base = "http://127.0.0.1:$Port"
$body = @{ username = 'tester'; password = 'tester-pass-123' } | ConvertTo-Json
$resp = Invoke-RestMethod -Uri "$base/api/login" -Method Post -Body $body -ContentType 'application/json'
$token = $resp.token
Log ("LOGIN_OK role=" + $resp.member.role)

$p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) -ArgumentList @('--server', $base, '--token', $token, '--screen', '2') -PassThru
Log "client pid=$($p.Id)"
Start-Sleep -Seconds 8
Log (Invoke-ById $p.Id 'UserCard')
Start-Sleep -Milliseconds 900
Log (Invoke-ById $p.Id 'OpenSettings')
Start-Sleep -Seconds 3

# 展开输入设备下拉，选定第一个真实设备
$combo = Find-ById (Get-Window $p.Id) 'InputDeviceChoice'
if (-not $combo) { Log 'INPUT_COMBO_MISSING'; exit 1 }
$exp = $null
if ($combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$exp)) {
  $exp.Expand(); Start-Sleep -Milliseconds 900
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
  $items = $combo.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
  Log ("INPUT_ITEMS_IN_COMBO=" + $items.Count)
  for ($i = 0; $i -lt [Math]::Min($items.Count, 5); $i++) { Log ("  item[$i]=" + $items[$i].Current.Name) }
  $pickIndex = if ($Pick -eq 'default') { 0 } else { [Math]::Min(1, $items.Count - 1) }
  if ($items.Count -gt 0) {
    $sel = $null
    if ($items[$pickIndex].TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
      $sel.Select(); Log ("SELECTED_INDEX=" + $pickIndex + " name=" + $items[$pickIndex].Current.Name)
    }
    Start-Sleep -Milliseconds 600
  }
  try { $exp.Collapse() } catch { }
}

Log (Invoke-ById $p.Id 'MicTestStart')
$max = -1.0
$deadline = (Get-Date).AddSeconds(6)
while ((Get-Date) -lt $deadline) {
  $v = Read-Range $p.Id 'MicTestLevel'
  if ($null -ne $v -and $v -gt $max) { $max = $v }
  Start-Sleep -Milliseconds 250
}
Log ("MIC_LEVEL_MAX=" + $max)
$texts = Dump-Texts $p.Id 'audio2-mictest'
Log ("STATUS=" + (($texts | Where-Object { $_ -match '采集|电平|Capturing|Level|没有|No level' }) -join ' | '))
Log (Invoke-ById $p.Id 'MicTestStop')

# 逐个真实设备试一遍：这台机器上默认端点可能是虚拟设备（NVIDIA Broadcast），
# 它没在跑时不会有 PCM——必须区分「功能坏了」和「选到虚拟麦克风」。
for ($i = 1; $i -lt [Math]::Min($items.Count, 5); $i++) {
  $exp2 = $null
  if (-not $combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$exp2)) { break }
  $exp2.Expand(); Start-Sleep -Milliseconds 700
  $cond2 = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
  $items2 = $combo.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond2)
  if ($items2.Count -le $i) { try { $exp2.Collapse() } catch { }; break }
  $sel2 = $null
  if ($items2[$i].TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel2)) {
    $sel2.Select(); Start-Sleep -Milliseconds 600
  }
  try { $exp2.Collapse() } catch { }
  $name = $items2[$i].Current.Name

  Log (Invoke-ById $p.Id 'MicTestStart')
  $maxI = -1.0
  $deadline = (Get-Date).AddSeconds(4)
  while ((Get-Date) -lt $deadline) {
    $v = Read-Range $p.Id 'MicTestLevel'
    if ($null -ne $v -and $v -gt $maxI) { $maxI = $v }
    Start-Sleep -Milliseconds 250
  }
  Log ("DEVICE[" + $i + "]=" + $name + " MIC_LEVEL_MAX=" + $maxI)
  Log (Invoke-ById $p.Id 'MicTestStop')
  Start-Sleep -Milliseconds 400
}


$path = Join-Path $OutDir 'audio2-settings.png'
& "$PSScriptRoot\..\capture-window.ps1" -ProcessId $p.Id -OutPath $path | Out-Null
Log ("SHOT=" + (Test-Path $path))
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Log 'DONE'
