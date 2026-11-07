param(
  [string]$ClientExe = '',
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence-window"
)

# 窗口放置验收（只观测，不改产品逻辑）：
#   1) --screen 2 应把窗口放到副屏（x 为负的那块）；
#   2) 关闭后记住位置，下一次启动（不带参数）应回到同一块屏；
#   3) --screen 1 应强制回主屏；
#   4) --screen 99 越界时必须回落主屏，而不是丢到屏幕外。
# 窗口只出现在副屏，不影响用户在主屏上的工作。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

function Rect-Of([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  $el = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
  return $el.Current.BoundingRectangle
}

function Wait-Window([int]$procId, [int]$seconds = 25) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    $r = Rect-Of $procId
    if ($null -ne $r -and $r.Width -gt 100) { return $r }
    Start-Sleep -Milliseconds 400
  }
  return $null
}

function Screen-OfPoint($rect) {
  if ($null -eq $rect) { return 'NO_WINDOW' }
  $cx = [int]($rect.X + $rect.Width / 2)
  $cy = [int]($rect.Y + $rect.Height / 2)
  $screen = [System.Windows.Forms.Screen]::AllScreens |
    Where-Object { $_.Bounds.Contains($cx, $cy) } | Select-Object -First 1
  if (-not $screen) { return 'OFFSCREEN' }
  $idx = [array]::IndexOf([System.Windows.Forms.Screen]::AllScreens, $screen) + 1
  $kind = 'primary'
  if (-not $screen.Primary) { $kind = 'secondary' }
  return ('screen' + $idx + '(' + $kind + ')')
}

function Kill-All([string]$exe) {
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq $exe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 700
}

function Stop-Current() {
  # 正常关窗，才能走 OnClosing 保存位置（Stop-Process 会跳过保存）。
  $p = Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 1500
    if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Force }
  }
  Start-Sleep -Milliseconds 600
}

function Start-Client([string[]]$clientArgs, [string]$tag) {
  Kill-All $ClientExe
  $out = Join-Path $OutDir "$tag.out.log"
  $err = Join-Path $OutDir "$tag.err.log"
  $proc = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList $clientArgs -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
  $rect = Wait-Window $proc.Id
  $placement = Select-String -Path $out -Pattern 'gui: window (placed|restored)|off-screen|placement failed|--screen .* out of range' -ErrorAction SilentlyContinue
  $screenLabel = Screen-OfPoint $rect
  Log ("$tag pid=" + $proc.Id + " rect=" + $rect + " screen=" + $screenLabel)
  if ($placement) {
    foreach ($line in $placement) { Log ("$tag LOG: " + $line.Line.Trim()) }
  } else {
    Log ("$tag LOG: (no placement line found)")
  }
  return $proc
}

$settingsFile = Join-Path $env:APPDATA 'PartyUp\settings.json'
if (Test-Path $settingsFile) { Remove-Item -Force $settingsFile }

$p = Start-Client @('--server', 'http://127.0.0.1:18085', '--screen', '2') 'run1-screen2'
$shot1 = Join-Path $OutDir 'placement-screen2.png'
if ($p.MainWindowHandle -ne 0) {
  & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $p.Id -OutPath $shot1 | Out-Null
}
Log ("SHOT1=" + (Test-Path $shot1))
Log ("SETTINGS_AFTER_PLACE=" + (Get-Content $settingsFile -Raw -ErrorAction SilentlyContinue))
Stop-Current

$null = Start-Client @('--server', 'http://127.0.0.1:18085') 'run2-restored'
Stop-Current

$null = Start-Client @('--server', 'http://127.0.0.1:18085', '--screen', '1') 'run3-screen1'
Stop-Current

$null = Start-Client @('--server', 'http://127.0.0.1:18085', '--screen', '99') 'run4-outofrange'
Stop-Current

Kill-All $ClientExe
if (Test-Path $settingsFile) { Remove-Item -Force $settingsFile; Log 'settings_deleted=True' }
Log 'DONE'
