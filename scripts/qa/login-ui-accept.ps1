<#
.SYNOPSIS
  登录第一屏（服务器列表 → 添加服务器 → 注册/引导页）UIA 回归。

.DESCRIPTION
  覆盖 7 组真实行为（不是截图人工看）：
    1  首次启动：选择服务器 + 空态文案 + AddServerInput 存在且预填 --server 地址
    2  添加地址：进登录页、ActiveServer* 可见、服务器簿新增并在重启后保留
    3  邀请链接：粘链接 → 直接进注册流程（不是登录页）
    4a 裸邀请码（已有已知服务器）→ 进注册流程
    4b 裸邀请码（无已知服务器）→ AddServerError 精确文案
    5  未初始化服务器（18090 空库）→ owner bootstrap 向导
    6  记住我：tokens.json 非空 base64 密文、原文不含明文 token、重启自动登录
    7  退出登录：tokens.json 中该账号条目消失

  环境隔离：服务端 18089（QA8\data，已初始化 + owner + 邀请码）/ 18090（QA8\data2，空库）；
  客户端每次 --screen 2（副屏）；同一时刻只允许一个 GUI（$env:TEMP\partyup-gui-probe.lock 串行）。
  跑前备份、跑后还原 %APPDATA%\PartyUp\{settings.json,servers.json,tokens.json}。

.NOTES
  - 不改产品代码；失败只记录「哪个元素/哪条文案不符」。
  - 证据默认写到 %LOCALAPPDATA%\PartyUpQA8\evidence，由外部复制到 .docs/。
  - 收尾不删除 QA8（证据在里面），需要清理时跑 -CleanupRoot。
#>
param(
  [string]$ClientExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\client\PartyUp.Client.exe'),
  [string]$ServerExe = (Join-Path $env:LOCALAPPDATA 'PartyUpQA\server\partyup-qa.exe'),
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'PartyUpQA8'),
  [string]$Cases = '1,2,3,4a,4b,5,6,7',
  [switch]$Dump,
  [switch]$KeepServers,
  [switch]$CleanupRoot
)

$ErrorActionPreference = 'Stop'

$AppDataDir = Join-Path $env:APPDATA 'PartyUp'
$OutDir = Join-Path $Root 'evidence'
$OwnerUser = 'uiowner'
$OwnerPass = 'ui-owner-pass-123'
$ClientHttp = 'http://127.0.0.1:18089'
$BootstrapHttp = 'http://127.0.0.1:18090'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LoginUiWin
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT point);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);
}
'@

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LoginUiDpapi
{
    [StructLayout(LayoutKind.Sequential)]
    public struct DATA_BLOB { public int cbData; public IntPtr pbData; }

    [DllImport("crypt32.dll", SetLastError = true)]
    private static extern bool CryptUnprotectData(ref DATA_BLOB pDataIn, IntPtr ppszDataDescr,
        IntPtr pOptionalEntropy, IntPtr pvReserved, IntPtr pPromptStruct, int dwFlags, out DATA_BLOB pDataOut);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LocalFree(IntPtr hMem);

    public static byte[] Unprotect(byte[] data)
    {
        var input = new DATA_BLOB();
        input.cbData = data.Length;
        input.pbData = Marshal.AllocHGlobal(data.Length);
        Marshal.Copy(data, 0, input.pbData, data.Length);
        try
        {
            DATA_BLOB output;
            if (!CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0, out output))
            {
                throw new InvalidOperationException("CryptUnprotectData failed, win32=" + Marshal.GetLastWin32Error());
            }
            try
            {
                var result = new byte[output.cbData];
                Marshal.Copy(output.pbData, result, 0, output.cbData);
                return result;
            }
            finally { LocalFree(output.pbData); }
        }
        finally { Marshal.FreeHGlobal(input.pbData); }
    }
}
'@

$script:Results = New-Object System.Collections.Generic.List[object]
$script:Servers = New-Object System.Collections.Generic.List[object]
$script:Client = $null
$script:LockFile = $null

# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------

function Log([string]$message) {
    Write-Host ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $message)
}

function Add-Result([string]$Case, [string]$Assertion, [bool]$Pass, [string]$Evidence) {
    $clean = ($Evidence -replace "\r?\n", ' ').Trim()
    if ($clean.Length -gt 600) { $clean = $clean.Substring(0, 600) + '…' }
    $row = [pscustomobject]@{
        Case      = $Case
        Assertion = $Assertion
        Result    = $(if ($Pass) { 'PASS' } else { 'FAIL' })
        Evidence  = $clean
    }
    $script:Results.Add($row)
    Log ("  " + $row.Result + " [" + $Case + "] " + $Assertion + " :: " + $clean)
}

function Reset-AppData {
    foreach ($leaf in @('settings.json', 'servers.json', 'tokens.json')) {
        $path = Join-Path $AppDataDir $leaf
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
}

function Backup-AppData {
    $dir = Join-Path $env:TEMP ('partyup-loginui-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($leaf in @('settings.json', 'servers.json', 'tokens.json')) {
        $source = Join-Path $AppDataDir $leaf
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $dir $leaf) -Force }
    }
    return $dir
}

function Restore-AppData([string]$BackupDir) {
    if (-not $BackupDir) { return }
    foreach ($leaf in @('settings.json', 'servers.json', 'tokens.json')) {
        $source = Join-Path $BackupDir $leaf
        $target = Join-Path $AppDataDir $leaf
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination $target -Force
        } elseif (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Force
        }
    }
    Remove-Item -Recurse -Force $BackupDir -ErrorAction SilentlyContinue
}

function Acquire-GuiLock([int]$TimeoutSec = 240) {
    $lock = Join-Path $env:TEMP 'partyup-gui-probe.lock'
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not (New-Item -ItemType File -Path $lock -ErrorAction SilentlyContinue)) {
        if ((Get-Date) -ge $deadline) {
            throw ("等 GUI 锁超时（" + $TimeoutSec + "s）：" + $lock + " 仍被占用（另一个 GUI 探针在跑？）")
        }
        Start-Sleep -Seconds 2
    }
    return $lock
}

function Release-GuiLock {
    if ($script:LockFile -and (Test-Path -LiteralPath $script:LockFile)) {
        Remove-Item -LiteralPath $script:LockFile -Force -ErrorAction SilentlyContinue
    }
    $script:LockFile = $null
}

function Wait-Port([int]$Port, [int]$TimeoutSec = 40) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect('127.0.0.1', $Port)
            return $true
        } catch {
            Start-Sleep -Milliseconds 300
        } finally {
            $client.Dispose()
        }
    } while ((Get-Date) -lt $deadline)
    return $false
}

function Test-PortListening([int]$Port) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect('127.0.0.1', $Port)
        $client.Dispose()
        return $true
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
# 服务端
# ---------------------------------------------------------------------------

function Start-QaServer([int]$Port, [string]$DataDir, [string]$DisplayName) {
    if (Test-PortListening $Port) {
        throw ("端口 " + $Port + " 已被占用（不是本脚本起的）；为避免干扰其它验收，直接失败。")
    }
    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $stdout = Join-Path $Root ("server-" + $Port + ".out.log")
    $stderr = Join-Path $Root ("server-" + $Port + ".err.log")
    $proc = Start-Process -FilePath $ServerExe -WindowStyle Hidden -PassThru `
        -ArgumentList @('serve', '--data', $DataDir, '--http', ('127.0.0.1:' + $Port), '--name', $DisplayName,
            '--stun-listen', 'off') `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not (Wait-Port $Port 45)) {
        throw ("服务端 " + $Port + " 未在 45s 内监听；stderr=" + (Get-Content $stderr -Raw -ErrorAction SilentlyContinue))
    }
    $token = $null
    for ($i = 0; $i -lt 60 -and -not $token; $i++) {
        $raw = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
        if ($raw) {
            $match = [regex]::Match($raw, 'BOOTSTRAP_TOKEN=(\S+)')
            if ($match.Success) { $token = $match.Groups[1].Value }
        }
        if (-not $token) { Start-Sleep -Milliseconds 200 }
    }
    $entry = [pscustomobject]@{ Port = $Port; Process = $proc; Stdout = $stdout; Stderr = $stderr; Token = $token }
    $script:Servers.Add($entry)
    Log ("server :" + $Port + " started pid=" + $proc.Id + " bootstrap_token=" + $(if ($token) { 'yes' } else { 'no' }) + " log=" + $stdout)
    return $entry
}

function Stop-QaServers {
    foreach ($server in $script:Servers) {
        if ($server.Process -and -not $server.Process.HasExited) {
            Stop-Process -Id $server.Process.Id -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 400
        }
    }
    $script:Servers.Clear()
}

function Invoke-Api([string]$Method, [string]$Uri, $Body, [string]$Token) {
    $headers = @{}
    if ($Token) { $headers['Authorization'] = 'Bearer ' + $Token }
    $params = @{ Method = $Method; Uri = $Uri; Headers = $headers; TimeoutSec = 20 }
    if ($null -ne $Body) {
        $params['ContentType'] = 'application/json'
        $params['Body'] = ($Body | ConvertTo-Json -Depth 6 -Compress)
    }
    return Invoke-RestMethod @params
}

# ---------------------------------------------------------------------------
# 客户端
# ---------------------------------------------------------------------------

function Start-Client([string]$Tag, [string[]]$ExtraArgs = @()) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $stdout = Join-Path $OutDir ('client-' + $Tag + '.out.log')
    $stderr = Join-Path $OutDir ('client-' + $Tag + '.err.log')
    $argList = @('--server', $ClientHttp, '--screen', '2') + $ExtraArgs
    $proc = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path -Parent $ClientExe) -PassThru `
        -ArgumentList $argList -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        $proc.Refresh()
        if ($proc.HasExited) {
            throw ("客户端提前退出 code=" + $proc.ExitCode + "；stderr=" + (Get-Content $stderr -Raw -ErrorAction SilentlyContinue))
        }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero -and [LoginUiWin]::IsWindowVisible($proc.MainWindowHandle)) {
            break
        }
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
        throw ("客户端 120s 内没有可见主窗口；stdout=" + (Get-Content $stdout -Raw -ErrorAction SilentlyContinue))
    }
    Start-Sleep -Milliseconds 1000
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
    $client = [pscustomobject]@{
        Tag     = $Tag
        Process = $proc
        Handle  = $proc.MainWindowHandle
        Root    = $root
        Stdout  = $stdout
        Stderr  = $stderr
    }
    $script:Client = $client
    Log ("client '" + $Tag + "' pid=" + $proc.Id + " hwnd=" + $proc.MainWindowHandle)
    return $client
}

function Stop-Client {
    if ($script:Client -and $script:Client.Process -and -not $script:Client.Process.HasExited) {
        Stop-Process -Id $script:Client.Process.Id -Force -ErrorAction SilentlyContinue
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline) {
            if (-not (Get-Process -Id $script:Client.Process.Id -ErrorAction SilentlyContinue)) { break }
            Start-Sleep -Milliseconds 200
        }
        Log ("client '" + $script:Client.Tag + "' stopped")
    }
    $script:Client = $null
}

function Get-WindowRect($client) {
    $rect = New-Object LoginUiWin+RECT
    if ([LoginUiWin]::GetWindowRect($client.Handle, [ref]$rect)) { return $rect }
    return $null
}

function Test-WindowOnSecondaryScreen($client) {
    $rect = Get-WindowRect $client
    if (-not $rect) { return $false }
    $screens = [System.Windows.Forms.Screen]::AllScreens
    foreach ($screen in $screens) {
        if ($screen.Primary) { continue }
        $b = $screen.Bounds
        $overlapX = [Math]::Min($rect.Right, $b.Right) - [Math]::Max($rect.Left, $b.X)
        $overlapY = [Math]::Min($rect.Bottom, $b.Bottom) - [Math]::Max($rect.Top, $b.Y)
        if ($overlapX -gt 200 -and $overlapY -gt 120) { return $true }
    }
    return $false
}

function Save-Shot($client, [string]$Name) {
    $path = Join-Path $OutDir $Name
    try {
        $output = & (Join-Path $PSScriptRoot '..\capture-window.ps1') -ProcessId $client.Process.Id -OutPath $path 2>&1
        Log ("  shot " + $Name + " -> " + ($output -join ' '))
        $null = Test-Path -LiteralPath $path
    } catch {
        Log ("  shot " + $Name + " FAILED: " + $_.Exception.Message)
    }
}

# ---------------------------------------------------------------------------
# UIA 辅助
# ---------------------------------------------------------------------------

function Find-ById($root, [string]$Id) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Wait-ById($root, [string]$Id, [int]$TimeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $element = Find-ById $root $Id
        if ($element) { return $element }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $null
}

function Find-ByName($root, [string]$Name) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Wait-ByName($root, [string]$Name, [int]$TimeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $element = Find-ByName $root $Name
        if ($element) { return $element }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $null
}

# Flyout / 菜单是独立 popup 窗口，不在主窗口的 UIA 子树里：从桌面根节点按 AutomationId 找。
function Wait-ByIdGlobal([string]$Id, [int]$TimeoutSec = 20) {
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $element = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants, $condition)
        if ($element) { return $element }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $null
}

function Get-Text($element) {
    if (-not $element) { return $null }
    try {
        $valuePattern = $null
        if ($element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valuePattern)) {
            return $valuePattern.Current.Value
        }
        $textPattern = $null
        if ($element.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$textPattern)) {
            return $textPattern.DocumentRange.GetText(-1)
        }
        return $element.Current.Name
    } catch {
        return $null
    }
}

function Set-Text($element, [string]$Text) {
    if (-not $element) { throw 'Set-Text: 元素不存在' }
    $valuePattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valuePattern)) {
        if (-not $valuePattern.Current.IsReadOnly) {
            $valuePattern.SetValue($Text)
            return 'ValuePattern'
        }
    }
    # Avalonia 的 TextBox 若没暴露 ValuePattern，退到真实键盘输入（窗口在副屏，不抢主屏焦点）。
    $element.SetFocus()
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait('^a')
    Start-Sleep -Milliseconds 120
    $escaped = $Text -replace '([+^%~()\[\]{}])', '{$1}'
    [System.Windows.Forms.SendKeys]::SendWait($escaped)
    Start-Sleep -Milliseconds 150
    return 'SendKeys'
}

function Invoke-Element($element) {
    if (-not $element) { throw 'Invoke-Element: 元素不存在' }
    $invokePattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invokePattern)) {
        $invokePattern.Invoke()
        return 'Invoke'
    }
    $togglePattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)) {
        $togglePattern.Toggle()
        return 'Toggle'
    }
    $selectionPattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$selectionPattern)) {
        $selectionPattern.Select()
        return 'Select'
    }
    throw ("元素没有可用的 Invoke/Toggle 模式：type=" + $element.Current.ControlType.ProgrammaticName + " name=" + $element.Current.Name)
}

function Get-ToggleState($element) {
    if (-not $element) { return $null }
    $togglePattern = $null
    if ($element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)) {
        return $togglePattern.Current.ToggleState.ToString()
    }
    return $null
}

# 真实鼠标点击（UIA Invoke 对带 Flyout 的按钮偶发不展开菜单时用）：点元素矩形中心，然后归还光标。
function Click-Element($element) {
    if (-not $element) { throw 'Click-Element: 元素不存在' }
    $rect = $element.Current.BoundingRectangle
    if ($rect.Width -le 0 -or $rect.Height -le 0) { throw 'Click-Element: 元素矩形为空' }
    $saved = New-Object LoginUiWin+POINT
    [void][LoginUiWin]::GetCursorPos([ref]$saved)
    $x = [int]($rect.X + $rect.Width / 2)
    $y = [int]($rect.Y + $rect.Height / 2)
    [void][LoginUiWin]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 120
    [LoginUiWin]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
    Start-Sleep -Milliseconds 60
    [LoginUiWin]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 150
    [void][LoginUiWin]::SetCursorPos($saved.X, $saved.Y)
}
function Dump-UiaTree($client, [string]$FileName) {
    $path = Join-Path $OutDir $FileName
    $lines = New-Object System.Collections.Generic.List[string]
    try {
        $all = $client.Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        $lines.Add("window_rect=" + ($(Get-WindowRect $client).Left) + "," + ($(Get-WindowRect $client).Top))
        foreach ($element in $all) {
            try {
                $controlType = $element.Current.ControlType.ProgrammaticName
                $name = $element.Current.Name
                $automationId = $element.Current.AutomationId
                if ($controlType -ne 'ControlType.Text' -and $controlType -ne 'ControlType.Edit' -and
                    $controlType -ne 'ControlType.Button' -and $controlType -ne 'ControlType.CheckBox' -and
                    $controlType -ne 'ControlType.MenuItem' -and $controlType -ne 'ControlType.List' -and
                    $controlType -ne 'ControlType.ComboBox' -and $controlType -ne 'ControlType.Custom') {
                    continue
                }
                $value = Get-Text $element
                $toggle = Get-ToggleState $element
                $lines.Add("CT=" + $controlType + " ID=[" + $automationId + "] NAME=[" + $name + "] VALUE=[" + $value + "] TOGGLE=[" + $toggle + "]")
            } catch {
                $lines.Add("CT=? <element read failed>")
            }
        }
    } catch {
        $lines.Add("UIA dump failed: " + $_.Exception.Message)
    }
    $lines | Set-Content -LiteralPath $path -Encoding utf8
    Log ("  uia dump -> " + $FileName + " (" + $lines.Count + " lines)")
}

# ---------------------------------------------------------------------------
# 断言辅助
# ---------------------------------------------------------------------------

function Assert-Exists($client, [string]$Case, [string]$Assertion, [string]$AutomationId, [int]$TimeoutSec = 20) {
    $element = Wait-ById $client.Root $AutomationId $TimeoutSec
    Add-Result $Case $Assertion ($null -ne $element) ("AutomationId=" + $AutomationId + " found=" + ($null -ne $element))
    return $element
}

function Assert-Text($client, [string]$Case, [string]$Assertion, [string]$AutomationId, [string]$Expected) {
    $element = Wait-ById $client.Root $AutomationId 15
    $actual = Get-Text $element
    Add-Result $Case $Assertion ($actual -eq $Expected) ("AutomationId=" + $AutomationId + " expected=[" + $Expected + "] actual=[" + $actual + "]")
    return $element
}

function Assert-EmptyStateText($client, [string]$Case, [string]$Assertion, [string]$ExpectedText, [int]$TimeoutSec = 15) {
    $element = Wait-ByName $client.Root $ExpectedText $TimeoutSec
    Add-Result $Case $Assertion ($null -ne $element) ("TextBlock name=[" + $ExpectedText + "] found=" + ($null -ne $element))
    return $element
}

function Assert-LogLine($client, [string]$Case, [string]$Assertion, [string]$Pattern, [int]$TimeoutSec = 25) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $hit = $null
    do {
        if (Test-Path -LiteralPath $client.Stdout) {
            $hit = Select-String -LiteralPath $client.Stdout -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        }
        if ($hit) { break }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    Add-Result $Case $Assertion ($null -ne $hit) $(if ($hit) { $hit.Line.Trim() } else { "日志未匹配 /" + $Pattern + "/" })
    return $hit
}

function Get-ServerBook {
    $path = Join-Path $AppDataDir 'servers.json'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

function Ensure-ServerPicker($client) {
    $input = Find-ById $client.Root 'AddServerInput'
    if ($input) { return $input }
    $back = Wait-ById $client.Root 'BackToServerList' 15
    if ($back) {
        Invoke-Element $back | Out-Null
        Start-Sleep -Milliseconds 900
    }
    return (Wait-ById $client.Root 'AddServerInput' 15)
}

# ---------------------------------------------------------------------------
# 用例
# ---------------------------------------------------------------------------

function Invoke-Case1($client) {
    Log 'CASE 1：首次启动第一屏'
    $heading = Wait-ByName $client.Root '选择服务器' 25
    Add-Result '1' '显示「选择服务器」标题' ($null -ne $heading) ("TextBlock name=选择服务器 found=" + ($null -ne $heading))
    Assert-EmptyStateText $client '1' '显示空态文案「还没有添加过服务器。」' '还没有添加过服务器。'
    $input = Assert-Exists $client '1' 'AddServerInput 输入框存在' 'AddServerInput'
    $value = Get-Text $input
    Add-Result '1' 'AddServerInput 预填 --server 地址' ($value -eq $ClientHttp) ("expected=[" + $ClientHttp + "] actual=[" + $value + "]")
    $onSecondary = Test-WindowOnSecondaryScreen $client
    $rect = Get-WindowRect $client
    Add-Result '1' '窗口落在副屏（--screen 2）' $onSecondary ("rect=" + $rect.Left + "," + $rect.Top + " " + ($rect.Right - $rect.Left) + "x" + ($rect.Bottom - $rect.Top))
    Save-Shot $client 'case1-first-screen.png'
    if ($Dump) { Dump-UiaTree $client 'uia-case1.txt' }
}

function Invoke-Case2($client) {
    Log 'CASE 2：添加地址 → 登录页 → 重启持久化'
    $input = Wait-ById $client.Root 'AddServerInput' 10
    $method = Set-Text $input $ClientHttp
    Add-Result '2' 'AddServerInput 可写入地址' ($null -ne $input) ("input_method=" + $method)
    $button = Wait-ById $client.Root 'AddServer' 10
    Invoke-Element $button | Out-Null
    $loginUser = Wait-ById $client.Root 'LoginUser' 30
    Add-Result '2' '添加地址后进入登录页（LoginUser 出现）' ($null -ne $loginUser) ("LoginUser found=" + ($null -ne $loginUser))
    $nameElement = Wait-ById $client.Root 'ActiveServerName' 10
    $nameText = Get-Text $nameElement
    Add-Result '2' 'ActiveServerName 可见且非空' (-not [string]::IsNullOrWhiteSpace($nameText)) ("name=[" + $nameText + "]")
    $urlElement = Wait-ById $client.Root 'ActiveServerUrl' 10
    $urlText = Get-Text $urlElement
    Add-Result '2' ('ActiveServerUrl = ' + $ClientHttp) ($urlText -eq $ClientHttp) ("expected=[" + $ClientHttp + "] actual=[" + $urlText + "]")
    $book = Get-ServerBook
    $entries = @()
    if ($book) { $entries = @($book.Servers | Where-Object { $_.Url -eq $ClientHttp }) }
    Add-Result '2' '服务器簿新增一条 18089' ($entries.Count -eq 1) ("servers.json matches=" + $entries.Count + " total=" + $(if ($book) { @($book.Servers).Count } else { 0 }))
    Save-Shot $client 'case2-login-page.png'

    Stop-Client
    $client = Start-Client 'case2-restart'
    # 文件层与界面层分开断言：如果文件里有、界面却回第一屏，就是「载入」链路的问题，而不是写入。
    $bookAfterRestart = Get-ServerBook
    $entriesAfterRestart = @()
    if ($bookAfterRestart) { $entriesAfterRestart = @($bookAfterRestart.Servers | Where-Object { $_.Url -eq $ClientHttp }) }
    Add-Result '2' '重启前 servers.json 仍含该服务器（文件层持久化）' ($entriesAfterRestart.Count -eq 1) ("servers.json matches=" + $entriesAfterRestart.Count + " total=" + $(if ($bookAfterRestart) { @($bookAfterRestart.Servers).Count } else { 0 }))
    $loginUser2 = Wait-ById $client.Root 'LoginUser' 25
    $screenNow = if ($loginUser2) { '登录页' } else { '第一屏/其它（LoginUser 不在 UIA 树里）' }
    Add-Result '2' '重启后仍直接进该服务器登录页（服务器簿被载入）' ($null -ne $loginUser2) ("LoginUser found=" + ($null -ne $loginUser2) + "；界面=" + $screenNow)
    Save-Shot $client 'case2-restart-login-page.png'
    if ($Dump -or -not $loginUser2) { Dump-UiaTree $client 'uia-case2-restart.txt' }
    $back = Wait-ById $client.Root 'BackToServerList' 10
    if ($back) { Invoke-Element $back | Out-Null; Start-Sleep -Milliseconds 900 }
    $list = Wait-ById $client.Root 'ServerList' 15
    Add-Result '2' '返回第一屏后 ServerList 可见' ($null -ne $list) ("ServerList found=" + ($null -ne $list))
    $rowContainer = Find-ById $client.Root 'ServerRow'
    $rowName = Get-Text (Find-ById $client.Root 'ServerRowName')
    $rowUrl = Get-Text (Find-ById $client.Root 'ServerRowUrl')
    $useButton = Find-ById $client.Root 'ServerUse'
    $removeButton = Find-ById $client.Root 'ServerRemove'
    # Border 容器本身没有 automation peer（UIA 树里看不到），断言行内可交互控件。
    $rowOk = ($rowName -eq 'UI-Test') -and ($rowUrl -eq $ClientHttp) -and ($null -ne $useButton) -and ($null -ne $removeButton)
    Add-Result '2' '列表行按 ID 显示服务器名/地址/进入+移除按钮' $rowOk ("ServerRowName=[" + $rowName + "] ServerRowUrl=[" + $rowUrl + "] ServerUse=" + ($null -ne $useButton) + " ServerRemove=" + ($null -ne $removeButton) + " row_container_ServerRow=" + ($null -ne $rowContainer))
    Save-Shot $client 'case2-server-list.png'
    if ($Dump) { Dump-UiaTree $client 'uia-case2-server-list.txt' }
}

function Invoke-Case3($client, [string]$InviteCode) {
    Log 'CASE 3：邀请链接 → 注册流程'
    $input = Ensure-ServerPicker $client
    if (-not $input) { throw 'CASE 3：找不到 AddServerInput（无法进入添加服务器区）' }
    Set-Text $input ($ClientHttp + '/?code=' + $InviteCode) | Out-Null
    Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
    $cancel = Wait-ById $client.Root 'CancelRegistration' 30
    Add-Result '3' '邀请链接添加后进入注册流程（CancelRegistration 出现）' ($null -ne $cancel) ("CancelRegistration found=" + ($null -ne $cancel))
    $loginUser = Find-ById $client.Root 'LoginUser'
    Add-Result '3' '不是登录页（LoginUser 不存在）' ($null -eq $loginUser) ("LoginUser found=" + ($null -ne $loginUser))
    $target = Wait-ById $client.Root 'RegistrationTarget' 10
    Add-Result '3' '注册第一步 RegistrationTarget 存在' ($null -ne $target) ("RegistrationTarget found=" + ($null -ne $target))
    $targetValue = Get-Text $target
    $expectedTarget = $ClientHttp + '/?code=' + $InviteCode
    Add-Result '3' '注册第一步预填完整邀请链接' ($targetValue -eq $expectedTarget) ("expected=[" + $expectedTarget + "] actual=[" + $targetValue + "]")
    Assert-LogLine $client '3' '客户端日志记录 kind=InviteLink' 'gui: server-info .*kind=InviteLink'
    Save-Shot $client 'case3-registration.png'
    if ($Dump) { Dump-UiaTree $client 'uia-case3-registration.txt' }
}

function Invoke-Case4a($client, [string]$InviteCode) {
    Log 'CASE 4a：裸邀请码（已有已知服务器）→ 注册流程'
    $cancel = Wait-ById $client.Root 'CancelRegistration' 10
    if ($cancel) { Invoke-Element $cancel | Out-Null }
    Start-Sleep -Milliseconds 900
    $back = Wait-ById $client.Root 'BackToServerList' 15
    if ($back) { Invoke-Element $back | Out-Null }
    Start-Sleep -Milliseconds 900
    $input = Wait-ById $client.Root 'AddServerInput' 15
    Set-Text $input $InviteCode | Out-Null
    Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
    $registration = Wait-ById $client.Root 'CancelRegistration' 30
    Add-Result '4a' '裸邀请码（有已知服务器）进入注册流程' ($null -ne $registration) ("CancelRegistration found=" + ($null -ne $registration))
    $loginUser = Find-ById $client.Root 'LoginUser'
    Add-Result '4a' '不是登录页（LoginUser 不存在）' ($null -eq $loginUser) ("LoginUser found=" + ($null -ne $loginUser))
    Assert-LogLine $client '4a' '客户端日志记录 kind=InviteCode' 'gui: server-info .*kind=InviteCode'
    Save-Shot $client 'case4a-registration.png'
}

function Invoke-Case4b([string]$InviteCode) {
    Log 'CASE 4b：裸邀请码（无已知服务器）→ AddServerError'
    Stop-Client
    Reset-AppData
    $client = Start-Client 'case4b'
    $input = Wait-ById $client.Root 'AddServerInput' 20
    Set-Text $input $InviteCode | Out-Null
    Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
    $expected = '只输入邀请码时，需要先有这台服务器的地址（或先在列表里选一台）。'
    $errorElement = Wait-ById $client.Root 'AddServerError' 20
    $errorText = Get-Text $errorElement
    Add-Result '4b' '无已知服务器时裸码给出精确提示' ($errorText -eq $expected) ("expected=[" + $expected + "] actual=[" + $errorText + "]")
    $registration = Find-ById $client.Root 'CancelRegistration'
    Add-Result '4b' '不进入注册流程' ($null -eq $registration) ("CancelRegistration found=" + ($null -ne $registration))
    Save-Shot $client 'case4b-add-server-error.png'
    return $client
}

function Invoke-Case5($client) {
    Log 'CASE 5：未初始化服务器 18090 → owner bootstrap 向导'
    $input = Wait-ById $client.Root 'AddServerInput' 15
    Set-Text $input $BootstrapHttp | Out-Null
    Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
    $token = Wait-ById $client.Root 'BootstrapToken' 30
    $user = Find-ById $client.Root 'BootstrapUser'
    $password = Find-ById $client.Root 'BootstrapPassword'
    $complete = Find-ById $client.Root 'CompleteBootstrap'
    $serverName = Get-Text (Find-ById $client.Root 'BootstrapServerName')
    $backButton = Find-ById $client.Root 'BootstrapBackToServerList'
    $all = ($null -ne $token) -and ($null -ne $user) -and ($null -ne $password) -and ($null -ne $complete) -and
        ($serverName -eq 'UI-Fresh') -and ($null -ne $backButton)
    Add-Result '5' '空库服务器进入 owner 向导（4 控件 + 服务器名 + 返回按钮）' $all ("token=" + ($null -ne $token) + " user=" + ($null -ne $user) + " password=" + ($null -ne $password) + " complete=" + ($null -ne $complete) + " server_name=[" + $serverName + "] back=" + ($null -ne $backButton))
    $loginUser = Find-ById $client.Root 'LoginUser'
    Add-Result '5' '不是登录页（LoginUser 不存在）' ($null -eq $loginUser) ("LoginUser found=" + ($null -ne $loginUser))
    Assert-LogLine $client '5' '客户端日志记录 initialized=False' "gui: server-info .*initialized=False"
    Save-Shot $client 'case5-bootstrap.png'
    if ($Dump) { Dump-UiaTree $client 'uia-case5-bootstrap.txt' }
}

function Write-ServerBookFixture {
    New-Item -ItemType Directory -Force -Path $AppDataDir | Out-Null
    $fixture = [ordered]@{
        Servers     = @([ordered]@{
                Url      = $ClientHttp
                Name     = 'UI-Test'
                LastUser = $null
                LastUsed = (Get-Date).ToString('o')
            })
        SelectedUrl = $ClientHttp
    }
    $path = Join-Path $AppDataDir 'servers.json'
    $fixture | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8
    Log ("  servers.json fixture -> " + $path)
}

function Invoke-Case6 {
    Log 'CASE 6：记住我 → tokens.json 密文 → 重启自动登录'
    Stop-Client
    Reset-AppData
    $client = Start-Client 'case6'
    # 本轮从空 appdata 起步，先用 UI 添加一次地址进入登录页；「记住我」的验证不依赖服务器簿，
    # 重启自动登录则由随后的重启用例覆盖（ServerBook.Load 已修，servers.json 会被真正读回）。
    $input = Ensure-ServerPicker $client
    if (-not $input) { throw 'CASE 6：找不到 AddServerInput' }
    Set-Text $input $ClientHttp | Out-Null
    Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
    $loginUser = Wait-ById $client.Root 'LoginUser' 30
    Add-Result '6' '登录页可用（本次会话内 UI 添加地址）' ($null -ne $loginUser) ("LoginUser found=" + ($null -ne $loginUser))
    Set-Text $loginUser $OwnerUser | Out-Null
    $passInput = Wait-ById $client.Root 'LoginPassword' 10
    Set-Text $passInput $OwnerPass | Out-Null
    $remember = Wait-ById $client.Root 'RememberMe' 10
    $toggle = Get-ToggleState $remember
    $toggled = 'already-on'
    if ($toggle -ne 'On' -and $toggle -ne 'Indeterminate') {
        Invoke-Element $remember | Out-Null
        Start-Sleep -Milliseconds 300
        $toggle = Get-ToggleState $remember
        $toggled = 'toggled-on'
    }
    Add-Result '6' 'RememberMe 处于勾选状态' ($toggle -eq 'On') ("ToggleState=" + $toggle + " (" + $toggled + ")")
    Invoke-Element (Wait-ById $client.Root 'Login' 10) | Out-Null
    $card = Wait-ById $client.Root 'UserCard' 45
    $selfLogin = Get-Text (Find-ById $client.Root 'SelfLoginName')
    $selfDisplay = Get-Text (Find-ById $client.Root 'SelfDisplayName')
    $loginOk = ($null -ne $card) -and ($selfLogin -eq $OwnerUser) -and (-not [string]::IsNullOrWhiteSpace($selfDisplay))
    Add-Result '6' '真实登录成功（UserCard + SelfLoginName=uiowner）' $loginOk ("UserCard=" + ($null -ne $card) + " SelfLoginName=[" + $selfLogin + "] SelfDisplayName=[" + $selfDisplay + "]")
    $tokenFile = Join-Path $AppDataDir 'tokens.json'
    $exists = Test-Path -LiteralPath $tokenFile
    Add-Result '6' 'tokens.json 已生成' $exists ("path=" + $tokenFile)
    $raw = $null
    $key = $null
    $plain = $null
    if ($exists) {
        $raw = Get-Content -LiteralPath $tokenFile -Raw
        $json = $raw | ConvertFrom-Json
        $entryKeys = @()
        if ($json.Entries) {
            $entryKeys = @($json.Entries.PSObject.Properties | ForEach-Object { $_.Name })
        }
        $key = $entryKeys | Where-Object { $_ -like ('*' + $ClientHttp + '*') } | Select-Object -First 1
        $b64 = $null
        if ($key) { $b64 = $json.Entries.$key }
        Add-Result '6' '存在该服务器/账号的记住我条目' ([bool]$key) ("key=[" + $key + "] keys=" + ($entryKeys -join ';'))
        $cipher = @()
        $base64Valid = $false
        if ($b64) {
            try { $cipher = [Convert]::FromBase64String($b64); $base64Valid = $true } catch { $base64Valid = $false }
        }
        Add-Result '6' '值为非空 base64' ($base64Valid -and $cipher.Length -gt 0) ("base64_len=" + $(if ($b64) { $b64.Length } else { 0 }) + " cipher_len=" + $cipher.Length + " valid_b64=" + $base64Valid)
        $decryptError = $null
        if ($base64Valid -and $cipher.Length -gt 0) {
            try {
                $plainBytes = [LoginUiDpapi]::Unprotect($cipher)
                $plain = [System.Text.Encoding]::UTF8.GetString($plainBytes)
            } catch {
                $decryptError = $_.Exception.Message
            }
        }
        Add-Result '6' 'DPAPI 可解出明文 token（证明存的是真凭据）' ([bool]$plain) ("token_len=" + $(if ($plain) { $plain.Length } else { 0 }) + " decrypt_error=" + $decryptError)
        if ($plain) {
            $containsPlain = $raw.Contains($plain)
            Add-Result '6' 'tokens.json 原文不含明文 token' (-not $containsPlain) ("contains_plaintext=" + $containsPlain + " token_len=" + $plain.Length + " cipher_prefix_b64=" + $b64.Substring(0, [Math]::Min(12, $b64.Length)))
            $plainAsBase64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($plain))
            Add-Result '6' '密文并非 token 明文的 base64' ($b64 -ne $plainAsBase64) ("cipher_is_plaintext_b64=" + ($b64 -eq $plainAsBase64))
        }
    }
    Save-Shot $client 'case6-logged-in.png'
    Stop-Client

    Log '  CASE 6（续）：重启（不带 --token）应自动登录'
    $client = Start-Client 'case6-restart'
    $hit = Assert-LogLine $client '6' '重启日志出现「auto-login with remembered token as uiowner」' 'gui: auto-login with remembered token as uiowner'
    $card2 = Wait-ById $client.Root 'UserCard' 45
    Add-Result '6' '自动登录后直接进入已登录界面' ($null -ne $card2) ("UserCard found=" + ($null -ne $card2) + "；与上一条同一根因（服务器簿未载入 → 没有 ActiveServer）")
    Save-Shot $client 'case6-auto-login.png'
    if ($Dump -or -not $card2) { Dump-UiaTree $client 'uia-case6-restart.txt' }
    return $client
}

function Invoke-Case7($client) {
    Log 'CASE 7：退出登录 → tokens.json 条目消失'
    # 自动登录若因服务器簿未载入而失败，这里先用 UI 登录一次再验证「退出清凭据」。
    if (-not (Find-ById $client.Root 'UserCard')) {
        $input = Ensure-ServerPicker $client
        if ($input) {
            Set-Text $input $ClientHttp | Out-Null
            Invoke-Element (Wait-ById $client.Root 'AddServer' 10) | Out-Null
            $loginUser = Wait-ById $client.Root 'LoginUser' 30
            if ($loginUser) {
                Set-Text $loginUser $OwnerUser | Out-Null
                $passInput = Wait-ById $client.Root 'LoginPassword' 10
                Set-Text $passInput $OwnerPass | Out-Null
                Invoke-Element (Wait-ById $client.Root 'Login' 10) | Out-Null
                Start-Sleep -Seconds 2
            }
        }
    }
    $card = Wait-ById $client.Root 'UserCard' 30
    $logout = $null
    if ($card) {
        Invoke-Element $card | Out-Null
        $logout = Wait-ByIdGlobal 'Logout' 5
        if (-not $logout) {
            # Flyout 偶发不因 UIA Invoke 展开（本次运行已见一次）：退回真实鼠标点击卡片中心。
            Click-Element $card
            $logout = Wait-ByIdGlobal 'Logout' 10
        }
    }
    Add-Result '7' '用户卡片菜单里有「退出登录」' ($null -ne $logout) ("Logout found=" + ($null -ne $logout))
    if ($logout) { Invoke-Element $logout | Out-Null }
    # 退出登录的语义是「回到第一屏选服务器」（backToServerList: true），不是留在登录页。
    $picker = Wait-ById $client.Root 'AddServerInput' 20
    $listAfter = Find-ById $client.Root 'ServerList'
    $loginUser = Find-ById $client.Root 'LoginUser'
    $selfAfterLogout = Find-ById $client.Root 'SelfDisplayName'
    $backHome = (($null -ne $picker) -or ($null -ne $listAfter)) -and ($null -eq $selfAfterLogout)
    Add-Result '7' '退出后回到未登录界面（第一屏/服务器列表，SelfDisplayName 消失）' $backHome ("AddServerInput found=" + ($null -ne $picker) + " ServerList found=" + ($null -ne $listAfter) + " LoginUser found=" + ($null -ne $loginUser) + " SelfDisplayName found=" + ($null -ne $selfAfterLogout))
    Start-Sleep -Milliseconds 1200
    $tokenFile = Join-Path $AppDataDir 'tokens.json'
    $count = -1
    $raw = ''
    if (Test-Path -LiteralPath $tokenFile) {
        $raw = Get-Content -LiteralPath $tokenFile -Raw
        if ($raw) {
            $json = $raw | ConvertFrom-Json
            $count = 0
            if ($json.Entries) { $count = @($json.Entries.PSObject.Properties).Count }
        } else {
            $count = 0
        }
    } else {
        $count = 0
    }
    Add-Result '7' 'tokens.json 中该账号条目已清除（Entries 为空）' ($count -eq 0) ("entries_count=" + $count + " file_exists=" + (Test-Path -LiteralPath $tokenFile) + " raw_len=" + $raw.Length)
    Save-Shot $client 'case7-after-logout.png'
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

$backupDir = $null
$inviteCode = $null
try {
    New-Item -ItemType Directory -Force -Path $Root, $OutDir | Out-Null
    Log ("client=" + $ClientExe)
    Log ("server=" + $ServerExe)
    Log ("out=" + $OutDir)

    if (-not (Test-Path -LiteralPath $ClientExe)) { throw ("客户端不存在: " + $ClientExe) }
    if (-not (Test-Path -LiteralPath $ServerExe)) { throw ("服务端不存在: " + $ServerExe) }

    $backupDir = Backup-AppData
    Log ("appdata backup -> " + $backupDir)

    if ($CleanupRoot) {
        Stop-QaServers
        if (Test-Path -LiteralPath $Root) { Remove-Item -Recurse -Force $Root }
        Log 'CLEANUP_ROOT=done'
        return
    }

    $script:LockFile = Acquire-GuiLock 240
    Log ("gui lock acquired: " + $script:LockFile)

    Reset-AppData
    if (Test-Path -LiteralPath (Join-Path $Root 'data')) { Remove-Item -Recurse -Force (Join-Path $Root 'data') }
    if (Test-Path -LiteralPath (Join-Path $Root 'data2')) { Remove-Item -Recurse -Force (Join-Path $Root 'data2') }

    $server18089 = Start-QaServer 18089 (Join-Path $Root 'data') 'UI-Test'
    $server18090 = Start-QaServer 18090 (Join-Path $Root 'data2') 'UI-Fresh'
    if (-not $server18089.Token) { throw '18089 没有拿到 BOOTSTRAP_TOKEN（数据目录不是空库？）' }

    $bootstrapResponse = Invoke-Api 'POST' ($ClientHttp + '/api/bootstrap') @{
        token        = $server18089.Token
        username     = $OwnerUser
        password     = $OwnerPass
        display_name = 'UI Owner'
    } $null
    $ownerToken = $bootstrapResponse.token
    Add-Result '0' '18089 已 bootstrap 出 owner（测试前置）' (-not [string]::IsNullOrWhiteSpace($ownerToken)) ("owner=" + $OwnerUser + " token_len=" + $(if ($ownerToken) { $ownerToken.Length } else { 0 }))
    $inviteResponse = Invoke-Api 'POST' ($ClientHttp + '/api/admin/invites') $null $ownerToken
    $inviteCode = $inviteResponse.invite.code
    Add-Result '0' '18089 已创建邀请码（测试前置）' (-not [string]::IsNullOrWhiteSpace($inviteCode)) ("invite_code=" + $inviteCode)

    $requested = $Cases.Split(',') | ForEach-Object { $_.Trim().ToLowerInvariant() }
    Log ("cases requested: " + ($requested -join ','))

    $client = $null
    if ($requested -contains '1') {
        $client = Start-Client 'case1'
        Invoke-Case1 $client
    }
    if ($requested -contains '2') {
        if (-not $client) { $client = Start-Client 'case2' }
        Invoke-Case2 $client
        # Stop-Client/Start-Client 在 case2 内部会替换 $script:Client；这里重新取
        $client = $script:Client
    }
    if ($requested -contains '3') {
        if (-not $client) {
            Reset-AppData
            Write-ServerBookFixture
            $client = Start-Client 'case3'
        }
        Invoke-Case3 $client $inviteCode
    }
    if ($requested -contains '4a') {
        if (-not $client) { $client = Start-Client 'case4a' }
        Invoke-Case4a $client $inviteCode
    }
    if ($requested -contains '4b') {
        $client = Invoke-Case4b $inviteCode
    }
    if ($requested -contains '5') {
        if (-not $client) { $client = Start-Client 'case5' }
        Invoke-Case5 $client
    }
    if ($requested -contains '6') {
        $client = Invoke-Case6
    }
    if ($requested -contains '7') {
        if (-not $client) {
            $client = Start-Client 'case7'
        }
        Invoke-Case7 $client
    }
}
catch {
    Log ("FATAL: " + $_.Exception.Message)
    Log ($_.ScriptStackTrace)
    Add-Result 'FATAL' '脚本执行异常' $false ($_.Exception.Message)
}
finally {
    Stop-Client
    if (-not $KeepServers) { Stop-QaServers }
    Restore-AppData $backupDir
    Release-GuiLock

    try {
        $lines = New-Object System.Collections.Generic.List[string]
        $lines.Add('CASE|ASSERTION|RESULT|EVIDENCE')
        foreach ($row in $script:Results) {
            $lines.Add(($row.Case + '|' + $row.Assertion + '|' + $row.Result + '|' + $row.Evidence))
        }
        $resultPath = Join-Path $OutDir 'results.txt'
        $lines | Set-Content -LiteralPath $resultPath -Encoding utf8
        $jsonPath = Join-Path $OutDir 'results.json'
        $script:Results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $jsonPath -Encoding utf8
        Log ('results -> ' + $resultPath)
        $pass = @($script:Results | Where-Object { $_.Result -eq 'PASS' }).Count
        $fail = @($script:Results | Where-Object { $_.Result -eq 'FAIL' }).Count
        Log ('SUMMARY pass=' + $pass + ' fail=' + $fail + ' total=' + $script:Results.Count)
    } catch {
        Log ('results write failed: ' + $_.Exception.Message)
    }
    Log 'DONE'
}
