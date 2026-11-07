# 抓取**指定窗口本身**的图像（GUI 取证用：窗口是否画出来了、面板里有什么）。
#
# 在 WSL 里调用：
#   pwsh.exe -NoProfile -ExecutionPolicy Bypass -File scripts/capture-window.ps1 `
#     -ProcessId 1234 -OutPath 'C:\Users\<you>\AppData\Local\Temp\partyup-gui.png'
#
# 设计变化（2026-11-07）：以前是「把窗口最大化提到前台 + 截主屏」。多屏机器上窗口开到副屏时，
# 主屏截到的是用户正在用的别的程序（实测截到浏览器），既拿不到证据也打扰用户。
# 现在按窗口矩形直接抓窗口内容：
#   1) 优先 PrintWindow(PW_RENDERFULLCONTENT)，不抢焦点、不改变窗口位置，副屏/负坐标都正确；
#   2) 若 PrintWindow 返回空白（部分 GPU 合成表面会这样），退回 CopyFromScreen(窗口矩形)；
#   3) 两条路都不改窗口状态，验收不再把用户的窗口挪来挪去。
#
# 退出码：0 = 已保存；3 = 找不到窗口；4 = 窗口不可见；5/6 = 取窗口矩形失败。
param(
    [Parameter(Mandatory = $true)]
    [string]$OutPath,

    # 按标题找窗口（兼容旧用法：只用于定位，不再 AppActivate 抢焦点）。
    [string]$ActivateTitle = '',

    [string]$ProcessName = '',

    # 两个同 exe 的客户端同时开时，按 pid 指定要截的那个窗口（优先于 -ProcessName）。
    [int]$ProcessId = 0,

    # 显式要求「最大化后再截」时才动窗口；默认关闭，避免打扰用户。
    [switch]$Maximize
)

$ErrorActionPreference = 'Stop'

# 本进程必须 DPI-aware：否则 GetWindowRect 返回虚拟化坐标，抓出来的图尺寸不对。
Add-Type -Namespace PartyUpEvidence -Name Dpi -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
'@
try {
    [void][PartyUpEvidence.Dpi]::SetProcessDpiAwarenessContext([System.IntPtr]-4)
} catch {
    Add-Type -Namespace PartyUpEvidence -Name DpiFallback -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetProcessDPIAware();
'@
    [void][PartyUpEvidence.DpiFallback]::SetProcessDPIAware()
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace PartyUpEvidence -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr hWnd, out RECT rect);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PrintWindow(System.IntPtr hWnd, System.IntPtr hdcBlt, uint flags);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetForegroundWindow(System.IntPtr hWnd);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hWnd);
'@

$process = $null
if ($ProcessId -ne 0) {
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 }
} elseif ($ProcessName) {
    $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
} elseif ($ActivateTitle) {
    $process = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -like "*$ActivateTitle*" -and $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
}
if (-not $process) {
    Write-Output "process not found (pid=$ProcessId name='$ProcessName' title='$ActivateTitle') or has no window"
    exit 3
}

$handle = $process.MainWindowHandle
if (-not [PartyUpEvidence.Win32]::IsWindowVisible($handle) -and -not $Maximize) {
    Write-Output "window is not visible (pid=$($process.Id)); pass -Maximize or show it first"
    exit 4
}

if ($Maximize) {
    [void][PartyUpEvidence.Win32]::ShowWindow($handle, 3)
    [void][PartyUpEvidence.Win32]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 700
}

$rect = New-Object PartyUpEvidence.Win32+RECT
if (-not [PartyUpEvidence.Win32]::GetWindowRect($handle, [ref]$rect)) {
    Write-Output 'GetWindowRect failed'
    exit 5
}
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    Write-Output "bad window rect ${width}x${height}"
    exit 6
}
Write-Output ("window pid=" + $process.Id + " hwnd=" + $handle + " rect=" + $rect.Left + "," + $rect.Top + " " + $width + "x" + $height + " title='" + $process.MainWindowTitle + "'")

$bitmap = New-Object System.Drawing.Bitmap $width, $height
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        # 1) PrintWindow：不改变窗口状态，副屏/负坐标都按窗口自身坐标系渲染。
        $hdc = $graphics.GetHdc()
        try {
            $printed = [PartyUpEvidence.Win32]::PrintWindow($handle, $hdc, 2)   # PW_RENDERFULLCONTENT
        } finally {
            $graphics.ReleaseHdc($hdc)
        }

        # 判断是否真的画上了内容：全黑/全白都视为失败，退回屏幕拷贝。
        $hasContent = $false
        $stepX = [Math]::Max(8, [int]($width / 12))
        $stepY = [Math]::Max(8, [int]($height / 12))
        for ($y = 4; $y -lt $height; $y += $stepY) {
            for ($x = 4; $x -lt $width; $x += $stepX) {
                $pixel = $bitmap.GetPixel($x, $y)
                if ($pixel.R -gt 12 -or $pixel.G -gt 12 -or $pixel.B -gt 12) { $hasContent = $true; break }
            }
            if ($hasContent) { break }
        }

        if ($hasContent -and $printed) {
            Write-Output 'capture=PrintWindow'
        } else {
            # 2) 退回屏幕拷贝：按窗口矩形取屏，负坐标（副屏在左）同样有效。
            $graphics.Clear([System.Drawing.Color]::Black)
            $graphics.CopyFromScreen(
                (New-Object System.Drawing.Point $rect.Left, $rect.Top),
                [System.Drawing.Point]::Empty,
                (New-Object System.Drawing.Size $width, $height))
            Write-Output 'capture=CopyFromScreen'
        }
    }
    finally {
        $graphics.Dispose()
    }

    $directory = Split-Path -Parent $OutPath
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $bitmap.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $bitmap.Dispose()
}

$size = (Get-Item -LiteralPath $OutPath).Length
Write-Output "saved=$OutPath bytes=$size"
