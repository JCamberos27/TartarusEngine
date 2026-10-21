# Writes tools/assets/decal_name_map.csv: one row per cooked texture of the decal library, from the
# four purchased packs (Art Talent Studio "709 Hand Painted Alpha Graffiti" and "50 Free Hand Painted
# Alpha Graffiti and Paints", TW "Surface Imperfections" Vol 1 and Vol 19) as extracted under -SourceRoot.
#
#   powershell -ExecutionPolicy Bypass -File tools\assets\gen_decal_name_map.ps1 -SourceRoot C:\...\decal_src
#
# Columns:
#   source_rel_path  the source PNG, relative to -SourceRoot (backslashes, as extracted)
#   dest_rel_path    the cooked PNG, relative to project/assets/Decals (forward slashes)
#   kind             mask     RGB white, A = source luminance (ATS white-on-black alpha sheets)
#                    splatter mask + soft edge vignette (TW Vol 19 liquid splatter)
#                    peel     RGB = source grey photo, A = inverted-luminance smoothstep + edge vignette (TW Vol 1)
#                    grunge   peel without the vignette, for the material Grunge slot (tiles triplanar)
#   category         folder path under Decals (what the .decal's "category" says)
#   tier             2048 or 4096 (cooked size, and the decal-array slot size)
#
# Names are UE-style and keep the vendor's number, zero-padded to 3 digits, so the free pack's numbers
# (Letter 217-221, Shape 208-212, ...) slot in after the 709 pack's. The one exception is Numbers: the
# 709 pack ships three digit sets that each restart at 1 (ATS_Number_<set>_<1..10>), so they are
# numbered set-major: set s, file n -> (s-1)*10 + n (001-030).
# Every source folder's file count is checked against what the packs shipped with; a mismatch stops.
param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'decal_name_map.csv' }

$p709  = 'ATS_709\709 Hand Painted Alpha Graffiti\PNG - Alpha'
$pFree = 'ATS_50Free\50 Free Hand Painted Alpha Graffiti and Paints\PNG - Alpha'
$pTw1  = 'TW_SurfaceImperfections_Vol01\TW_Surface Imperfections - Vol 1'
$pTw19 = 'TW_SurfaceImperfections_Vol19\TW_Surface Imperfections - Vol 19'

$az = [char[]]'ABCDEFGHIJKLMNOPQRSTUVWXYZ' | ForEach-Object { [string]$_ }
$glyphs = @{
    'Tag'    = $az
    'Brush'  = $az + @('Question', 'Plus')
    'Bubble' = $az
    'Block'  = $az + @('0','1','2','3','4','5','6','7','8','9') +
               @('Hash','Percent','Plus','Equals','Minus','Quote','Question','Exclaim','BracketL','ParenL','Colon')
}
$alphabetStyle = @{ 1 = 'Tag'; 2 = 'Brush'; 3 = 'Bubble'; 4 = 'Block' }

# (source folder, file-name regex with the number in group 'n' (and 'set'), expected count,
#  destination folder, texture name prefix)
$rules = @(
    @{ Dir = "$p709\Letters";          Re = '^ATS_Letter_(?<n>\d+)$';          Count = 216; Cat = 'Graffiti/Letters';  Name = 'T_Decal_GraffitiLetter' }
    @{ Dir = "$pFree\Graffiti\Letters"; Re = '^ATS_Letter_(?<n>\d+)$';         Count = 5;   Cat = 'Graffiti/Letters';  Name = 'T_Decal_GraffitiLetter' }
    @{ Dir = "$p709\Numbers";          Re = '^ATS_Number_(?<set>\d)_(?<n>\d+)$'; Count = 30; Cat = 'Graffiti/Numbers'; Name = 'T_Decal_GraffitiNumber' }
    @{ Dir = "$p709\Shapes";           Re = '^ATS_Shape_(?<n>\d+)$';           Count = 207; Cat = 'Graffiti/Shapes';   Name = 'T_Decal_GraffitiShape' }
    @{ Dir = "$pFree\Graffiti\Shapes"; Re = '^ATS_Shape_(?<n>\d+)$';           Count = 5;   Cat = 'Graffiti/Shapes';   Name = 'T_Decal_GraffitiShape' }
    @{ Dir = "$p709\Spray";            Re = '^ATS_Spray_(?<n>\d+)$';           Count = 82;  Cat = 'Graffiti/Spray';    Name = 'T_Decal_GraffitiSpray' }
    @{ Dir = "$pFree\Graffiti\Spray";  Re = '^ATS_Spray_(?<n>\d+)$';           Count = 5;   Cat = 'Graffiti/Spray';    Name = 'T_Decal_GraffitiSpray' }
    @{ Dir = "$p709\Letter Patterns";  Re = '^ATS_Letter_Pattern_(?<n>\d+)$';  Count = 12;  Cat = 'Graffiti/Patterns'; Name = 'T_Decal_GraffitiPattern' }
    @{ Dir = "$p709\Tribal Stripe Decal Graffiti for Car";          Re = '^ATS_Tribal_Stripe_(?<n>\d+)$'; Count = 35; Cat = 'Graffiti/Stripes'; Name = 'T_Decal_GraffitiStripe' }
    @{ Dir = "$pFree\Graffiti\Tribal Stripe Decal Graffiti for Car"; Re = '^ATS_Tribal_Stripe_(?<n>\d+)$'; Count = 5;  Cat = 'Graffiti/Stripes'; Name = 'T_Decal_GraffitiStripe' }
    @{ Dir = "$pFree\Decals and Signs";     Re = '^ATS_Sign_(?<n>\d+)$';             Count = 5;  Cat = 'Signs';            Name = 'T_Decal_Sign' }
    @{ Dir = "$pFree\Stains and Splatters"; Re = '^ATS_Stain_&_Splatter_(?<n>\d+)$'; Count = 10; Cat = 'Stains/Splatters'; Name = 'T_Decal_Stain' }
    @{ Dir = "$pFree\Stamp";                Re = '^ATS_Stamp_(?<n>\d+)$';            Count = 5;  Cat = 'Stamps';           Name = 'T_Decal_Stamp' }
    @{ Dir = "$pFree\War Paint";            Re = '^ATS_War_Paint_(?<n>\d+)$';        Count = 10; Cat = 'WarPaint';         Name = 'T_Decal_WarPaint' }
)

function Get-Pngs([string]$rel) {
    $dir = Join-Path $SourceRoot $rel
    if (-not (Test-Path -LiteralPath $dir)) { throw "Missing source folder: $dir" }
    Get-ChildItem -LiteralPath $dir -Filter *.png -File
}
function Row($src, $dst, $kind, $cat, $tier) {
    [pscustomobject]@{ source_rel_path = $src; dest_rel_path = $dst; kind = $kind; category = $cat; tier = $tier }
}

$rows = New-Object System.Collections.Generic.List[object]

# Alphabets
foreach ($i in 1..4) {
    $style = $alphabetStyle[$i]; $names = $glyphs[$style]
    $rel = "$p709\Alphabets\Alphabet $i"
    $files = @(Get-Pngs $rel)
    if ($files.Count -ne $names.Count) { throw "$rel has $($files.Count) files, expected $($names.Count)" }
    foreach ($f in $files) {
        $m = [regex]::Match($f.BaseName, "^ATS_Alphabet_${i}_(?<n>\d+)$")
        if (-not $m.Success) { throw "Unexpected file $($f.FullName)" }
        $n = [int]$m.Groups['n'].Value
        if ($n -lt 1 -or $n -gt $names.Count) { throw "Unexpected glyph number $n in $($f.FullName)" }
        $cat = "Graffiti/Alphabets/$style"
        $rows.Add((Row "$rel\$($f.Name)" "$cat/T_Decal_Alpha${style}_$($names[$n - 1]).png" 'mask' $cat 2048))
    }
}

# The other ATS categories
foreach ($r in $rules) {
    $files = @(Get-Pngs $r.Dir)
    if ($files.Count -ne $r.Count) { throw "$($r.Dir) has $($files.Count) files, expected $($r.Count)" }
    foreach ($f in $files) {
        $m = [regex]::Match($f.BaseName, $r.Re)
        if (-not $m.Success) { throw "Unexpected file $($f.FullName)" }
        $n = [int]$m.Groups['n'].Value
        if ($m.Groups['set'].Success) { $n = ([int]$m.Groups['set'].Value - 1) * 10 + $n }
        $rows.Add((Row "$($r.Dir)\$($f.Name)" ('{0}/{1}_{2:D3}.png' -f $r.Cat, $r.Name, $n) 'mask' $r.Cat 2048))
    }
}

# TW Surface Imperfections: a 2K decal, a 4K hero decal and (Vol 1 only) a 4K tiling grunge map each
foreach ($tw in @(
        @{ Dir = $pTw1;  Re = '^TW_Surface Imperfections \((?<n>\d+)\)$'; Cat = 'Grime/PeelingPaint';     Name = 'PeelingPaint';   Kind = 'peel' }
        @{ Dir = $pTw19; Re = '^TW_Surface Imperfection \((?<n>\d+)\)$';  Cat = 'Stains/LiquidSplatter';  Name = 'LiquidSplatter'; Kind = 'splatter' })) {
    $files = @(Get-Pngs $tw.Dir)
    if ($files.Count -ne 100) { throw "$($tw.Dir) has $($files.Count) files, expected 100" }
    foreach ($f in $files) {
        $m = [regex]::Match($f.BaseName, $tw.Re)
        if (-not $m.Success) { throw "Unexpected file $($f.FullName)" }
        $n = [int]$m.Groups['n'].Value
        $src = "$($tw.Dir)\$($f.Name)"
        $rows.Add((Row $src ('{0}/T_Decal_{1}_{2:D3}.png' -f $tw.Cat, $tw.Name, $n) $tw.Kind $tw.Cat 2048))
        $rows.Add((Row $src ('{0}/T_Decal_{1}_{2:D3}_4K.png' -f $tw.Cat, $tw.Name, $n) $tw.Kind $tw.Cat 4096))
        if ($tw.Kind -eq 'peel') {
            $rows.Add((Row $src ('{0}/T_Grunge_{1}_{2:D3}.png' -f $tw.Cat, $tw.Name, $n) 'grunge' $tw.Cat 4096))
        }
    }
}

$dupes = $rows | Group-Object { $_.dest_rel_path.ToLowerInvariant() } | Where-Object Count -gt 1
if ($dupes) { throw "Duplicate destinations: $(($dupes | ForEach-Object Name) -join ', ')" }

$sorted = $rows | Sort-Object { $_.dest_rel_path } -Culture ([Globalization.CultureInfo]::InvariantCulture)
# Plain UTF-8 (no BOM), LF, quoted only where needed - reviewable in a diff.
function Q([string]$s) { if ($s -match '[",]') { '"' + ($s -replace '"', '""') + '"' } else { $s } }
$lines = @('source_rel_path,dest_rel_path,kind,category,tier') +
         ($sorted | ForEach-Object { (Q $_.source_rel_path), (Q $_.dest_rel_path), $_.kind, $_.category, $_.tier -join ',' })
[IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding $false))
$sorted | Group-Object category | Sort-Object Name | ForEach-Object { '{0,-28} {1,4}' -f $_.Name, $_.Count }
"$Out : $($sorted.Count) rows"
