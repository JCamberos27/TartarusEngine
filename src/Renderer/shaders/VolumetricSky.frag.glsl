#version 460 core
// The volumetric fog over the sky: the integrated volume's last slice (everything between the eye
// and the fog's far distance). Blended as dst * a + rgb (GL_ONE, GL_SRC_ALPHA), the same
// c * transmittance + in-scatter every lit surface applies in ModelShading.glsl.
out vec4 FragColor;
layout(binding = 6) uniform sampler3D uVolume;
uniform vec4 uViewport; // x, y, width, height of this view in the framebuffer
uniform float uLastSlice; // texture z of the last slice's centre

void main() {
    vec2 uv = (gl_FragCoord.xy - uViewport.xy) / uViewport.zw;
    FragColor = textureLod(uVolume, vec3(uv, uLastSlice), 0.0);
}
