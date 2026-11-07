param(
  # 角色 / 管理 API / 审计 / 封禁 / bootstrap 安全边界的真机验收。
  # 全部隔离在 18084 + %LOCALAPPDATA%\PartyUpQA3，绝不触碰 8080 / 18081 / 18082。
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA3",
  [int]$Port = 18084,
  [string]$ServerExe = "",
  [int]$TtlWaitSeconds = 0,
  [switch]$SkipClient
)

# 真机验收（规格 `.docs/identity-and-onboarding-20261107.md` §10）：
#   A. owner 唯一 + 旧库回填（users CLI / API）
#   B. 权限矩阵：user 403 ADMIN_ONLY、admin 危险设置/动 owner 403 OWNER_ONLY、owner 全通过
#   C. 封禁闭环：在线客户端被踢 + 登录 403 MEMBER_BANNED + /api/me 401 + 解封恢复
#   D. 审计：动作可查、最新在前、普通用户 403
#   E. bootstrap 边界：非回环明文 403 + 日志理由；回环 201；已初始化 409
# 证据写到 $Root\evidence\role-accept-<ts>.txt（含原始 curl 输出）。
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$ProgressPreference = 'SilentlyContinue'

$evidenceDir = Join-Path $Root 'evidence'
New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidence = Join-Path $evidenceDir "role-accept-$stamp.txt"
$script:lines = [System.Collections.Generic.List[string]]::new()
$script:pass = 0
$script:fail = 0

function Log([string]$text) {
  $script:lines.Add($text)
  Write-Host $text
}
function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { $script:pass++; Log "PASS $name" }
  else { $script:fail++; Log "FAIL $name :: $detail" }
  if ($detail) { Log "     $detail" }
}
# C <method> <url> [token] [jsonBody] → @{ Code; Body }
function C {
  param([string]$Method, [string]$Url, [string]$Token = '', [string]$Body = '')
  $a = @('-s', '-X', $Method, '-w', "`n__STATUS__%{http_code}")
  if ($Body) { $a += @('-H', 'Content-Type: application/json', '--data-binary', $Body) }
  if ($Token) { $a += @('-H', "Authorization: Bearer $Token") }
  $a += $Url
  $raw = & curl.exe @a 2>$null
  $text = ($raw | Out-String).TrimEnd()
  $idx = $text.LastIndexOf('__STATUS__')
  if ($idx -lt 0) { return [pscustomobject]@{ Code = 0; Body = $text } }
  $code = 0
  [void][int]::TryParse($text.Substring($idx + 10).Trim(), [ref]$code)
  [pscustomobject]@{ Code = $code; Body = $text.Substring(0, $idx).TrimEnd() }
}
function Field([string]$text, [string]$name) {
  $m = [regex]::Match($text, '"' + [regex]::Escape($name) + '"\s*:\s*"([^"]*)"')
  if ($m.Success) { return $m.Groups[1].Value }
  return ''
}
# Kv 解析「KEY=VALUE」行（服务端脚本 stdout 的机器可读块）。
function Kv([string]$text, [string]$key) {
  $m = [regex]::Match($text, '(?m)^' + [regex]::Escape($key) + '=(\S+)\s*$')
  if ($m.Success) { return $m.Groups[1].Value }
  return ''
}
function Raw([string]$label, [string]$text) {
  Log "  --- $label ---"
  foreach ($line in ($text -split "`n")) { if ($line.Trim()) { Log "  $line" } }
}

$serverScript = Join-Path $PSScriptRoot 'auth-qa3-server.ps1'
$clientsScript = Join-Path $PSScriptRoot 'auth-qa3-clients.ps1'
$exe = if ($ServerExe) { $ServerExe } else { Join-Path $Root 'partyup-qa.exe' }
$base = "http://127.0.0.1:$Port"
$data = Join-Path $Root 'data'

function Stop-QaServer {
  $procs = Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" |
    Where-Object { $_.CommandLine -like "*$Root*" }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  if ($procs) { Start-Sleep -Milliseconds 400 }
}
function Stop-QaClients {
  $procs = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" |
    Where-Object { $_.ExecutablePath -like "*client-auth*" -or $_.CommandLine -like "*client-auth*" }
  foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
  if ($procs) { Start-Sleep -Milliseconds 400 }
}

Log "ROLE_ACCEPT root=$Root port=$Port exe=$exe"
Log "STARTED_AT=$(Get-Date -Format o)"

# ============================================================
# A. 主实例：alice=owner、bob=user（bootstrap 建 owner；bob 邀请注册）
# ============================================================
Log ''
Log '=== PHASE A: roles / permission matrix / ban / audit ==='
$serverOut = & $serverScript -Root $Root -Port $Port -Fresh 2>&1 | Out-String
$aliceToken = Kv $serverOut 'QA3_ALICE_TOKEN'
$aliceId = Kv $serverOut 'QA3_ALICE_ID'
$bobToken = Kv $serverOut 'QA3_BOB_TOKEN'
$bobId = Kv $serverOut 'QA3_BOB_ID'
Check 'A1 main instance started (alice+bob tokens)' ([bool]$aliceToken -and [bool]$bobToken) "alice=$([bool]$aliceToken) bob=$([bool]$bobToken)"

# owner 唯一 + bootstrap_allowed=false（已初始化）
$r = C GET "$base/api/admin/members" $aliceToken
$ownerCount = ([regex]::Matches($r.Body, '"role":"owner"')).Count
Check 'A2 exactly one owner via /api/admin/members' ($r.Code -eq 200 -and $ownerCount -eq 1) "code=$($r.Code) ownerCount=$ownerCount"
Raw 'members' $r.Body

$r = C GET "$base/api/server-info"
Check 'A3 server-info anonymous: initialized=true bootstrap_allowed=false' ($r.Code -eq 200 -and $r.Body -match '"initialized":true' -and $r.Body -match '"bootstrap_allowed":false') "code=$($r.Code) body=$($r.Body)"
Raw 'server-info' $r.Body

# carol：邀请码注册（user）
$r = C POST "$base/api/admin/invites" $aliceToken '{}'
$inviteCode = Field $r.Body 'code'
Check 'A4 owner creates invite via admin API' ($r.Code -eq 201 -and [bool]$inviteCode) "code=$($r.Code) invite=$inviteCode"
$r = C POST "$base/api/invite/$inviteCode" '' '{"username":"carol","password":"carol-pass-123","display_name":"Carol"}'
$carolToken = Field $r.Body 'token'
$carolId = ''
$m = [regex]::Match($r.Body, '"member":\{"id":"([^"]+)"')
if ($m.Success) { $carolId = $m.Groups[1].Value }
Check 'A5 carol registers as user' ($r.Code -eq 201 -and [bool]$carolToken -and [bool]$carolId) "code=$($r.Code) id=$carolId"

# bob：promote to admin（owner 操作）
$r = C POST "$base/api/admin/members/$bobId/role" $aliceToken '{"role":"admin"}'
Check 'A6 owner promotes bob to admin' ($r.Code -eq 200) "code=$($r.Code) body=$($r.Body)"

# ============================================================
# B. 权限矩阵
# ============================================================
Log ''
Log '=== PHASE B: permission matrix ==='
$r = C GET "$base/api/admin/overview" $carolToken
Check 'B1 user GET /api/admin/overview -> 403 ADMIN_ONLY' ($r.Code -eq 403 -and $r.Body -match 'ADMIN_ONLY') "code=$($r.Code)"
$r = C PATCH "$base/api/admin/settings" $carolToken '{"announcement":"x"}'
Check 'B2 user PATCH settings -> 403 ADMIN_ONLY' ($r.Code -eq 403 -and $r.Body -match 'ADMIN_ONLY') "code=$($r.Code)"

$r = C PATCH "$base/api/admin/settings" $bobToken '{"announcement":"hello from admin"}'
Check 'B3 admin PATCH non-dangerous setting -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C PATCH "$base/api/admin/settings" $bobToken '{"voice_mode":"direct"}'
Check 'B4 admin PATCH dangerous voice_mode -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code) body=$($r.Body)"
$r = C PATCH "$base/api/admin/settings" $bobToken '{"max_screen_shares":8}'
Check 'B5 admin PATCH dangerous max_screen_shares -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"
# 值必须没被 admin 改掉
$r = C GET "$base/api/admin/settings" $aliceToken
Check 'B6 admin dangerous change did not take effect (voice_mode=relay)' ($r.Body -match '"voice_mode":"relay"') "body=$($r.Body)"

$r = C POST "$base/api/admin/members/$aliceId/kick" $bobToken ''
Check 'B7 admin kick owner -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"
$r = C POST "$base/api/admin/members/$aliceId/ban" $bobToken '{"reason":"x"}'
Check 'B8 admin ban owner -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"
$r = C POST "$base/api/admin/members/$aliceId/role" $bobToken '{"role":"user"}'
Check 'B9 admin demote owner -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"
$r = C POST "$base/api/admin/members/$carolId/role" $bobToken '{"role":"admin"}'
Check 'B10 admin appoints admin -> 403 OWNER_ONLY (owner-only op)' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"

# owner：危险设置生效 + server-info 反映 + 审计 server.mode_change
$r = C PATCH "$base/api/admin/settings" $aliceToken '{"voice_mode":"direct","screen_mode":"direct"}'
Check 'B11 owner PATCH dangerous modes -> 200' ($r.Code -eq 200) "code=$($r.Code) body=$($r.Body)"
$r = C GET "$base/api/server-info"
Check 'B12 server-info reflects new modes (direct/direct)' ($r.Body -match '"voice_mode":"direct"' -and $r.Body -match '"screen_mode":"direct"') "body=$($r.Body)"

# admin：成员/频道/邀请可管理
$r = C GET "$base/api/admin/members" $bobToken
Check 'B13 admin GET members -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C POST "$base/api/admin/channels" $bobToken '{"name":"admin-created"}'
$channelId = Field $r.Body 'id'
Check 'B14 admin create channel -> 201' ($r.Code -eq 201 -and [bool]$channelId) "code=$($r.Code)"
$r = C DELETE "$base/api/admin/channels?id=$channelId" $bobToken
Check 'B15 admin delete empty channel -> 204' ($r.Code -eq 204) "code=$($r.Code)"

# ============================================================
# C. 封禁闭环（含在线客户端被踢）
# ============================================================
Log ''
Log '=== PHASE C: ban loop with live client ==='
if (-not $SkipClient) {
  try {
    & $clientsScript -Root $Root -Tag 'carol' -Token $carolToken -Server $base -Copy | Out-Null
    Check 'C0 carol client launched' $true 'client-auth/carol'
  } catch {
    Check 'C0 carol client launched' $false "$_"
  }
}
$r = C POST "$base/api/admin/members/$carolId/ban" $aliceToken '{"reason":"acceptance ban"}'
Check 'C1 owner bans carol -> 200' ($r.Code -eq 200) "code=$($r.Code)"
# C1b：封禁必须用 reason=banned + 关闭码 4003（服务端日志是主证据）。
$errLog = Join-Path $Root 'server.err'
$hit = ''
if (Test-Path $errLog) { $hit = (Select-String -Path $errLog -Pattern 'reason=banned close_code=4003' -SimpleMatch | Select-Object -First 1).Line }
Check 'C1b server log: ban -> reason=banned close_code=4003' ([bool]$hit) "line=$hit"
if ($hit) { Log "  RAW_ERR: $hit" }
$r = C GET "$base/api/me" $carolToken
Check 'C2 banned token /api/me -> 401 (session revoked)' ($r.Code -eq 401) "code=$($r.Code)"
$r = C POST "$base/api/login" '' '{"username":"carol","password":"carol-pass-123"}'
Check 'C3 banned login -> 403 MEMBER_BANNED' ($r.Code -eq 403 -and $r.Body -match 'MEMBER_BANNED') "code=$($r.Code) body=$($r.Body)"
if (-not $SkipClient) {
  Start-Sleep -Seconds 3
  $carolLog = Join-Path $Root 'client-auth\logs\carol.log'
  $hit = ''
  if (Test-Path $carolLog) { $hit = (Select-String -Path $carolLog -Pattern 'connection lost|connection closed' -SimpleMatch:$false | Select-Object -First 1).Line }
  Check 'C4 online client kicked (log: connection lost/closed)' ([bool]$hit) "log=$carolLog line=$hit"
  if ($hit) { Log "  RAW_CLIENT: $hit" }
}

$r = C POST "$base/api/admin/members/$carolId/unban" $aliceToken ''
Check 'C5 owner unbans carol -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C POST "$base/api/login" '' '{"username":"carol","password":"carol-pass-123"}'
Check 'C6 carol login after unban -> 200' ($r.Code -eq 200) "code=$($r.Code)"
if ($r.Code -eq 200) { $carolToken = Field $r.Body 'token' }

# ============================================================
# D. 审计
# ============================================================
Log ''
Log '=== PHASE D: audit ==='
$r = C GET "$base/api/admin/audit?limit=100" $aliceToken
$auditBody = $r.Body
$wantActions = @('member.ban', 'member.unban', 'settings.update', 'server.mode_change', 'channel.create', 'member.role_change')
$missing = @()
foreach ($a in $wantActions) { if ($auditBody -notmatch [regex]::Escape('"' + $a + '"')) { $missing += $a } }
Check 'D1 audit contains required actions' ($r.Code -eq 200 -and $missing.Count -eq 0) "code=$($r.Code) missing=$($missing -join ',')"
$r = C GET "$base/api/admin/audit" $carolToken
Check 'D2 user GET audit -> 403 ADMIN_ONLY' ($r.Code -eq 403 -and $r.Body -match 'ADMIN_ONLY') "code=$($r.Code)"
Raw 'audit-head' (($auditBody -split "`n")[0])

# ============================================================
# E. owner 转让（唯一性保持）
# ============================================================
Log ''
Log '=== PHASE E: owner transfer ==='
$r = C POST "$base/api/admin/owner/transfer" $aliceToken ("{`"member_id`":`"$bobId`"}")
Check 'E1 owner transfers ownership to bob -> 200' ($r.Code -eq 200) "code=$($r.Code) body=$($r.Body)"
$r = C GET "$base/api/me" $aliceToken
Check 'E2 old owner now admin' ($r.Code -eq 200 -and $r.Body -match '"role":"admin"' -and $r.Body -match '"admin":true') "body=$($r.Body)"
$r = C GET "$base/api/me" $bobToken
Check 'E3 new owner role=owner' ($r.Code -eq 200 -and $r.Body -match '"role":"owner"') "body=$($r.Body)"
$r = C GET "$base/api/admin/members" $bobToken
$ownerCount = ([regex]::Matches($r.Body, '"role":"owner"')).Count
Check 'E4 still exactly one owner after transfer' ($r.Code -eq 200 -and $ownerCount -eq 1) "ownerCount=$ownerCount"
$r = C POST "$base/api/admin/owner/transfer" $aliceToken ("{`"member_id`":`"$aliceId`"}")
Check 'E5 ex-owner (admin) transfer -> 403 OWNER_ONLY' ($r.Code -eq 403 -and $r.Body -match 'OWNER_ONLY') "code=$($r.Code)"
# 转回 alice，保持数据一致
$r = C POST "$base/api/admin/owner/transfer" $bobToken ("{`"member_id`":`"$aliceId`"}")
Check 'E6 new owner transfers back -> 200' ($r.Code -eq 200) "code=$($r.Code)"

# ============================================================
# H. 会话终止原因与关闭码（login_elsewhere=4001 / kicked=4002 / banned=4003）
# ============================================================
Log ''
Log '=== PHASE H: session.replaced reasons / close codes ==='
$errLog = Join-Path $Root 'server.err'

# H1 login_elsewhere：carol 已有在线客户端，再登录一次 → 旧连接 4001。
if (-not $SkipClient) {
  try { & $clientsScript -Root $Root -Tag 'carol2' -Token $carolToken -Server $base | Out-Null } catch { }
  Start-Sleep -Seconds 2
}
$r = C POST "$base/api/login" '' '{"username":"carol","password":"carol-pass-123"}'
Check 'H1 relogin carol -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$hit = ''
if (Test-Path $errLog) { $hit = (Select-String -Path $errLog -Pattern 'reason=login_elsewhere close_code=4001' -SimpleMatch | Select-Object -First 1).Line }
Check 'H2 server log: relogin -> reason=login_elsewhere close_code=4001' ([bool]$hit) "line=$hit"
if ($hit) { Log "  RAW_ERR: $hit" }
$m = [regex]::Match($r.Body, '"token":"([^"]+)"')
if ($m.Success) { $carolToken = $m.Groups[1].Value }
if (-not $SkipClient) {
  Start-Sleep -Seconds 2
  $c2Log = Join-Path $Root 'client-auth\logs\carol2.log'
  $hit2 = ''
  if (Test-Path $c2Log) { $hit2 = (Select-String -Path $c2Log -Pattern 'connection lost|connection closed' | Select-Object -First 1).Line }
  Check 'H3 replaced carol client sees disconnect' ([bool]$hit2) "line=$hit2"
}

# H4 kicked：bob 在线时被 owner 踢下线 → 4002。
if (-not $SkipClient) {
  try { & $clientsScript -Root $Root -Tag 'bob' -Token $bobToken -Server $base | Out-Null } catch { }
  Start-Sleep -Seconds 2
}
$r = C POST "$base/api/admin/members/$bobId/kick" $aliceToken ''
Check 'H4 owner kicks bob -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$hit = ''
if (Test-Path $errLog) { $hit = (Select-String -Path $errLog -Pattern 'reason=kicked close_code=4002' -SimpleMatch | Select-Object -First 1).Line }
Check 'H5 server log: kick -> reason=kicked close_code=4002' ([bool]$hit) "line=$hit"
if ($hit) { Log "  RAW_ERR: $hit" }
if (-not $SkipClient) {
  Start-Sleep -Seconds 2
  $bobLog = Join-Path $Root 'client-auth\logs\bob.log'
  $hit2 = ''
  if (Test-Path $bobLog) { $hit2 = (Select-String -Path $bobLog -Pattern 'connection lost|connection closed' | Select-Object -First 1).Line }
  Check 'H6 kicked bob client sees disconnect' ([bool]$hit2) "line=$hit2"
}

# ============================================================
# I. 开放注册（registration=open）
# ============================================================
Log ''
Log '=== PHASE I: open registration ==='
$r = C PATCH "$base/api/admin/settings" $aliceToken '{"registration":"open"}'
Check 'I1 owner enables open registration -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C GET "$base/api/server-info"
Check 'I2 server-info registration=open' ($r.Code -eq 200 -and $r.Body -match '"registration":"open"') "body=$($r.Body)"
$r = C POST "$base/api/register" '' '{"username":"eve","password":"eve-pass-123","display_name":"Eve"}'
Check 'I3 anonymous register under open -> 201' ($r.Code -eq 201) "code=$($r.Code) body=$($r.Body)"
$eveRole = ''
if ($r.Body -match '"role":"([^"]+)"') { $eveRole = $Matches[1] }
Check 'I4 registered member role=user' ($eveRole -eq 'user') "role=$eveRole"
$r = C PATCH "$base/api/admin/settings" $aliceToken '{"member_limit":4}'
Check 'I5 set member_limit=4 -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C POST "$base/api/register" '' '{"username":"frank","password":"frank-pass-123"}'
Check 'I6 register over member_limit -> 409' ($r.Code -eq 409) "code=$($r.Code) body=$($r.Body)"
$r = C PATCH "$base/api/admin/settings" $aliceToken '{"registration":"closed"}'
Check 'I7 set registration=closed -> 200' ($r.Code -eq 200) "code=$($r.Code)"
$r = C POST "$base/api/register" '' '{"username":"frank","password":"frank-pass-123"}'
Check 'I8 register when closed -> 403' ($r.Code -eq 403) "code=$($r.Code) body=$($r.Body)"
# 还原设置，避免影响后续 F/G 阶段。
$null = C PATCH "$base/api/admin/settings" $aliceToken '{"registration":"invite","member_limit":0}'

# ============================================================
# F. 停主实例，用 users CLI 核对持久化角色与 owner 唯一性
# ============================================================
Log ''
Log '=== PHASE F: users CLI (persisted state) ==='
Stop-QaClients
Stop-QaServer
$usersOut = & $exe users --data $data 2>&1 | Out-String
Log $usersOut.Trim()
$owners = [regex]::Match($usersOut, 'OWNERS=(\d+)')
$usersCount = [regex]::Match($usersOut, 'USERS=(\d+)')
Check 'F1 users CLI: exactly one owner persisted' ($owners.Success -and [int]$owners.Groups[1].Value -eq 1) "owners=$($owners.Groups[1].Value)"
Check 'F2 users CLI: alice is owner after transfer back' ($usersOut -match 'member=alice\s+member_id=\S+\s+role=owner') "see raw above"
Check 'F3 users CLI shows 4 users (alice/bob/carol/eve)' ($usersCount.Success -and [int]$usersCount.Groups[1].Value -eq 4) "users=$($usersCount.Groups[1].Value)"

# ============================================================
# G. bootstrap 安全边界（独立实例，绑 0.0.0.0，全新数据目录）
# ============================================================
Log ''
Log '=== PHASE G: bootstrap boundary (non-loopback plaintext) ==='
$bootstrapData = 'data-bootstrap'
$serverOut = & $serverScript -Root $Root -Port $Port -DataSubdir $bootstrapData -Fresh -NoBootstrap -ListenAddr "0.0.0.0:$Port" 2>&1 | Out-String
$bootToken = Kv $serverOut 'QA3_BOOTSTRAP_TOKEN'
Check 'G0 bootstrap-only instance started on 0.0.0.0' ([bool]$bootToken) "token=$([bool]$bootToken)"

$lanIp = ''
try {
  $cfg = Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -ne $null -and $_.IPv4Address } | Select-Object -First 1
  if ($cfg) { $lanIp = $cfg.IPv4Address.IPAddress }
} catch { }
if (-not $lanIp) {
  $lanIp = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.AddressState -eq 'Preferred' } |
    Select-Object -First 1).IPAddress
}
Log "LAN_IP=$lanIp"
Check 'G1 LAN IP discovered for non-loopback test' ([bool]$lanIp) "lanIp=$lanIp"

if ($lanIp) {
  $r = C GET "http://${lanIp}:$Port/api/server-info"
  Check 'G2 non-loopback server-info: bootstrap_allowed=false' ($r.Code -eq 200 -and $r.Body -match '"bootstrap_allowed":false') "code=$($r.Code) body=$($r.Body)"
  $r = C POST "http://${lanIp}:$Port/api/bootstrap" '' ("{`"token`":`"$bootToken`",`"username`":`"mallory`",`"password`":`"secret123`"}")
  Check 'G3 non-loopback plaintext bootstrap -> 403' ($r.Code -eq 403) "code=$($r.Code) body=$($r.Body)"
  $errLog = Join-Path $Root 'server.err'
  $hit = ''
  if (Test-Path $errLog) { $hit = (Select-String -Path $errLog -Pattern 'bootstrap rejected' -SimpleMatch | Select-Object -First 1).Line }
  Check 'G4 server log records rejection reason' ([bool]$hit) "line=$hit"
  if ($hit) { Log "  RAW_ERR: $hit" }
  # 回环仍然允许：同一个 token 从 127.0.0.1 bootstrap 成功
  $r = C POST "http://127.0.0.1:$Port/api/bootstrap" '' ("{`"token`":`"$bootToken`",`"username`":`"mallory`",`"password`":`"secret123`"}")
  Check 'G5 loopback bootstrap with same token -> 201' ($r.Code -eq 201) "code=$($r.Code) body=$($r.Body)"
  $r = C POST "http://127.0.0.1:$Port/api/bootstrap" '' ("{`"token`":`"$bootToken`",`"username`":`"eve`",`"password`":`"secret123`"}")
  Check 'G6 second bootstrap -> 409 ALREADY_BOOTSTRAPPED' ($r.Code -eq 409 -and $r.Body -match 'ALREADY_BOOTSTRAPPED') "code=$($r.Code) body=$($r.Body)"
}

Stop-QaServer
Stop-QaClients

# ============================================================
# 汇总与落盘
# ============================================================
Log ''
Log "SUMMARY PASS=$script:pass FAIL=$script:fail"
$result = if ($script:fail -eq 0) { 'PASS' } else { 'FAIL' }
Log "RESULT=$result"
Log "EVIDENCE=$evidence"
$script:lines -join "`n" | Set-Content -Encoding UTF8 $evidence
Write-Output "QA3_EVIDENCE=$evidence"
Write-Output "QA3_PASS=$script:pass"
Write-Output "QA3_FAIL=$script:fail"
Write-Output "QA3_RESULT=$result"
if ($script:fail -ne 0) { exit 1 }
