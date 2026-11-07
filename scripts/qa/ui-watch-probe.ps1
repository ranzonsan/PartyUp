param(
  [Parameter(Mandatory = $true)][int]$AlicePid,
  [Parameter(Mandatory = $true)][int]$BobPid,
  [Parameter(Mandatory = $true)][string]$AliceId,
  [int]$WaitSeconds = 12
)

# 用于快速定位「观看收不到帧」是发布侧（WGC 静态桌面不出帧）还是转发/接收侧：
# 让发布者改用内置合成源（恒定 30fps），再看观看者的帧计数是否增长。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

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
    $pat.Invoke()
    return "CLICKED:$id"
  }
  return "NO_INVOKE:$id"
}

# 1) 发布者：打开共享设置弹窗 → 选最后一项（合成源固定在列表末尾）→ 开始共享
'OPEN_DIALOG=' + (Invoke-ById $AlicePid 'Publish')
Start-Sleep -Milliseconds 900
$combo = Find-ById (Get-Window $AlicePid) 'ShareSource'
if (-not $combo) { 'COMBO_NOT_FOUND'; exit 1 }
$exp = $null
if (-not $combo.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$exp)) {
  'NO_EXPAND'; exit 1
}
$exp.Expand()
Start-Sleep -Milliseconds 800
$cond = New-Object System.Windows.Automation.PropertyCondition(
  [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
  [System.Windows.Automation.ControlType]::ListItem)
$items = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
  [System.Windows.Automation.TreeScope]::Descendants, $cond)
'ITEM_COUNT=' + $items.Count
if ($items.Count -gt 0) {
  $last = $items[$items.Count - 1]
  $sel = $null
  if ($last.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$sel)) {
    $sel.Select()
    Start-Sleep -Milliseconds 500
  }
}
try { $exp.Collapse() } catch { }
'START_SHARE=' + (Invoke-ById $AlicePid 'ShareSetupStart')
Start-Sleep -Seconds 3

# 2) 观看者：点「观看共享」
'WATCH_CLICK=' + (Invoke-ById $BobPid $AliceId)

# 3) 采样观看者界面里的帧计数
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$max = -1
while ((Get-Date) -lt $deadline) {
  $w = Get-Window $BobPid
  if ($w) {
    $tc = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Text)
    foreach ($t in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $tc)) {
      $m = [regex]::Match($t.Current.Name, '·\s*(\d+)\s*帧')
      if ($m.Success) {
        $v = [int]$m.Groups[1].Value
        if ($v -gt $max) { $max = $v }
      }
    }
  }
  Start-Sleep -Milliseconds 700
}
'FRAMES_MAX=' + $max
