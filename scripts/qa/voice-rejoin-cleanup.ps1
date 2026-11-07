param([string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA'))

# Cleanup + post-condition check for the voice-rejoin investigation.
$procs = Get-CimInstance Win32_Process | Where-Object {
  ($_.CommandLine -like "*$Root*") -or ($_.ExecutablePath -like "$Root*")
}
foreach ($p in $procs) {
  Write-Output ("STOP " + $p.ProcessId + " " + $p.Name)
  Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 600
$left = Get-CimInstance Win32_Process | Where-Object {
  ($_.CommandLine -like "*$Root*") -or ($_.ExecutablePath -like "$Root*")
}
Write-Output ("LEFT_PROCS=" + @($left).Count)
$conn = @(Get-NetTCPConnection -LocalPort 18086 -ErrorAction SilentlyContinue)
Write-Output ("PORT_18086_LISTENERS=" + @($conn | Where-Object { $_.State -eq 'Listen' }).Count)
$settings = (Join-Path $env:APPDATA 'PartyUp\settings.json')
if (Test-Path $settings) {
  Write-Output ("SETTINGS_EXISTS=1 written=" + (Get-Item $settings).LastWriteTime.ToString('o'))
  Remove-Item -Force $settings
  Write-Output 'SETTINGS_REMOVED=1'
} else {
  Write-Output 'SETTINGS_EXISTS=0'
}
Write-Output ('APPDATA_PARTYUP_ITEMS=' + @(Get-ChildItem (Join-Path $env:APPDATA 'PartyUp') -ErrorAction SilentlyContinue).Count)
