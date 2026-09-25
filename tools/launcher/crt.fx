// The launch screen's CRT: a curved tube with RGB convergence error, phosphor glow, scanlines,
// an aperture grille, a slow rolling refresh band, flicker, grain and a vignette.
//
// WPF pixel shader (ps_3_0). Rebuild crt.ps after editing (Windows SDK):
//   fxc /nologo /T ps_3_0 /E main /O3 /Fo crt.ps crt.fx

sampler2D input : register(s0);
float time : register(c0);      // seconds
float2 size : register(c1);     // the rendered size in device pixels

// Barrel distortion: the picture bulges like the face of a tube.
float2 Curve(float2 uv)
{
    uv = uv * 2.0 - 1.0;
    float2 offset = abs(uv.yx) / float2(6.0, 4.6);
    uv = uv + uv * offset * offset;
    return uv * 0.5 + 0.5;
}

float3 Tap(float2 uv) { return tex2D(input, uv).rgb; }

float4 main(float2 screen : TEXCOORD) : COLOR
{
    float2 uv = Curve(screen);
    // Past the glass: the black of the bezel, with a soft falloff at the tube's edge.
    float2 edge = smoothstep(0.0, 0.006, uv) * smoothstep(0.0, 0.006, 1.0 - uv);
    if (edge.x * edge.y <= 0.0) return float4(0, 0, 0, 1);

    float2 px = 1.0 / size;
    float2 fromCentre = uv - 0.5;

    // Convergence error: red and blue slip apart toward the edges.
    float spread = (1.2 + 5.0 * dot(fromCentre, fromCentre)) * px.x;
    float3 col;
    col.r = tex2D(input, uv + float2(spread, 0)).r;
    col.g = tex2D(input, uv).g;
    col.b = tex2D(input, uv - float2(spread, 0)).b;

    // Phosphor glow: a near ring and a wider, fainter one.
    float3 glow = Tap(uv + px * float2(2.0, 0)) + Tap(uv - px * float2(2.0, 0))
                + Tap(uv + px * float2(0, 2.0)) + Tap(uv - px * float2(0, 2.0));
    float3 halo = Tap(uv + px * float2(5.0, 3.0)) + Tap(uv + px * float2(-5.0, 3.0))
                + Tap(uv + px * float2(5.0, -3.0)) + Tap(uv + px * float2(-5.0, -3.0))
                + Tap(uv + px * float2(9.0, 0)) + Tap(uv - px * float2(9.0, 0));
    col += glow * 0.06 + halo * 0.03;

    // Scanlines: a fixed 540-line picture, the same on any monitor, never finer than 2.5 device
    // pixels a line (below that they'd alias). Bright lines bloom over the gap.
    float lines = min(540.0, size.y / 2.5);
    float scan = sin(uv.y * lines * 6.2831853);
    float lum = dot(col, float3(0.3, 0.59, 0.11));
    col *= lerp(0.62, 1.0, saturate(0.5 + 0.5 * scan + lum * 0.35));

    // Aperture grille: 720 red-green-blue triads across (at least 3 device pixels each), as
    // smooth stripes so they don't shimmer when the triad isn't a whole number of pixels.
    float triads = min(720.0, size.x / 3.0);
    float f = frac(screen.x * triads);
    float3 mask = 0.8 + 0.2 * float3(cos(6.2831853 * (f - 0.1667)), cos(6.2831853 * (f - 0.5)), cos(6.2831853 * (f - 0.8333)));
    col *= mask * 1.12;

    // A slow refresh band rolling down the glass, and the tube's faint flicker.
    float band = frac(uv.y - time * 0.11);
    col *= 1.0 + 0.06 * smoothstep(0.88, 1.0, band) * (1.0 - smoothstep(0.995, 1.0, band));
    col *= 0.975 + 0.025 * sin(time * 113.0) * sin(time * 7.3);

    // Grain.
    float grain = frac(sin(dot(screen * size + frac(time * 13.7) * 91.0, float2(12.9898, 78.233))) * 43758.5453);
    col += (grain - 0.5) * 0.028;

    // Vignette and the glass: a dark, slightly cool black and a soft highlight at the top left.
    float vignette = pow(saturate(16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y)), 0.28);
    col = col * vignette + float3(0.010, 0.012, 0.015) * vignette;
    float2 shine = uv - float2(0.22, 0.12);
    col += 0.018 * saturate(1.0 - length(shine * float2(1.0, 1.6)) * 2.2);

    return float4(saturate(col) * edge.x * edge.y, 1);
}
