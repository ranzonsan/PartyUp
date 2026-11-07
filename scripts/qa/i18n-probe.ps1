param(
  [string]$ClientExe = '',
  [string]$Root = "$env:LOCALAPPDATA\PartyUpQA4",
  [int]$Port = 18085,
  [string]$OutDir = "$env:LOCALAPPDATA\PartyUpQA4\evidence",
  [string]$ServerExe = "$env:LOCALAPPDATA\PartyUpQA3\partyup-qa.exe",
  [switch]$SkipServer
)

# i18n 基础设施的真机验收：
#   1) 起一个隔离服务器（18085）并 bootstrap 一个账号；
#   2) 用新构建的客户端登录，截「应用设置」中文界面；
#   3) 把 %APPDATA%\PartyUp\settings.json 的 Language 改成 en，重启客户端，截英文界面；
#   4) 把两次界面里抓到的关键文案打印出来，证明文案确实来自资源而不是写死的 XAML。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Log([string]$m) { Write-Output ("[" + (Get-Date -Format 'HH:mm:ss') + "] " + $m) }

function Get-Window([int]$procId) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return $null }
  return [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
}

function Find-ById($window, [string]$id) {
  if (-not $window) { return $null }
  $c = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
  return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $c)
}

function Invoke-ById([int]$procId, [string]$id) {
  $t = Find-ById (Get-Window $procId) $id
  if (-not $t) { return "NOT_FOUND:$id" }
  $pat = $null
  if ($t.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) {
    $pat.Invoke(); return "CLICKED:$id"
  }
  return "NO_INVOKE:$id"
}

function Capture([int]$procId, [string]$name) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p -or $p.MainWindowHandle -eq 0) { return "NO_WINDOW" }
  $path = Join-Path $OutDir $name
  & "$PSScriptRoot\..\capture-window.ps1" -ProcessId $procId -OutPath $path | Out-Null
  if (Test-Path $path) { return "SHOT:$path" }
  return "SHOT_FAIL"
}

# 收集窗口里所有文本（按控件树顺序），用于打印关键文案
function Dump-Texts([int]$procId, [string]$name) {
  $w = Get-Window $procId
  if (-not $w) { return @() }
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Text)
  $texts = @()
  foreach ($t in $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
    $n = $t.Current.Name
    if ($n) { $texts += $n }
  }
  $path = Join-Path $OutDir "$name.texts.txt"
  $texts | Set-Content -Encoding UTF8 $path
  return $texts
}

# ---------------- 1) 服务器 ----------------
$base = "http://127.0.0.1:$Port"
if (-not $SkipServer) {
  $listening = $false
  try { Invoke-WebRequest -Uri "$base/api/health" -UseBasicParsing -TimeoutSec 2 | Out-Null; $listening = $true } catch { }
  if (-not $listening) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'data') | Out-Null
    $stdout = Join-Path $Root 'server.out'
    $stderr = Join-Path $Root 'server.err'
    Remove-Item -Force -ErrorAction SilentlyContinue $stdout, $stderr
    $srv = Start-Process -FilePath $ServerExe -ArgumentList @(
      'serve', '--data', (Join-Path $Root 'data'), '--http', "127.0.0.1:$Port", '--name', 'PartyUp i18n'
    ) -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    Log "server started pid=$($srv.Id)"
    $srv.Id | Set-Content (Join-Path $Root 'server.pid')
    for ($i = 0; $i -lt 60; $i++) {
      Start-Sleep -Milliseconds 500
      if ((Test-Path $stdout) -and (Select-String -Path $stdout -Pattern 'READY' -Quiet -ErrorAction SilentlyContinue)) { break }
    }
  } else { Log 'server already listening' }
}

$bootstrap = (Select-String -Path (Join-Path $Root 'server.out') -Pattern 'BOOTSTRAP_TOKEN=(\S+)' -ErrorAction SilentlyContinue |
  Select-Object -Last 1)
if (-not $bootstrap) {
  # 已有成员时不会打印 token：直接登录已知账号
  $body = @{ username = 'tester'; password = 'tester-pass-123' } | ConvertTo-Json
  $resp = Invoke-RestMethod -Uri "$base/api/login" -Method Post -Body $body -ContentType 'application/json'
  $token = $resp.token
  Log "login existing account"
} else {
  $bootToken = $bootstrap.Matches[0].Groups[1].Value
  $body = @{ token = $bootToken; username = 'tester'; password = 'tester-pass-123'; display_name = '测试者' } | ConvertTo-Json
  $resp = Invoke-RestMethod -Uri "$base/api/bootstrap" -Method Post -Body $body -ContentType 'application/json'
  $token = $resp.token
  Log "bootstrapped account"
}
Log ("TOKEN_LEN=" + $token.Length)

# ---------------- 2) 中文界面 ----------------
$settingsFile = Join-Path $env:APPDATA 'PartyUp\settings.json'
New-Item -ItemType Directory -Force -Path (Split-Path $settingsFile) | Out-Null
@{ Language = 'zh-Hans'; Theme = 'light'; TextSize = 'normal'; RememberMe = $true } |
  ConvertTo-Json | Set-Content -Encoding UTF8 $settingsFile

function Run-Client([string]$tag) {
  $p = Start-Process -FilePath $ClientExe -WorkingDirectory (Split-Path $ClientExe) -ArgumentList @(
    '--server', $base, '--token', $token, '--no-media', '--screen', '2'
  ) -PassThru
  Log "client[$tag] pid=$($p.Id)"
  Start-Sleep -Seconds 7
  Capture $p.Id "$tag-shell.png" | ForEach-Object { Log $_ }
  Invoke-ById $p.Id 'UserCard' | ForEach-Object { Log $_ }
  Start-Sleep -Milliseconds 900
  Invoke-ById $p.Id 'OpenSettings' | ForEach-Object { Log $_ }
  Start-Sleep -Seconds 2
  Capture $p.Id "$tag-settings.png" | ForEach-Object { Log $_ }
  $texts = Dump-Texts $p.Id "$tag-settings"
  Log ("TEXTS_" + $tag + "=" + ($texts -join ' | '))
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 700
}

Run-Client 'zh'

# ---------------- 3) 英文界面 ----------------
@{ Language = 'en'; Theme = 'light'; TextSize = 'normal'; RememberMe = $true } |
  ConvertTo-Json | Set-Content -Encoding UTF8 $settingsFile
Log 'settings.json Language=en written'

Run-Client 'en'

# ---------------- 4) 收尾 ----------------
Log ('EVIDENCE_DIR=' + $OutDir)
Get-ChildItem $OutDir | ForEach-Object { Log ("FILE=" + $_.Name + " " + $_.Length) }
