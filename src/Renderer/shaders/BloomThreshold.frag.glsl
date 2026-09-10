#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform float uThreshold;
uniform float uKnee; // soft-knee width as a fraction of uThreshold (0 = hard cutoff)

// Karis/UE4-style soft-knee threshold: a quadratic ramp blends bright pixels in gradually near
// the cutoff instead of a hard edge, avoiding a visible ring around marginal-brightness pixels.
// Colour ratio is preserved so hue doesn't shift as energy is extracted.
void main() {
    vec3 color = texture(uHdr, vUV).rgb;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));

    float knee = uThreshold * uKnee + 1e-5;
    float soft = luma - uThreshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);

    float contribution = max(soft, luma - uThreshold);
    float scale = contribution / max(luma, 1e-4);
    FragColor = vec4(color * scale, 1.0);
}
