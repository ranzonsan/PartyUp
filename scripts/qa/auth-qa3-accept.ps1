param(
  # 认证链路（A logout / B TTL / C 单端登录 / D 登录限速）的 18084 真机验收编排。
  # 前置：先 build 出 $Root\partyup-qa.exe（Windows Go，见脚本头注释），再运行本脚本。
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA3",
  [int]$Port = 18084,
  [string]$EvidenceDir = "",
  [int]$TtlWaitSeconds = 65
)

# 真机验收编排。边界：
#   - 只操作 $Root（PartyUpQA3）、只监听 18084、STUN 用 34784；
#   - 客户端只用 $Root\client-auth 副本，杀进程按 client-auth 路径过滤；
#   - 证据（命令原文 + 输出）写到 $Root\evidence\auth-accept-<stamp>.txt。
$ErrorActionPreference = 'Stop'
# 控制台/管道统一 UTF-8：curl 与客户端日志都是 UTF-8，默认代码页会把中文解成乱码，
# 进而让 ConvertFrom-Json 失败（Step 3 曾因此中止）。
$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$base = "http://127.0.0.1:$Port"
$exe = Join-Path $Root 'partyup-qa.exe'
$srvScript = Join-Path $PSScriptRoot 'auth-qa3-server.ps1'
$cliScript = Join-Path $PSScriptRoot 'auth-qa3-clients.ps1'
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'

if (-not $EvidenceDir) { $EvidenceDir = Join-Path $Root 'evidence' }
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$transcriptPath = Join-Path $EvidenceDir "auth-accept-$stamp.txt"

# 任何终止性错误都要留下证据文件并失败退出，否则真机失败只剩「脚本没跑完」。
trap {
  Say ("ERROR: " + $_.Exception.Message)
  Say ($_.ScriptStackTrace)
  $script:lines | Set-Content -Encoding UTF8 $transcriptPath
  Write-Output ("EVIDENCE=" + $transcriptPath)
  exit 1
}

$script:lines = [System.Collections.Generic.List[string]]::new()
$script:pass = 0
$script:fail = 0
function Say([string]$s) {
  $script:lines.Add($s)
  Write-Output $s
}
function C([string[]]$a) {
  # 真 curl 输出：验收要贴的就是原始 HTTP 报文/状态行。
  $out = & $curl @a 2>&1
  Say ($out -join "`n")
  return $out
}
function Code([string[]]$a) {
  # 只取 HTTP 状态码（-o NUL 丢 body）。
  # 注意：PowerShell 的裸 @(...) 不是 splat（会当成一个数组参数），必须拼成数组变量再 @all。
  $all = @('-s', '-o', 'NUL', '-w', '%{http_code}') + $a
  $out = & $curl @all 2>&1
  return ("$out").Trim()
}
function Check([bool]$ok, [string]$label) {
  if ($ok) { $script:pass++; Say ("CHECK PASS: " + $label) }
  else { $script:fail++; Say ("CHECK FAIL: " + $label) }
}
function JsonField([object]$resp, [string]$name) { return $resp.$name }

Say "===== auth acceptance on $base ($stamp) ====="
Say ("EXE=" + $exe)

# ---------------------------------------------------------------
# PHASE 1：启动隔离实例（18084 / PartyUpQA3），bootstrap alice + 邀请注册 bob
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 1: start isolated server (18084, data under PartyUpQA3) ====="
$setupOut = & $srvScript -Root $Root -Port $Port -Fresh
Say ($setupOut -join "`n")
$state = Get-Content (Join-Path $Root 'tokens.json') -Raw | ConvertFrom-Json
$aliceToken1 = $state.aliceToken
$bobToken = $state.bobToken
Say ("server_pid=" + $state.serverPid + " data=" + $state.dataDir)

# ---------------------------------------------------------------
# PHASE 2：起 alice 客户端（client-auth 副本），先证明旧端在线
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 2: launch alice client from client-auth (old end) ====="
$cliOut = & $cliScript -Root $Root -Tag alice -Account alice -RestartAll -Copy
Say ($cliOut -join "`n")
Start-Sleep -Seconds 3
$aliceLog = Join-Path (Join-Path $Root 'client-auth') 'logs\alice.log'
Say "--- alice client log tail (old end online) ---"
$aliceTail = Get-Content $aliceLog -Tail 20 -ErrorAction SilentlyContinue
Say ($aliceTail -join "`n")
Check ([bool](Select-String -Path $aliceLog -Pattern 'auto-login' -SimpleMatch -Quiet)) "old client connected (auto-login log present)"

# ---------------------------------------------------------------
# PHASE 3（C）：同一账号第二次登录 → 旧端被踢、server.log 有 session replaced、
# DB 只剩 1 条 active session、旧 token /api/me 401
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 3 (C): second login of same account replaces the old end ====="
$loginOut = & $curl -s -X POST "$base/api/login" -H 'Content-Type: application/json' `
  -d '{"username":"alice","password":"alice-pass-123"}' 2>&1
Say "--- curl POST /api/login (2nd login) ---"
Say ($loginOut -join "`n")
$loginText = $loginOut -join "`n"
# 不用 ConvertFrom-Json 解析 curl 输出：JSON 里的中文（display_name）会受控制台代码页
# 影响，token 本身是 ASCII，用正则提取是稳的。
if ($loginText -notmatch '"token":"([^"]+)"') { throw "login response has no token: $loginText" }
$aliceToken2 = $Matches[1]
Say ("alice token2 = " + $aliceToken2)
Start-Sleep -Seconds 2

Say "--- curl GET /api/me with OLD token (expect 401) ---"
$oldMeCode = Code @("$base/api/me", '-H', "Authorization: Bearer $aliceToken1")
Say ("OLD_TOKEN_ME_STATUS=" + $oldMeCode)
Check ($oldMeCode -eq '401') "old token rejected after new login (401)"

Say "--- server.err: session replaced log ---"
$replacedLog = Get-Content (Join-Path $Root 'server.err') -ErrorAction SilentlyContinue |
  Where-Object { $_ -like '*session replaced*' } | Select-Object -Last 3
Say ($replacedLog -join "`n")
Check ([bool]$replacedLog) "server log contains 'session replaced'"

Say "--- partyup-qa.exe sessions --active (expect exactly 1 active session for alice) ---"
$sessOut = & $exe sessions --data $state.dataDir --active
Say ($sessOut -join "`n")
$aliceActive = @($sessOut | Where-Object { $_ -like '*member=alice*' -and $_ -like '*active=True*' }).Count
Say ("ALICE_ACTIVE_SESSIONS=" + $aliceActive)
Check ($aliceActive -eq 1) "exactly one active session for alice in DB"

Say "--- old client log tail (expect 'connection closed') ---"
Start-Sleep -Seconds 1
$aliceTail2 = Get-Content $aliceLog -Tail 20 -ErrorAction SilentlyContinue
Say ($aliceTail2 -join "`n")
Check ([bool](Select-String -Path $aliceLog -Pattern 'connection lost|connection closed' -Quiet)) "old client observed disconnect"

# 用新 token 起第二个客户端窗口（= 另一端）。它会在下一步 logout 时再被关闭。
Say ""
Say "===== PHASE 3b: launch second alice client with the NEW token ====="
$cliOut2 = & $cliScript -Root $Root -Tag alice2 -Account alice -Token $aliceToken2
Say ($cliOut2 -join "`n")
Start-Sleep -Seconds 2
$alice2Log = Join-Path (Join-Path $Root 'client-auth') 'logs\alice2.log'

# ---------------------------------------------------------------
# PHASE 4（A）：logout → 204 无 body、旧 token /api/me 401、WS 被 graceful 关闭
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 4 (A): POST /api/logout revokes the token ====="
Say "--- curl -i POST /api/logout (expect 204, no body) ---"
$logoutOut = C @('-i', '-X', 'POST', "$base/api/logout", '-H', "Authorization: Bearer $aliceToken2")
$logoutText = $logoutOut -join "`n"
Check ($logoutText -match 'HTTP/1\.[01] 204') "logout returns 204"
Check ($logoutText -notmatch '(?m)^\{') "logout body is empty"

Say "--- curl -i GET /api/me with the logged-out token (expect 401) ---"
$loggedOutCode = Code @("$base/api/me", '-H', "Authorization: Bearer $aliceToken2")
Say ("LOGGED_OUT_ME_STATUS=" + $loggedOutCode)
Check ($loggedOutCode -eq '401') "logged-out token rejected (401)"

Say "--- partyup-qa.exe sessions (expect alice has 0 active) ---"
$sessOut2 = & $exe sessions --data $state.dataDir --active
Say ($sessOut2 -join "`n")
$aliceActive2 = @($sessOut2 | Where-Object { $_ -like '*member=alice*' }).Count
Say ("ALICE_ACTIVE_AFTER_LOGOUT=" + $aliceActive2)
Check ($aliceActive2 -eq 0) "no active session for alice after logout"

Say "--- alice2 client log tail (expect 'connection closed' from session.replaced logout) ---"
Start-Sleep -Seconds 1
$alice2Tail = Get-Content $alice2Log -Tail 20 -ErrorAction SilentlyContinue
Say ($alice2Tail -join "`n")
Check ([bool](Select-String -Path $alice2Log -Pattern 'connection lost|connection closed' -Quiet)) "client WS closed by logout"

# ---------------------------------------------------------------
# PHASE 5（D）：连续 10 次错误密码 → 11 次 429 + Retry-After
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 5 (D): login rate limit (10 failures / 15m) ====="
$codes = @()
for ($i = 1; $i -le 10; $i++) {
  $c = Code @('-X', 'POST', "$base/api/login", '-H', 'Content-Type: application/json', `
      '-d', '{"username":"bob","password":"definitely-wrong"}')
  $codes += $c
  Say ("attempt ${i}: HTTP " + $c)
}
$allowed = @($codes | Where-Object { $_ -eq '401' }).Count
Say ("ATTEMPTS_401=" + $allowed)
Check ($allowed -eq 10) "first 10 wrong passwords return 401 (counted, not blocked)"

Say "--- 11th attempt: curl -i (expect 429 + Retry-After) ---"
$blocked = C @('-i', '-X', 'POST', "$base/api/login", '-H', 'Content-Type: application/json', `
    '-d', '{"username":"bob","password":"definitely-wrong"}')
$blockedText = $blocked -join "`n"
Check ($blockedText -match 'HTTP/1\.[01] 429') "11th attempt returns 429"
Check ($blockedText -match '(?im)^Retry-After:\s*\d+') "429 carries Retry-After header"
Check ($blockedText -match 'RATE_LIMITED') "429 body code is RATE_LIMITED"

# ---------------------------------------------------------------
# PHASE 6（B）：--session-ttl=1m 实例，等过期后 /api/me 401
# ---------------------------------------------------------------
Say ""
Say "===== PHASE 6 (B): --session-ttl=1m expiry ====="
$ttlOut = & $srvScript -Root $Root -Port $Port -Fresh -DataSubdir 'data-ttl' -SessionTTL '1m'
Say ($ttlOut -join "`n")
$ttlState = Get-Content (Join-Path $Root 'tokens.json') -Raw | ConvertFrom-Json
$ttlToken = $ttlState.aliceToken
$ttlData = $ttlState.dataDir

Say "--- curl GET /api/me before expiry (expect 200) ---"
$before = Code @("$base/api/me", '-H', "Authorization: Bearer $ttlToken")
Say ("TTL_ME_BEFORE=" + $before)
Check ($before -eq '200') "token valid right after issue"

Say ("waiting " + $TtlWaitSeconds + "s for 1m TTL to expire ...")
Start-Sleep -Seconds $TtlWaitSeconds

Say "--- curl -i GET /api/me after expiry (expect 401) ---"
$after = C @('-i', "$base/api/me", '-H', "Authorization: Bearer $ttlToken")
$afterText = $after -join "`n"
Check ($afterText -match 'HTTP/1\.[01] 401') "expired token rejected (401)"

Say "--- partyup-qa.exe sessions for data-ttl (expect active=False / expired) ---"
$ttlSess = & $exe sessions --data $ttlData
Say ($ttlSess -join "`n")
Check ([bool]($ttlSess | Where-Object { $_ -like '*member=alice*' -and $_ -like '*active=False*' })) "DB marks session inactive after TTL"

Say "--- server.err tail (TTL instance) ---"
$ttlErr = Get-Content (Join-Path $Root 'server.err') -Tail 10 -ErrorAction SilentlyContinue
Say ($ttlErr -join "`n")

# ---------------------------------------------------------------
# 收尾：汇总 + 落盘证据
# ---------------------------------------------------------------
Say ""
Say "===== SUMMARY ====="
Say ("PASS=" + $script:pass + " FAIL=" + $script:fail)
Say ("RESULT=" + $(if ($script:fail -eq 0) { 'PASS' } else { 'FAIL' }))
$script:lines | Set-Content -Encoding UTF8 $transcriptPath
Write-Output ("EVIDENCE=" + $transcriptPath)
if ($script:fail -ne 0) { exit 1 }
