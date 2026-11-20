# Copies the third-party assets that are not in git from the team's shared Google Drive into this
# checkout, then checks the checkout against project/external_assets.csv.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "<...>\Tartarus Assets"
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -VerifyOnly
#   add -SizeOnly to either for the old quick check (missing or a different size only, no hashing)
#
# -Source is the shared "Tartarus Assets" folder (or its Used folder) as Google Drive for desktop
# shows it, e.g. "G:\My Drive\Tartarus Assets" or a "Shared with me" shortcut. Used\ mirrors this
# repo, so files land at the same project paths their tracked .meta files sit at.
# Only the files this repo's external_assets.csv lists are copied: those missing, a different size, or
# the same size with a different sha256 (a re-cooked 16-bit wav keeps its size but not its content).
# A copy lands only when the Drive file's sha256 matches the CSV; a Drive file that doesn't (not yet
# re-exported) is reported and the checkout's file is left alone. Anything else on the Drive (a script, a
# stale .meta or prefab) is reported, never copied over the checkout's own - those live in git. Nothing
# in the checkout is deleted.
# Hashing ~16 GB takes a few minutes the first time, so each checkout file's sha256 is cached by size + last-write time in
# the checkout's git dir (tartarus-asset-hashes.csv, per worktree); later runs only hash files that changed. -SizeOnly skips hashing.
# Exit codes: 0 every listed file present and matching; 1 files missing or different; 2 bad arguments.
param([string]$Source, [switch]$VerifyOnly, [switch]$SizeOnly)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$project = Join-Path $root 'project'
$manifest = Join-Path $project 'external_assets.csv'
$rows = @(Import-Csv $manifest)  # @(): a one-row CSV is still a list (.Count)

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '').ToLowerInvariant() }
    finally { $fs.Dispose(); $sha.Dispose() }
}

# Checkout hash cache: rel path -> size|ticks|sha256. Kept in the git dir so it is per checkout and never committed.
# A worktree's .git is a file: ask git for its own git dir so it gets a cache too.
$cacheFile = if (Test-Path -LiteralPath (Join-Path $root '.git') -PathType Container) { Join-Path $root '.git\tartarus-asset-hashes.csv' } else { $null }
if (-not $cacheFile -and (Test-Path -LiteralPath (Join-Path $root '.git') -PathType Leaf)) { try {
    $gp = & git -C $root rev-parse --git-path tartarus-asset-hashes.csv 2>$null
    if ($LASTEXITCODE -eq 0 -and $gp) {
        $cacheFile = if ([System.IO.Path]::IsPathRooted($gp)) { $gp } else { Join-Path $root $gp }
    }
} catch { $cacheFile = $null } }
# Drive clutter that is never a listed asset; keep in step with EXCLUDE_SUFFIXES in asset_manifest.py.
$clutterSuffixes = @('.bak', '.recovery.json', 'desktop.ini', 'Thumbs.db', '.fetchpart')
function Test-Clutter([string]$rel) {
    foreach ($s in $script:clutterSuffixes) { if ($rel.EndsWith($s, [StringComparison]::OrdinalIgnoreCase)) { return $true } }
    return $false
}
$cache = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
$cacheDirty = $false
if ($cacheFile -and (Test-Path -LiteralPath $cacheFile)) {
    foreach ($line in [System.IO.File]::ReadAllLines($cacheFile)) {
        $i = $line.IndexOf(',')
        if ($i -gt 0) { $cache[$line.Substring(0, $i)] = $line.Substring($i + 1) }
    }
}
$hashed = 0
function Get-LocalSha([string]$rel, [System.IO.FileInfo]$f) {
    $key = "$($f.Length)|$($f.LastWriteTimeUtc.Ticks)|"
    $hit = $null
    if ($script:cache.TryGetValue($rel, [ref]$hit) -and $hit.StartsWith($key)) { return $hit.Substring($key.Length) }
    $h = Get-Sha256 $f.FullName
    $script:cache[$rel] = $key + $h
    $script:cacheDirty = $true
    $script:hashed++
    return $h
}
function Save-Cache {
    if (-not $script:cacheFile -or -not $script:cacheDirty) { return }
    $lines = foreach ($k in $script:cache.Keys) { "$k,$($script:cache[$k])" }
    [System.IO.File]::WriteAllLines($script:cacheFile, [string[]]@($lines))
    $script:cacheDirty = $false
}

if (-not $VerifyOnly) {
    if (-not $Source) { Write-Host 'Pass -Source "<path to the shared Tartarus Assets folder>" (or -VerifyOnly).'; exit 2 }
    $used = if (Test-Path (Join-Path $Source 'Used\project')) { Join-Path $Source 'Used' } else { $Source }
    $from = Join-Path $used 'project'
    if (-not (Test-Path $from)) { Write-Host "No project folder under $used - is that the Tartarus Assets folder?"; exit 2 }
    Write-Host "Copying the listed assets from $from -> $project ..."
    $added = 0; $updated = 0; $current = 0; $absent = 0
    $driveStale = New-Object System.Collections.Generic.List[string]
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    # try/finally: a copy that throws mid-run still keeps the hashes worked out so far.
    try {
    foreach ($r in $rows) {
        $rel = $r.path -replace '/', '\'
        [void]$listed.Add($rel)
        $src = Join-Path $from $rel
        $dst = Join-Path $project $rel
        $d = Get-Item -LiteralPath $dst -ErrorAction SilentlyContinue
        if ($d -and $d.Length -eq [int64]$r.size -and ($SizeOnly -or (Get-LocalSha $rel $d) -eq $r.sha256)) { $current++; continue }
        $s = Get-Item -LiteralPath $src -ErrorAction SilentlyContinue
        if (-not $s) { $absent++; continue }
        if ($s.Length -ne [int64]$r.size) { $driveStale.Add($r.path); continue }
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        # Copy beside the file, check the copy's hash, then swap it in: the Drive file is read once.
        $part = "$dst.fetchpart"
        try {
            Copy-Item -LiteralPath $src -Destination $part -Force
            $p = Get-Item -LiteralPath $part
            $h = Get-Sha256 $part
            if ($h -ne $r.sha256) { $driveStale.Add($r.path); continue }
            Move-Item -LiteralPath $part -Destination $dst -Force
        } finally {
            if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
        }
        $cache[$rel] = "$($p.Length)|$($p.LastWriteTimeUtc.Ticks)|$h"; $cacheDirty = $true
        if ($d) { $updated++ } else { $added++ }
    }
    } finally { Save-Cache }
    Write-Host "$added copied (missing), $updated updated (different size or sha256), $current already current, $absent not on the Drive."
    if ($driveStale.Count) {
        $driveStale | Select-Object -First 10 | ForEach-Object { Write-Host "not copied (Drive copy doesn't match the CSV)  $_" }
        Write-Host "$($driveStale.Count) file(s) on the Drive don't match external_assets.csv (not re-exported yet?) and were not copied."
    }
    # What else is on the Drive: reported so someone moves it off, never copied.
    # -Name gives paths relative to $from: FullName can spell the root differently (8.3 short names like RUNNER~1).
    $extra = Get-ChildItem -LiteralPath $from -Recurse -File -Force -Name |
        Where-Object { $_ -ne 'external_assets.csv' -and -not (Test-Clutter $_) -and -not $listed.Contains($_) }
    if ($extra) {
        $extra | Select-Object -First 10 | ForEach-Object { Write-Host "not copied (not a listed asset)  $_" }
        Write-Host "$(@($extra).Count) file(s) on the Drive are not listed assets and were not copied; they belong in git or off the Drive."
    }
}

$missing = New-Object System.Collections.Generic.List[string]
$wrong = New-Object System.Collections.Generic.List[string]
$changed = New-Object System.Collections.Generic.List[string]
foreach ($r in $rows) {
    $rel = $r.path -replace '/', '\'
    $f = Get-Item -LiteralPath (Join-Path $project $rel) -ErrorAction SilentlyContinue
    if (-not $f) { $missing.Add($r.path) }
    elseif ($f.Length -ne [int64]$r.size) { $wrong.Add($r.path) }
    elseif (-not $SizeOnly -and (Get-LocalSha $rel $f) -ne $r.sha256) { $changed.Add($r.path) }
}
Save-Cache
$missing | Select-Object -First 20 | ForEach-Object { Write-Host "missing    $_" }
$wrong | Select-Object -First 20 | ForEach-Object { Write-Host "different  $_" }
$changed | Select-Object -First 20 | ForEach-Object { Write-Host "changed    $_" }
$tail = if ($SizeOnly) { ' (sizes only, -SizeOnly)' } else { ", $($changed.Count) the same size but a different sha256 ($hashed hashed, the rest cached)" }
Write-Host "$($rows.Count) external asset files listed: $($missing.Count) missing, $($wrong.Count) a different size$tail."
if ($missing.Count -or $wrong.Count -or $changed.Count) { exit 1 }
Write-Host 'All external assets present.'
exit 0
