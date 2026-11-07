# ICE 固定端口 + 对外 IP 映射的真机验收（task-6）。
#
# 判据（对应任务描述 ① ② ③）：
#   ① 启动 stdout 打印 ICE_PORT_RANGE=50000-50010 / ICE_PUBLIC_IP=127.0.0.1；
#   ② 语音房间建 PeerConnection 后，服务端进程确实绑定了 50000-50010 区间内的 UDP socket；
#   ③ 两个「最小 WS 客户端」（自写：auth → channel.subscribe → voice.join）各自收到
#      服务端 media.signal offer，SDP 里 host 候选的 IP 全部是映射后的 127.0.0.1、
#      UDP 端口全部落在 50000-50010。
# 另外验证非法 flag 会在启动前被拒绝（不 silent）。
#
# 说明：本脚本只走控制面 + 信令面，不建立真实 RTP 媒体（那需要原生客户端）；
# 媒体面是否真的能通由 Lead 的 E2E 验收覆盖，这里明确列为「未验证项」。

param(
    [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA3-ice'),
    [string]$Exe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA3-ice\partyup-qa.exe'),
    [int]$Port = 18098,
    [int]$StunPort = 34798,
    # 验收默认 50000-50010（与任务描述一致）。若某台机器上该区间已被其它服务占用，
    # 可用 -IcePortRange 换区间；脚本只断言「候选端口落在传入区间内」。
    [string]$IcePortRange = '50000-50010',
    [string]$IcePublicIP = '127.0.0.1',
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$script:iceMin = [int]($IcePortRange.Split('-')[0])
$script:iceMax = if ($IcePortRange.Contains('-')) { [int]($IcePortRange.Split('-')[1]) } else { $script:iceMin }
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8

$data = Join-Path $Root 'data'
$logs = Join-Path $Root 'logs'
$evidenceDir = Join-Path $Root 'evidence'
$stdout = Join-Path $logs 'server.out'
$stderr = Join-Path $logs 'server.err'
$base = "http://127.0.0.1:$Port"
$wsUrl = "ws://127.0.0.1:$Port/ws"
$script:pass = 0
$script:fail = 0
$script:lines = New-Object System.Collections.Generic.List[string]
$serverProc = $null

function Log([string]$msg) { Write-Host $msg; $script:lines.Add($msg) }

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { $script:pass++; Log "PASS $name" }
    else { $script:fail++; Log "FAIL $name" }
    if ($detail) { Log "     $detail" }
}

function Field([string]$json, [string]$name) {
    $m = [regex]::Match($json, '"' + [regex]::Escape($name) + '":"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value } else { return '' }
}

function Http([string]$method, [string]$path, [string]$token, [string]$body) {
    $headers = @{}
    if ($token) { $headers['Authorization'] = "Bearer $token" }
    $p = @{ Method = $method; Uri = "$base$path"; UseBasicParsing = $true; TimeoutSec = 15 }
    if ($headers.Count -gt 0) { $p.Headers = $headers }
    if ($body) { $p.Body = $body; $p.ContentType = 'application/json' }
    try {
        $r = Invoke-WebRequest @p
        return [pscustomobject]@{ Code = [int]$r.StatusCode; Body = $r.Content }
    } catch {
        $resp = $_.Exception.Response
        if ($resp) {
            $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
            return [pscustomobject]@{ Code = [int]$resp.StatusCode; Body = $sr.ReadToEnd() }
        }
        throw
    }
}

function New-Ulid {
    return ([guid]::NewGuid().ToString('N').Substring(0, 26)).ToUpper()
}

function WsSend($ws, [string]$type, $payload) {
    $envl = @{ v = 1; id = (New-Ulid); type = $type; ts = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); payload = $payload } |
        ConvertTo-Json -Depth 10 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($envl)
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(, $bytes)
    $null = $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true,
        [Threading.CancellationToken]::None).GetAwaiter().GetResult()
}

function WsRecv($ws, [int]$timeoutMs) {
    $buf = New-Object byte[] 131072
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(, $buf)
    $cts = New-Object Threading.CancellationTokenSource
    $cts.CancelAfter($timeoutMs)
    try {
        $res = $ws.ReceiveAsync($seg, $cts.Token).GetAwaiter().GetResult()
    } catch {
        return $null
    } finally {
        $cts.Dispose()
    }
    if ($res.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return $null }
    return [Text.Encoding]::UTF8.GetString($buf, 0, $res.Count)
}

function WsWait($ws, [string]$wantType, [int]$timeoutMs = 6000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $t = WsRecv $ws 500
        if (-not $t) { continue }
        try { $j = $t | ConvertFrom-Json } catch { continue }
        if ($j.type -eq $wantType) { return $j }
    }
    return $null
}

function Stop-Qa {
    if ($serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 400
    Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
}

try {
    Stop-Qa
    # 只清数据目录（exe 与脚本可能就放在 Root 下，不能整目录删）。
    if ($Fresh -and (Test-Path $data)) { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force -Path $data, $logs, $evidenceDir | Out-Null
    if (-not (Test-Path $Exe)) { throw "server exe not found: $Exe" }

    Log "=== PHASE 0: invalid flags rejected before startup ==="
    $badOut = Join-Path $logs 'bad-range.out'
    $badErr = Join-Path $logs 'bad-range.err'
    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -Wait `
        -ArgumentList @('serve', '--data', (Join-Path $Root 'data-bad'), '--http', "127.0.0.1:$Port",
        '--ice-port-range', '1-65535') `
        -RedirectStandardOutput $badOut -RedirectStandardError $badErr
    $badText = ''
    if (Test-Path $badErr) { $badText = Get-Content -Path $badErr -Raw }
    Check '0a oversized port span rejected -> exit != 0' ($p.ExitCode -ne 0) "exit=$($p.ExitCode)"
    Check '0b error mentions --ice-port-range' ($badText -match 'ice-port-range') "stderr=$($badText -replace '\s+',' ')"

    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow -Wait `
        -ArgumentList @('serve', '--data', (Join-Path $Root 'data-bad2'), '--http', "127.0.0.1:$Port",
        '--ice-public-ip', 'not-an-ip') `
        -RedirectStandardOutput $badOut -RedirectStandardError $badErr
    $badText = ''
    if (Test-Path $badErr) { $badText = Get-Content -Path $badErr -Raw }
    Check '0c invalid public IP rejected -> exit != 0' ($p.ExitCode -ne 0) "exit=$($p.ExitCode)"
    Check '0d error mentions --ice-public-ip' ($badText -match 'ice-public-ip') "stderr=$($badText -replace '\s+',' ')"

    Log ''
    Log "=== PHASE 1: start server on :$Port with fixed ICE range ==="
    $serverProc = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -ArgumentList @('serve', '--data', $data, '--http', "127.0.0.1:$Port",
        '--stun-listen', "127.0.0.1:$StunPort", '--name', 'PartyUp-ICE', '--log-level', 'info',
        '--ice-port-range', $IcePortRange, '--ice-public-ip', $IcePublicIP) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $ready = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Path $stdout) {
            $txt = Get-Content -Path $stdout -Raw
            if ($txt -match 'READY') { $ready = $true; break }
        }
        if ($serverProc.HasExited) { break }
    }
    Check '1a server reached READY' $ready "pid=$($serverProc.Id) exited=$($serverProc.HasExited)"
    if (-not $ready) {
        if (Test-Path $stderr) { Log ('  STDERR: ' + (Get-Content $stderr -Raw)) }
        throw 'server did not start'
    }
    $out = Get-Content -Path $stdout -Raw
    $iceRange = ''
    $m = [regex]::Match($out, '(?m)^ICE_PORT_RANGE=(\S+)$')
    if ($m.Success) { $iceRange = $m.Groups[1].Value }
    $iceIP = ''
    $m = [regex]::Match($out, '(?m)^ICE_PUBLIC_IP=(\S+)$')
    if ($m.Success) { $iceIP = $m.Groups[1].Value }
    Check "1b stdout ICE_PORT_RANGE=$IcePortRange" ($iceRange -eq $IcePortRange) "got=$iceRange"
    Check "1c stdout ICE_PUBLIC_IP=$IcePublicIP" ($iceIP -eq $IcePublicIP) "got=$iceIP"

    Log ''
    Log '=== PHASE 2: bootstrap owner + channel + bob ==="'
    $bootToken = ''
    $m = [regex]::Match($out, '(?m)^BOOTSTRAP_TOKEN=(\S+)$')
    if ($m.Success) { $bootToken = $m.Groups[1].Value }
    Check '2a bootstrap token printed' ([bool]$bootToken) ''

    $r = Http POST '/api/bootstrap' '' ('{"token":"' + $bootToken + '","username":"alice","password":"alice-pass-123"}')
    $aliceToken = Field $r.Body 'token'
    Check '2b bootstrap alice -> 201' ($r.Code -eq 201 -and [bool]$aliceToken) "code=$($r.Code)"

    $r = Http POST '/api/admin/channels' $aliceToken '{"name":"voice"}'
    $channelId = Field $r.Body 'id'
    Check '2c create channel -> 201' ($r.Code -eq 201 -and [bool]$channelId) "code=$($r.Code)"

    $r = Http POST '/api/admin/invites' $aliceToken '{}'
    $invite = Field $r.Body 'code'
    Check '2d create invite -> 201' ($r.Code -eq 201 -and [bool]$invite) "code=$($r.Code)"
    $r = Http POST "/api/invite/$invite" '' '{"username":"bob","password":"bob-pass-123"}'
    $bobToken = Field $r.Body 'token'
    Check '2e register bob -> 201' ($r.Code -eq 201 -and [bool]$bobToken) "code=$($r.Code)"

    Log ''
    Log '=== PHASE 3: two minimal WS clients join voice; inspect offer candidates ==='

    # 注意：服务端在 sfu.Join 里**先**发 media.signal(voice offer)、**后**回
    # voice.join.ok；另外订阅频道时还可能先收到一条 screen 的 offer。
    # 所以不能「收到第一个 media.signal 就用」，必须边收边存、按 media=voice 过滤。
    function Get-Msg($ws, [string]$wantType, [int]$timeoutMs, $sink) {
        $deadline = (Get-Date).AddMilliseconds($timeoutMs)
        while ((Get-Date) -lt $deadline) {
            $t = WsRecv $ws 500
            if (-not $t) { continue }
            try { $j = $t | ConvertFrom-Json } catch { continue }
            if ($j.type -eq 'media.signal') { [void]$sink.Add($j) }
            if ($j.type -eq $wantType) { return $j }
        }
        return $null
    }

    function Find-VoiceOffer($sink) {
        foreach ($m in $sink) {
            if ($m.type -eq 'media.signal' -and $m.payload.media -eq 'voice' -and $m.payload.kind -eq 'offer') { return $m }
        }
        return $null
    }

    function Join-VoiceAndGetSDP([string]$token, [string]$label) {
        $ws = New-Object System.Net.WebSockets.ClientWebSocket
        $ws.ConnectAsync([Uri]$wsUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
        $sink = New-Object System.Collections.ArrayList
        WsSend $ws 'auth' @{ token = $token }
        $auth = Get-Msg $ws 'auth.ok' 8000 $sink
        if (-not $auth) { throw "${label}: no auth.ok" }
        WsSend $ws 'channel.subscribe' @{ channel_id = $channelId }
        $sub = Get-Msg $ws 'channel.subscribe.ok' 8000 $sink
        if (-not $sub) { throw "${label}: no channel.subscribe.ok" }
        WsSend $ws 'voice.join' @{ channel_id = $channelId }
        $join = Get-Msg $ws 'voice.join.ok' 10000 $sink
        if (-not $join) { throw "${label}: no voice.join.ok" }
        $sig = Find-VoiceOffer $sink
        $deadline = (Get-Date).AddSeconds(15)
        while (-not $sig -and (Get-Date) -lt $deadline) {
            $t = WsRecv $ws 500
            if (-not $t) { continue }
            try { $j = $t | ConvertFrom-Json } catch { continue }
            if ($j.type -eq 'media.signal') { [void]$sink.Add($j); $sig = Find-VoiceOffer $sink }
        }
        if (-not $sig) {
            $types = ($sink | ForEach-Object { "$($_.type):$($_.payload.media)/$($_.payload.kind)" }) -join ','
            throw "${label}: no voice media.signal offer (got: $types)"
        }
        $payload = $sig.payload.payload | ConvertFrom-Json
        return [pscustomobject]@{ Ws = $ws; Kind = $sig.payload.kind; Media = $sig.payload.media; Sdp = $payload.sdp }
    }

    $alice = Join-VoiceAndGetSDP $aliceToken 'alice'
    Check '3a alice voice.join received media.signal offer' ($alice.Kind -eq 'offer' -and $alice.Media -eq 'voice' -and [bool]$alice.Sdp) "kind=$($alice.Kind) media=$($alice.Media)"

    $bob = Join-VoiceAndGetSDP $bobToken 'bob'
    Check '3b bob voice.join received media.signal offer' ($bob.Kind -eq 'offer' -and $bob.Media -eq 'voice' -and [bool]$bob.Sdp) "kind=$($bob.Kind) media=$($bob.Media)"

    # 服务端绑定：进程确实占用配置区间的 UDP socket。
    Start-Sleep -Milliseconds 500
    $udp = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object {
            $_.LocalPort -ge $script:iceMin -and $_.LocalPort -le $script:iceMax -and $_.OwningProcess -eq $serverProc.Id
        })
    $udpDesc = ($udp | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' '
    Check "3c server process owns UDP socket(s) in $IcePortRange" ($udp.Count -ge 1) "count=$($udp.Count) ports=$udpDesc"

    function Check-SDP([string]$label, [string]$sdp) {
        $cands = [regex]::Matches($sdp, '(?m)^a=candidate:\S+ \d+ udp \d+ (\S+) (\d+) typ host')
        $ips = @($cands | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
        $ports = @($cands | ForEach-Object { [int]$_.Groups[2].Value } | Sort-Object -Unique)
        $badPorts = @($ports | Where-Object { $_ -lt $script:iceMin -or $_ -gt $script:iceMax })
        $badIPs = @($ips | Where-Object { $_ -ne $IcePublicIP })
        Check "$label offer has UDP host candidates" ($cands.Count -ge 1) "count=$($cands.Count)"
        Check "$label all candidate IPs are rewritten to $IcePublicIP" ($ips.Count -ge 1 -and $badIPs.Count -eq 0) "ips=$($ips -join ',')"
        Check "$label all candidate ports within $IcePortRange" ($ports.Count -ge 1 -and $badPorts.Count -eq 0) "ports=$($ports -join ',')"
        if ($cands.Count -gt 0) {
            $raw = @($cands | Select-Object -First 3 | ForEach-Object { $_.Value })
            Log "  RAW_OFFER[$label]: $($raw -join ' | ')"
        }
    }
    Check-SDP 'alice' $alice.Sdp
    Check-SDP 'bob' $bob.Sdp

    # 服务端日志：两次 offer sent。
    $errText = ''
    if (Test-Path $stderr) { $errText = Get-Content -Path $stderr -Raw }
    $offerLogs = [regex]::Matches($errText, 'msg="offer sent"')
    Check '3d server log has 2 offer-sent lines' ($offerLogs.Count -ge 2) "count=$($offerLogs.Count)"

    # 收尾 WebSocket（发 voice.leave 再关闭，保持服务端状态干净）。
    foreach ($c in @($alice, $bob)) {
        try { WsSend $c.Ws 'voice.leave' @{ channel_id = $channelId } } catch { }
        try { $c.Ws.Dispose() } catch { }
    }

    Log ''
    Log '=== SUMMARY ==='
    Log "PASS=$($script:pass) FAIL=$($script:fail)"
    $result = if ($script:fail -eq 0) { 'PASS' } else { 'FAIL' }
    Log "RESULT=$result"
}
catch {
    Log "FATAL: $_"
    Log "PASS=$($script:pass) FAIL=$($script:fail)"
    Log 'RESULT=FAIL'
}
finally {
    Stop-Qa
    New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $evidence = Join-Path $evidenceDir "ice-accept-$stamp.txt"
    $script:lines | Set-Content -Path $evidence -Encoding UTF8
    Log "EVIDENCE=$evidence"
    Log "QA3_ICE_EVIDENCE=$evidence"
    Log "QA3_ICE_PASS=$($script:pass)"
    Log "QA3_ICE_FAIL=$($script:fail)"
}
