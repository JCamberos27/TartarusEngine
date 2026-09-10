# Plays an optional studio intro fullscreen, then launches Tartarus Engine (Release build).
# Point a desktop shortcut at this instead of the exe to get the intro.
#
# Paths resolve relative to this script's location in the repo (tools/), so a clone anywhere
# works. Set TARTARUS_INTRO_VIDEO to an .mp4 to play an intro; with it unset or the file
# missing, the script skips straight to launching the engine.

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $PSScriptRoot
$videoPath  = $env:TARTARUS_INTRO_VIDEO
$engineDir  = Join-Path $repoRoot "build\Release"
$engineExe  = Join-Path $engineDir "TartarusEngine.exe"

if ($videoPath -and (Test-Path $videoPath)) {
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase

    $media = New-Object System.Windows.Controls.MediaElement
    $media.Source = New-Object System.Uri($videoPath)
    $media.LoadedBehavior = [System.Windows.Controls.MediaState]::Manual
    $media.UnloadedBehavior = [System.Windows.Controls.MediaState]::Manual
    $media.Stretch = [System.Windows.Media.Stretch]::Uniform
    $media.Volume = 1.0

    $window = New-Object System.Windows.Window
    $window.WindowStyle = [System.Windows.WindowStyle]::None
    $window.WindowState = [System.Windows.WindowState]::Maximized
    $window.ResizeMode = [System.Windows.ResizeMode]::NoResize
    $window.Background = [System.Windows.Media.Brushes]::Black
    $window.Topmost = $true
    $window.Content = $media

    # Any of these end the intro early: playback finishing, a decode failure (unsupported
    # codec, missing file, etc. — never block the engine launch on that), Escape, or a click.
    $media.Add_MediaEnded({ $window.Close() })
    $media.Add_MediaFailed({ $window.Close() })
    $window.Add_KeyDown({
        param($s, $e)
        if ($e.Key -eq [System.Windows.Input.Key]::Escape) { $window.Close() }
    })
    $window.Add_MouseLeftButtonDown({ $window.Close() })
    $window.Add_ContentRendered({ $media.Play() })

    $window.ShowDialog() | Out-Null
}

Start-Process -FilePath $engineExe -WorkingDirectory $engineDir
