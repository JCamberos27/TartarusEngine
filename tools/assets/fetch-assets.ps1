# Copies the third-party assets that are not in git from the team's shared Google Drive into this
# checkout, then checks the checkout against project/external_assets.csv.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "<...>\Tartarus Assets"
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -VerifyOnly
#
# -Source is the shared "Tartarus Assets" folder (or its Used folder) as Google Drive for desktop
# shows it, e.g. "G:\My Drive\Tartarus Assets" or a "Shared with me" shortcut. Used\ mirrors this
# repo, so files land at the same project paths their tracked .meta files sit at.
# Only the files this repo's external_assets.csv lists are copied, and only when missing or a different
# size: anything else on the Drive (a script, a stale .meta or prefab) is reported, never copied over the
# checkout's own - those live in git. Nothing in the checkout is deleted.
# Exit codes: 0 every listed file present; 1 files missing or a different size; 2 bad arguments.
param([string]$Source, [switch]$VerifyOnly)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$manifest = Join-Path $root 'project\external_assets.csv'
$rows = Import-Csv $manifest

if (-not $VerifyOnly) {
    if (-not $Source) { Write-Host 'Pass -Source "<path to the shared Tartarus Assets folder>" (or -VerifyOnly).'; exit 2 }
    $used = if (Test-Path (Join-Path $Source 'Used\project')) { Join-Path $Source 'Used' } else { $Source }
    $from = Join-Path $used 'project'
    if (-not (Test-Path $from)) { Write-Host "No project folder under $used - is that the Tartarus Assets folder?"; exit 2 }
    Write-Host "Copying the listed assets from $from -> $root\project ..."
    $copied = 0; $absent = 0
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($r in $rows) {
        $rel = $r.path -replace '/', '\'
        [void]$listed.Add($rel)
        $src = Join-Path $from $rel
        $dst = Join-Path (Join-Path $root 'project') $rel
        $s = Get-Item -LiteralPath $src -ErrorAction SilentlyContinue
        if (-not $s) { $absent++; continue }
        $d = Get-Item -LiteralPath $dst -ErrorAction SilentlyContinue
        if ($d -and $d.Length -eq $s.Length) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $copied++
    }
    Write-Host "$copied copied, $($rows.Count - $copied - $absent) already present, $absent not on the Drive."
    # What else is on the Drive: reported so someone moves it off, never copied.
    $extra = Get-ChildItem -LiteralPath $from -Recurse -File -Force |
        ForEach-Object { $_.FullName.Substring($from.Length + 1) } |
        Where-Object { $_ -ne 'external_assets.csv' -and $_ -notmatch '(^|\\)desktop\.ini$' -and -not $listed.Contains($_) }
    if ($extra) {
        $extra | Select-Object -First 10 | ForEach-Object { Write-Host "not copied (not a listed asset)  $_" }
        Write-Host "$(@($extra).Count) file(s) on the Drive are not listed assets and were not copied; they belong in git or off the Drive."
    }
}

$missing = New-Object System.Collections.Generic.List[string]
$wrong = New-Object System.Collections.Generic.List[string]
foreach ($r in $rows) {
    $p = Join-Path (Join-Path $root 'project') ($r.path -replace '/', '\')
    $f = Get-Item -LiteralPath $p -ErrorAction SilentlyContinue
    if (-not $f) { $missing.Add($r.path) }
    elseif ($f.Length -ne [int64]$r.size) { $wrong.Add($r.path) }
}
$missing | Select-Object -First 20 | ForEach-Object { Write-Host "missing    $_" }
$wrong | Select-Object -First 20 | ForEach-Object { Write-Host "different  $_" }
Write-Host "$($rows.Count) external asset files listed: $($missing.Count) missing, $($wrong.Count) a different size."
if ($missing.Count -or $wrong.Count) { exit 1 }
Write-Host 'All external assets present.'
exit 0
