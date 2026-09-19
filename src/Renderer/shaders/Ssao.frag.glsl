#version 460 core
in vec2 vUV;
out float FragColor;

uniform sampler2D uDepth;
uniform sampler2D uNoise;
uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform vec2 uScreenSize;
uniform vec3 uKernel[32];
uniform float uRadius;
uniform float uBias;

// Reconstruct view-space position from a screen UV and hardware depth [0,1].
vec3 ReconstructViewPos(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProjection * clip;
    return view.xyz / view.w;
}

void main() {
    float depth = texture(uDepth, vUV).r;
    // Sky / far-plane pixels have depth ~1.0 — no geometry, no occlusion.
    if (depth >= 0.9999) { FragColor = 1.0; return; }

    vec3 fragPos = ReconstructViewPos(vUV, depth);

    // Reconstruct view-space normal from depth derivatives, then orient it toward the eye.
    // "Toward the eye" means against the view RAY to this pixel (fragPos, the eye is at the
    // origin), not the camera's forward axis: testing N.z flipped the ground's normal into the
    // floor whenever the camera pitched above level (the ground normal then points away along
    // view z while still facing the eye), so the whole hemisphere sampled inside geometry and
    // the floor went dark just from tilting the camera up.
    vec3 N = normalize(cross(dFdx(fragPos), dFdy(fragPos)));
    if (dot(N, fragPos) > 0.0) N = -N;

    // Randomise the TBN tangent frame using the tiling noise texture.
    vec3 rvec = normalize(vec3(
        texture(uNoise, vUV * (uScreenSize / 4.0)).rg * 2.0 - 1.0, 0.0));
    vec3 T = normalize(rvec - N * dot(rvec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < 32; ++i) {
        vec3 samplePos = fragPos + TBN * uKernel[i] * uRadius;

        // Project sample position to screen.
        vec4 offset = uProjection * vec4(samplePos, 1.0);
        vec2 sampleUV = offset.xy / offset.w * 0.5 + 0.5;

        float sampleDepth = texture(uDepth, sampleUV).r;
        vec3  sampleView  = ReconstructViewPos(sampleUV, sampleDepth);

        // Range check: suppress contributions from geometry far behind the surface.
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / abs(fragPos.z - sampleView.z));

        // In OpenGL view space z increases toward the camera.
        // If the scene surface at sampleUV is at or closer than samplePos → occlusion.
        occlusion += (sampleView.z >= samplePos.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }

    FragColor = 1.0 - (occlusion / 32.0);
}
