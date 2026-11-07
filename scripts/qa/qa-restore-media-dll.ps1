param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'),
  [string]$TargetDir = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client'),
  [string]$ClientExe = '',
  [Parameter(Mandatory = $true)][string]$SourceDll,
  [string]$ExpectShaPrefix = ''
)

# Restore a media DLL into a client build dir after an A/B swap.
# Kills leftover QA clients first (a live client keeps the DLL mapped and the copy fails
# with "file is in use"), then copies and verifies the hash.
$ErrorActionPreference = 'Stop'
function Log([string]$m) { [Console]::WriteLine("[" + (Get-Date -Format 'HH:mm:ss.fff') + "] " + $m) }

$clients = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
  Where-Object { ($_.ExecutablePath -like "$Root*") -or ($ClientExe -and $_.ExecutablePath -ieq $ClientExe) }
foreach ($c in $clients) {
  Log ("KILL_CLIENT pid=" + $c.ProcessId + " path=" + $c.ExecutablePath)
  Stop-Process -Id $c.ProcessId -Force -ErrorAction SilentlyContinue
}
if ($clients) { Start-Sleep -Seconds 1 }

$target = Join-Path $TargetDir 'partyup_media.dll'
if (-not (Test-Path $SourceDll)) { throw ("source dll missing: " + $SourceDll) }
Copy-Item $SourceDll $target -Force
$sha = (Get-FileHash -Algorithm SHA256 $target).Hash
Log ("RESTORED_SHA=" + $sha)
Log ("RESTORED_PATH=" + $target)
if ($ExpectShaPrefix -and -not $sha.StartsWith($ExpectShaPrefix.ToUpper())) {
  throw ("sha mismatch: expected prefix " + $ExpectShaPrefix)
}
Log 'RESTORE_OK'
