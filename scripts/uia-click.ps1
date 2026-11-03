param(
  [int]$ProcessId = 0,
  [string]$TokenLike = '',
  [string]$Name,
  [string]$Type = 'Button'
)

# 用 UI Automation 在某一个 PartyUp.Client 窗口里按控件 Name 点一下。
# 两个 GUI 客户端同时开时，用 -TokenLike 区分（命令行里带该 token 的那个进程）。
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
if ($p.MainWindowHandle -eq 0) { 'NO_WINDOW'; exit 3 }

$condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
$window = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
$target = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
if (-not $target) { "NOT_FOUND: $Name (pid=$($p.Id))"; exit 4 }

$pattern = $null
if ($target.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
  $pattern.Invoke()
  "CLICKED: $Name (pid=$($p.Id))"
  exit 0
}
"NO_INVOKE_PATTERN: $Name (pid=$($p.Id))"
exit 5
