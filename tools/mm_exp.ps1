# Motion matching experiments in parallel (docs/MOTION_MATCHING_HANDOFF.md, "Fast iteration").
#
#   powershell -ExecutionPolicy Bypass -File tools\mm_exp.ps1 -Name <experiment>
#
# Copies the runnable build (build\Release without .pdb / .lib / .exp - ~64 MB: the exe, TartarusGame.dll, TartarusEditor.dll,
# PhysX, Managed, assets) to work\exp\<experiment>\ and prints its exe. Run it with tools\mm_smoke.ps1 -Exe <that exe>
# -LogDir <a dir of its own>, 2-3 at once - and rebuild the main build meanwhile: the copy is untouched.
# (Experiments that only change tuning values need no copy at all: one build, TARTARUS_MM_TUNE set differently per run.)
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Name)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$from = Join-Path $repo 'build\Release'
$to = Join-Path $repo ('work\exp\' + $Name)
New-Item -ItemType Directory -Force -Path $to | Out-Null
& robocopy $from $to /E /XF *.pdb *.lib *.exp /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
$exe = Join-Path $to 'TartarusEngine.exe'
Write-Host $exe
