# Starts the editor and prints its load log (%LOCALAPPDATA%\TartarusEngine\Logs\Editor.log) in this console
# until the editor's window is up. The Release editor is a windowed app with no console of its own.
# Exit codes: 0 the window came up; 1 the editor closed before it did (the log above says why).
param([Parameter(Mandatory)][string]$Exe)

$log = Join-Path $env:LOCALAPPDATA 'TartarusEngine\Logs\Editor.log'
$started = Get-Date
$engine = Start-Process -FilePath $Exe -PassThru
$null = $engine.Handle # without this, Windows PowerShell never reports the ExitCode
$pos = 0L

# New lines since the last read. The engine moves the previous run's log aside and starts a new one, so
# only a file written since the launch counts, and a shorter file starts again from the top.
function Write-NewLines {
    try {
        $item = Get-Item -LiteralPath $log -ErrorAction Stop
        if ($item.LastWriteTime -lt $started) { return }
        $fs = New-Object IO.FileStream($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
        try {
            if ($fs.Length -lt $script:pos) { $script:pos = 0L }
            $null = $fs.Seek($script:pos, [IO.SeekOrigin]::Begin)
            $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
            $end = $text.LastIndexOf("`n")
            if ($end -lt 0) { return } # wait for a whole line
            $script:pos += [Text.Encoding]::UTF8.GetByteCount($text.Substring(0, $end + 1))
            foreach ($line in ($text.Substring(0, $end) -split "`r?`n")) {
                if ($line -match '^\[(error|fatal)') { Write-Host $line -ForegroundColor Red }
                elseif ($line -match '^\[warn') { Write-Host $line -ForegroundColor Yellow }
                else { Write-Host $line }
            }
        } finally { $fs.Dispose() }
    } catch {}
}

while ($true) {
    Write-NewLines
    $engine.Refresh()
    if ($engine.HasExited) {
        Write-NewLines
        Write-Host ''
        Write-Host "*** The editor closed before its window came up (exit code $($engine.ExitCode)). ***" -ForegroundColor Red
        exit 1
    }
    if ($engine.MainWindowHandle -ne [IntPtr]::Zero) { # its window is showing
        Write-NewLines
        exit 0
    }
    Start-Sleep -Milliseconds 150
}
