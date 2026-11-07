param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [int]$Port = 18091
)

# 「服务器管理」页真机验收（UIA 驱动真实控件，不靠人工看）：
#   打开路径必须走真实入口：用户卡片 → 菜单「应用设置」→「打开服务器管理」。
#   断言：成员列表、角色标注(所有者)、概览、危险设置可用(owner)、生成邀请码、封禁成员、审计日志。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
# Flyout 里的 MenuItem 对 UIA InvokePattern 常抛异常，需要真实鼠标点击兜底。
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeMouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, KEYUP = 0x0002;
    public static void Down(int x, int y) { SetCursorPos(x, y); mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero); }
    public static void Up() { mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero); }
    public static void Key(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); keybd_event(vk, 0, KEYUP, UIntPtr.Zero); }
}
"@
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

function Find-ById([int]$pid_, [string]$id, [int]$timeoutSec = 15) {
  $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    $root = $AE::RootElement.FindFirst($TS::Children, $cond)
    if ($root) {
      $all = $root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
      foreach ($el in $all) {
        if ($el.Current.AutomationId -eq $id) { return $el }
      }
    }
    Start-Sleep -Milliseconds 250
  }
  return $null
}

function Find-ByName([string]$name, [int]$timeoutSec = 8) {
  $cond = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name)
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    $el = $AE::RootElement.FindFirst($TS::Descendants, $cond)
    if ($el) { return $el }
    Start-Sleep -Milliseconds 250
  }
  return $null
}

function Invoke-El($el, [string]$what) {
  if (-not $el) { throw "element not found: $what" }
  $p = $null
  if ($el.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) {
    try { $p.Invoke(); Log ("CLICK " + $what); return }
    catch { Log ("INVOKE_FAILED " + $what + " -> physical click") }
  }
  if ($el.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$p)) {
    try { $p.Select(); Log ("SELECT " + $what); return } catch { }
  }
  # 兜底：按包围盒中心真实点击（Flyout 菜单项走这条）。
  $r = $el.Current.BoundingRectangle
  if ($r.Width -gt 0 -and $r.Height -gt 0) {
    [NativeMouse]::Down([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
    Start-Sleep -Milliseconds 60
    [NativeMouse]::Up()
    Log ("MOUSE_CLICK " + $what)
    return
  }
  throw "element not invokable: $what"
}

function Text-Of($el) { if ($el) { return $el.Current.Name } return '' }

$root = Join-Path $env:LOCALAPPDATA 'PartyUpQA9'
$dataDir = Join-Path $root 'data'
$outDir = Join-Path $root 'evidence'
New-Item -ItemType Directory -Force -Path $dataDir, $outDir | Out-Null

$files = @('settings.json', 'servers.json', 'tokens.json')
# 独立配置根目录：多个验收脚本并发时不会互相污染服务器簿/记住我状态。
# 必须用产品自带的 --appdata（设 APPDATA 环境变量无效：Windows 的 KnownFolder API 不读它）。
$testAppData = Join-Path $root 'appdata'
New-Item -ItemType Directory -Force -Path $testAppData | Out-Null

$srv = $null; $cli = $null
$lockPath = Join-Path $env:TEMP 'partyup-gui-probe.lock'
$lockHeld = $false
$results = New-Object System.Collections.ArrayList
function Assert([string]$name, $ok, [string]$evidence) {
  [void]$results.Add([pscustomobject]@{ case = $name; pass = [bool]$ok; evidence = $evidence })
  Log (("ASSERT {0} = {1}  {2}" -f $name, ([bool]$ok), $evidence))
}

try {
  # 与其它 GUI 探针串行（单实例互斥是用户级，两个客户端会互相踢掉）。
  $lockDeadline = (Get-Date).AddSeconds(300)
  while (-not (New-Item -ItemType File -Path $lockPath -ErrorAction SilentlyContinue)) {
    if ((Get-Date) -gt $lockDeadline) { throw 'GUI lock timeout (another probe is running)' }
    Start-Sleep -Seconds 3
  }
  $lockHeld = $true

  # 清理自己的旧进程
  Get-Process partyup-qa -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ServerExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ClientExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 600

  $srvOut = Join-Path $outDir 'server.out.log'
  $srvErr = Join-Path $outDir 'server.err.log'
  $srv = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port", '--name', 'PartyUp admin'
    ) -WindowStyle Hidden -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru
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

  $body = @{ token = $bootstrap; username = 'admin1'; password = 'admin-pass-123' } | ConvertTo-Json
  $session = Invoke-RestMethod -Method Post -Uri "$base/api/bootstrap" -ContentType 'application/json' -Body $body
  $ownerToken = $session.token
  Log 'BOOTSTRAP_OK'

  # 第二个成员（用邀请码注册）
  $invite = Invoke-RestMethod -Method Post -Uri "$base/api/admin/invites" -Headers @{ Authorization = "Bearer $ownerToken" } -ContentType 'application/json' -Body '{}'
  # 响应是 {"invite": {...}}（与 admin 列表同壳），不是裸对象。
  $inviteCode = $invite.invite.code
  Log ("INVITE_CODE=" + $inviteCode)
  $regUrl = "$base/api/invite/" + $inviteCode
  $regBody = @{ username = 'member1'; password = 'member-pass-123' } | ConvertTo-Json
  $reg = Invoke-RestMethod -Method Post -Uri $regUrl -ContentType 'application/json' -Body $regBody
  Log ("SECOND_MEMBER_OK token_len=" + $reg.token.Length)

  $cliOut = Join-Path $outDir 'client.out.log'
  $cliErr = Join-Path $outDir 'client.err.log'
  $cli = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', $base, '--token', $ownerToken, '--screen', '2', '--appdata', $testAppData) `
    -RedirectStandardOutput $cliOut -RedirectStandardError $cliErr -PassThru

  $userCard = Find-ById $cli.Id 'UserCard' 40
  Assert 'main-ui-loaded' ($null -ne $userCard) 'UserCard 出现'
  Invoke-El $userCard 'UserCard'
  # Flyout 里的 MenuItem 在 Avalonia 下既可能不支持 InvokePattern、包围盒也可能取到 0；
  # 因此按 AutomationId 定位（按 Name 会命中别的元素），再交给 Invoke-El 的多路兜底。
  $settingsItem = Find-ById $cli.Id 'OpenSettings' 10
  Assert 'settings-menu-visible' ($null -ne $settingsItem) '菜单项 OpenSettings 出现'
  Invoke-El $settingsItem 'OpenSettings'
  Start-Sleep -Milliseconds 900

  $openAdmin = Find-ById $cli.Id 'OpenServerAdmin' 12
  Assert 'admin-entry-visible' ($null -ne $openAdmin) '设置页出现「打开服务器管理」（HasServerAdminRights=true）'
  Invoke-El $openAdmin 'OpenServerAdmin'
  Start-Sleep -Seconds 2

  $memberList = Find-ById $cli.Id 'AdminMemberList' 20
  Assert 'admin-panel-opened' ($null -ne $memberList) 'AdminMemberList 出现'
  $roleCaption = Find-ById $cli.Id 'AdminRoleCaption' 8
  Assert 'role-caption-owner' ((Text-Of $roleCaption) -eq '所有者') ("RoleCaption=" + (Text-Of $roleCaption))

  # 概览：读第一行文本（AdminView 顶部 TextBlock 无 AutomationId，用成员数间接验证 + 列表项数）
  $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $cli.Id)
  $win = $AE::RootElement.FindFirst($TS::Children, $cond)
  $items = @()
  if ($memberList) {
    $ic = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    $items = $memberList.FindAll($TS::Descendants, $ic)
  }
  Assert 'member-list-has-2' ($items.Count -ge 2) ("ListItem 数=" + $items.Count + " names=" + (($items | ForEach-Object { $_.Current.Name }) -join '|'))

  # 选 member1 → 封禁
  $target = $items | Where-Object { $_.Current.Name -like '*member1*' } | Select-Object -First 1
  Assert 'member1-listed' ($null -ne $target) 'member1 在成员列表'
  if ($target) { Invoke-El $target 'member1'; Start-Sleep -Milliseconds 500 }
  $banBtn = Find-ById $cli.Id 'AdminBan' 8
  Assert 'ban-enabled' (($null -ne $banBtn) -and $banBtn.Current.IsEnabled) ("IsEnabled=" + $(if ($banBtn) { $banBtn.Current.IsEnabled } else { 'n/a' }))
  if ($banBtn -and $banBtn.Current.IsEnabled) {
    Invoke-El $banBtn 'AdminBan'
    Start-Sleep -Seconds 2
  }
  $status = Find-ById $cli.Id 'AdminStatus' 8
  Assert 'ban-status-text' ((Text-Of $status) -like '*封禁*') ("AdminStatus=" + (Text-Of $status))
  $errEl = Find-ById $cli.Id 'AdminError' 3
  Assert 'no-admin-error' ($null -eq $errEl) ("AdminError=" + (Text-Of $errEl))

  # 生成邀请码
  $createInvite = Find-ById $cli.Id 'AdminCreateInvite' 8
  if ($createInvite) { Invoke-El $createInvite 'AdminCreateInvite'; Start-Sleep -Seconds 2 }
  $newCode = Find-ById $cli.Id 'AdminNewInviteCode' 8
  Assert 'invite-code-created' ((Text-Of $newCode).Length -ge 4) ("code=" + (Text-Of $newCode))

  # 危险设置对 owner 可用
  $voiceMode = Find-ById $cli.Id 'AdminVoiceMode' 8
  Assert 'dangerous-settings-enabled-for-owner' (($null -ne $voiceMode) -and $voiceMode.Current.IsEnabled) ("IsEnabled=" + $(if ($voiceMode) { $voiceMode.Current.IsEnabled } else { 'n/a' }))

  # 审计日志：至少有一条（成员列表页签下方 ItemsControl 里的文本）
  $auditSeen = $false
  $all = $win.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
  foreach ($el in $all) {
    if ($el.Current.Name -like '*member.ban*' -or $el.Current.Name -like '*invite.create*' -or $el.Current.Name -like '*member.role*') {
      $auditSeen = $true; Log ("AUDIT_LINE: " + $el.Current.Name); break
    }
  }
  Assert 'audit-visible' $auditSeen '审计行含 member.ban / invite.create'

  $shot = Join-Path $outDir 'admin-panel.png'
  & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $cli.Id -OutPath $shot | ForEach-Object { Log $_ }
  Assert 'screenshot' (Test-Path $shot) $shot

  # UIA dump 便于补齐 AutomationId
  $dump = Join-Path $outDir 'uia-dump.txt'
  $lines = New-Object System.Collections.ArrayList
  foreach ($el in $all) {
    [void]$lines.Add(("{0}`t{1}`t{2}`t{3}" -f $el.Current.ControlType.ProgrammaticName, $el.Current.AutomationId, $el.Current.Name, $el.Current.IsEnabled))
  }
  Set-Content -Path $dump -Value $lines -Encoding UTF8
  Log ("UIA_DUMP=" + $dump + " lines=" + $lines.Count)

  $json = Join-Path $outDir 'results.json'
  $results | ConvertTo-Json -Depth 4 | Set-Content -Path $json -Encoding UTF8
  $passCount = ($results | Where-Object { $_.pass }).Count
  Log ("SUMMARY pass={0}/{1}" -f $passCount, $results.Count)
}
finally {
  if ($cli) { $cli.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 1200; if (Get-Process -Id $cli.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $cli.Id -Force } }
  if ($srv -and (Get-Process -Id $srv.Id -ErrorAction SilentlyContinue)) { Stop-Process -Id $srv.Id -Force }
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $ClientExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
  if ($lockHeld) { Remove-Item -Force $lockPath -ErrorAction SilentlyContinue }
  Log 'CLEANUP=done'
}
