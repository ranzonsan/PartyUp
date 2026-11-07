$ErrorActionPreference = 'Continue'
Write-Output '== AudioEndpoint devices =='
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Output ("STATUS=" + $_.Status + " NAME=" + $_.FriendlyName + " ID=" + $_.InstanceId)
}
Write-Output '== Audio devices (class Audio) =='
Get-PnpDevice -Class Audio -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Output ("STATUS=" + $_.Status + " NAME=" + $_.FriendlyName)
}
