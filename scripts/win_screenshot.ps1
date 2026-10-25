# Capture the game window (client area only) to PNGs, repeatedly.
# Run from WSL: powershell.exe -NoProfile -ExecutionPolicy Bypass -File <unc path> -OutDir <unc dir> ...
# Only the game window is captured (PrintWindow), never the rest of the desktop.
param(
  [Parameter(Mandatory = $true)][string]$OutDir,
  [int]$IntervalMs = 1500,
  [int]$Count = 20,
  [int]$StartDelayMs = 0,
  [string]$Process = "bf3"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

Start-Sleep -Milliseconds $StartDelayMs
for ($i = 0; $i -lt $Count; $i++) {
  $p = Get-Process $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
  if ($p) {
    $r = New-Object Win+RECT
    [void][Win]::GetClientRect($p.MainWindowHandle, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -gt 0 -and $h -gt 0) {
      $bmp = New-Object System.Drawing.Bitmap $w, $h
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      $hdc = $g.GetHdc()
      [void][Win]::PrintWindow($p.MainWindowHandle, $hdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
      $g.ReleaseHdc($hdc); $g.Dispose()
      $name = "shot_{0:D3}_{1}.png" -f $i, (Get-Date -Format "HHmmss.fff")
      $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
      $bmp.Dispose()
    }
  }
  Start-Sleep -Milliseconds $IntervalMs
}
