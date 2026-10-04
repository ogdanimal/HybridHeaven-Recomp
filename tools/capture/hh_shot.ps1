# PrintWindow is the default and captures only this window's own content, even
# when it is covered. -Screen copies the SCREEN area instead, which captures
# whatever is lying over the window -- other people's windows included -- so it
# is opt-in and should almost never be used. (It used to be the default: a run
# without -Print grabbed the user's desktop.)
param([string]$Out = "C:\Users\Public\hh_shot.png", [string]$Match = "Hybrid Heaven: Recompiled", [switch]$Raise, [switch]$Screen)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HHWin {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@

[void][HHWin]::SetProcessDPIAware()

$p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$Match*" } | Select-Object -First 1
if ($null -eq $p) { Write-Output "NOWINDOW"; exit 1 }

if ($Raise) { [void][HHWin]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 700 }

$r = New-Object HHWin+RECT
[void][HHWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { Write-Output "BADRECT $w x $h"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
if (-not $Screen) {
    $hdc = $g.GetHdc()
    [void][HHWin]::PrintWindow($p.MainWindowHandle, $hdc, 2)
    $g.ReleaseHdc($hdc)
}
else {
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output ("OK {0} '{1}' {2}x{3} at {4},{5}" -f $p.Id, $p.MainWindowTitle, $w, $h, $r.Left, $r.Top)
