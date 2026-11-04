param(
  [Parameter(Mandatory = $true)][string]$ClientDir,
  [Parameter(Mandatory = $true)][string]$Server,
  [Parameter(Mandatory = $true)][string]$Channel,
  [Parameter(Mandatory = $true)][string]$TokenA,
  [string]$Logs = "$env:TEMP\pu-gui\run\preview"
)

# 只验证一件事：共享者自己的预览是不是「活的」。
# 静态桌面截两张图会得到相同字节（上一轮就是这样，属假阴性），所以这里先在主屏上放一个
# 每 200ms 换色的窗口制造真实画面变化，再截共享者窗口两张图对比 —— 不同才说明预览在刷新。
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$clientExe = Join-Path $ClientDir 'PartyUp.Client.exe'
New-Item -ItemType Directory -Force -Path $Logs | Out-Null

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms

$BtnStartShare = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('5byA5aeL5YWx5Lqr')) # 开始共享
$BtnStopShare = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('5YGc5q2i'))          # 停止

function Invoke-ByName([int]$procId, [string]$name) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return 'NO_WINDOW' }
  $w = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $name)
  $t = $w.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  if (-not $t) { return "NOT_FOUND:$name" }
  $pattern = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
    $pattern.Invoke(); return "CLICKED:$name"
  }
  'NO_INVOKE'
}

$log = Join-Path $Logs 'client-a.log'
Remove-Item -Force -ErrorAction SilentlyContinue $log, "$log.err"
$a = Start-Process -FilePath $clientExe `
  -ArgumentList @('--server', $Server, '--token', $TokenA, '--channel', $Channel, '--voice-join') `
  -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru

$animator = $null
try {
  for ($i = 0; $i -lt 80; $i++) {
    Start-Sleep -Milliseconds 400
    if ((Get-Content -Raw $log -ErrorAction SilentlyContinue) -match 'voice auto-join transport=\S+ ok=True') { break }
  }
  Start-Sleep -Seconds 2
  "CLICK_START=$(Invoke-ByName $a.Id $BtnStartShare)"
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    if ((Get-Content -Raw $log -ErrorAction SilentlyContinue) -match 'share: started') { break }
  }
  Start-Sleep -Seconds 2

  # 主屏上的换色窗口（真实画面变化，会被共享源捕获）。
  $animator = Start-Process -FilePath 'powershell.exe' -ArgumentList @(
    '-NoProfile', '-WindowStyle', 'Hidden', '-Command',
    "Add-Type -AssemblyName System.Windows.Forms; `$f=New-Object System.Windows.Forms.Form; `$f.FormBorderStyle='None'; `$f.StartPosition='Manual'; `$f.Location=New-Object System.Drawing.Point(40,40); `$f.Size=New-Object System.Drawing.Size(600,400); `$f.TopMost=`$true; `$f.Show(); for(`$i=0;`$i -lt 200;`$i++){ `$f.BackColor=[System.Drawing.Color]::FromArgb((`$i*37)%256,(`$i*91)%256,(`$i*53)%256); [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 100 }; `$f.Close()"
  ) -PassThru
  Start-Sleep -Seconds 2

  $s1 = Join-Path $Logs 'preview-1.png'
  $s2 = Join-Path $Logs 'preview-2.png'
  & (Join-Path $here 'capture-window.ps1') -OutPath $s1 -ProcessId $a.Id | Out-Null
  Start-Sleep -Milliseconds 1500
  & (Join-Path $here 'capture-window.ps1') -OutPath $s2 -ProcessId $a.Id | Out-Null

  if ((Test-Path $s1) -and (Test-Path $s2)) {
    $h1 = (Get-FileHash $s1 -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $s2 -Algorithm SHA256).Hash
    $len1 = (Get-Item $s1).Length
    $len2 = (Get-Item $s2).Length
    "PREVIEW_HASH1=$h1"
    "PREVIEW_HASH2=$h2"
    "PREVIEW_SIZE1=$len1 PREVIEW_SIZE2=$len2"
    "PREVIEW_MOVING=$([bool]($h1 -ne $h2))"
  } else {
    'PREVIEW_MOVING=NO_SCREENSHOT'
  }

  "CLICK_STOP=$(Invoke-ByName $a.Id $BtnStopShare)"
  Start-Sleep -Seconds 2
  $txt = Get-Content -Raw $log
  "SHARE_STARTED=$([bool]($txt -match 'share: started'))"
  "SHARE_STOPPED=$([bool]($txt -match 'share: stopped'))"
}
finally {
  if ($animator -and -not $animator.HasExited) { $animator | Stop-Process -Force -ErrorAction SilentlyContinue }
  if ($a -and -not $a.HasExited) { $a | Stop-Process -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
  "CLIENTS_REMAINING=$((Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Measure-Object).Count)"
}
