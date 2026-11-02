# 硬件 H.264 vs 软件 VP8 的编码器 CPU/fps 对照测量（2560×1440，10 s）
#
# 为什么需要「屏幕活动生成器」：WGC 是变化驱动采集，桌面静止时 10 s 只出 2 帧，
# 那样的数字既测不出编码成本也没法比 fps。所以每次测量前开一个可见的控制台窗口
# 持续滚屏（真实桌面内容变化），让采集有稳定 60 fps 的输入。
#
# 用法（在 WSL 里）：
#   pwsh.exe -NoProfile -ExecutionPolicy Bypass -File \
#     '\\wsl.localhost\<distro>\<repo>\spikes\media-dll-smoke\measure-encoder-cpu.ps1'
#
# 输出：每轮一行汇总 + 两轮对照表；详细日志落在 $OutDir。
# VP8 对照组靠 PARTYUP_MEDIA_FORCE_VP8=1 让编码器工厂不声明 H.264（同一份 DLL）。
param(
  [string]$StageDir = (Join-Path $env:LOCALAPPDATA 'PartyUp\media-stage'),
  [string]$OutDir = (Join-Path $env:TEMP 'partyup-cpu'),
  [int]$Seconds = 10
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Start-ScreenActivity {
  # 可见的控制台窗口，持续滚屏 ~(Seconds+6) 秒；滚屏行数与间隔决定画面变化频率。
  $lines = [int](($Seconds + 6) * 40)
  $script = "1..$lines | ForEach-Object { Write-Host (('█' * (($_ % 70) + 1))); Start-Sleep -Milliseconds 25 }"
  return Start-Process -FilePath 'pwsh.exe' -ArgumentList @('-NoProfile', '-Command', $script) -PassThru
}

function Invoke-OneMeasure {
  param([string]$Label, [hashtable]$Env)

  foreach ($key in $Env.Keys) { Set-Item -Path "Env:$key" -Value $Env[$key] }
  $log = Join-Path $OutDir "$Label.log"

  $activity = Start-ScreenActivity
  Start-Sleep -Milliseconds 900   # 让滚屏窗口先画出内容
  try {
    Push-Location $StageDir
    & .\screen_capture_smoke.exe --seconds $Seconds --png "$Label.png" *>&1 |
      Out-File -Encoding utf8 $log
    $code = $LASTEXITCODE
    Pop-Location
  } finally {
    if ($activity -and -not $activity.HasExited) { Stop-Process -Id $activity.Id -Force }
    foreach ($key in $Env.Keys) { Remove-Item -Path "Env:$key" -ErrorAction SilentlyContinue }
  }

  $text = Get-Content -Raw -Encoding utf8 $log
  $clean = $text -replace "`e\[[0-9;]*m", ''

  function Pick([string]$pattern) {
    $m = [regex]::Match($clean, $pattern)
    if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '?' }
  }

  $codec = Pick 'send video codecs: (\S+)'
  $frames = Pick 'frame events\s+:\s+(\d+) in'
  $fps = Pick 'frame events\s+:\s+\d+ in \d+ s \(([\d.]+) fps'
  $cpu = Pick 'process cpu\s+:\s+([\d.]+ s over)'
  $cpu2 = Pick 'process cpu\s+:\s+[\d.]+ s over \d+ s wall = ([\d.]+% of one core)'
  $viewer = Pick 'viewer decoded\s+:\s+(\S+ over \d+ frame)'
  $stageAll = Pick 'stage avg ms\s+:\s+(.+)'
  $result = Pick 'RESULT: (\w+)'

  [pscustomobject]@{
    label = $Label
    codec = $codec
    frames = $frames
    fps = $fps
    cpu = "$cpu $cpu2"
    viewer = $viewer
    stage = $stageAll
    result = $result
    exit = $code
  }
}

Write-Host "== 对照测量开始（每轮 $Seconds s，2560×1440，含屏幕活动生成器）"

$h264 = Invoke-OneMeasure -Label 'hw-h264' -Env @{}
$vp8 = Invoke-OneMeasure -Label 'sw-vp8' -Env @{ PARTYUP_MEDIA_FORCE_VP8 = '1' }

Write-Host ''
Write-Host '== 对照结果 =='
@($h264, $vp8) | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
Write-Host "详细日志与 PNG: $OutDir"
