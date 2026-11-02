<#
.SYNOPSIS
  Decode a PNG with GDI+ (System.Drawing) and print brightness statistics, to
  prove the frame captured by the WGC probe is not an all-black image.

.DESCRIPTION
  This checker is independent from the probe: the probe only writes the PNG,
  while this script re-opens it with a third-party decoder (Windows GDI+, not a
  hand-written PNG parser) and reports mean RGB, mean luma, min/max luma,
  non-black percentage and bright percentage.

  NOTE: comments in this file are ASCII on purpose. The script forwards itself
  to Windows PowerShell 5.1 (which has the full GDI+ stack), and 5.1 reads
  BOM-less scripts as ANSI; non-ASCII comments would corrupt the parse.

.PARAMETER Path
  One or more PNG paths.

.EXAMPLE
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `
    '\\wsl.localhost\<distro>\<repo>\spikes\wgc-probe\check-png.ps1' `
    -Path '\\wsl.localhost\<distro>\<repo>\spikes\wgc-probe\out\run1-...\x.png'
#>
param(
  [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
  [string[]]$Path
)

$ErrorActionPreference = 'Stop'

# PowerShell 7's System.Drawing needs System.Drawing.Common +
# System.Private.Windows.GdiPlus + System.Private.Windows.Core references in
# Add-Type. Windows PowerShell 5.1 ships the complete GDI+ stack, so forward.
if ($PSVersionTable.PSEdition -eq 'Core') {
  $fwd = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Path') + $Path
  & powershell.exe @fwd
  exit $LASTEXITCODE
}

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class WgcPngStats {
  public static string Run(string path) {
    using (var bmp = new Bitmap(path)) {
      int w = bmp.Width, h = bmp.Height;
      var rect = new Rectangle(0, 0, w, h);
      var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      long sumR = 0, sumG = 0, sumB = 0, n = 0, nonzero = 0, bright = 0;
      int minL = 255, maxL = 0;
      try {
        byte[] buf = new byte[w * 4];
        for (int y = 0; y < h; y++) {
          Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), buf, 0, buf.Length);
          for (int x = 0; x < w; x++) {
            byte b = buf[x * 4 + 0], g = buf[x * 4 + 1], r = buf[x * 4 + 2];
            sumR += r; sumG += g; sumB += b; n++;
            int l = (r * 299 + g * 587 + b * 114) / 1000;
            if (l < minL) minL = l;
            if (l > maxL) maxL = l;
            if (l > 0) nonzero++;
            if (l > 128) bright++;
          }
        }
      } finally { bmp.UnlockBits(data); }
      double meanR = (double)sumR / n, meanG = (double)sumG / n, meanB = (double)sumB / n;
      return string.Format(
        "path={0}\n  size={1}x{2} pixels={3}\n  mean_rgb=({4:F2},{5:F2},{6:F2}) mean_luma={7:F2}\n  min_luma={8} max_luma={9} nonzero_pct={10:F3} bright_pct={11:F3}\n  verdict={12}",
        path, w, h, n, meanR, meanG, meanB,
        (0.299 * sumR + 0.587 * sumG + 0.114 * sumB) / n,
        minL, maxL, 100.0 * nonzero / n, 100.0 * bright / n,
        (maxL == 0 ? "ALL_BLACK" : "NOT_BLACK"));
    }
  }
}
"@

$failed = $false
foreach ($item in $Path) {
  if (-not (Test-Path -LiteralPath $item)) {
    Write-Host "MISSING: $item"
    $failed = $true
    continue
  }
  # GDI+'s Bitmap(string) rejects UNC paths ("unsupported path format"), so
  # stage a UNC input on the local disk first. NOTE: Resolve-Path in 5.1
  # returns a provider-qualified string ("Microsoft.PowerShell.Core\FileSystem::..."),
  # so test the original argument, not the resolved one.
  $localPath = $item
  $tempPath = $null
  if ($item.StartsWith('\\')) {
    $tempPath = Join-Path $env:TEMP ('wgc-check-' + [guid]::NewGuid().ToString('N') + '.png')
    Copy-Item -LiteralPath $item -Destination $tempPath -Force
    $localPath = $tempPath
  }
  Write-Host ("input=" + $item)
  try {
    [WgcPngStats]::Run($localPath)
  } finally {
    if ($tempPath) { Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue }
  }
}

if ($failed) { exit 1 }
