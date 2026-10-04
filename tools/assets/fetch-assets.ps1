# Copies the third-party assets that are not in git from the team's shared Google Drive into this
# checkout, then checks the checkout against project/external_assets.csv.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -Source "<...>\Tartarus Assets"
#   powershell -ExecutionPolicy Bypass -File tools\assets\fetch-assets.ps1 -VerifyOnly
#
# -Source is the shared "Tartarus Assets" folder (or its Used folder) as Google Drive for desktop
# shows it, e.g. "G:\My Drive\Tartarus Assets" or a "Shared with me" shortcut. Used\ mirrors this
# repo, so files land at the same project paths their tracked .meta files sit at. Only missing or
# changed files are copied; nothing in the checkout is deleted.
# Exit codes: 0 every listed file present; 1 files missing or a different size; 2 bad arguments.
param([string]$Source, [switch]$VerifyOnly)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$manifest = Join-Path $root 'project\external_assets.csv'

if (-not $VerifyOnly) {
    if (-not $Source) { Write-Host 'Pass -Source "<path to the shared Tartarus Assets folder>" (or -VerifyOnly).'; exit 2 }
    $used = if (Test-Path (Join-Path $Source 'Used\project')) { Join-Path $Source 'Used' } else { $Source }
    if (-not (Test-Path (Join-Path $used 'project'))) { Write-Host "No project folder under $used - is that the Tartarus Assets folder?"; exit 2 }
    Write-Host "Copying $used\project -> $root\project ..."
    # The repo's own external_assets.csv is the authority; never overwrite it from the Drive copy.
    robocopy (Join-Path $used 'project') (Join-Path $root 'project') /E /XF external_assets.csv desktop.ini `
        /R:2 /W:2 /MT:8 /NP /NDL /NFL /NJH | Select-Object -Last 12 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ge 8) { Write-Host "robocopy failed ($LASTEXITCODE)"; exit 1 }
}

$rows = Import-Csv $manifest
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
