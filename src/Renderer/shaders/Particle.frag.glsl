#version 460 core
in vec2 vCorner;
in vec4 vColor;
flat in vec4 vAtlas;
flat in vec4 vParams;
uniform sampler2D uParticleTex;
uniform int uTextured;
out vec4 FragColor;
vec2 cellUV(vec2 q,float cell) {
    vec2 grid=max(vAtlas.xy,vec2(1));
    vec2 cellXY=vec2(mod(cell,grid.x),grid.y-1.0-floor(cell/grid.x));
    // Stay half a texel inside each cell to avoid bleeding across the atlas.
    vec2 halfTexel=.5/vec2(textureSize(uParticleTex,0));
    vec2 lo=cellXY/grid+halfTexel,hi=(cellXY+1.0)/grid-halfTexel;
    return mix(lo,hi,q);
}
void main() {
    if(vParams.w==1.0) {
        vec2 q=vCorner*.5+.5;
        float side=step(.5,vParams.z)-.5,chan=floor(4.0*fract(2.0*vParams.z));
        vec4 sampleMask=texture(uParticleTex,vec2(.365+.27*q.x+.5*side,1-q.y));
        float m=clamp(chan<.5?sampleMask.r:chan<1.5?sampleMask.g:chan<2.5?sampleMask.b:sampleMask.a,0,1);
        float a=vColor.a*m;
        vec3 glow=vColor.rgb*m*m*pow(max(1-q.y,1e-4),.1);
        FragColor=vec4(vec3(.12,.115,.11)*a+glow,a);return;
    }
    if(uTextured!=0) {
        vec2 q=vCorner*.5+.5;
        vec4 a=texture(uParticleTex,cellUV(q,vAtlas.z)),b=texture(uParticleTex,cellUV(q,vAtlas.w));
        vec4 texel=mix(a,b,vParams.y);
        if(vParams.z>=1.0)texel=vec4(vec3(1),texel[int(vParams.z)-1]);
        FragColor=texel*vColor;
        if(FragColor.a<.001)discard;
    } else {
        float r2=dot(vCorner,vCorner);if(r2>=1)discard;
        FragColor=vec4(vColor.rgb,vColor.a*(1-smoothstep(0,1,r2)));
    }
}
