param(
  [string]$PathMatch = '*PartyUp.Client*',
  [string]$CommandLineLike = '',
  [int]$Samples = 3,
  [int]$IntervalMs = 1500,
  [string]$ModulesOut = ''
)

# 内存基线归因测量：对匹配的 PartyUp.Client 进程采样 WS / Private / 模块数 / 句柄数 / 线程数，
# 并单独报告 partyup_media.dll 是否在模块列表里（判断延迟加载能不能省内存的直接证据）。
# 选择方式二选一：-PathMatch（按 exe 路径，默认）或 -CommandLineLike（按命令行，用于区分同 exe 的多个进程）。
$p = $null
if ($CommandLineLike -ne '') {
  $found = Get-CimInstance Win32_Process -Filter "Name='PartyUp.Client.exe'" -ErrorAction SilentlyContinue |
           Where-Object { $_.CommandLine -like $CommandLineLike } |
           Select-Object -First 1
  if ($found) { $p = Get-Process -Id $found.ProcessId -ErrorAction SilentlyContinue }
}
if (-not $p) {
  $p = Get-Process PartyUp.Client -ErrorAction SilentlyContinue |
       Where-Object { $_.Path -like $PathMatch } |
       Select-Object -First 1
}
if (-not $p) { 'NO_PROCESS'; exit 2 }

for ($i = 1; $i -le $Samples; $i++) {
  Start-Sleep -Milliseconds $IntervalMs
  $p.Refresh()
  $mods = @($p.Modules)
  $mediaDll = @($mods | Where-Object { $_.ModuleName -like 'partyup_media*' })
  '{0}|ws={1}|priv={2}|mods={3}|handles={4}|threads={5}|mediaDll={6}' -f `
      $i, $p.WorkingSet64, $p.PrivateMemorySize64, $mods.Count, $p.Handles, $p.Threads.Count, $mediaDll.Count
}

if ($ModulesOut -ne '') {
  $p.Refresh()
  $names = @($p.Modules | ForEach-Object { $_.ModuleName })
  [System.IO.File]::WriteAllLines($ModulesOut, [string[]]$names)
  "MODULES_WRITTEN=$ModulesOut count=$($names.Count)"
}
