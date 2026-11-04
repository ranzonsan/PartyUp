# Generates placeholder packaging assets (one-off script; outputs are committed).
#
#   assets/icon.ico   - multi-size app icon (16/32/48/256, PNG-compressed entries).
#                       Used by `vpk --icon` and the csproj <ApplicationIcon>.
#                       When the real logo is ready, replace only this file.
#   assets/splash.png - splash shown by the Velopack Setup.exe while installing (500x300).
#
# This machine has no Pillow / ImageMagick, so System.Drawing (built into Windows
# PowerShell 5.1) is used. The script is intentionally ASCII-only: Windows
# PowerShell 5.1 reads .ps1 without a UTF-8 BOM as ANSI, and non-ASCII text then
# breaks the parser.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make-assets.ps1
[CmdletBinding()]
param(
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# $PSScriptRoot is not reliably available while parameter defaults are evaluated
# in Windows PowerShell 5.1, so resolve the default here instead.
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets'
}

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function New-RoundedRectPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconFrame {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # Precompute floats: inline arithmetic inside `New-Object Type(...)` argument lists
    # is parsed unreliably by Windows PowerShell 5.1 (it tries to multiply the whole
    # argument array), so keep the constructor arguments as plain variables.
    $pad = [float][Math]::Max(1.0, $Size * 0.045)
    $inner = [float]($Size - 2.0 * $pad)
    $rect = New-Object System.Drawing.RectangleF($pad, $pad, $inner, $inner)
    $radius = [float]($Size * 0.20)
    $path = New-RoundedRectPath -X $rect.X -Y $rect.Y -W $rect.Width -H $rect.Height -R $radius

    # Dark base with a blue gradient (matches the client dark theme).
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 51, 78, 122),
        [System.Drawing.Color]::FromArgb(255, 15, 21, 33),
        90.0)
    $g.FillPath($brush, $path)

    # Orange dot = "someone is talking".
    if ($Size -ge 32) {
        $dot = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 138, 76))
        $d = [float]($Size * 0.17)
        $g.FillEllipse($dot, [float]($Size * 0.66), [float]($Size * 0.18), $d, $d)
        $dot.Dispose()
    }

    # White "P".
    $fontSize = [float]($Size * 0.58)
    $font = New-Object System.Drawing.Font(
        'Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $textTop = [float]($Size * 0.01)
    $textRect = New-Object System.Drawing.RectangleF(0.0, $textTop, [float]$Size, [float]$Size)
    $g.DrawString('P', $font, $white, $textRect, $fmt)

    $white.Dispose(); $fmt.Dispose(); $font.Dispose(); $brush.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

function Write-Ico {
    param([string]$Path, [System.Drawing.Bitmap[]]$Frames)

    $pngBlobs = New-Object 'System.Collections.Generic.List[byte[]]'
    foreach ($frame in $Frames) {
        $ms = New-Object System.IO.MemoryStream
        $frame.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngBlobs.Add($ms.ToArray())
        $ms.Dispose()
    }

    $out = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($out)
    $bw.Write([UInt16]0)                         # reserved
    $bw.Write([UInt16]1)                         # type: icon
    $bw.Write([UInt16]$Frames.Count)             # image count

    $offset = 6 + 16 * $Frames.Count
    for ($i = 0; $i -lt $Frames.Count; $i++) {
        $frame = $Frames[$i]
        $blob = $pngBlobs[$i]
        $dim = if ($frame.Width -ge 256) { [byte]0 } else { [byte]$frame.Width }  # 0 means 256
        $bw.Write($dim)                          # width
        $bw.Write($dim)                          # height
        $bw.Write([byte]0)                       # color count
        $bw.Write([byte]0)                       # reserved
        $bw.Write([UInt16]1)                     # planes
        $bw.Write([UInt16]32)                    # bit count
        $bw.Write([UInt32]$blob.Length)          # bytes in resource
        $bw.Write([UInt32]$offset)               # offset
        $offset += $blob.Length
    }
    foreach ($blob in $pngBlobs) { $bw.Write($blob) }
    $bw.Flush()

    [System.IO.File]::WriteAllBytes($Path, $out.ToArray())
    $bw.Dispose(); $out.Dispose()
    foreach ($f in $Frames) { $f.Dispose() }
}

function New-Splash {
    param([int]$Width = 500, [int]$Height = 300)

    $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    $rect = New-Object System.Drawing.RectangleF(0, 0, $Width, $Height)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 30, 42, 62),
        [System.Drawing.Color]::FromArgb(255, 10, 14, 22),
        90.0)
    $g.FillRectangle($bg, $rect)

    # Accent bar + wordmark.
    $accent = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 138, 76))
    $g.FillRectangle($accent, [float]($Width * 0.09), [float]($Height * 0.24), [float]($Width * 0.012), [float]($Height * 0.42))

    $titleFont = New-Object System.Drawing.Font(
        'Segoe UI', [float]($Height * 0.17), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $subFont = New-Object System.Drawing.Font(
        'Segoe UI', [float]($Height * 0.062), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 244, 247, 252))
    $dim = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 158, 172, 194))

    $g.DrawString('PartyUp', $titleFont, $white, [float]($Width * 0.135), [float]($Height * 0.28))
    $g.DrawString('Self-hosted voice & screen sharing', $subFont, $dim,
        [float]($Width * 0.138), [float]($Height * 0.50))

    $white.Dispose(); $dim.Dispose(); $titleFont.Dispose(); $subFont.Dispose()
    $accent.Dispose(); $bg.Dispose(); $g.Dispose()
    return $bmp
}

# 16/32/48/256: explorer / taskbar / shortcuts / high DPI.
$frames = @(
    (New-IconFrame -Size 16),
    (New-IconFrame -Size 32),
    (New-IconFrame -Size 48),
    (New-IconFrame -Size 256)
)
Write-Ico -Path (Join-Path $OutDir 'icon.ico') -Frames $frames

$splash = New-Splash
$splash.Save((Join-Path $OutDir 'splash.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$splash.Dispose()

Write-Host ("assets written: {0}" -f $OutDir)
Get-ChildItem $OutDir -Filter 'icon.ico' | ForEach-Object { Write-Host ("  icon.ico  {0} bytes" -f $_.Length) }
Get-ChildItem $OutDir -Filter 'splash.png' | ForEach-Object { Write-Host ("  splash.png {0} bytes" -f $_.Length) }
