param([string]$Dir = "C:\Users\Public\hhshots", [string]$Match = "Hybrid Heaven: Recompiled",
      [int]$Count = 40, [int]$IntervalMs = 500, [int]$WaitSec = 120)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HHWin2 {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][HHWin2]::SetProcessDPIAware()

New-Item -ItemType Directory -Force -Path $Dir | Out-Null
Get-ChildItem -Path $Dir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

# Wait for the window to appear.
$p = $null
$deadline = (Get-Date).AddSeconds($WaitSec)
while ($null -eq $p -and (Get-Date) -lt $deadline) {
    $p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$Match*" } | Select-Object -First 1
    if ($null -eq $p) { Start-Sleep -Milliseconds 200 }
}
if ($null -eq $p) { Write-Output "NOWINDOW"; exit 1 }
Write-Output ("window {0} '{1}'" -f $p.Id, $p.MainWindowTitle)

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
    $r = New-Object HHWin2+RECT
    [void][HHWin2]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -gt 0 -and $h -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][HHWin2]::PrintWindow($p.MainWindowHandle, $hdc, 2)
        $g.ReleaseHdc($hdc)
        $name = "{0}\shot_{1:d3}_{2:d5}ms.png" -f $Dir, $i, [int]$sw.ElapsedMilliseconds
        $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
    }
    Start-Sleep -Milliseconds $IntervalMs
}
Write-Output ("done {0} shots over {1} ms" -f $Count, $sw.ElapsedMilliseconds)
