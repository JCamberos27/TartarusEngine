# What is the "Tartarus Engine" desktop shortcut about to run, and is it the latest?
#
# Called by run-editor.cmd before it builds. Fetches GitHub, then prints which branch and commit
# the checkout is on, whether that is up to date with GitHub's main (and what the newest update
# is), and which open pull requests could be tried instead. Waits a few seconds for a key so a
# double-click still just launches:
#   U / M  - switch to main and pull the latest
#   1-9    - check out that open pull request to try it (needs the GitHub CLI, `gh`)
#   other  - launch as it is
#
# Switching never touches uncommitted changes: with edits to tracked files it refuses instead.
# -Brief prints a one-line reminder of what is being launched (no fetch, no prompt).
# -NoPrompt prints the report and exits. -Choice <key> answers the prompt (used to test this).
param(
    [switch]$Brief,
    [switch]$NoPrompt,
    [string]$Choice = '',
    [int]$TimeoutSec = 8
)

$ErrorActionPreference = 'Continue'
$env:GIT_TERMINAL_PROMPT = '0'
Set-Location (Split-Path -Parent $PSScriptRoot)

function Run-Git { & git.exe @args 2>$null }

function Line($text, $color = 'Gray') { Write-Host $text -ForegroundColor $color }

function Head-Line {
    $hash = Run-Git log -1 --format=%h
    $subject = Run-Git log -1 --format=%s
    $when = Run-Git log -1 --format=%cr
    return "$hash  $subject  ($when)"
}

function Behind-Count($range) {
    $n = Run-Git rev-list --count $range
    if ($LASTEXITCODE -ne 0 -or -not $n) { return 0 }
    return [int]$n
}

function Show-Report {
    $branch = (Run-Git branch --show-current)
    if (-not $branch) { $branch = '(detached HEAD)' }
    Line ''
    Line '  Tartarus Engine - what this shortcut will run' 'Cyan'
    Line '  ---------------------------------------------' 'Cyan'
    Line "  Branch : $branch"
    Line "  Update : $(Head-Line)"

    $script:behindMain = Behind-Count 'HEAD..origin/main'
    $aheadMain = Behind-Count 'origin/main..HEAD'
    $script:onMain = ($branch -eq 'main')

    if (-not $script:online) {
        Line '  [?] Could not reach GitHub, so this may not be the newest.' 'Yellow'
    } elseif ($script:onMain -and $script:behindMain -eq 0) {
        Line '  [OK] Up to date with GitHub main - this is the latest update.' 'Green'
    } elseif ($script:onMain) {
        Line "  [!] GitHub main has $($script:behindMain) newer update(s) than this copy:" 'Yellow'
        Run-Git log HEAD..origin/main --format='        - %s (%cr)' -n 5 | ForEach-Object { Line $_ 'Yellow' }
        Line '      Press U to update to them.' 'Yellow'
    } elseif ($aheadMain -eq 0) {
        Line '  [!] This branch is already merged into main - press M to go back to main.' 'Yellow'
    } else {
        Line "  Testing a branch that is $aheadMain change(s) ahead of main (not merged yet):" 'Magenta'
        Run-Git log origin/main..HEAD --format='        - %s' -n 5 | ForEach-Object { Line $_ 'Magenta' }
        if ($script:behindMain -gt 0) { Line "      (main has $($script:behindMain) newer update(s) that are not in this branch.)" 'DarkYellow' }
        $upstreamBehind = Behind-Count 'HEAD..@{u}'
        if ($upstreamBehind -gt 0) { Line "      [!] The branch was updated on GitHub ($upstreamBehind newer commit(s)) - run 'git pull' to get them." 'Yellow' }
    }

    $script:prs = @()
    if ($script:online -and (Get-Command gh -ErrorAction SilentlyContinue)) {
        $json = & gh pr list --state open --limit 9 --json number,title,headRefName 2>$null
        if ($LASTEXITCODE -eq 0 -and $json) { $script:prs = @($json | ConvertFrom-Json) }
    }
    if ($script:prs.Count -gt 0) {
        Line ''
        Line '  Open pull requests you can try (not merged yet):' 'Cyan'
        for ($i = 0; $i -lt $script:prs.Count; $i++) {
            $marker = if ($script:prs[$i].headRefName -eq $branch) { '  <- running now' } else { '' }
            Line ("    [{0}] #{1}  {2}{3}" -f ($i + 1), $script:prs[$i].number, $script:prs[$i].title, $marker)
        }
    }
    Line ''
}

function Try-Switch([scriptblock]$action, $what) {
    $dirty = Run-Git status --porcelain --untracked-files=no
    if ($dirty) {
        Line "  Not switching: you have uncommitted changes to tracked files ($what)." 'Red'
        return $false
    }
    & $action
    return ($LASTEXITCODE -eq 0)
}

if ($Brief) {
    $branch = (Run-Git branch --show-current)
    $behind = Behind-Count 'HEAD..origin/main'
    $state = if ($branch -eq 'main' -and $behind -eq 0) { 'latest main' } elseif ($branch -eq 'main') { "main, $behind behind GitHub" } else { "test branch $branch" }
    Line "  Launching: $(Head-Line)  [$state]" 'Cyan'
    exit 0
}

Run-Git fetch origin --prune --quiet
$script:online = ($LASTEXITCODE -eq 0)
Show-Report

if ($NoPrompt -and -not $Choice) { exit 0 }

$key = $Choice
if (-not $key) {
    $opts = @()
    if (-not $script:onMain -or $script:behindMain -gt 0) { $opts += 'U/M = latest main' }
    if ($script:prs.Count -gt 0) { $opts += ($(if ($script:prs.Count -eq 1) { "1" } else { "1-$($script:prs.Count)" }) + " = try a pull request") }
    if ($opts.Count -eq 0) { exit 0 }
    Write-Host ("  {0}   |   any other key, or wait {1}s = launch as is" -f ($opts -join '   '), $TimeoutSec) -ForegroundColor White
    try {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            if ([Console]::KeyAvailable) { $key = [Console]::ReadKey($true).KeyChar.ToString(); break }
            Start-Sleep -Milliseconds 100
        }
    } catch { exit 0 }  # no console to read from: just launch
}

$switched = $false
if ($key -match '^[uUmM]$') {
    $switched = Try-Switch { Run-Git checkout main | Out-Null; if ($LASTEXITCODE -eq 0) { Run-Git pull --ff-only origin main | Out-Null } } 'main'
} elseif ($key -match '^[1-9]$' -and [int]$key -le $script:prs.Count) {
    $n = $script:prs[[int]$key - 1].number
    $switched = Try-Switch { & gh pr checkout $n 2>$null | Out-Null } "PR #$n"
}
if ($switched) {
    Run-Git fetch origin --prune --quiet
    Show-Report
}
exit 0
