#version 460 core
layout(location=0) in vec4 aPosSize;
layout(location=1) in vec4 aColor;
layout(location=2) in vec4 aAxis;
layout(location=3) in vec4 aParams; // rotation, frame blend, mask channel / flame seed, alignment
layout(location=4) in vec4 aAtlas;  // columns, rows, frame A, frame B
layout(location=5) in vec4 aPivot;
layout(location=6) in vec4 aSmoke; // density, noise scale, turbulence, softness
layout(location=7) in vec4 aSmokeMotion; // particle age, stable seed, evolution speed
uniform mat4 uView,uProj;
out vec2 vCorner;
out vec4 vColor;
flat out vec4 vAtlas;
flat out vec4 vParams;
flat out vec4 vSmoke;
flat out vec4 vSmokeMotion;
const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
vec3 safeSide(vec3 axis,vec3 eye) {
    vec3 side=cross(axis,eye);
    if(dot(side,side)<1e-10)side=cross(axis,abs(axis.y)<.9?vec3(0,1,0):vec3(1,0,0));
    return normalize(side);
}
void main() {
    vec2 corner=corners[gl_VertexID],c=corner-aPivot.xy;
    vec4 pos=uView*vec4(aPosSize.xyz,1);
    float mode=aParams.w;
    if(mode==1.0 || mode==2.0) {
        vec3 axis=normalize(mat3(uView)*aAxis.xyz);
        vec3 side=safeSide(axis,-pos.xyz);
        float along=mode==1.0?(c.y*.5+.48)*aAxis.w:c.y*.5*aAxis.w;
        pos.xyz+=axis*along+side*c.x*.5*aPosSize.w;
    } else {
        float angle=aParams.x;
        vec2 rotated=mat2(cos(angle),sin(angle),-sin(angle),cos(angle))*c;
        if(mode==3.0)pos+=uView*vec4(rotated.x*aPosSize.w*.5,0,rotated.y*aPosSize.w*.5,0);
        else pos.xy+=rotated*aPosSize.w*.5;
    }
    gl_Position=uProj*pos;vCorner=corner;vColor=aColor;vAtlas=aAtlas;vParams=aParams;
    vSmoke=aSmoke;vSmokeMotion=aSmokeMotion;
}
