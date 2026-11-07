param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [int]$Port = 18093
)

# owner / admin / 普通成员 三种角色在「服务器管理」入口与页内的差异：
#   普通成员：设置里**没有**入口；直接打 /api/admin/* 必须 403（服务端为准）。
#   管理员：有入口；页内能管成员，但**危险设置只读、不能转交所有权**。
#   owner：危险设置可编辑、可转交。
# 全部用真实 UIA 控件与真实 HTTP 断言，不靠看截图。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeMouse2 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public static void Down(int x, int y) { SetCursorPos(x, y); mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero); }
    public static void Up() { mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero); }
}
"@

function Find-ById([int]$pid_, [string]$id, [int]$timeoutSec = 12) {
  $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    $root = $AE::RootElement.FindFirst($TS::Children, $cond)
    if ($root) {
      $all = $root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
      foreach ($el in $all) { if ($el.Current.AutomationId -eq $id) { return $el } }
    }
    Start-Sleep -Milliseconds 250
  }
  return $null
}

function Invoke-El($el, [string]$what) {
  if (-not $el) { throw "element not found: $what" }
  $p = $null
  if ($el.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) {
    try { $p.Invoke(); Log ("CLICK " + $what); return } catch { }
  }
  $r = $el.Current.BoundingRectangle
  if ($r.Width -gt 0) {
    [NativeMouse2]::Down([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
    Start-Sleep -Milliseconds 60
    [NativeMouse2]::Up()
    Log ("MOUSE_CLICK " + $what); return
  }
  throw "not invokable: $what"
}

function Open-Settings([int]$pid_) {
  $card = Find-ById $pid_ 'UserCard' 40
  if (-not $card) { throw 'UserCard not found' }
  Invoke-El $card 'UserCard'
  $item = Find-ById $pid_ 'OpenSettings' 10
  if (-not $item) { throw 'OpenSettings not found' }
  Invoke-El $item 'OpenSettings'
  Start-Sleep -Milliseconds 800
}

$root = Join-Path $env:LOCALAPPDATA 'PartyUpQA10'
$dataDir = Join-Path $root 'data'
$outDir = Join-Path $root 'evidence'
New-Item -ItemType Directory -Force -Path $dataDir, $outDir | Out-Null
$testAppData = Join-Path $root 'appdata'
New-Item -ItemType Directory -Force -Path $testAppData | Out-Null
$lockPath = Join-Path $env:TEMP 'partyup-gui-probe.lock'
$lockHeld = $false
$srv = $null
$cli = $null
$results = New-Object System.Collections.ArrayList
function Assert([string]$name, $ok, [string]$evidence) {
  [void]$results.Add([pscustomobject]@{ case = $name; pass = [bool]$ok; evidence = $evidence })
  Log (("ASSERT {0} = {1}  {2}" -f $name, ([bool]$ok), $evidence))
}
function Start-Client([string]$token) {
  $out = Join-Path $outDir ('client-' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.log')
  $script:cli = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', "http://127.0.0.1:$Port", '--token', $token, '--screen', '2', '--appdata', $testAppData) `
    -RedirectStandardOutput $out -RedirectStandardError ($out + '.err') -PassThru
  Log ("CLIENT_STARTED pid=" + $script:cli.Id + " log=" + $out)
  return $out
}
function Stop-Client() {
  if ($script:cli) {
    try { $script:cli.CloseMainWindow() | Out-Null } catch { }
    Start-Sleep -Milliseconds 1200
    if (Get-Process -Id $script:cli.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $script:cli.Id -Force -ErrorAction SilentlyContinue }
    $script:cli = $null
  }
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ClientExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 800
}

try {
  $lockDeadline = (Get-Date).AddSeconds(300)
  while (-not (New-Item -ItemType File -Path $lockPath -ErrorAction SilentlyContinue)) {
    if ((Get-Date) -gt $lockDeadline) { throw 'GUI lock timeout' }
    Start-Sleep -Seconds 3
  }
  $lockHeld = $true

  Get-Process partyup-qa -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ServerExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500

  $srvOut = Join-Path $outDir 'server.out.log'
  $srv = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port", '--name', 'PartyUp roles', '--stun-listen', 'off'
    ) -WindowStyle Hidden -RedirectStandardOutput $srvOut -RedirectStandardError (Join-Path $outDir 'server.err.log') -PassThru
  $bootstrap = $null
  $deadline = (Get-Date).AddSeconds(25)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    if (Test-Path $srvOut) {
      $m = Select-String -Path $srvOut -Pattern 'BOOTSTRAP_TOKEN=(\S+)' -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($m) { $bootstrap = $m.Matches[0].Groups[1].Value; break }
    }
  }
  if (-not $bootstrap) { throw 'bootstrap token not found' }
  $base = "http://127.0.0.1:$Port"

  $owner = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' `
    -Body (@{ token = $bootstrap; username = 'owner1'; password = 'owner-pass-123' } | ConvertTo-Json)
  $ownerToken = $owner.token
  $inv = Invoke-RestMethod -Method Post -Uri "$base/api/admin/invites" -Headers @{ Authorization = "Bearer $ownerToken" } `
    -ContentType 'application/json' -Body '{}'
  $code = $inv.invite.code
  $member = Invoke-RestMethod -Method Post -Uri "$base/api/invite/$code" -ContentType 'application/json' `
    -Body (@{ username = 'plain1'; password = 'plain-pass-123' } | ConvertTo-Json)
  $memberToken = $member.token
  $memberId = $member.member.id
  Log ("SETUP_OK member_id=" + $memberId)

  # ---- 阶段 A：普通成员 ----
  $null = Start-Client $memberToken
  $card = Find-ById $cli.Id 'UserCard' 40
  Assert 'member-logged-in' ($null -ne $card) 'UserCard 出现'
  Open-Settings $cli.Id
  $entry = Find-ById $cli.Id 'OpenServerAdmin' 4
  Assert 'member-has-no-admin-entry' ($null -eq $entry) ('OpenServerAdmin found=' + ($null -ne $entry))
  try {
    Invoke-RestMethod -Method Get -Uri "$base/api/admin/overview" -Headers @{ Authorization = "Bearer $memberToken" } | Out-Null
    Assert 'member-api-403' $false 'GET /api/admin/overview 竟然成功'
  } catch {
    $st = $_.Exception.Response.StatusCode.value__
    Assert 'member-api-403' ($st -eq 403) ("status=" + $st)
  }
  Stop-Client

  # ---- 阶段 B：提升为管理员 ----
  Invoke-RestMethod -Method Post -Uri "$base/api/admin/members/$memberId/role" -Headers @{ Authorization = "Bearer $ownerToken" } `
    -ContentType 'application/json' -Body (@{ role = 'admin' } | ConvertTo-Json) | Out-Null
  Log 'PROMOTED to admin'

  $null = Start-Client $memberToken
  $null = Find-ById $cli.Id 'UserCard' 40
  Open-Settings $cli.Id
  $entry = Find-ById $cli.Id 'OpenServerAdmin' 10
  Assert 'admin-has-entry' ($null -ne $entry) 'OpenServerAdmin 出现'
  Invoke-El $entry 'OpenServerAdmin'
  $null = Find-ById $cli.Id 'AdminMemberList' 20
  $roleCap = Find-ById $cli.Id 'AdminRoleCaption' 8
  $roleCapText = if ($roleCap) { $roleCap.Current.Name } else { '' }
  Assert 'admin-role-caption' ($roleCapText -eq '管理员') ('RoleCaption=' + $roleCapText)
  $voice = Find-ById $cli.Id 'AdminVoiceMode' 8
  Assert 'admin-dangerous-readonly' (($null -ne $voice) -and (-not $voice.Current.IsEnabled)) ('VoiceMode.IsEnabled=' + $(if ($voice) { $voice.Current.IsEnabled } else { 'n/a' }))
  $transfer = Find-ById $cli.Id 'AdminTransferOwnership' 8
  Assert 'admin-cannot-transfer' (($null -ne $transfer) -and (-not $transfer.Current.IsEnabled)) ('Transfer.IsEnabled=' + $(if ($transfer) { $transfer.Current.IsEnabled } else { 'n/a' }))
  $save = Find-ById $cli.Id 'AdminSaveSettings' 8
  Assert 'admin-can-save-settings' (($null -ne $save) -and $save.Current.IsEnabled) 'AdminSaveSettings enabled'
  Stop-Client

  # 服务端二次确认：admin 改危险设置必须被拒
  try {
    Invoke-RestMethod -Method Patch -Uri "$base/api/admin/settings" -Headers @{ Authorization = "Bearer $memberToken" } `
      -ContentType 'application/json' -Body (@{ voice_mode = 'direct' } | ConvertTo-Json) | Out-Null
    Assert 'admin-dangerous-patch-403' $false 'admin 竟然改成功了'
  } catch {
    $st = $_.Exception.Response.StatusCode.value__
    Assert 'admin-dangerous-patch-403' ($st -eq 403) ("status=" + $st)
  }

  # ---- 阶段 C：降回普通成员，入口应消失 ----
  Invoke-RestMethod -Method Post -Uri "$base/api/admin/members/$memberId/role" -Headers @{ Authorization = "Bearer $ownerToken" } `
    -ContentType 'application/json' -Body (@{ role = 'user' } | ConvertTo-Json) | Out-Null
  $null = Start-Client $memberToken
  $null = Find-ById $cli.Id 'UserCard' 40
  Open-Settings $cli.Id
  $entry = Find-ById $cli.Id 'OpenServerAdmin' 4
  Assert 'demoted-entry-hidden' ($null -eq $entry) ('OpenServerAdmin found=' + ($null -eq $entry))
  Stop-Client

  $shot = Join-Path $outDir 'admin-roles.png'
  Log ("SUMMARY pass=" + (($results | Where-Object { $_.pass }).Count) + "/" + $results.Count +
       " (最后一轮界面已关闭；角色门槛以断言为准)")
  $results | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $outDir 'results.json') -Encoding UTF8
}
finally {
  Stop-Client
  if ($srv -and (Get-Process -Id $srv.Id -ErrorAction SilentlyContinue)) { Stop-Process -Id $srv.Id -Force }
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
  if ($lockHeld) { Remove-Item -Force $lockPath -ErrorAction SilentlyContinue }
  Log 'CLEANUP=done'
}
