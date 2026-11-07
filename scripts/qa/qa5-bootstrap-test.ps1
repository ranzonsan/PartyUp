$ErrorActionPreference = 'Stop'
$root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA')
foreach ($sub in @('data', 'evidence', 'client-a', 'client-b')) {
  $p = Join-Path $root $sub
  New-Item -ItemType Directory -Force -Path $p | Out-Null
  Write-Output ("DIR_OK " + $p)
}
Set-Content -Path (Join-Path $root 'evidence\probe.txt') -Value ('hello ' + (Get-Date -Format o)) -Encoding UTF8
Write-Output 'FILE_OK'
Write-Output ('PS_VERSION=' + $PSVersionTable.PSVersion.ToString())
