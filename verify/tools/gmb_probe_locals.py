"""G-MB stage A: ModelFragment.glsl's probe loop reads locals (uniforms, or the slot state)."""
import sys

p = sys.argv[1]
raw = open(p, 'rb').read().decode('utf-8')
CR, LF = chr(13), chr(10)
crlf = CR + LF in raw
s = raw.replace(CR + LF, LF)
start = s.index('#ifdef _REFLECTION_PROBES\n        // Parallax box projection')
end = s.index('#endif', start)
block = s[start:end]
head = '        if (uProbeCount > 0) {'
assert block.count(head) == 1
locals_ = '''        // This draw's probes: the uniforms, or the copy's own (G-MB slot state, the same values).
        int probeCount = uProbeCount;
        vec3 probeCenter[2] = vec3[2](uProbeCenter[0], uProbeCenter[1]);
        vec3 probeHalfSize[2] = vec3[2](uProbeHalfSize[0], uProbeHalfSize[1]);
        float probeBlend[2] = float[2](uProbeBlend[0], uProbeBlend[1]);
        int probeLayer[2] = int[2](uProbeLayer[0], uProbeLayer[1]);
#ifndef _NO_SLOT_STATE
        if (uSlotState == 1) {
            probeCount = int(uSlotStates[vSlot].Head.z);
            for (int pj = 0; pj < 2; ++pj) {
                vec4 a = uSlotStates[vSlot].Probe[2 * pj], b = uSlotStates[vSlot].Probe[2 * pj + 1];
                probeCenter[pj] = a.xyz;
                probeLayer[pj] = int(a.w);
                probeHalfSize[pj] = b.xyz;
                probeBlend[pj] = b.w;
            }
        }
#endif
        if (probeCount > 0) {'''
block = block.replace(head, locals_)
for a, b in [('uProbeCount', 'probeCount'), ('uProbeCenter[pi]', 'probeCenter[pi]'),
             ('uProbeHalfSize[pi]', 'probeHalfSize[pi]'), ('uProbeBlend[pi]', 'probeBlend[pi]'),
             ('uProbeLayer[pi]', 'probeLayer[pi]')]:
    if a == 'uProbeCount':
        # keep the local's initializer and the comment's mention
        parts = block.split('int probeCount = uProbeCount;')
        assert len(parts) == 2
        parts[1] = parts[1].replace('for (int pi = 0; pi < uProbeCount; ++pi)', 'for (int pi = 0; pi < probeCount; ++pi)')
        block = 'int probeCount = uProbeCount;'.join(parts)
    else:
        block = block.replace(a, b)
assert 'uProbeCenter[pi]' not in block and 'pi < uProbeCount' not in block
s = s[:start] + block + s[end:]
open(p, 'wb').write((s.replace(LF, CR + LF) if crlf else s).encode('utf-8'))
print('ok')
