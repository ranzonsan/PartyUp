param(
  [string]$ClientExe = '',
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [int]$Port = 18095
)

# 手动测试环境：一个隔离服务端 + 两个客户端（alice / bob，token 直连，已登录）。
# 两个客户端各自独立配置根（--appdata），因此可以同时运行（单实例互斥按作用域区分）。
$ErrorActionPreference = 'Stop'
function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinPos {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public const uint NOSIZE = 0x0001, NOZORDER = 0x0004, NOACTIVATE = 0x0010;
    public static void Move(IntPtr h, int x, int y) { SetWindowPos(h, IntPtr.Zero, x, y, 0, 0, NOSIZE | NOZORDER | NOACTIVATE); }
}
"@

$root = Join-Path $env:LOCALAPPDATA 'PartyUpQA11'
$dataDir = Join-Path $root 'data'
$logDir = Join-Path $root 'logs'

# 先停上一次本脚本起的服务端/客户端，再删数据目录（否则 db 被占用删不掉）。
Get-Process partyup-qa -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ServerExe } |
  ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ClientExe } |
  ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 900

if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
New-Item -ItemType Directory -Force -Path $dataDir, $logDir | Out-Null
foreach ($name in @('alice', 'bob')) {
  $p = Join-Path $root $name
  if (Test-Path $p) { Remove-Item -Recurse -Force $p }
  New-Item -ItemType Directory -Force -Path $p | Out-Null
}

$srvOut = Join-Path $logDir 'server.out.log'
$srvErr = Join-Path $logDir 'server.err.log'
$srv = Start-Process -FilePath $ServerExe -ArgumentList @(
    'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port", '--name', 'PartyUp 测试服',
    '--stun-listen', 'off'
  ) -WindowStyle Hidden -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru
Log ("SERVER pid=" + $srv.Id + " port=" + $Port)

$bootstrap = $null
$deadline = (Get-Date).AddSeconds(25)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 400
  if (Test-Path $srvOut) {
    $m = Select-String -Path $srvOut -Pattern 'BOOTSTRAP_TOKEN=(\S+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($m) { $bootstrap = $m.Matches[0].Groups[1].Value; break }
  }
}
if (-not $bootstrap) { throw 'bootstrap token not found (server failed to start?)' }
$base = "http://127.0.0.1:$Port"

$alice = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' `
  -Body (@{ token = $bootstrap; username = 'alice'; password = 'alice-pass-123'; display_name = '爱丽丝' } | ConvertTo-Json)
$aliceToken = $alice.token
Log ("ALICE owner id=" + $alice.member.id)

$inv = Invoke-RestMethod -Method Post -Uri "$base/api/admin/invites" `
  -Headers @{ Authorization = "Bearer $aliceToken" } -ContentType 'application/json' -Body '{}'
$code = $inv.invite.code
$bob = Invoke-RestMethod -Method Post -Uri "$base/api/invite/$code" -ContentType 'application/json' `
  -Body (@{ username = 'bob'; password = 'bob-pass-1234'; display_name = '鲍勃' } | ConvertTo-Json)
$bobToken = $bob.token
Log ("BOB member id=" + $bob.member.id + " via invite " + $code)

# bob 提升为管理员：两个窗口都能打开「服务器管理」，但危险设置只有 alice(owner) 能改，
# 正好用手测对比 owner / admin 的差异。
Invoke-RestMethod -Method Post -Uri "$base/api/admin/members/$($bob.member.id)/role" `
  -Headers @{ Authorization = "Bearer $aliceToken" } -ContentType 'application/json' `
  -Body (@{ role = 'admin' } | ConvertTo-Json) | Out-Null
Log 'BOB promoted to admin'

# 默认频道（频道是文本/语音/共享的挂载点，没有频道就没法测语音和消息）。
# 注意：创建走管理端点 POST /api/admin/channels（没有公开的 POST /api/channels）。
try {
  $chans = Invoke-RestMethod -Method Get -Uri "$base/api/admin/channels" -Headers @{ Authorization = "Bearer $aliceToken" }
  if (($chans.channels | Measure-Object).Count -eq 0) {
    foreach ($n in @('大厅', '开黑二队')) {
      Invoke-RestMethod -Method Post -Uri "$base/api/admin/channels" -Headers @{ Authorization = "Bearer $aliceToken" } `
        -ContentType 'application/json' -Body (@{ name = $n } | ConvertTo-Json) | Out-Null
    }
    Log 'CHANNELS created 大厅 / 开黑二队'
  } else {
    Log ("CHANNELS existing=" + ($chans.channels | Measure-Object).Count)
  }
} catch { Log ("CHANNEL setup failed: " + $_.Exception.Message) }

function Start-Client([string]$who, [string]$token, [string]$appData, [int]$offsetX, [int]$offsetY) {
  $out = Join-Path $logDir ("client-" + $who + ".out.log")
  $p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) -ArgumentList @(
      '--server', $base, '--token', $token, '--screen', '2', '--appdata', $appData
    ) -RedirectStandardOutput $out -RedirectStandardError ($out + '.err') -PassThru
  $deadline = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { break }
    if ($p.HasExited) { throw ("client " + $who + " exited early, see " + $out) }
  }
  if ($p.MainWindowHandle -ne 0 -and ($offsetX -ne 0 -or $offsetY -ne 0)) {
    # 两个窗口错开，避免完全重叠（副屏物理坐标 -2560..-366）。
    [WinPos]::Move($p.MainWindowHandle, -2560 + $offsetX, $offsetY)
  }
  Log ("CLIENT " + $who + " pid=" + $p.Id + " hwnd=" + $p.MainWindowHandle + " log=" + $out)
  return $p
}

$a = Start-Client 'alice' $aliceToken (Join-Path $root 'alice') 0 0
$b = Start-Client 'bob' $bobToken (Join-Path $root 'bob') 110 120

Log 'READY'
Log ("SERVER_URL=http://127.0.0.1:" + $Port)
Log 'ACCOUNTS alice/alice-pass-123 (owner, 爱丽丝) · bob/bob-pass-1234 (成员, 鲍勃)'
Log ("PIDS server=" + $srv.Id + " alice=" + $a.Id + " bob=" + $b.Id)
Log ("DATA=" + $root)
