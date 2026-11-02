# 抓取 Windows 主屏截图（用于 GUI 取证：窗口是否真的画出来了、消息列表里有没有内容）。
#
# 在 WSL 里调用：
#   pwsh.exe -NoProfile -File scripts/capture-window.ps1 `
#     -OutPath 'C:\Users\<you>\AppData\Local\Temp\partyup-gui.png' -ProcessName 'PartyUp.Client'
# 然后用 read_image 看这张图（WSL 通过 /mnt/c 只读访问该路径）。
#
# 为什么要 -ProcessName：直接截主屏会截到最上层窗口（实测截到浏览器）。给了进程名就用
# SetForegroundWindow + ShowWindow(SW_MAXIMIZE) 把目标窗口最大化提到前台，再截主屏——
# 这样图里只有被验证的那个窗口，不会因为窗口被别的窗口压住而截错。
#
# 退出码：0 = 已保存；非 0 = 失败。
param(
    [Parameter(Mandatory = $true)]
    [string]$OutPath,

    # 兼容旧用法：按标题激活（AppActivate）。
    [string]$ActivateTitle = '',

    # 推荐用法：按进程名找到主窗口，最大化并提到前台。
    [string]$ProcessName = ''
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if ($ProcessName) {
    Add-Type -Namespace PartyUpEvidence -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetForegroundWindow(System.IntPtr hWnd);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);
'@

    $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
    if (-not $process) {
        Write-Output "process '$ProcessName' not found (or has no window)"
        exit 3
    }

    $handle = $process.MainWindowHandle
    [void][PartyUpEvidence.Win32]::ShowWindow($handle, 3)      # SW_MAXIMIZE
    [void][PartyUpEvidence.Win32]::SetForegroundWindow($handle)
    Write-Output "maximized pid=$($process.Id) hwnd=$handle title='$($process.MainWindowTitle)'"
    Start-Sleep -Milliseconds 900
}
elseif ($ActivateTitle) {
    $shell = New-Object -ComObject WScript.Shell
    $activated = $shell.AppActivate($ActivateTitle)
    Write-Output "activate='$ActivateTitle' result=$activated"
    Start-Sleep -Milliseconds 700
}

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
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
