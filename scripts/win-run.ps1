param(
    [string]$Exe = 'C:\src\PartyUp\spikes\frame-presentation\bin\Debug\net10.0\Spike.Frame.exe'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { throw "not found: $Exe" }

Set-Location C:\src\PartyUp
Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe)
Write-Host "launched: $Exe"
