param(
  [string]$ClientExe,
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence-login"
)

# 第一屏（服务器列表）肉眼验收：窗口放副屏、不抢焦点、截图按窗口矩形抓。
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

$settings = Join-Path $env:APPDATA 'PartyUp\settings.json'
$serverBook = Join-Path $env:APPDATA 'PartyUp\servers.json'
$backup = Join-Path $env:TEMP ("partyup-prefs-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $backup | Out-Null
foreach ($f in @($settings, $serverBook)) {
  if (Test-Path $f) { Copy-Item -LiteralPath $f -Destination (Join-Path $backup (Split-Path $f -Leaf)) -Force }
}

function Kill-All {
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq $ClientExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 700
}

try {
  Kill-All
  if (Test-Path $settings) { Remove-Item -Force $settings }
  if (Test-Path $serverBook) { Remove-Item -Force $serverBook }

  $out = Join-Path $OutDir 'first-screen.out.log'
  $err = Join-Path $OutDir 'first-screen.err.log'
  $p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', 'http://127.0.0.1:18085', '--screen', '2') `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru

  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline) {
    if ((Get-Process -Id $p.Id -ErrorAction SilentlyContinue) -and $p.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 400
  }
  Start-Sleep -Seconds 2

  $shot = Join-Path $OutDir 'first-screen.png'
  & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $p.Id -OutPath $shot | ForEach-Object { Log $_ }
  Log ("SHOT=" + (Test-Path $shot))

  $placement = Select-String -Path $out -Pattern 'gui: window (placed|restored)|off-screen' -ErrorAction SilentlyContinue
  foreach ($line in $placement) { Log ("LOG: " + $line.Line.Trim()) }

  $p.CloseMainWindow() | Out-Null
  Start-Sleep -Milliseconds 1500
  if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Force }
}
finally {
  Kill-All
  foreach ($leaf in @('settings.json', 'servers.json')) {
    $src = Join-Path $backup $leaf
    $dst = Join-Path $env:APPDATA ("PartyUp\" + $leaf)
    if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination $dst -Force }
    elseif (Test-Path $dst) { Remove-Item -Force $dst }
  }
  Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
  Log 'RESTORED_PREFS=True'
}
Log 'DONE'
