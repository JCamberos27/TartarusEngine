# Copies the third-party assets that are not in git from the team's shared Google Drive store into this
# checkout. Every external file is tracked by a "payload" ({sha256, size}) in its own .meta; the Drive's
# Store folder holds the files as Store\<sha[0:2]>\<sha256><lowercase ext>.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "<...>\Tartarus Assets"
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -VerifyOnly
#
# -Source is the shared "Tartarus Assets" folder as Google Drive for desktop shows it, e.g.
# "G:\My Drive\Tartarus Assets" or a "Shared with me" shortcut; its Store subfolder is used (a path that
# already is the Store folder works too).
# Walks every .meta under project\ (not Library). A payload file that is missing is copied from the store;
# one that is present with a different size is reported "different (local edit?)" and left alone unless -Force.
# Nothing in the checkout is deleted.
# Exit codes: 0 every payload present; 1 files still missing; 2 bad arguments.
param([string]$Source, [switch]$VerifyOnly, [switch]$Force)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$project = Join-Path $root 'project'

$store = $null
if (-not $VerifyOnly) {
    if (-not $Source) { Write-Host 'Pass -Source "<path to the shared Tartarus Assets folder>" (or -VerifyOnly).'; exit 2 }
    $store = if (Test-Path -LiteralPath (Join-Path $Source 'Store')) { Join-Path $Source 'Store' } else { $Source }
    if (-not (Test-Path -LiteralPath $store)) { Write-Host "No Store folder under $Source - is that the Tartarus Assets folder?"; exit 2 }
    Write-Host "Fetching missing payloads from $store -> $project ..."
}

$fetched = 0; $present = 0
$missing = New-Object System.Collections.Generic.List[string]
$different = New-Object System.Collections.Generic.List[string]
$libDir = Join-Path $project 'Library'
foreach ($m in Get-ChildItem -LiteralPath $project -Recurse -File -Filter '*.meta') {
    if ($m.FullName.StartsWith($libDir + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
    $text = [System.IO.File]::ReadAllText($m.FullName)
    if ($text.IndexOf('"payload"') -lt 0) { continue }
    try { $pl = ($text | ConvertFrom-Json).payload } catch { continue }
    if (-not $pl -or -not $pl.sha256) { continue }
    $target = $m.FullName.Substring(0, $m.FullName.Length - 5)
    $rel = $target.Substring($project.Length + 1)
    $f = Get-Item -LiteralPath $target -ErrorAction SilentlyContinue
    if ($f) {
        if ($f.Length -eq [int64]$pl.size) { $present++; continue }
        if (-not $Force) { $different.Add($rel); continue }
    }
    if ($VerifyOnly) { $missing.Add($rel); continue }
    $ext = [System.IO.Path]::GetExtension($target).ToLowerInvariant()
    $sha = ([string]$pl.sha256).ToLowerInvariant()
    $src = Join-Path (Join-Path $store $sha.Substring(0, 2)) ($sha + $ext)
    if (-not (Test-Path -LiteralPath $src)) { $missing.Add($rel); continue }
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $src -Destination $target -Force
    $fetched++
}
$missing | Select-Object -First 20 | ForEach-Object { Write-Host "missing    $_" }
$different | Select-Object -First 20 | ForEach-Object { Write-Host "different (local edit?)  $_" }
Write-Host "$fetched fetched, $present already present, $($missing.Count) missing, $($different.Count) different (local edit?)."
if ($missing.Count) { exit 1 }
Write-Host 'All external assets present.'
exit 0
