$ErrorActionPreference = 'Continue'
$root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA')
$src = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client')
Write-Output ("SRC_EXISTS=" + (Test-Path $src))
Write-Output ("SRC_ITEMS=" + (@(Get-ChildItem $src).Count))
$dst = Join-Path $root 'client-a'
Write-Output ("DST_EXISTS=" + (Test-Path $dst))
$out = robocopy $src $dst /MIR /NFL /NDL /NJH /NJS /NP 2>&1
$rc = $LASTEXITCODE
Write-Output ("ROBO_RC=$rc")
$out | Select-Object -Last 15 | ForEach-Object { Write-Output ("ROBO: " + $_) }
Write-Output ("DST_ITEMS=" + (@(Get-ChildItem $dst).Count))
