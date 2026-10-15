# Drags the editor window's bottom-right corner with synthetic mouse input and measures how long
# the window keeps resizing after the button is released (the "resize runs on by itself" bug).
#
#   powershell -ExecutionPolicy Bypass -File tools\probes\window-resize-drag.ps1 -Exe build\Release\TartarusEngine.exe
#
# Runs the editor twice, see-through window on (TARTARUS_SEE_THROUGH=1) and off (=0), unless
# -SeeThrough picks one. It moves the real mouse, so nobody should be using the PC while it runs.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [ValidateSet('both', 'on', 'off')][string]$SeeThrough = 'both',
    [int]$Steps = 300,          # mouse steps in the drag
    [int]$StepPx = 2,           # pixels per step, on each axis
    [int]$LoadTimeoutSec = 120
)
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class DragProbe {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy, mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public int type; public MOUSEINPUT mi; public int pad0, pad1; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
    static void Mouse(int flags, int dx, int dy) {
        var i = new INPUT[1];
        i[0].type = 0; i[0].mi.dwFlags = flags; i[0].mi.dx = dx; i[0].mi.dy = dy;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Down() { Mouse(0x0002, 0, 0); }
    public static void Up()   { Mouse(0x0004, 0, 0); }
    public static void Move(int dx, int dy) { Mouse(0x0001, dx, dy); } // relative
}
"@

function Run-Probe([string]$mode) {
    $env:TARTARUS_SEE_THROUGH = if ($mode -eq 'on') { '1' } else { '0' }
    $p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent (Resolve-Path $Exe)) -PassThru
    try {
        $deadline = (Get-Date).AddSeconds($LoadTimeoutSec)
        do {
            Start-Sleep -Milliseconds 250
            $p.Refresh()
            $h = $p.MainWindowHandle
        } while (($h -eq [IntPtr]::Zero -or -not [DragProbe]::IsWindowVisible($h)) -and (Get-Date) -lt $deadline -and -not $p.HasExited)
        if ($h -eq [IntPtr]::Zero) { throw "editor window never appeared" }
        Start-Sleep -Seconds 3                            # let the first frames settle
        [void][DragProbe]::ShowWindow($h, 9)              # SW_RESTORE (not maximized)
        [void][DragProbe]::SetWindowPos($h, [IntPtr]::Zero, 100, 100, 1280, 800, 0x0004) # SWP_NOZORDER
        [void][DragProbe]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 800

        $r = New-Object DragProbe+RECT
        [void][DragProbe]::GetWindowRect($h, [ref]$r)
        $startW = $r.R - $r.L; $startH = $r.B - $r.T
        [void][DragProbe]::SetCursorPos($r.R - 3, $r.B - 3)
        Start-Sleep -Milliseconds 300
        [DragProbe]::Down()
        $sw = [Diagnostics.Stopwatch]::StartNew()
        for ($i = 0; $i -lt $Steps; $i++) {
            [DragProbe]::Move($StepPx, $StepPx)
            Start-Sleep -Milliseconds 1                   # ~1 kHz-ish steps, like a gaming mouse
        }
        $dragMs = $sw.ElapsedMilliseconds
        [DragProbe]::Up()
        $release = $sw.ElapsedMilliseconds

        # Watch the window for 4 s after the release; note when its size last changed.
        [void][DragProbe]::GetWindowRect($h, [ref]$r)
        $atRelease = "{0}x{1}" -f ($r.R - $r.L), ($r.B - $r.T)
        $last = "$atRelease"; $lastChange = $release
        while ($sw.ElapsedMilliseconds - $release -lt 4000) {
            [void][DragProbe]::GetWindowRect($h, [ref]$r)
            $now = "{0}x{1}" -f ($r.R - $r.L), ($r.B - $r.T)
            if ($now -ne $last) { $last = $now; $lastChange = $sw.ElapsedMilliseconds }
            Start-Sleep -Milliseconds 5
        }
        $expect = "{0}x{1}" -f ($startW + $Steps * $StepPx), ($startH + $Steps * $StepPx)
        [pscustomobject]@{
            SeeThrough      = $mode
            DragMs          = $dragMs
            SizeAtRelease   = $atRelease
            FinalSize       = $last
            ExpectedSize    = $expect
            RunOnAfterUpMs  = $lastChange - $release
        }
    } finally {
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
        Remove-Item Env:TARTARUS_SEE_THROUGH -ErrorAction SilentlyContinue
    }
}

$modes = if ($SeeThrough -eq 'both') { @('on', 'off') } else { @($SeeThrough) }
$modes | ForEach-Object { Run-Probe $_ } | Format-Table -AutoSize
