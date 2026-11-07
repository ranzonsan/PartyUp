param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [int]$Port = 18088,
  [string]$EvidenceDir = (Join-Path $PSScriptRoot '..\..\.docs\kick-ban-20261107')
)

# 踢出 / 封禁 / 解封 三态真机验收（只写脚本与证据，不改产品代码）。
#
# 判据（每条都留原始日志行）：
#   1) kicked  ：B 在线时 owner 调 admin API 踢它 → 客户端 `session terminated reason=kicked`、
#                无重连尝试、登录页提示「你已被管理员移出这台服务器。」（截图+UIA 文案）。
#   2) 踢后可登录：被踢账号立即用正确密码登录成功（API + GUI 两条路径）。
#   3) banned  ：C 在线时被封 → `reason=banned` + 封禁提示、无重连；随后登录被 403 MEMBER_BANNED，
#                并记录客户端 LoginError 的真实文案（是否可读，如实记录）。
#   4) 解封恢复：owner unban → 该账号 GUI 重新登录成功。
#
# 隔离：服务端 %LOCALAPPDATA%\PartyUpQA7 数据；端口 18088；name "PartyUp kickban"。
# 全程副屏（--screen 2）、控制台隐藏、截图按窗口矩形（capture-window.ps1）。
# 同一时刻只跑一个 GUI 客户端：起客户端前若发现别的 PartyUp.Client 进程，等 5–10 秒重试，
# 绝不杀别人的进程（只杀本脚本自己启动的 pid）。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

function Log([string]$m) { Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

$root = Join-Path $env:LOCALAPPDATA 'PartyUpQA7'
$dataDir = Join-Path $root 'data'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $env:TEMP ("partyup-kickban-" + $stamp)
$prefsBackup = Join-Path $env:TEMP ("partyup-kb-prefs-" + [guid]::NewGuid().ToString('N'))
$appDataDir = Join-Path $env:APPDATA 'PartyUp'
$settingsPath = Join-Path $appDataDir 'settings.json'
$serversPath = Join-Path $appDataDir 'servers.json'
$tokensPath = Join-Path $appDataDir 'tokens.json'
$base = "http://127.0.0.1:$Port"
$results = [System.Collections.Generic.List[object]]::new()
$server = $null
$client = $null

New-Item -ItemType Directory -Force -Path $outDir, $prefsBackup | Out-Null

# ----------------------------------------------------------------------
# 记录 / API / 等待 / UIA 小工具
# ----------------------------------------------------------------------
function Record {
  param([string]$Scene, [string]$Expect, [string]$Actual, [bool]$Pass, [string]$Note = '')
  $results.Add([pscustomobject]@{
      scene = $Scene; expect = $Expect; actual = $Actual; pass = $Pass; note = $Note
    })
  $flag = if ($Pass) { 'PASS' } else { 'FAIL' }
  Log ("RESULT=$flag scene=" + $Scene)
  Log ("  expect: " + $Expect)
  Log ("  actual: " + $Actual)
  if ($Note) { Log ("  note: " + $Note) }
}

function Write-Results {
  try {
    $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outDir 'results.json') -Encoding utf8
    $lines = foreach ($r in $results) {
      "SCENE=$($r.scene)`tPASS=$($r.pass)`tEXPECT=$($r.expect)`tACTUAL=$($r.actual)`tNOTE=$($r.note)"
    }
    $lines | Set-Content -LiteralPath (Join-Path $outDir 'results.txt') -Encoding utf8
  } catch {
    Log ("WRITE_RESULTS_FAILED: " + $_.Exception.Message)
  }
}

function Invoke-Api {
  param([string]$Method, [string]$Path, [string]$Token = '', $Body = $null)
  $headers = @{}
  if ($Token) { $headers['Authorization'] = "Bearer $Token" }
  $uri = $base + $Path
  try {
    if ($null -ne $Body) {
      $json = $Body | ConvertTo-Json -Compress
      $resp = Invoke-RestMethod -Method $Method -Uri $uri -Headers $headers `
        -ContentType 'application/json' -Body $json -TimeoutSec 20
    } else {
      $resp = Invoke-RestMethod -Method $Method -Uri $uri -Headers $headers -TimeoutSec 20
    }
    $raw = ''
    try { $raw = $resp | ConvertTo-Json -Depth 12 -Compress } catch { $raw = [string]$resp }
    return @{ ok = $true; status = 200; body = $resp; raw = $raw }
  } catch {
    $status = 0
    $raw = ''
    if ($_.Exception.Response) {
      try { $status = [int]$_.Exception.Response.StatusCode } catch { }
      try { $raw = $_.Exception.Response.Content.ReadAsStringAsync().GetAwaiter().GetResult() } catch { }
    }
    if (-not $raw -and $_.ErrorDetails) { $raw = [string]$_.ErrorDetails.Message }
    return @{ ok = $false; status = $status; body = $null; raw = $raw; error = $_.Exception.Message }
  }
}

function Select-LogHits {
  param([string]$Path, [string]$Pattern)
  if (-not (Test-Path -LiteralPath $Path)) { return @() }
  return @(Select-String -Path $Path -Pattern $Pattern -Encoding utf8 -ErrorAction SilentlyContinue)
}

function Get-LogText {
  param([string]$Path, [string]$Pattern)
  $hits = Select-LogHits -Path $Path -Pattern $Pattern
  return (($hits | ForEach-Object { $_.Line.Trim() }) -join ' | ')
}

function Get-LogCount {
  param([string]$Path, [string]$Pattern)
  return (Select-LogHits -Path $Path -Pattern $Pattern).Count
}

function Wait-Log {
  param([string]$Path, [string]$Pattern, [int]$TimeoutSec = 30)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $hits = Select-LogHits -Path $Path -Pattern $Pattern
    if ($hits.Count -gt 0) { return $hits }
    Start-Sleep -Milliseconds 300
  }
  return @()
}

function Wait-NoForeignClients {
  param([int]$TimeoutSec = 180)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  $announced = $false
  while ((Get-Date) -lt $deadline) {
    $procs = @(Get-Process PartyUp.Client -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { return $true }
    if (-not $announced) {
      Log ("WAIT_FOREIGN_CLIENT " + (($procs | ForEach-Object { "pid=" + $_.Id + " path=" + $_.Path }) -join '; '))
      $announced = $true
    }
    Start-Sleep -Seconds 6
  }
  return $false
}

function Get-UiaWindow {
  param([int]$ProcId)
  $p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  return [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
}

function Find-UiaById {
  param($Window, [string]$Id)
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
  return $Window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Wait-UiaById {
  param([int]$ProcId, [string]$Id, [int]$TimeoutSec = 20)
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $win = Get-UiaWindow -ProcId $ProcId
    if ($win) {
      $el = Find-UiaById -Window $win -Id $Id
      if ($el) { return $el }
    }
    Start-Sleep -Milliseconds 300
  }
  return $null
}

function Set-UiaText {
  param([int]$ProcId, [string]$Id, [string]$Value)
  $el = Wait-UiaById -ProcId $ProcId -Id $Id -TimeoutSec 15
  if (-not $el) { throw "UIA 元素未找到: $Id" }
  $vp = $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
  $vp.SetValue($Value)
}

function Invoke-UiaButton {
  param([int]$ProcId, [string]$Id)
  $el = Wait-UiaById -ProcId $ProcId -Id $Id -TimeoutSec 15
  if (-not $el) { throw "UIA 按钮未找到: $Id" }
  $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Get-UiaTextById {
  param([int]$ProcId, [string]$Id)
  $win = Get-UiaWindow -ProcId $ProcId
  if (-not $win) { return $null }
  $el = Find-UiaById -Window $win -Id $Id
  if (-not $el) { return $null }
  return $el.Current.Name
}

function Save-UiaDump {
  param([int]$ProcId, [string]$Name)
  $target = Join-Path $outDir $Name
  try {
    $win = Get-UiaWindow -ProcId $ProcId
    if (-not $win) { 'NO_WINDOW' | Set-Content -LiteralPath $target -Encoding utf8; return }
    $lines = @()
    $textCond = New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
      [System.Windows.Automation.ControlType]::Text)
    $texts = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $textCond)
    foreach ($t in $texts) {
      $n = $t.Current.Name
      if ($n) { $lines += ("TEXT: " + $n) }
    }
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
      [System.Windows.Automation.Condition]::TrueCondition)
    $limit = [Math]::Min($all.Count, 500)
    for ($i = 0; $i -lt $limit; $i++) {
      $e = $all.Item($i)
      try {
        $id = $e.Current.AutomationId
        if ($id) {
          $lines += ("ID: " + $id + " type=" + $e.Current.ControlType.ProgrammaticName + " name=" + $e.Current.Name)
        }
      } catch { }
    }
    $lines | Set-Content -LiteralPath $target -Encoding utf8
    Log ("UIA_DUMP=" + $target + " lines=" + $lines.Count)
  } catch {
    Log ("UIA_DUMP_FAILED(" + $Name + "): " + $_.Exception.Message)
  }
}

function Save-Shot {
  param([int]$ProcId, [string]$Name)
  $shot = Join-Path $outDir $Name
  & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $ProcId -OutPath $shot | ForEach-Object { Log ("SHOT " + $_) }
  Log ("SHOT_FILE=" + $shot + " exists=" + (Test-Path -LiteralPath $shot))
  return $shot
}

# 记住我 token 的落盘文件里是否还有该账号的条目（只判断 key 是否存在，不解密）。
function Test-VaultEntry {
  param([string]$UserName)
  if (-not (Test-Path -LiteralPath $tokensPath)) { return $false }
  try {
    $data = Get-Content -LiteralPath $tokensPath -Raw -Encoding utf8 | ConvertFrom-Json
    $keys = @($data.Entries.PSObject.Properties.Name)
    return [bool](@($keys | Where-Object { $_ -like "*|$UserName" }).Count -gt 0)
  } catch {
    return $false
  }
}

function Start-Server {
  # 只清理本脚本占用的端口（18088）对应的 partyup-qa，绝不动 18081/18082/18084/18086/18087 上别人的实例。
  Get-CimInstance Win32_Process -Filter "Name='partyup-qa.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and ($_.CommandLine -like "*127.0.0.1:$Port*") } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 600
  if ((Split-Path -Leaf $root) -eq 'PartyUpQA7' -and (Test-Path -LiteralPath $root)) {
    $retry = 0
    while ((Test-Path -LiteralPath $root) -and $retry -lt 10) {
      Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
      if (Test-Path -LiteralPath $root) { Start-Sleep -Milliseconds 300; $retry++ }
    }
  }
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $srvOut = Join-Path $outDir 'server.out.log'
  $srvErr = Join-Path $outDir 'server.err.log'
  $proc = Start-Process -FilePath $ServerExe -ArgumentList @(
    'serve', '--data', $dataDir, '--http', "127.0.0.1:$Port", '--name', '"PartyUp kickban"'
  ) -WindowStyle Hidden -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru
  $deadline = (Get-Date).AddSeconds(30)
  $bootstrap = $null
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 400
    $hits = Select-LogHits -Path $srvOut -Pattern 'BOOTSTRAP_TOKEN=(\S+)'
    if ($hits.Count -gt 0) { $bootstrap = $hits[0].Matches[0].Groups[1].Value; break }
  }
  if (-not $bootstrap) { throw "server bootstrap token not found in $srvOut" }
  Log ("SERVER_READY pid=" + $proc.Id + " bootstrap_len=" + $bootstrap.Length)
  return @{ proc = $proc; out = $srvOut; err = $srvErr; bootstrap = $bootstrap }
}

function Start-Client {
  param([string]$Token, [string]$Tag)
  if (-not (Wait-NoForeignClients -TimeoutSec 180)) {
    throw '检测到别的 PartyUp.Client 进程持续占用单实例（等满 180s），未杀他人进程，中止'
  }
  $out = Join-Path $outDir ("client-" + $Tag + ".out.log")
  $err = Join-Path $outDir ("client-" + $Tag + ".err.log")
  $proc = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) `
    -ArgumentList @('--server', $base, '--token', $Token, '--screen', '2') `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
  $deadline = (Get-Date).AddSeconds(45)
  while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.HasExited) {
      $tail = ''
      if (Test-Path -LiteralPath $err) { $tail = ((Get-Content -LiteralPath $err -Tail 15 -Encoding utf8) -join ' | ') }
      throw ("client exited early code=" + $proc.ExitCode + " err=" + $tail)
    }
    if ($proc.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 400
  }
  $proc.Refresh()
  if ($proc.MainWindowHandle -eq 0) { throw 'client window not found within 45s' }
  Log ("CLIENT_STARTED tag=" + $Tag + " pid=" + $proc.Id + " hwnd=" + $proc.MainWindowHandle)
  return @{ proc = $proc; out = $out; err = $err }
}

function Stop-Client {
  param($Client)
  if ($Client -and $Client.proc) {
    Stop-Process -Id $Client.proc.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $Client.proc.Id -Timeout 10 -ErrorAction SilentlyContinue
  }
  Start-Sleep -Milliseconds 1200
}

# ----------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------
try {
  # 0) 本机偏好隔离：备份后清空（跑完还原），避免上一台服务器/记住我干扰。
  foreach ($f in @($settingsPath, $serversPath, $tokensPath)) {
    if (Test-Path -LiteralPath $f) {
      Copy-Item -LiteralPath $f -Destination (Join-Path $prefsBackup (Split-Path $f -Leaf)) -Force
      Remove-Item -Force -LiteralPath $f
    }
  }
  Log "PREFS_ISOLATED=True"

  # 1) 隔离服务端 + owner
  $server = Start-Server
  $boot = Invoke-Api -Method Post -Path '/api/bootstrap' -Body @{
    token = $server.bootstrap; username = 'kbowner'; password = 'kb-owner-pass-123'
  }
  if ($boot.status -ne 200) { throw ("bootstrap failed status=" + $boot.status + " raw=" + $boot.raw) }
  $ownerToken = $boot.body.token
  Log ("BOOTSTRAP_OK owner_token_len=" + $ownerToken.Length)

  # 2) 邀请码建 B / C
  $invB = Invoke-Api -Method Post -Path '/api/admin/invites' -Token $ownerToken
  $invC = Invoke-Api -Method Post -Path '/api/admin/invites' -Token $ownerToken
  $codeB = $invB.body.invite.code
  $codeC = $invC.body.invite.code
  $regB = Invoke-Api -Method Post -Path ("/api/invite/" + $codeB) -Body @{
    username = 'kbob'; password = 'kbob-pass-123'
  }
  $regC = Invoke-Api -Method Post -Path ("/api/invite/" + $codeC) -Body @{
    username = 'kcarol'; password = 'kcarol-pass-123'
  }
  if ($regB.status -lt 200 -or $regB.status -ge 300) { throw ("register B failed status=" + $regB.status + " raw=" + $regB.raw) }
  if ($regC.status -lt 200 -or $regC.status -ge 300) { throw ("register C failed status=" + $regC.status + " raw=" + $regC.raw) }
  $bToken = $regB.body.token
  $cToken = $regC.body.token
  $members = Invoke-Api -Method Get -Path '/api/admin/members' -Token $ownerToken
  $bId = ($members.body.members | Where-Object { $_.name -eq 'kbob' }).id
  $cId = ($members.body.members | Where-Object { $_.name -eq 'kcarol' }).id
  if (-not $bId -or -not $cId) { throw ("member ids not found: b=" + $bId + " c=" + $cId) }
  Log ("MEMBERS_READY b=" + $bId + " c=" + $cId + " b_token_len=" + $bToken.Length + " c_token_len=" + $cToken.Length)

  # ==================================================================
  # 场景 1+2：kicked（以及在踢出后用正确密码重新登录）
  # ==================================================================
  try {
    $client = Start-Client -Token $bToken -Tag 'b'
    $hits = Wait-Log -Path $client.out -Pattern 'gui: connected as kbob' -TimeoutSec 45
    Log ("B_CONNECTED=" + ($hits.Count -gt 0) + " line=" + (Get-LogText -Path $client.out -Pattern 'gui: connected as kbob'))
    if ($hits.Count -eq 0) { throw 'B 客户端未在 45s 内完成登录/连接' }
    Save-Shot -ProcId $client.proc.Id -Name '01-b-online.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '01-b-online.uia.txt'

    # owner 踢 B
    $kick1 = Invoke-Api -Method Post -Path ("/api/admin/members/" + $bId + "/kick") -Token $ownerToken
    Log ("KICK1 status=" + $kick1.status + " raw=" + $kick1.raw)
    $hits = Wait-Log -Path $client.out -Pattern 'ws: session terminated by server reason=kicked' -TimeoutSec 20
    $termLine = Get-LogText -Path $client.out -Pattern 'ws: session terminated by server reason=kicked'
    $guiLine = ''
    if ($hits.Count -gt 0) {
      Start-Sleep -Seconds 3
      $guiLine = Get-LogText -Path $client.out -Pattern 'gui: session terminated reason=kicked'
    }
    $reconnect = Get-LogText -Path $client.out -Pattern 'ws: reconnect attempt|ws: reconnected|ws: connection lost|重连中'
    $notice = $null
    if (Wait-UiaById -ProcId $client.proc.Id -Id 'SessionNotice' -TimeoutSec 20) {
      Start-Sleep -Seconds 2
      $notice = Get-UiaTextById -ProcId $client.proc.Id -Id 'SessionNotice'
    }
    Save-Shot -ProcId $client.proc.Id -Name '02-b-kicked.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '02-b-kicked.uia.txt'
    $oldB = Invoke-Api -Method Get -Path '/api/me' -Token $bToken
    $srvTerm = Get-LogText -Path $server.err -Pattern 'session ended by server.*reason=kicked.*close_code=4002'

    Record -Scene '1-kicked 客户端终止原因' `
      -Expect '客户端日志出现 ws: session terminated by server reason=kicked' `
      -Actual ("client: " + $termLine + " ; gui: " + $guiLine + " ; server: " + $srvTerm) `
      -Pass ([bool]($hits.Count -gt 0 -and $guiLine -and $srvTerm))

    Record -Scene '1-kicked 不自动重连' `
      -Expect '踢出后没有任何 ws: reconnect attempt / connection lost / 重连中' `
      -Actual $(if ($reconnect) { $reconnect } else { '(none)' }) `
      -Pass ([bool](-not $reconnect))

    Record -Scene '1-kicked 界面提示' `
      -Expect '登录页 SessionNotice = 你已被管理员移出这台服务器。' `
      -Actual ("SessionNotice=" + $notice) `
      -Pass ([bool]($notice -eq '你已被管理员移出这台服务器。'))

    Record -Scene '1-kicked 旧 token 已撤销' `
      -Expect 'GET /api/me with 旧 token → 401' `
      -Actual ("status=" + $oldB.status + " raw=" + $oldB.raw) `
      -Pass ([bool]($oldB.status -eq 401))

    # 2) 被踢账号立即用正确密码登录：先 API（服务端行为），再 GUI（客户端路径）。
    $reloginApi = Invoke-Api -Method Post -Path '/api/login' -Body @{
      username = 'kbob'; password = 'kbob-pass-123'
    }
    Record -Scene '2-kicked 后重新登录（API）' `
      -Expect 'POST /api/login 正确密码 → 200 且拿到新 token' `
      -Actual ("status=" + $reloginApi.status + " token_len=" + $(if ($reloginApi.body) { $reloginApi.body.token.Length } else { 0 })) `
      -Pass ([bool]($reloginApi.status -eq 200 -and $reloginApi.body.token))

    $beforeCount = Get-LogCount -Path $client.out -Pattern 'gui: connected as kbob'
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginUser' -Value 'kbob'
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginPassword' -Value 'kbob-pass-123'
    Save-Shot -ProcId $client.proc.Id -Name '03-b-login-form.png'
    Invoke-UiaButton -ProcId $client.proc.Id -Id 'Login'
    $deadline = (Get-Date).AddSeconds(35)
    $reloginOk = $false
    while ((Get-Date) -lt $deadline) {
      if ((Get-LogCount -Path $client.out -Pattern 'gui: connected as kbob') -gt $beforeCount) { $reloginOk = $true; break }
      Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2
    $reloginLine = Get-LogText -Path $client.out -Pattern 'gui: connected as kbob'
    $loginFailLine = Get-LogText -Path $client.out -Pattern 'gui: login rejected|gui: login failed|attach session failed'
    Record -Scene '2-kicked 后重新登录（GUI）' `
      -Expect '登录页填正确密码点登录 → 客户端再次 gui: connected as kbob' `
      -Actual ("relogin=" + $reloginOk + " log=" + $reloginLine + " ; failures=" + $loginFailLine) `
      -Pass ([bool]$reloginOk)
    Save-Shot -ProcId $client.proc.Id -Name '04-b-relogin-ok.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '04-b-relogin-ok.uia.txt'

    # 补充：记住我 token 是否写入、以及再次被踢后是否清除。
    $vaultAfterLogin = Test-VaultEntry -UserName 'kbob'
    $kick2 = Invoke-Api -Method Post -Path ("/api/admin/members/" + $bId + "/kick") -Token $ownerToken
    $h2 = Wait-Log -Path $client.out -Pattern 'ws: session terminated by server reason=kicked' -TimeoutSec 20
    Start-Sleep -Seconds 4
    $vaultAfterKick = Test-VaultEntry -UserName 'kbob'
    Record -Scene '1b-kicked 记住我 token 清除（补充）' `
      -Expect '登录成功后 tokens.json 有 kbob 条目；再次被踢后条目被清除' `
      -Actual ("after_login=" + $vaultAfterLogin + " after_kick=" + $vaultAfterKick + " kick2_status=" + $kick2.status + " term_lines=" + (Get-LogCount -Path $client.out -Pattern 'ws: session terminated by server reason=kicked')) `
      -Pass ([bool]($vaultAfterLogin -and (-not $vaultAfterKick)))
  } catch {
    Log ("SCENARIO_B_ERROR: " + $_.Exception.Message)
    Record -Scene '1+2-kicked 场景执行' -Expect '场景完整跑完' -Actual ("异常: " + $_.Exception.Message) -Pass $false
  } finally {
    Stop-Client -Client $client
    $client = $null
  }

  # ==================================================================
  # 场景 3+4：banned（在线被封）+ 解封后恢复
  # ==================================================================
  try {
    $client = Start-Client -Token $cToken -Tag 'c'
    $hits = Wait-Log -Path $client.out -Pattern 'gui: connected as kcarol' -TimeoutSec 45
    Log ("C_CONNECTED=" + ($hits.Count -gt 0) + " line=" + (Get-LogText -Path $client.out -Pattern 'gui: connected as kcarol'))
    if ($hits.Count -eq 0) { throw 'C 客户端未在 45s 内完成登录/连接' }
    Save-Shot -ProcId $client.proc.Id -Name '05-c-online.png'

    # owner 封禁 C
    $ban = Invoke-Api -Method Post -Path ("/api/admin/members/" + $cId + "/ban") -Token $ownerToken -Body @{
      reason = '真机验收：封禁路径'
    }
    Log ("BAN status=" + $ban.status + " raw=" + $ban.raw)
    $hits = Wait-Log -Path $client.out -Pattern 'ws: session terminated by server reason=banned' -TimeoutSec 20
    $termLine = Get-LogText -Path $client.out -Pattern 'ws: session terminated by server reason=banned'
    $guiLine = ''
    if ($hits.Count -gt 0) {
      Start-Sleep -Seconds 3
      $guiLine = Get-LogText -Path $client.out -Pattern 'gui: session terminated reason=banned'
    }
    $reconnect = Get-LogText -Path $client.out -Pattern 'ws: reconnect attempt|ws: reconnected|ws: connection lost|重连中'
    $notice = $null
    if (Wait-UiaById -ProcId $client.proc.Id -Id 'SessionNotice' -TimeoutSec 20) {
      Start-Sleep -Seconds 2
      $notice = Get-UiaTextById -ProcId $client.proc.Id -Id 'SessionNotice'
    }
    Save-Shot -ProcId $client.proc.Id -Name '06-c-banned.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '06-c-banned.uia.txt'
    $srvTerm = Get-LogText -Path $server.err -Pattern 'session ended by server.*reason=banned.*close_code=4003'

    Record -Scene '3-banned 客户端终止原因' `
      -Expect '客户端日志出现 ws: session terminated by server reason=banned' `
      -Actual ("client: " + $termLine + " ; gui: " + $guiLine + " ; server: " + $srvTerm) `
      -Pass ([bool]($hits.Count -gt 0 -and $guiLine -and $srvTerm))

    Record -Scene '3-banned 不自动重连' `
      -Expect '封禁后没有 ws: reconnect attempt / connection lost / 重连中' `
      -Actual $(if ($reconnect) { $reconnect } else { '(none)' }) `
      -Pass ([bool](-not $reconnect))

    Record -Scene '3-banned 界面提示' `
      -Expect '登录页 SessionNotice = 你已被管理员封禁，无法再登录这台服务器。' `
      -Actual ("SessionNotice=" + $notice) `
      -Pass ([bool]($notice -eq '你已被管理员封禁，无法再登录这台服务器。'))

    # 登录被服务端拒绝（403 MEMBER_BANNED）
    $bannedLoginApi = Invoke-Api -Method Post -Path '/api/login' -Body @{
      username = 'kcarol'; password = 'kcarol-pass-123'
    }
    Record -Scene '3-banned 登录被服务端拒绝（API）' `
      -Expect 'POST /api/login → 403 且 code=MEMBER_BANNED' `
      -Actual ("status=" + $bannedLoginApi.status + " raw=" + $bannedLoginApi.raw) `
      -Pass ([bool]($bannedLoginApi.status -eq 403 -and $bannedLoginApi.raw -match 'MEMBER_BANNED'))

    # 客户端 LoginError 文案（是否可读）
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginUser' -Value 'kcarol'
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginPassword' -Value 'kcarol-pass-123'
    Invoke-UiaButton -ProcId $client.proc.Id -Id 'Login'
    Start-Sleep -Seconds 6
    $loginError = Get-UiaTextById -ProcId $client.proc.Id -Id 'LoginError'
    $rejectLine = Get-LogText -Path $client.out -Pattern 'gui: login rejected'
    Save-Shot -ProcId $client.proc.Id -Name '07-c-login-banned-error.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '07-c-login-banned-error.uia.txt'
    $readable = [bool]($loginError -and ($loginError -notmatch 'MEMBER_BANNED') -and ($loginError -notmatch '登录失败：') -and ($loginError -match '封禁'))
    Record -Scene '3-banned 客户端 LoginError 文案可读性' `
      -Expect '可读中文封禁原因（不含 MEMBER_BANNED / 登录失败：原始错误）' `
      -Actual ("LoginError=" + $loginError + " ; log=" + $rejectLine) `
      -Pass $readable `
      -Note $(if ($readable) { '' } else { '客户端文案问题（DescribeAuthError 未映射 MEMBER_BANNED，落进默认「登录失败：{code}: {message}」），未改代码，如实上报' })

    # 4) 解封 → 恢复
    $unban = Invoke-Api -Method Post -Path ("/api/admin/members/" + $cId + "/unban") -Token $ownerToken
    Log ("UNBAN status=" + $unban.status + " raw=" + $unban.raw)
    $beforeCount = Get-LogCount -Path $client.out -Pattern 'gui: connected as kcarol'
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginUser' -Value 'kcarol'
    Set-UiaText -ProcId $client.proc.Id -Id 'LoginPassword' -Value 'kcarol-pass-123'
    Invoke-UiaButton -ProcId $client.proc.Id -Id 'Login'
    $deadline = (Get-Date).AddSeconds(35)
    $unbanOk = $false
    while ((Get-Date) -lt $deadline) {
      if ((Get-LogCount -Path $client.out -Pattern 'gui: connected as kcarol') -gt $beforeCount) { $unbanOk = $true; break }
      Start-Sleep -Milliseconds 400
    }
    Start-Sleep -Seconds 2
    $reloginLine = Get-LogText -Path $client.out -Pattern 'gui: connected as kcarol'
    Record -Scene '4-解封后重新登录（GUI）' `
      -Expect 'unban 后 GUI 用正确密码登录 → gui: connected as kcarol' `
      -Actual ("unban_status=" + $unban.status + " relogin=" + $unbanOk + " log=" + $reloginLine) `
      -Pass ([bool]($unban.status -eq 200 -and $unbanOk))
    Save-Shot -ProcId $client.proc.Id -Name '08-c-unbanned-relogin-ok.png'
    Save-UiaDump -ProcId $client.proc.Id -Name '08-c-unbanned-relogin-ok.uia.txt'
  } catch {
    Log ("SCENARIO_C_ERROR: " + $_.Exception.Message)
    Record -Scene '3+4-banned 场景执行' -Expect '场景完整跑完' -Actual ("异常: " + $_.Exception.Message) -Pass $false
  } finally {
    Stop-Client -Client $client
    $client = $null
  }
}
finally {
  Stop-Client -Client $client
  if ($server -and $server.proc) {
    Stop-Process -Id $server.proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600
  }
  # 只清理本脚本自己构建目录的客户端残留，别动别的构建目录。
  Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -ieq $ClientExe } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }

  # 还原本机偏好（settings.json / servers.json / tokens.json）
  foreach ($leaf in @('settings.json', 'servers.json', 'tokens.json')) {
    $src = Join-Path $prefsBackup $leaf
    $dst = Join-Path $appDataDir $leaf
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $dst -Force }
    elseif (Test-Path -LiteralPath $dst) { Remove-Item -Force -LiteralPath $dst }
  }
  Remove-Item -Recurse -Force $prefsBackup -ErrorAction SilentlyContinue
  Log 'PREFS_RESTORED=True'

  Write-Results

  # 证据复制到 .docs（在删除隔离数据目录之前）
  try {
    New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
    Copy-Item -Path (Join-Path $outDir '*') -Destination $EvidenceDir -Recurse -Force
    $cnt = (Get-ChildItem -LiteralPath $EvidenceDir -File | Measure-Object).Count
    Log ("EVIDENCE_COPIED=" + $EvidenceDir + " files=" + $cnt + " src=" + $outDir)
  } catch {
    Log ("EVIDENCE_COPY_FAILED: " + $_.Exception.Message + " src=" + $outDir)
  }

  # 删隔离实例数据目录（只删 PartyUpQA7）
  if ((Split-Path -Leaf $root) -eq 'PartyUpQA7' -and (Test-Path -LiteralPath $root)) {
    Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue
  }
  Log 'CLEANUP=done'
}
Log 'DONE'
