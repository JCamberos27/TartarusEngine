// Reading the HDRI for the IBL bakes (IblIrradiance, IblPrefilter): its Y rotation and the radiance cap.
uniform float uEnvRotation; // #108 — HDRI Y rotation (radians), same convention as SkyHdri.frag
vec3 RotateEnv(vec3 d) {
    float c = cos(uEnvRotation), s = sin(uEnvRotation);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

uniform float uRadianceClamp; // #277 — >0: cap the brightest channel (keeps hue); removes an HDRI's sun
vec3 ClampRadiance(vec3 c) {
    float m = max(c.r, max(c.g, c.b));
    return (uRadianceClamp > 0.0 && m > uRadianceClamp) ? c * (uRadianceClamp / m) : c;
}
