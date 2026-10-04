# Capturing the game window from outside it

Two PowerShell helpers for verifying a rendering change **without asking anyone to
look at the screen**. Both capture with `PrintWindow(hWnd, hdc, PW_RENDERFULLCONTENT)`,
which reads the window's own contents even while it is behind another window, so a
run can be screenshotted on a timer while you keep working.

They capture the game window and nothing else — not the desktop.

```
# one shot
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hh_shot.ps1 \
    -Out C:\Users\Public\hh.png

# a burst: waits for the window to appear, then N shots
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hh_burst.ps1 \
    -Dir C:\Users\Public\hhshots -Count 30 -IntervalMs 350
```

Start `hh_burst.ps1` first and launch the game second; it polls for the window for
`-WaitSec` seconds, and each file is named with the millisecond it was taken at.

Both match the window by a title SUBSTRING, defaulting to `Hybrid Heaven:
Recompiled` -- the bare "Hybrid Heaven" once matched an unrelated window.
`hh_shot.ps1 -Screen` copies the screen area instead of using PrintWindow; it
captures anything lying over the game, so do not use it on someone else's desktop.

Three things that are not obvious:

- **`SetProcessDPIAware` is required.** PowerShell is DPI-unaware by default, so
  `GetWindowRect` returns the *scaled* rect and the capture comes out cropped —
  1356x817 of a 1920x1080 window at 141%.
- **This works for the WSLg window too.** A Linux build running under WSLg is an
  ordinary Windows window as far as `PrintWindow` is concerned; its title carries a
  `(Ubuntu)` suffix, which is also how you tell the two builds apart.
- **Running the scripts straight off the WSL UNC path works** — unlike anything
  `npm` executes. `-File \\wsl.localhost\...\hh_shot.ps1` is fine.

Composing them with `magick montage` into a contact sheet is what makes a boot
sequence readable in one look.
