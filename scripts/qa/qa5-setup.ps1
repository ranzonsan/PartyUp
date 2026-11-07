$ErrorActionPreference = 'Stop'
$root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA')
$src = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client')
$dllSrc = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\partyup_media.dll')
$want = '7181922ADD7BACE4AAE67ED38E6D14F786824196DA57B68C19C535CC7C4DA3F1'
if ((Get-FileHash -Algorithm SHA256 $dllSrc).Hash -ne $want) { throw 'newest media dll hash mismatch' }

foreach ($name in @('client-a', 'client-b')) {
  $dst = Join-Path $root $name
  New-Item -ItemType Directory -Force -Path $dst | Out-Null
  $null = robocopy $src $dst /MIR /NFL /NDL /NJH /NJS /NP
  if ($LASTEXITCODE -ge 8) { throw "robocopy $name rc=$LASTEXITCODE" }
  Copy-Item -Force $dllSrc (Join-Path $dst 'partyup_media.dll')
  Write-Output ("CLIENT_" + $name + "_ITEMS=" + (@(Get-ChildItem $dst).Count))
  Write-Output ("CLIENT_" + $name + "_DLL_SHA=" + (Get-FileHash -Algorithm SHA256 (Join-Path $dst 'partyup_media.dll')).Hash)
  Write-Output ("CLIENT_" + $name + "_EXE_SHA=" + (Get-FileHash -Algorithm SHA256 (Join-Path $dst 'PartyUp.Client.exe')).Hash)
}
Write-Output 'SETUP_DONE'
