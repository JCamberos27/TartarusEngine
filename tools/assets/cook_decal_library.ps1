# Cooks the decal library (project/assets/Decals) from the purchased texture packs, following
# tools/assets/decal_name_map.csv (written by gen_decal_name_map.ps1), and writes each cooked PNG's
# .meta and each decal's DA_<name>.decal next to it.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\cook_decal_library.ps1 `
#       -SourceRoot C:\...\decal_src -ProjectRoot <repo> [-DriveRoot "G:\My Drive\Tartarus Assets"] [-Force] [-DecalsOnly]
#
#   -SourceRoot  the packs as extracted (the folder the map's source_rel_path is relative to);
#                not needed with -DecalsOnly
#   -ProjectRoot the repo root; output goes to <ProjectRoot>\project\assets\Decals
#   -DriveRoot   optional: afterwards mirror the cooked PNGs to <DriveRoot>\Used\project\assets\Decals
#                (robocopy, PNGs only: .meta and .decal files live in git)
#   -Force       re-cook everything; otherwise a PNG newer than its source is kept
#   -Only        regex on dest_rel_path, to cook a subset (e.g. 'PeelingPaint_00[1-5]')
#   -Threads     worker threads (default: half the logical processors, max 12)
#   -Stats       print luminance percentiles of the listed sources instead of cooking (threshold tuning)
#   -Sheets      write a contact sheet per category into this folder (up to 48 per category, drawn
#                over a blue-grey checker so alpha is visible), after cooking
#   -DecalsOnly  only (re)write the DA_*.decal files of the rows that pass -Only: no texture cooking,
#                .meta writing, contact sheets or Drive mirror (a decal whose PNG is missing is skipped)
#   -Relief      also cook each peel decal's normal and surface maps (<stem>_N.png, <stem>_S.png,
#                both tiers; -Only and -Force apply). A decal whose _N/_S exist references them.
#
# Cooked format: PNG, RGBA 8-bit, straight alpha.
#   mask      ATS white-on-black sheets (alpha channel opaque, the mask is the luminance). 2048^2.
#             RGB = 255 so a tint works; A = remap(luminance, 3% -> 0 .. 100% -> 1): the noise floor
#             under 3% goes to 0, everything above keeps its anti-aliased ramp.
#   splatter  TW Vol 19 liquid splatter: as mask, times an edge vignette. 2048^2.
#   peel      TW Vol 1 peeling / cracked paint (light paint, dark = peeled / exposed). RGB = the grey
#             photo; A = smoothstep(PeelLo, PeelHi, 1 - luminance) times the edge vignette, so intact
#             paint is clear and flakes, cracks and exposed patches show. 2048^2.
#   grunge    peel without the vignette (tiles in the material Grunge slot). 2048^2, T_Grunge_*.
#   Vignette: per-axis smoothstep over the outer 6% of the width, multiplied (no hard box edge).
#   Peel thresholds: see $PeelLo / $PeelHi below and the note there.
#   relief    (peel, -Relief) a height field in millimetres over the decal's 2 m box, from the photo:
#               h = 0.35 * P1 + 0.5 * max(0, P1 - P3) - 0.5 * C + 0.6 * (L0.6 - L4)
#             P = paint (1 - the peel coverage), C = crack interiors (smoothstep(0.62, 0.85, 1 - lum)),
#             L = luminance, the suffix a Gaussian sigma in mm (so 2K and 4K match): the paint stands
#             0.35 mm proud with a soft 1 mm step, its edges curl up (the lip term), cracks sink and
#             the photo's fine grain adds texture.
#             _N  RGB = n * 0.5 + 0.5, n = normalize(-dh/dx, -dh/dup, 1) * gain 2 (Scharr), OpenGL
#                 green-up (+y = image top), flattened by the vignette; 24-bit opaque.
#             _S  r roughness 0.45 paint .. 0.85 plaster .. 0.95 cracks, g metal 0, b AO (cavity
#                 below its 2.5 mm neighbourhood and cracks, 0.5 .. 1), a height (-0.6 .. +0.8 mm).
#             Both linear (.meta isSRGB false; the normal's textureType 1, NormalMap).
#
# substrate: peel decals (Grime/PeelingPaint) carry a "substrate" block, the PBR material that shows
#   where the paint is missing: {"preset": "assets/Decals/Substrates/SB_Wall_Concrete.substrate"}
#   (tiling 1.5 m, crop [0, 0.21, 1, 0.93] to skip the sheet's trim bands, mirrored repeat, no roughness
#   map - the pack's is solid black - and detail derived from the albedo; see the preset).
#
# .meta: the engine's texture meta (metaVersion 1, "type": "texture", importer block as AssetLibrary
# writes it): sRGB, compression 1 (Normal: BC3 for RGBA), mips, trilinear, aniso 8, maxTextureSize =
# tier, clamp for decals / repeat for grunge. GUIDs are minted the way AssetGuid::Generate does (a
# random non-zero 64-bit value, 16 hex digits) and an existing .meta keeps its GUID. .decal files get
# no .meta: the engine's AssetDatabase only gives known extensions one (".decal" isn't one).
param(
    [string]$SourceRoot,
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$DriveRoot,
    [string]$MapPath,
    [switch]$Force,
    [string]$Only,
    [int]$Threads = 0,
    [switch]$Stats,
    [string]$Sheets,
    [switch]$Relief,
    [switch]$DecalsOnly
)
$ErrorActionPreference = 'Stop'
if (-not $SourceRoot -and -not $DecalsOnly) { throw '-SourceRoot is required (except with -DecalsOnly)' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $MapPath) { $MapPath = Join-Path $here 'decal_name_map.csv' }
if ($Threads -le 0) { $Threads = [Math]::Max(1, [Math]::Min(12, [int]([Environment]::ProcessorCount / 2))) }

# Mask tuning. ATS: luminance under MaskFloor is the sheets' noise floor.
$MaskFloor = 0.03
# Peeling paint: in the TW Vol 1 photos intact paint sits at luminance ~0.85-0.95 (the per-image
# median is 0.79-0.92), the exposed grey plaster / substrate at ~0.45-0.75, and crack interiors, flake
# shadows and holes at ~0.02-0.35. On darkness d = 1 - luminance, smoothstep(0.15, 0.40, d) keeps
# paint (luminance > 0.85) clear, makes everything darker than 0.60 opaque (all exposed substrate,
# cracks and shadows) and ramps the dirty-paint / flake-edge band in between. A first try at
# smoothstep(0.42, 0.62) only caught cracks and holes and dropped the exposed plaster.
# Picked from -Stats percentiles (TW 5, 15, ... 95, 100) and the contact sheets.
$PeelLo = 0.15
$PeelHi = 0.40
$VignetteFrac = 0.06

Add-Type -AssemblyName System.Drawing
if (-not ('TartarusDecalCook' -as [type])) {
# The pixel work (C#: Add-Type compiles it once per session) lives in cook_decal_library.cs next to this script.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition (Get-Content -Raw -Encoding UTF8 (Join-Path $PSScriptRoot 'cook_decal_library.cs'))
}
[TartarusDecalCook]::MaskFloor = $MaskFloor
[TartarusDecalCook]::PeelLo = $PeelLo
[TartarusDecalCook]::PeelHi = $PeelHi
[TartarusDecalCook]::Vignette = $VignetteFrac

$decalsRoot = Join-Path $ProjectRoot 'project\assets\Decals'
$rows = @(Import-Csv -LiteralPath $MapPath)
if ($Only) { $rows = @($rows | Where-Object { $_.dest_rel_path -match $Only }) }
if (-not $rows) { throw "No rows in $MapPath$(if ($Only) { " match '$Only'" })" }

if ($Stats) {
    $rows | Select-Object -ExpandProperty source_rel_path -Unique | ForEach-Object {
        '{0,-60} {1}' -f (Split-Path $_ -Leaf), [TartarusDecalCook]::Stats((Join-Path $SourceRoot $_))
    }
    '(columns: luminance at the 1/5/10/25/50/75/90/95/99th percentile)'
    return
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$texTime = $sw.Elapsed

# ---- Textures -------------------------------------------------------------------------------------
if (-not $DecalsOnly) {
$todo = [ordered]@{}
$skipped = 0
foreach ($r in $rows) {
    $src = Join-Path $SourceRoot $r.source_rel_path
    $dst = Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $src)) { throw "Missing source: $src" }
    if (-not $Force -and (Test-Path -LiteralPath $dst) -and
        (Get-Item -LiteralPath $dst).LastWriteTimeUtc -gt (Get-Item -LiteralPath $src).LastWriteTimeUtc) {
        $skipped++; continue
    }
    if (-not $todo.Contains($src)) { $todo[$src] = New-Object System.Collections.Generic.List[TartarusDecalOut] }
    $o = New-Object TartarusDecalOut
    $o.Dst = $dst; $o.Kind = $r.kind; $o.Size = [int]$r.tier
    $todo[$src].Add($o)
}
# Relief maps (separate freshness check: a fresh albedo can still need its _N / _S).
$reliefCount = 0
if ($Relief) {
    foreach ($r in $rows) {
        if ($r.kind -ne 'peel') { continue }
        $src = Join-Path $SourceRoot $r.source_rel_path
        $stem = (Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')) -replace '\.png$', ''
        $srcTime = (Get-Item -LiteralPath $src).LastWriteTimeUtc
        $fresh = -not $Force -and @("${stem}_N.png", "${stem}_S.png" | Where-Object {
            -not (Test-Path -LiteralPath $_) -or (Get-Item -LiteralPath $_).LastWriteTimeUtc -le $srcTime }).Count -eq 0
        if ($fresh) { $skipped++; continue }
        if (-not $todo.Contains($src)) { $todo[$src] = New-Object System.Collections.Generic.List[TartarusDecalOut] }
        $o = New-Object TartarusDecalOut
        $o.Dst = $stem; $o.Kind = 'relief'; $o.Size = [int]$r.tier
        $todo[$src].Add($o)
        $reliefCount++
    }
    "Relief: $reliefCount normal + surface pair(s) to cook."
}
$outCount = ($todo.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
"Cooking $outCount texture(s) from $($todo.Count) source(s) on $Threads thread(s); $skipped up to date."
if ($todo.Count) {
    $srcs = [string[]]@($todo.Keys)
    $outs = [TartarusDecalOut[][]]@($todo.Values | ForEach-Object { , $_.ToArray() })
    $failed = [TartarusDecalCook]::Run($srcs, $outs, $Threads)
    $failed | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    if ($failed) { throw "$($failed.Count) source(s) failed" }
}
$texTime = $sw.Elapsed
}

# ---- .meta and .decal -----------------------------------------------------------------------------
$utf8 = New-Object Text.UTF8Encoding $false
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
function New-Guid64 {
    $b = New-Object byte[] 8
    do { $rng.GetBytes($b); $v = [BitConverter]::ToUInt64($b, 0) } while ($v -eq 0)
    $v.ToString('x16')
}
function Write-IfChanged([string]$path, [string]$text) {
    if ((Test-Path -LiteralPath $path) -and [IO.File]::ReadAllText($path) -ceq $text) { return $false }
    [IO.File]::WriteAllText($path, $text, $utf8); return $true
}
# JSON as nlohmann's dump(2) lays it out (2-space indent, arrays one element per line).
function Fmt($v) {
    if ($v -is [bool]) { return $(if ($v) { 'true' } else { 'false' }) }
    if ($v -is [string]) { return '"' + ($v -replace '\\', '\\' -replace '"', '\"') + '"' }
    if ($v -is [int]) { return $v.ToString([Globalization.CultureInfo]::InvariantCulture) }
    $s = ([double]$v).ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    if ($s -notmatch '[.eE]') { $s += '.0' }
    return $s
}
function To-Json($v, [int]$ind = 0) {
    $pad = ' ' * ($ind + 2); $end = ' ' * $ind
    if ($v -is [System.Collections.Specialized.OrderedDictionary]) {
        if ($v.Count -eq 0) { return '{}' }
        $parts = foreach ($k in $v.Keys) { $pad + '"' + $k + '": ' + (To-Json $v[$k] ($ind + 2)) }
        return "{`n" + ($parts -join ",`n") + "`n$end}"
    }
    if ($v -is [array]) {
        if ($v.Count -eq 0) { return '[]' }
        $parts = foreach ($e in $v) { $pad + (To-Json $e ($ind + 2)) }
        return "[`n" + ($parts -join ",`n") + "`n$end]"
    }
    return Fmt $v
}

function Get-Tags([string]$category) {
    $words = foreach ($seg in $category -split '/') { [regex]::Matches($seg, '[A-Z][a-z]*|[a-z]+|\d+') | ForEach-Object { $_.Value.ToLowerInvariant() } }
    , @($words | Select-Object -Unique)
}

# Per-category defaults (see docs/PROJECT_ASSETS.md, "Decals").
function Get-DecalJson($r) {
    $cat = $r.category
    $tier = [int]$r.tier
    $tex = 'assets/Decals/' + $r.dest_rel_path
    $ch = [ordered]@{ albedo = 1.0; normal = 0.0; roughness = 0.0; metal = 0.0; ao = 0.0; emissive = 0.0 }
    $texN = ''; $texS = ''
    $base = @(1.0, 1.0, 1.0); $rough = 0.6; $size = @(1.0, 1.0, 0.25)
    $edge = 0.08; $hb = 0.0
    $paint = [ordered]@{ enabled = $false; roughness = 0.35; metal = 0.0; relief = 0.3; uv = $false }
    switch -Regex ($cat) {
        '^Graffiti/' {
            $ch.roughness = 1.0; $rough = 0.6; $hb = 0.35
            $paint.enabled = $true; $paint.roughness = 0.45; $paint.relief = 0.15
            if ($cat -match '^Graffiti/(Alphabets|Letters|Numbers)') { $size = @(0.6, 0.6, 0.25) }
        }
        '^WarPaint$' {
            $ch.roughness = 1.0; $rough = 0.6; $hb = 0.35
            $paint.enabled = $true; $paint.roughness = 0.45; $paint.relief = 0.15
        }
        '^Grime/PeelingPaint$' {
            $ch.roughness = 1.0; $rough = 0.4; $ch.ao = 0.5; $edge = 0.15
            $size = @(2.0, 2.0, 0.25)
            $stemPath = (Join-Path $decalsRoot ($r.dest_rel_path -replace '/', [string][char]92)) -replace '\.png$', ''
            if ((Test-Path -LiteralPath "${stemPath}_N.png") -and (Test-Path -LiteralPath "${stemPath}_S.png")) {
                $ch.normal = 1.0; $ch.ao = 1.0; $hb = 0.4
                $texN = $tex -replace '\.png$', '_N.png'; $texS = $tex -replace '\.png$', '_S.png'
            }
        }
        '^Stains/LiquidSplatter$' {
            $ch.roughness = 1.0; $rough = 0.5; $hb = 0.5; $base = @(0.85, 0.85, 0.85)
            $size = @(1.5, 1.5, 0.25)
        }
        '^Stains/Splatters$' {
            $ch.roughness = 1.0; $rough = 0.5; $base = @(0.25, 0.25, 0.25)
        }
        '^(Signs|Stamps)$' {
            $paint.enabled = $true
            $size = @(0.5, 0.5, 0.25)
        }
    }
    $j = [ordered]@{
        decalVersion   = 1
        textures       = [ordered]@{ albedo = $tex; normal = $texN; surface = $texS; aux = '' }
        lumaMask       = $false
        invertMask     = $false
        tier           = $tier
        atlas          = [ordered]@{ cols = 1; rows = 1 }
        defaultSize    = $size
        channels       = $ch
        baseColor      = $base
        roughness      = $rough
        metal          = 0.0
        emissive       = @(0.0, 0.0, 0.0)
        angleFade      = @(0.25, 0.05)
        edgeFade       = $edge
        heightBlend    = $hb
        heightContrast = 4.0
        paint          = $paint
        weathering     = [ordered]@{ fade = 0.0; bleach = 0.0; peel = 0.0; buff = 0.0 }
        category       = $cat
        tags           = (Get-Tags $cat)
    }
    if ($cat -match '^Grime/PeelingPaint$') {
        # The Wall_Concrete substrate preset (assets/Decals/Substrates/SB_Wall_Concrete.substrate).
        $j.Insert(($j.Keys.Count - 2), 'substrate', [ordered]@{ preset = 'assets/Decals/Substrates/SB_Wall_Concrete.substrate' })
    }
    (To-Json $j) + "`n"
}

$allRows = @(Import-Csv -LiteralPath $MapPath)
$metaNew = 0; $metaKept = 0; $decalWritten = 0; $decalCount = 0
foreach ($r in $allRows) {
    if ($Only -and $r.dest_rel_path -notmatch $Only) { continue }
    $png = Join-Path $decalsRoot ($r.dest_rel_path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $png)) { continue }
    $dir = Split-Path $png

    # .meta: keep an existing GUID (and never rewrite an unchanged file).
    $meta = "$png.meta"
    $guid = $null
    if (Test-Path -LiteralPath $meta) {
        $m = [regex]::Match([IO.File]::ReadAllText($meta), '"guid"\s*:\s*"([0-9a-fA-F]{16})"')
        if ($m.Success) { $guid = $m.Groups[1].Value.ToLowerInvariant() }
    }
    if ($guid) { $metaKept++ } else { $guid = New-Guid64; $metaNew++ }
    $folder = 'assets/Decals/' + ($r.dest_rel_path.Substring(0, $r.dest_rel_path.LastIndexOf('/')))
    $wrap = if ($r.kind -eq 'grunge') { 0 } else { 1 }
    $metaJson = [ordered]@{
        folder      = $folder
        guid        = $guid
        importer    = [ordered]@{
            anisoLevel = 8; compression = 1; filterMode = 2; generateMipmaps = $true; isSRGB = $true
            maxTextureSize = [int]$r.tier; textureType = 0; wrapMode = $wrap
        }
        metaVersion = 1
        type        = 'texture'
    }
    if (-not $DecalsOnly) { [void](Write-IfChanged $meta (To-Json $metaJson)) }

    # Relief maps: linear metas (normal map type for _N), GUIDs kept.
    if ($r.kind -eq 'peel' -and -not $DecalsOnly) {
        foreach ($suf in '_N', '_S') {
            $rp = ($png -replace '\.png$', '') + $suf + '.png'
            if (-not (Test-Path -LiteralPath $rp)) { continue }
            $rm = "$rp.meta"; $rg = $null
            if (Test-Path -LiteralPath $rm) {
                $m2 = [regex]::Match([IO.File]::ReadAllText($rm), '"guid"\s*:\s*"([0-9a-fA-F]{16})"')
                if ($m2.Success) { $rg = $m2.Groups[1].Value.ToLowerInvariant() }
            }
            if ($rg) { $metaKept++ } else { $rg = New-Guid64; $metaNew++ }
            $rj = [ordered]@{
                folder      = $folder
                guid        = $rg
                importer    = [ordered]@{
                    anisoLevel = 8; compression = 1; filterMode = 2; generateMipmaps = $true; isSRGB = $false
                    maxTextureSize = [int]$r.tier; textureType = $(if ($suf -eq '_N') { 1 } else { 0 }); wrapMode = $wrap
                }
                metaVersion = 1
                type        = 'texture'
            }
            [void](Write-IfChanged $rm (To-Json $rj))
        }
    }

    if ($r.kind -ne 'grunge') {
        $stem = [IO.Path]::GetFileNameWithoutExtension($png)
        $decal = Join-Path $dir ('DA_' + $stem.Substring('T_Decal_'.Length) + '.decal')
        if (Write-IfChanged $decal (Get-DecalJson $r)) { $decalWritten++ }
        $decalCount++
    }
}
"Metas: $metaNew new, $metaKept kept. Decals: $decalCount ($decalWritten written)."
"Textures took $([Math]::Round($texTime.TotalSeconds, 1)) s; total $([Math]::Round($sw.Elapsed.TotalSeconds, 1)) s."

# ---- Contact sheets -------------------------------------------------------------------------------
if ($Sheets -and -not $DecalsOnly) {
    New-Item -ItemType Directory -Force $Sheets | Out-Null
    $byCat = $allRows | Group-Object { if ($_.kind -eq 'grunge') { "$($_.category) (grunge)" } elseif ($_.tier -eq '4096') { "$($_.category) (4K)" } else { $_.category } }
    foreach ($g in $byCat) {
        $items = @($g.Group | Where-Object { Test-Path -LiteralPath (Join-Path $decalsRoot ($_.dest_rel_path -replace '/', '\')) })
        if (-not $items) { continue }
        if ($items.Count -gt 48) {
            $step = $items.Count / 48.0
            $items = @(0..47 | ForEach-Object { $items[[int][Math]::Floor($_ * $step)] })
        }
        $files = [string[]]@($items | ForEach-Object { Join-Path $decalsRoot ($_.dest_rel_path -replace '/', '\') })
        $labels = [string[]]@($items | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.dest_rel_path) -replace '^T_(Decal|Grunge)_', '' })
        $name = ($g.Name -replace '[/ ]', '_' -replace '[()]', '') + '.png'
        [TartarusDecalCook]::Sheet($files, $labels, (Join-Path $Sheets $name), 160, 8)
        "sheet $(Join-Path $Sheets $name) ($($files.Count))"
    }
}

# ---- Drive ----------------------------------------------------------------------------------------
if ($DriveRoot -and -not $DecalsOnly) {
    $dst = Join-Path $DriveRoot 'Used\project\assets\Decals'
    robocopy $decalsRoot $dst *.png /E /MT:16 /R:3 /W:5 /NP /NDL /NFL /XF *.tmp | Select-Object -Last 12
    if ($LASTEXITCODE -ge 8) { throw "robocopy to $dst failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}
