# The launch screen on a CRT: the same composition as launch-screen.ps1 (the art on the left; the
# lockup, status, progress bar and version on the right) drawn in a borderless WPF window, with a
# pixel shader (crt.fx / crt.ps) turning the whole window into a curved, scanlined tube that
# powers on, with its sounds. The build runs in the background the whole time. Any key
# (or a click) skips the intro; M mutes the sound, and is remembered.
#
#   launch-crt.ps1 [-Build]
#     Exit codes: 0 built (or no -Build); on a failed build the screen lists the errors and asks -
#     10 launch the previous build, 11 close. 99: this screen couldn't start (no WPF, no window);
#     nothing was built, so run-editor.cmd falls back to the console screen (launch-screen.ps1).
param([switch]$Build)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$buildLog = Join-Path $root 'build\last-build.log'
$progressFile = Join-Path $root 'build\launch-progress.txt'
$settingsFile = Join-Path $root 'build\launch-settings.txt'
$invariant = [Globalization.CultureInfo]::InvariantCulture

# --- everything that can fail before the build starts: a failure here means "use the console" ---
try {
    # The native side (CrtLaunch.cs) is compiled once and cached by a hash of its source: loading
    # the cached DLL takes milliseconds where compiling takes about half a second.
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml, System.Windows.Forms, System.Drawing
    $helperSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'CrtLaunch.cs'))
    $sha = [Security.Cryptography.SHA256]::Create()
    $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($helperSource + [Environment]::Version))).Replace('-', '').Substring(0, 12)
    $cacheDir = if (Test-Path (Join-Path $root 'build')) { Join-Path $root 'build\launcher-cache' } else { Join-Path $env:TEMP 'tartarus-launcher-cache' }
    $helperDll = Join-Path $cacheDir "CrtLaunch-$key.dll"
    $refs = @([System.Windows.Media.Visual].Assembly.Location, [System.Windows.DependencyObject].Assembly.Location,
              [System.Windows.Window].Assembly.Location, [System.Xaml.XamlType].Assembly.Location)
    $loaded = $false
    if (Test-Path $helperDll) { try { Add-Type -Path $helperDll; $loaded = $true } catch {} }
    if (-not $loaded) {
        try {
            New-Item -ItemType Directory -Force $cacheDir | Out-Null
            Get-ChildItem $cacheDir -Filter 'CrtLaunch-*.dll' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
            Add-Type -TypeDefinition $helperSource -ReferencedAssemblies $refs -OutputAssembly $helperDll -OutputType Library
            Add-Type -Path $helperDll
        } catch {
            Add-Type -TypeDefinition $helperSource -ReferencedAssemblies $refs   # an unwritable cache: compile in memory
        }
    }
    # Before any window exists: render at the display's real resolution, not a stretched bitmap.
    [void][CrtLaunchNative]::SetProcessDPIAware()

    # --- the text ------------------------------------------------------------------------------
    $sections = [CrtLaunchArt]::ReadSections((Join-Path $PSScriptRoot 'TartarusEngineAscii.txt'))
    $art = $sections['art']
    $banner = $sections['banner']

    $versionText = ''
    try {
        $cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
        if ($cmake -match 'project\(\s*\w+\s+VERSION\s+([\d.]+)') { $versionText = "v$($Matches[1])" }
    } catch {}
    $git = [CrtLaunchGit]::Describe([string]$root)
    if ($git) { $versionText = (@($versionText, $git) | Where-Object { $_ }) -join "   $([char]0x00B7)   " }

    # --- the window ------------------------------------------------------------------------------
    # The stage is laid out once at 1650 x 1100 (3:2) and scaled to the window by a Viewbox. The
    # text sits on Consolas cells 0.55 em wide and 10.5 units tall, the console's shape.
    $xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Tartarus Engine" WindowStyle="None" ResizeMode="NoResize" Background="Black"
        ShowInTaskbar="True" WindowStartupLocation="Manual" Cursor="None">
  <Window.Resources>
    <Style TargetType="TextBlock">
      <Setter Property="FontFamily" Value="Consolas"/>
      <Setter Property="FontSize" Value="9"/>
      <Setter Property="LineStackingStrategy" Value="BlockLineHeight"/>
      <Setter Property="LineHeight" Value="10.5"/>
      <Setter Property="TextOptions.TextFormattingMode" Value="Ideal"/>
    </Style>
  </Window.Resources>
  <Grid x:Name="Tube" Background="Black">
    <Viewbox Stretch="Uniform">
      <Grid x:Name="Stage" Width="1650" Height="1100" Background="Black" RenderTransformOrigin="0.5,0.5">
        <Grid.RenderTransform><ScaleTransform x:Name="Power" ScaleX="0.05" ScaleY="0.004"/></Grid.RenderTransform>
        <Grid x:Name="Glow">
          <Grid.Effect><DropShadowEffect ShadowDepth="0" BlurRadius="8" Color="#FFF4EA" Opacity="0.4" RenderingBias="Performance"/></Grid.Effect>
          <StackPanel Orientation="Horizontal" HorizontalAlignment="Center" VerticalAlignment="Center">
            <Grid>
              <TextBlock x:Name="Art" Opacity="0"/>
              <StackPanel x:Name="Errors" VerticalAlignment="Center" HorizontalAlignment="Center" Opacity="0">
                <TextBlock Text="B U I L D   F A I L E D" Foreground="#EB5A50" HorizontalAlignment="Center" FontSize="13" LineHeight="15"/>
                <TextBlock x:Name="ErrorList" Foreground="#EC8074" Margin="0,26,0,0" TextTrimming="CharacterEllipsis" FontSize="10.5" LineHeight="14" MaxWidth="600"/>
                <TextBlock Text="The previous build does not contain your latest changes." Foreground="#7C7C84"
                           HorizontalAlignment="Center" Margin="0,26,0,0" FontSize="10.5" LineHeight="13"/>
              </StackPanel>
            </Grid>
            <StackPanel Margin="91,0,0,0" VerticalAlignment="Center">
              <TextBlock x:Name="Banner"/>
              <TextBlock x:Name="Status" HorizontalAlignment="Center" Margin="0,31,0,0" Foreground="#8A8A92" Opacity="0" FontSize="12" LineHeight="14"/>
              <Grid Height="16" Margin="0,5,0,0">
                <Grid x:Name="Bar" Width="317" Height="2" VerticalAlignment="Center" Opacity="0">
                  <Rectangle Fill="#2C2C32"/>
                  <Rectangle x:Name="BarFill" Fill="#E4B868" HorizontalAlignment="Left" Width="0"/>
                </Grid>
                <StackPanel x:Name="Prompt" Orientation="Horizontal" HorizontalAlignment="Center" VerticalAlignment="Center" Opacity="0"
                            IsHitTestVisible="False">
                  <TextBlock x:Name="PromptYes" Text="[Y]  launch the previous build" Foreground="#C8C8CE" FontSize="11" LineHeight="13"
                             Background="Transparent" Padding="6,2"/>
                  <TextBlock x:Name="PromptNo" Text="[N]  close" Foreground="#C8C8CE" FontSize="11" LineHeight="13"
                             Background="Transparent" Padding="6,2" Margin="40,0,0,0"/>
                </StackPanel>
              </Grid>
              <TextBlock x:Name="Version" HorizontalAlignment="Center" Margin="0,16,0,0" Foreground="#6C6C76" Opacity="0" FontSize="11" LineHeight="13"/>
            </StackPanel>
          </StackPanel>
        </Grid>
        <TextBlock x:Name="SoundHint" HorizontalAlignment="Right" VerticalAlignment="Bottom" Margin="0,0,70,52"
                   Foreground="#6A6A72" FontSize="10" LineHeight="12" Opacity="0"/>
        <Rectangle x:Name="Flash" Fill="#F4F7FF" Opacity="0" IsHitTestVisible="False"/>
      </Grid>
    </Viewbox>
  </Grid>
</Window>
'@
    $window = [Windows.Markup.XamlReader]::Parse($xaml)
    $ui = @{}
    foreach ($n in 'Tube', 'Stage', 'Power', 'Glow', 'Art', 'Errors', 'ErrorList', 'Banner', 'Status', 'Bar', 'BarFill', 'Prompt', 'PromptYes', 'PromptNo', 'Version', 'SoundHint', 'Flash') {
        $ui[$n] = $window.FindName($n)
    }

    [CrtLaunchArt]::Fill($ui.Art, $art)

    # The lockup is painted by a gradient that is also its wipe: solid up to the edge, a gold
    # leading edge, clear beyond it.
    $ui.Banner.Text = $banner -join "`n"
    $wipe = New-Object Windows.Media.LinearGradientBrush
    # In the lockup's own coordinates: a relative brush would stretch over each line's length, and
    # the lines differ, so each would wipe at its own pace.
    $wipe.MappingMode = 'Absolute'
    $wipe.StartPoint = New-Object Windows.Point 0, 0; $wipe.EndPoint = New-Object Windows.Point 800, 0
    $ui.Banner.Add_SizeChanged({ $wipe.EndPoint = New-Object Windows.Point ([Math]::Max(1.0, $ui.Banner.ActualWidth)), 0 })
    $white = [Windows.Media.Color]::FromRgb(236, 236, 240); $gold = [Windows.Media.Color]::FromRgb(255, 214, 140)
    $clear = [Windows.Media.Color]::FromArgb(0, 255, 214, 140)
    $stops = @(
        (New-Object Windows.Media.GradientStop $white, 0.0), (New-Object Windows.Media.GradientStop $white, 0.0),
        (New-Object Windows.Media.GradientStop $gold, 0.0), (New-Object Windows.Media.GradientStop $clear, 0.0),
        (New-Object Windows.Media.GradientStop $clear, 1.0))
    foreach ($s in $stops) { $wipe.GradientStops.Add($s) }
    $ui.Banner.Foreground = $wipe
    function Set-Wipe([double]$p, [Windows.Media.Color]$body) {
        $edge = 0.06
        $stops[0].Color = $body; $stops[1].Color = $body
        $stops[1].Offset = [Math]::Max(0.0, [Math]::Min(1.0, $p - $edge))
        $stops[2].Offset = [Math]::Max(0.0, [Math]::Min(1.0, $p))
        $stops[3].Offset = [Math]::Max(0.0, [Math]::Min(1.0, $p + 0.0005))
        if ($p -ge 1.0 + $edge) { $stops[2].Color = $body } else { $stops[2].Color = $gold }
    }
    Set-Wipe 0.0 $white
    $ui.Version.Text = $versionText

    # The CRT. Without ps_3_0 hardware the screen still plays, just without the tube.
    $crt = $null
    try {
        if ([Windows.Media.RenderCapability]::IsPixelShaderVersionSupported(3, 0)) {
            $crt = New-Object CrtEffect (Join-Path $PSScriptRoot 'crt.ps')
            $ui.Tube.Effect = $crt
        }
    } catch { $crt = $null }

    # Sized to 3:2 at 80% of the height of the monitor under the mouse, centred on it.
    $screen = [Windows.Forms.Screen]::FromPoint([Windows.Forms.Cursor]::Position).WorkingArea
    $dpi = 1.0
    try { $g = [Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $dpi = $g.DpiY / 96.0; $g.Dispose() } catch {}
    $workW = $screen.Width / $dpi; $workH = $screen.Height / $dpi
    $h = $workH * 0.8; $w = $h * 1.5
    if ($w -gt $workW * 0.9) { $w = $workW * 0.9; $h = $w / 1.5 }
    $window.Width = $w; $window.Height = $h
    $window.Left = $screen.Left / $dpi + ($workW - $w) / 2
    $window.Top = $screen.Top / $dpi + ($workH - $h) / 2
    try {
        $window.Icon = [Windows.Media.Imaging.BitmapFrame]::Create((New-Object Uri (Join-Path $root 'extern\branding\tartarus_icon.ico')))
    } catch {}
    $taskbar = New-Object Windows.Shell.TaskbarItemInfo
    $window.TaskbarItemInfo = $taskbar

    # The tube's sounds (CrtLaunchSound: synthesized and mixed on its own thread). A missing sound
    # never stops the show.
    $muted = $false
    try { $muted = ([IO.File]::ReadAllText($settingsFile)) -match 'sound\s*=\s*off' } catch {}
    $sound = $null
    try { $sound = New-Object CrtLaunchSound; $sound.SetMuted($muted); $sound.Start() } catch { $sound = $null }
} catch {
    exit 99
}

# --- the build, started once the screen is ready to show it ---------------------------------------
$buildProc = $null
if ($Build) {
    $buildProc = Start-Process -FilePath 'cmd.exe' -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -ArgumentList '/c', 'cmake --build build --config Release --parallel > build\last-build.log 2>&1'
    $null = $buildProc.Handle # without this, Windows PowerShell never reports the ExitCode
}

$expectProjects = 0; $expectSeconds = 0.0
try {
    $p = ([IO.File]::ReadAllText($progressFile)).Trim() -split '\s+'
    $expectProjects = [int]$p[0]; $expectSeconds = [double]::Parse($p[1], $invariant)
} catch {}
function Read-BuildLog {
    try {
        $fs = New-Object IO.FileStream($buildLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
        try { return (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return '' }
}
function Count-Projects([string]$log) { ([regex]::Matches($log, '\.vcxproj -> ')).Count }
function Clamp01([double]$x) { [Math]::Max(0.0, [Math]::Min(1.0, $x)) }
function Ease-Out([double]$x) { $x = Clamp01 $x; 1.0 - [Math]::Pow(1.0 - $x, 3) }
function Ease-In([double]$x) { $x = Clamp01 $x; $x * $x * $x }
function Lerp-Color([Windows.Media.Color]$a, [Windows.Media.Color]$b, [double]$t) {
    [Windows.Media.Color]::FromRgb([byte]($a.R + ($b.R - $a.R) * $t), [byte]($a.G + ($b.G - $a.G) * $t), [byte]($a.B + ($b.B - $a.B) * $t))
}
$candle = [Windows.Media.Color]::FromRgb(255, 222, 170)

# --- the show: one clock, one state machine, run every frame --------------------------------------
# intro -> build -> finish | failed -> off (power-down) -> close
$S = @{
    Phase = 'intro'; Skip = 0.0; PhaseAt = 0.0; Shown = 0.0; Projects = 0; LastPoll = -1.0
    Flicker = 0.12; FlickerTarget = 0.12; NextFlicker = 0.0; ExitCode = 0; BuildSeconds = 0.0; Crash = $null
    Rand = (New-Object Random); Closing = $false; Started = $false
}
$clock = [Diagnostics.Stopwatch]::StartNew()
$introEnd = 2.7

function Enter-Phase([string]$name) {
    $S.Phase = $name; $S.PhaseAt = $clock.Elapsed.TotalSeconds + $S.Skip
}
function Set-SoundHint { $ui.SoundHint.Text = if ($muted) { 'M   sound on' } else { 'M   mute' } }
Set-SoundHint

function Show-Failure {
    $log = Read-BuildLog
    $prefix = [string]$root + '\'
    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($log -split "`r?`n")) {
        if ($l -notmatch '(?i)(\berror\b|CMake Error)') { continue }
        $l = ($l -replace '\s*\[[^\]]*\.vcxproj\]\s*$', '').Trim()
        $l = $l.Replace($prefix, '').Replace($prefix.Replace('\', '/'), '')
        if (-not $errors.Contains($l)) { $errors.Add($l) }
    }
    if ($errors.Count -eq 0) {
        foreach ($l in (($log -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -Last 8)) { $errors.Add($l.Trim()) }
    }
    $shown = @($errors | Select-Object -First 14)
    $text = ($shown | ForEach-Object { if ($_.Length -gt 104) { $_.Substring(0, 101) + '...' } else { $_ } }) -join "`n"
    if ($errors.Count -gt $shown.Count) { $text += "`n... and $($errors.Count - $shown.Count) more in build\last-build.log" }
    $ui.ErrorList.Text = $text
    $ui.Status.Text = 'B U I L D   F A I L E D'
    $ui.Status.Foreground = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(235, 90, 80))
    $taskbar.ProgressState = 'Error'; $taskbar.ProgressValue = 1.0
}

function Update-Frame {
    $now = $clock.Elapsed.TotalSeconds
    $t = $now + $S.Skip
    if ($crt) {
        $crt.Time = $now
        $src = [Windows.PresentationSource]::FromVisual($window)
        $scale = if ($src) { $src.CompositionTarget.TransformToDevice.M22 } else { 1.0 }
        $crt.Size = New-Object Windows.Point ($ui.Tube.ActualWidth * $scale), ($ui.Tube.ActualHeight * $scale)
    }
    $since = $t - $S.PhaseAt
    # The whine and hum rise with the picture, and fade out as the window closes.
    if ($sound) {
        $sound.SetHum($(switch ($S.Phase) { 'intro' { Clamp01 (($t - 0.25) / 1.1) } 'close' { 1.0 - (Clamp01 ($since / 0.15)) } default { 1.0 } }))
    }

    switch ($S.Phase) {
        'intro' {
            # Power on: a bright line draws out across the tube, then opens into the picture.
            $ui.Power.ScaleX = 0.05 + 0.95 * (Ease-Out ($t / 0.14))
            $ui.Power.ScaleY = 0.004 + 0.996 * (Ease-Out (($t - 0.16) / 0.34))
            $ui.Flash.Opacity = if ($t -lt 0.16) { 1.0 } else { 0.9 * (1.0 - (Clamp01 (($t - 0.16) / 0.45))) }
            $ui.Art.Opacity = Ease-Out (($t - 0.5) / 1.0)
            Set-Wipe (1.08 * (Clamp01 (($t - 1.7) / 0.75))) $white
            $late = Clamp01 (($t - 2.35) / 0.35)
            $ui.Version.Opacity = $late; $ui.Status.Opacity = $late; $ui.Bar.Opacity = $late
            if ($sound) { $ui.SoundHint.Opacity = $late }
            if ($buildProc) { $ui.Status.Text = 'B U I L D I N G' }
            if ($t -ge $introEnd) { Enter-Phase 'build' }
        }
        'build' {
            if ($buildProc -and -not $buildProc.HasExited) {
                if ($t - $S.LastPoll -ge 0.3) { $S.Projects = Count-Projects (Read-BuildLog); $S.LastPoll = $t }
                $byProjects = if ($expectProjects -gt 0) { $S.Projects / [double]$expectProjects } else { 0.0 }
                $byTime = if ($expectSeconds -gt 0) { 0.9 * $now / $expectSeconds } else { 0.9 * (1 - [Math]::Exp(-$now / 90)) }
                $target = [Math]::Min(0.97, [Math]::Max($byProjects, $byTime))
                if ($target -gt $S.Shown) { $S.Shown += ($target - $S.Shown) * 0.08 }
                $ui.BarFill.Width = 317 * $S.Shown
                $taskbar.ProgressState = 'Normal'; $taskbar.ProgressValue = $S.Shown

                # Candlelight: the lockup's warmth wanders, eased, toward a new random level.
                if ($t -ge $S.NextFlicker) { $S.FlickerTarget = 0.05 + $S.Rand.NextDouble() * 0.4; $S.NextFlicker = $t + 0.25 + $S.Rand.NextDouble() * 0.3 }
                $S.Flicker += ($S.FlickerTarget - $S.Flicker) * 0.06
                Set-Wipe 1.2 (Lerp-Color $white $candle $S.Flicker)
                $ui.Status.Text = 'B U I L D I N G' + (' .' * ([int]($t * 2.5) % 4))
                return
            }
            if ($buildProc) {
                $buildProc.WaitForExit()
                try { $S.BuildSeconds = ($buildProc.ExitTime - $buildProc.StartTime).TotalSeconds } catch {}
                if ($buildProc.ExitCode -ne 0) {
                    Show-Failure; Enter-Phase 'failed'
                    # The choice can be clicked too, so it works even if the keyboard is elsewhere.
                    $window.Cursor = [Windows.Input.Cursors]::Arrow; $ui.Prompt.IsHitTestVisible = $true
                    [CrtLaunchNative]::TakeForeground((New-Object Windows.Interop.WindowInteropHelper $window).Handle)
                    return
                }
            }
            Enter-Phase 'finish'
        }
        'finish' {
            $S.Shown += (1.0 - $S.Shown) * 0.2
            $ui.BarFill.Width = 317 * $S.Shown
            Set-Wipe 1.2 (Lerp-Color $white $candle ($S.Flicker * (1 - (Clamp01 ($since / 0.4)))))
            if ($buildProc) { $taskbar.ProgressValue = $S.Shown }
            if ($ui.Status.Tag -ne 'ready') {
                $ui.Status.Tag = 'ready'
                $ui.Status.Inlines.Clear()
                $ready = New-Object Windows.Documents.Run 'R E A D Y'
                $ready.Foreground = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(120, 220, 140))
                $ui.Status.Inlines.Add($ready)
                if ($S.BuildSeconds -gt 0) {
                    $b = $S.BuildSeconds
                    $took = if ($b -ge 60) { '{0} min {1} s' -f [int][Math]::Floor($b / 60), [int]($b % 60) } else { $b.ToString('0.0', $invariant) + ' s' }
                    $ui.Status.Inlines.Add((New-Object Windows.Documents.Run "     built in $took"))
                }
                if ($buildProc) {
                    $projects = Count-Projects (Read-BuildLog)
                    if ($projects -gt 0 -and ($S.BuildSeconds -gt 8 -or $expectProjects -eq 0)) {
                        try { [IO.File]::WriteAllText($progressFile, "$projects $($S.BuildSeconds.ToString('0.0', $invariant))") } catch {}
                    }
                }
            }
            if ($since -ge 1.1) { $taskbar.ProgressState = 'None'; $S.ExitCode = 0; Enter-Phase 'close' }
        }
        'failed' {
            $fade = Ease-Out ($since / 0.45)
            $ui.Art.Opacity = 1.0 - $fade
            $ui.Errors.Opacity = Ease-Out (($since - 0.3) / 0.4)
            $ui.Bar.Opacity = 1.0 - $fade
            $ui.Prompt.Opacity = Ease-Out (($since - 0.3) / 0.4)
            Set-Wipe 1.2 $white
        }
        'close' {
            # No power-off: the picture stays as it is while the hum fades (so it doesn't click
            # off), then the window closes.
            if ($since -ge 0.15 -and -not $S.Closing) { $S.Closing = $true; $window.Close() }
        }
    }
}

$window.Add_KeyDown({
    param($sender, $e)
    if ($e.Key -eq 'M' -and $sound) {
        $script:muted = -not $muted
        Set-SoundHint
        $sound.SetMuted($muted)
        try { [IO.File]::WriteAllText($settingsFile, $(if ($muted) { "sound=off`r`n" } else { "sound=on`r`n" })) } catch {}
        return
    }
    if ($S.Phase -eq 'intro') {
        $S.Skip = $introEnd - $clock.Elapsed.TotalSeconds     # jump straight to the finished frame
    } elseif ($S.Phase -eq 'failed') {
        if ($e.Key -eq 'Y') { Choose 10 }
        elseif ($e.Key -eq 'N' -or $e.Key -eq 'Escape') { Choose 11 }
    }
})
function Choose([int]$code) { if ($S.Phase -eq 'failed') { $S.ExitCode = $code; Enter-Phase 'close' } }
$hover = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(255, 214, 140))
$plain = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(200, 200, 206))
foreach ($b in $ui.PromptYes, $ui.PromptNo) {
    $b.Cursor = [Windows.Input.Cursors]::Hand
    $b.Add_MouseEnter({ param($sender) $sender.Foreground = $hover })
    $b.Add_MouseLeave({ param($sender) $sender.Foreground = $plain })
}
$ui.PromptYes.Add_MouseLeftButtonUp({ Choose 10 })
$ui.PromptNo.Add_MouseLeftButtonUp({ Choose 11 })
# A click during the intro skips it, like a key.
$window.Add_MouseLeftButtonDown({ if ($S.Phase -eq 'intro') { $S.Skip = $introEnd - $clock.Elapsed.TotalSeconds } })
$onFrame = [EventHandler] {
    if ($S.Closing) { return }
    # The show's clock starts with the first frame on screen, so none of the power-on is lost to
    # the window's own start-up.
    if (-not $S.Started) {
        $S.Started = $true; $clock.Restart()
        if ($sound) { $sound.PowerOnSound() }
    }
    try { Update-Frame } catch { $S.Crash = $_; $S.Closing = $true; $window.Close() }
}
$window.Add_ContentRendered({
    [CrtLaunchNative]::TakeForeground((New-Object Windows.Interop.WindowInteropHelper $window).Handle)
    $window.Activate() | Out-Null
    [CrtLaunchNative]::HideConsole()
})
[Windows.Media.CompositionTarget]::add_Rendering($onFrame)
try { [void]$window.ShowDialog() } catch { $S.Crash = $_ }
[Windows.Media.CompositionTarget]::remove_Rendering($onFrame)
if ($sound) { $sound.Dispose() }

# The screen broke mid-show: still report the build honestly (run-editor.cmd prints the log).
if ($S.Crash) {
    if ($buildProc) { $buildProc.WaitForExit(); if ($buildProc.ExitCode -ne 0) { exit 1 } }
    exit 0
}
exit $S.ExitCode
