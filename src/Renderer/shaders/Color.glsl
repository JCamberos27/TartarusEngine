// The sRGB transfer function both ways (IEC 61966-2-1). Guarded: several includers can meet in one program.
#ifndef COLOR_GLSL
#define COLOR_GLSL
vec3 SrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 LinearToSrgb(vec3 c) { // clamped to the displayable range first
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
#endif // COLOR_GLSL
