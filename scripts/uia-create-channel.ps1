param(
  [Parameter(Mandatory = $true)][int]$ProcessId,
  [Parameter(Mandatory = $true)][string]$ChannelName
)

# 在 GUI 里建频道：左栏第一个文本框填名字，再点「+」按钮。
# 仅用于自动化验收（GUI 没有命令行建频道入口）。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if (-not $proc) { 'NO_PROCESS'; exit 2 }
$window = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)

$editCond = New-Object System.Windows.Automation.PropertyCondition(
  [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
  [System.Windows.Automation.ControlType]::Edit)
$edits = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $editCond)
if ($edits.Count -eq 0) { 'NO_EDIT'; exit 3 }
$nameBox = $edits[0]
$valuePattern = $nameBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
$valuePattern.SetValue($ChannelName)
Start-Sleep -Milliseconds 200

$btnCond = New-Object System.Windows.Automation.PropertyCondition(
  [System.Windows.Automation.AutomationElement]::NameProperty, '+')
$addButton = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
if (-not $addButton) { 'NO_ADD_BUTTON'; exit 4 }
$addButton.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
"CREATED_CHANNEL=$ChannelName"
