# W2 mirror/sink debug batch #2 (needs locks\perf owned by W2 + a GPU GO; ~5 min). Metals (mirror + sink basin/faucet) reflect a DARK room-cube sample.
# Each run = run_dbg.ps1 (tour_dbg.json: mirror head-on, kitchen sink basin, front facade). Scale 0.5 on the visualised terms; read the mean of the
# mirror / basin regions in gi-p2\argus\slot\out\dbg_<tag>\png (the exposure multiplies everything: compare runs, not absolute values).
$s = 'D:\Jacob\tblocks\descent\gi-p2\argus\slot\run_dbg.ps1'
# 1. the prefiltered cube sample itself (before F, before any intensity)
& $s -Tag 'pref' -Snippet 'ambient = prefiltered * 0.5;'
# 2. the BRDF term: F * ab.x + ab.y
& $s -Tag 'specW' -Snippet 'ambient = specW * 0.05;'
# 3. parallax correction off: the probe cube is sampled along R instead of the box-corrected direction
& $s -Tag 'noparallax' -From 'vec3 correctedR = normalize(intersect - uProbeCenter[pi]);' -To 'vec3 correctedR = R;' -Snippet 'ambient = prefiltered * 0.5;'
# 4. mip 0 (sharpest) for the room cube
& $s -Tag 'mip0' -From 'probeSpec = mix(probeSpec, textureLod(uProbeCubes, vec4(correctedR, float(uProbeLayer[pi])), roughness * uIBLSpecularMaxLod).rgb, roomFace);' -To 'probeSpec = mix(probeSpec, textureLod(uProbeCubes, vec4(correctedR, float(uProbeLayer[pi])), 0.0).rgb, roomFace);' -Snippet 'ambient = prefiltered * 0.5;'
# 5. the room cube along the SURFACE NORMAL instead of R (what a diffuse-like lookup would see): is the cube dark everywhere or only along R?
& $s -Tag 'cubeN' -From 'vec3 correctedR = normalize(intersect - uProbeCenter[pi]);' -To 'vec3 correctedR = N;' -Snippet 'ambient = prefiltered * 0.5;'
Get-Date -Format u
