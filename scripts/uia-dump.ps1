param(
  [int]$ProcessId = 0,
  [string]$TokenLike = '',
  [string]$Filter = ''
)

# 转储某个 PartyUp.Client 窗口里 UIA 能看到的文本控件（取证用：确认面板文案真的进了视觉树）。
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$p = $null
if ($ProcessId -ne 0) {
  $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
} elseif ($TokenLike -ne '') {
  $found = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
           Where-Object { $_.CommandLine -like "*$TokenLike*" } |
           Select-Object -First 1
  if ($found) { $p = Get-Process -Id $found.ProcessId -ErrorAction SilentlyContinue }
}
if (-not $p) { 'NO_PROCESS'; exit 2 }

$window = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Text)
$all = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
"pid=$($p.Id) text_count=$($all.Count)"
foreach ($t in $all) {
  $n = $t.Current.Name
  if ($n -and ($Filter -eq '' -or $n -like "*$Filter*")) { 'TEXT: ' + $n }
}
