#version 460 core
in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

uniform vec3 uCameraPos;
uniform float uMinorSpacing;
uniform float uMajorEvery;
uniform float uFadeDistance;
uniform float uOpacity;   // 0..1 master multiplier on grid-line alpha
uniform int   uShowAxes;
uniform float uAxisThickness; // screen-pixel width of the coloured axis lines

float GridLine(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 deriv = fwidth(c);
    vec2 g = abs(fract(c - 0.5) - 0.5) / max(deriv, vec2(1e-6));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

// A crisp, anti-aliased line ~`px` screen pixels wide, given the signed distance to the axis
// (in world units) and its per-pixel world-space derivative.
float AxisLine(float distToAxis, float deriv, float px) {
    float hw = deriv * px * 0.5;
    return 1.0 - smoothstep(hw, hw + deriv, abs(distToAxis));
}

void main() {
    vec3 rd = normalize(vFarPoint - vNearPoint);

    // --- Ground-plane grid (world y = 0). When the ray never hits it (looking up / straight
    // along the plane) the grid contributes nothing, but the axis lines below still can.
    float gridAlpha = 0.0;   // minor + major grid lines — these get the grazing fade
    float axisAlpha = 0.0;   // X / Z coloured axis rules — orientation reference, no grazing fade
    vec3  axisColor = vec3(0.0);

    float t = -vNearPoint.y / (vFarPoint.y - vNearPoint.y);
    if (t > 0.0) {
        vec3 worldPos = vNearPoint + t * (vFarPoint - vNearPoint);

        float minor = GridLine(worldPos.xz, uMinorSpacing);
        float major = GridLine(worldPos.xz, uMinorSpacing * uMajorEvery);
        // Density fade: once one screen pixel spans most of a minor cell, those lines can only
        // alias into a flat grey haze — fade them out (major lines are 10x coarser, so they
        // stay resolvable much longer and carry the grid on their own from far away). This is
        // the other half of what makes Unity's grid read cleanly at any zoom.
        float minorPerPx = fwidth(worldPos.x) / max(uMinorSpacing, 1e-4);
        float minorFade = 1.0 - smoothstep(0.25, 2.2, minorPerPx);
        float majorPerPx = fwidth(worldPos.x) / max(uMinorSpacing * uMajorEvery, 1e-4);
        float majorFade = 1.0 - smoothstep(0.5, 2.5, majorPerPx);
        // Base lines are deliberately faint — Unity-style, the grid is a reference not a
        // surface. uOpacity scales the whole thing from Preferences.
        gridAlpha = max(minor * 0.085 * minorFade, major * 0.28 * majorFade) * uOpacity;

        float dist = length(worldPos.xz - uCameraPos.xz);
        float distFade = clamp(1.0 - dist / uFadeDistance, 0.0, 1.0);
        gridAlpha *= distFade * distFade;

        // Coloured axis rules through the origin: the line running along X (world Z = 0) is red,
        // the line running along Z (world X = 0) is blue — matching the gizmo and the Inspector
        // XYZ tints. Thin (~1.3 px, anti-aliased) and saturated so they read as axes, not as a
        // faded grey bar. Cleared in a small patch at the origin (that's the gizmo's, #42) and
        // faded past a long radius so an empty Front view isn't lasered edge to edge (#86).
        if (uShowAxes == 1) {
            float deriv = fwidth(worldPos.x);
            float r = length(worldPos.xz);
            float envelope = smoothstep(0.10, 0.6, r) * (1.0 - smoothstep(40.0, 75.0, r)) * distFade;
            float xLine = AxisLine(worldPos.z, deriv, uAxisThickness) * envelope; // X axis (red)
            float zLine = AxisLine(worldPos.x, deriv, uAxisThickness) * envelope; // Z axis (blue)
            if (xLine >= zLine && xLine > 0.001) { axisColor = vec3(1.0, 0.0, 0.0); axisAlpha = xLine; }
            else if (zLine > 0.001)              { axisColor = vec3(0.0, 0.0, 1.0); axisAlpha = zLine; }
        }
    }

    // The grid lines dissolve as the view tilts toward the horizon so a grazing angle doesn't
    // turn them into a shimmering wall (the main thing Unity's grid does). The coloured axis
    // lines are exempt — they stay readable at any angle.
    gridAlpha *= smoothstep(0.02, 0.28, abs(rd.y));

    // --- Green Y axis: the vertical line {x = 0, z = 0}. Independent of the ground plane, so it
    // reads exactly when the grid can't — looking along the horizon. Closest approach of the
    // eye ray to that line; skipped when the ray is ~parallel to it (looking straight down).
    float yAlpha = 0.0;
    if (uShowAxes == 1) {
        vec3  v = vec3(0.0, 1.0, 0.0);
        float b = dot(rd, v);
        float denom = 1.0 - b * b;
        if (denom > 1e-4) {
            vec3  w0 = vNearPoint;
            float d  = dot(rd, w0);
            float e  = dot(v, w0);
            float sc = (b * e - d) / denom;   // param along the ray
            float yc = (e - b * d) / denom;   // y where the ray is nearest the axis
            if (sc > 0.0) {
                vec3  pRay = vNearPoint + sc * rd;
                float toAxis  = length(pRay - vec3(0.0, yc, 0.0));
                float camDist = length(pRay - uCameraPos);
                float hw = (camDist * 0.0016 + 0.004) * (uAxisThickness / 1.3); // px width, scaled to match X/Z
                float core = 1.0 - smoothstep(hw, hw * 2.4, toAxis);
                float originClear = smoothstep(0.05, 0.35, abs(yc));   // tiny gap at the exact origin
                float heightFade = 1.0 - smoothstep(uFadeDistance * 0.3, uFadeDistance * 0.65, abs(yc));
                float camFade = clamp(1.0 - camDist / uFadeDistance, 0.0, 1.0);
                yAlpha = core * originClear * heightFade * camFade * camFade;
            }
        }
    }

    // Composite: grid, then the X/Z axis over it, then the Y axis on top.
    float outAlpha = gridAlpha;
    vec3  outColor = vec3(0.55);
    if (axisAlpha > 0.0) { outColor = axisColor; outAlpha = max(outAlpha, axisAlpha); }
    if (yAlpha > outAlpha) { outColor = vec3(0.0, 1.0, 0.0); outAlpha = yAlpha; } // Y axis (green)

    if (outAlpha <= 0.003) discard;
    FragColor = vec4(outColor, outAlpha);
}
