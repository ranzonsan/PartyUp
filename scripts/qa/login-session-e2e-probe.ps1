param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [int]$Port = 18087
)

# 会话终止（单端登录）真机验收：
#   1) 客户端 A 用 token1 已连接；
#   2) 另一处用同一账号 POST /api/login → 服务端撤销 token1 并向 A 推送 session.replaced(login_elsewhere) 再关连接(4001)；
#   3) 断言：A 记录到 reason=login_elsewhere、回到登录页显示提示、**不再自动重连**（token 已失效，重连只会永远失败）。
# 全程副屏 + 隐藏窗口。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

$root = Join-Path $env:LOCALAPPDATA 'PartyUpQA6'
$dataDir = Join-Path $root 'data'
$outDir = Join-Path $root 'evidence'
New-Item -ItemType Directory -Force -Path $dataDir, $outDir | Out-Null

$settings = Join-Path $env:APPDATA 'PartyUp\settings.json'
$servers = Join-Path $env:APPDATA 'PartyUp\servers.json'
$backup = Join-Path $env:TEMP ("partyup-sess-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $backup | Out-Null
foreach ($f in @($settings, $servers)) {
  if (Test-Path $f) { Copy-Item -LiteralPath $f -Destination (Join-Path $backup (Split-Path $f -Leaf)) -Force }
}

$srv = $null; $cli = $null
try {
  Get-Process partyup-qa -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq $ServerExe } | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq $ClientExe } | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 600
  if (Test-Path $settings) { Remove-Item -Force $settings }
  if (Test-Path $servers) { Remove-Item -Force $servers }

  $srvOut = Join-Path $outDir 'server.out.log'
  $srvErr = Join-Path $outDir 'server.err.log'
  $srv = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port", '--name', 'PartyUp session'
    ) -WindowStyle Hidden -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru
  $deadline = (Get-Date).AddSeconds(25)
  $bootstrap = $null
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if (Test-Path $srvOut) {
      $m = Select-String -Path $srvOut -Pattern 'BOOTSTRAP_TOKEN=(\S+)' -ErrorAction SilentlyContinue |
        Select-Object -First 1
      if ($m) { $bootstrap = $m.Matches[0].Groups[1].Value; break }
    }
  }
  if (-not $bootstrap) { throw 'bootstrap token not found in server stdout' }
  Log 'SERVER_READY=PASS'

  $base = "http://127.0.0.1:$Port"
  $body = @{ token = $bootstrap; username = 'sessowner'; password = 'sess-pass-123' } | ConvertTo-Json
  $session1 = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' -Body $body
  $token1 = $session1.token
  Log ("BOOTSTRAP_OK token_len=" + $token1.Length)

  $cliOut = Join-Path $outDir 'client-a.out.log'
  $cliErr = Join-Path $outDir 'client-a.err.log'
  $cli = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', $base, '--token', $token1, '--screen', '2') `
    -RedirectStandardOutput $cliOut -RedirectStandardError $cliErr -PassThru
  $deadline = (Get-Date).AddSeconds(30)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if ((Select-String -Path $cliOut -Pattern '已连接：|gui: logged in|session ready' -ErrorAction SilentlyContinue)) { break }
  }
  Log ("CLIENT_CONNECTED=" + [bool](Select-String -Path $cliOut -Pattern '已连接：' -ErrorAction SilentlyContinue))

  # 另一处用同一账号登录 → 服务端撤销 token1
  $loginBody = @{ username = 'sessowner'; password = 'sess-pass-123' } | ConvertTo-Json
  $session2 = Invoke-RestMethod -Method Post -Uri "$base/api/login" -ContentType 'application/json' -Body $loginBody
  Log ("SECOND_LOGIN_OK token_changed=" + ($session2.token -ne $token1))
  Start-Sleep -Seconds 4

  $lines = Select-String -Path $cliOut -Pattern 'session terminated|session.replaced|已断开|会话已结束|重连' -ErrorAction SilentlyContinue
  foreach ($line in $lines) { Log ("CLIENT: " + $line.Line.Trim()) }
  $terminated = [bool](Select-String -Path $cliOut -Pattern 'session terminated reason=login_elsewhere' -ErrorAction SilentlyContinue)
  $reconnected = [bool](Select-String -Path $cliOut -Pattern 'ws: reconnecting|重连中…' -ErrorAction SilentlyContinue)
  Log ("ASSERT_TERMINATED=" + $terminated)
  Log ("ASSERT_NO_RECONNECT=" + (-not $reconnected))

  $shot = Join-Path $outDir 'session-replaced.png'
  if ($cli.MainWindowHandle -ne 0) {
    & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $cli.Id -OutPath $shot | ForEach-Object { Log $_ }
  }
  Log ("SHOT=" + (Test-Path $shot))

  # token 应已失效：用 token1 调 /api/me 必须 401
  try {
    Invoke-RestMethod -Method Get -Uri "$base/api/me" -Headers @{ Authorization = "Bearer $token1" } | Out-Null
    Log 'ASSERT_OLD_TOKEN_REVOKED=False'
  } catch {
    Log ("ASSERT_OLD_TOKEN_REVOKED=True status=" + $_.Exception.Response.StatusCode.value__)
  }
}
finally {
  if ($cli) { $cli.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 1200; if (Get-Process -Id $cli.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $cli.Id -Force } }
  if ($srv -and (Get-Process -Id $srv.Id -ErrorAction SilentlyContinue)) { Stop-Process -Id $srv.Id -Force }
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ClientExe } | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  foreach ($leaf in @('settings.json', 'servers.json')) {
    $src = Join-Path $backup $leaf
    $dst = Join-Path $env:APPDATA ("PartyUp\" + $leaf)
    if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination $dst -Force }
    elseif (Test-Path $dst) { Remove-Item -Force $dst }
  }
  Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
  Log 'CLEANUP=done'
}
Log 'DONE'
