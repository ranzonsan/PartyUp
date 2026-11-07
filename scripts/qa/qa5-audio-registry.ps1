$ErrorActionPreference = 'Continue'
function Get-Endpoints([string]$kind) {
  $root = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$kind"
  if (-not (Test-Path $root)) { Write-Output "$kind root missing"; return }
  Get-ChildItem $root | ForEach-Object {
    $guid = Split-Path $_.Name -Leaf
    $state = (Get-ItemProperty -Path $_.PSPath -Name DeviceState -ErrorAction SilentlyContinue).DeviceState
    $props = Join-Path $_.PSPath 'Properties'
    $name = ''
    if (Test-Path $props) {
      $p = Get-ItemProperty -Path $props -ErrorAction SilentlyContinue
      # {a45c254e-df1c-4efd-8020-67d146a850e0},14 = friendly name
      $name = $p.'{a45c254e-df1c-4efd-8020-67d146a850e0},14'
    }
    Write-Output ("$kind state=$state name=$name guid=$guid")
  }
}
Write-Output '== RENDER =='
Get-Endpoints 'Render'
Write-Output '== CAPTURE =='
Get-Endpoints 'Capture'
