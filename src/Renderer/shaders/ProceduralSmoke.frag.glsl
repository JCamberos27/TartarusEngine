#version 460 core
in vec2 vCorner;
in vec4 vColor;
flat in vec4 vSmoke;
flat in vec4 vSmokeMotion;
out vec4 FragColor;

float hash(vec2 p) {
    vec3 q=fract(vec3(p.xyx)*vec3(.1031,.1030,.0973));
    q+=dot(q,q.yxz+33.33);
    return fract((q.x+q.y)*q.z);
}
float valueNoise(vec2 p) {
    vec2 cell=floor(p),f=fract(p);
    f=f*f*f*(f*(f*6.0-15.0)+10.0);
    return mix(mix(hash(cell),hash(cell+vec2(1,0)),f.x),
               mix(hash(cell+vec2(0,1)),hash(cell+vec2(1,1)),f.x),f.y);
}
float simpleNoise(vec2 uv,float scale) {
    float sum=0.0,weight=.125;
    for(int i=0;i<3;++i) {
        sum+=weight*valueNoise(uv*scale);
        scale*=.5;weight*=2.0;
    }
    return sum/.875;
}
vec2 gradient(vec2 cell) {
    float angle=hash(cell)*6.2831853;
    return vec2(cos(angle),sin(angle));
}
float gradientNoise(vec2 p) {
    vec2 cell=floor(p),f=fract(p);
    vec2 blend=f*f*f*(f*(f*6.0-15.0)+10.0);
    float n=mix(mix(dot(gradient(cell),f),dot(gradient(cell+vec2(1,0)),f-vec2(1,0)),blend.x),
                mix(dot(gradient(cell+vec2(0,1)),f-vec2(0,1)),dot(gradient(cell+vec2(1,1)),f-vec2(1,1)),blend.x),blend.y);
    return clamp(.5+n*.75,0.0,1.0);
}
float lobe(vec2 p,vec2 center,vec2 extent) {
    vec2 q=(p-center)/extent;
    return exp(-2.0*dot(q,q));
}
void main() {
    // Adapted from Donion Tech's procedural smoke graph:
    // https://www.youtube.com/watch?v=5FW17vL3P3M
    // Two scrolling simple-noise layers (20/10) multiplied by gradient noise (4).
    float seed=vSmokeMotion.y*999.0;
    float age=vSmokeMotion.x*vSmokeMotion.z;
    vec2 direction=(vec2(hash(vec2(seed,1.7)),hash(vec2(seed,8.3)))-.5)*.6;
    float angle=(hash(vec2(seed,15.2))-.5)*20.0;
    mat2 rotation=mat2(cos(angle),sin(angle),-sin(angle),cos(angle));
    vec2 uv=rotation*(vCorner*.5+direction*(seed+age))+.5;
    vec2 curl=vec2(valueNoise(uv*3.0),valueNoise(uv*3.0+vec2(19.1,7.3)))-.5;
    vec2 domain=uv+curl*vSmoke.z*.3;
    float scale=vSmoke.y/3.0;
    float fine=simpleNoise(domain,20.0*scale);
    float medium=simpleNoise(domain,10.0*scale);
    float broad=gradientNoise(domain*4.0*scale);
    float cloud=fine*medium*broad;

    // Replace the tutorial's sampled particle mask with asymmetric procedural lobes.
    // There is no sphere envelope or radial cutoff to stamp a round puff into the smoke.
    vec2 maskPos=rotation*vCorner+curl*vSmoke.z*.6;
    float softness=mix(.7,1.3,vSmoke.w);
    vec2 offset=(vec2(hash(vec2(seed,31.7)),hash(vec2(seed,42.1)))-.5)*.3;
    float mask=lobe(maskPos,offset,vec2(.72,.42)*softness);
    mask=max(mask,.8*lobe(maskPos,vec2(.32,.24)+offset,vec2(.42,.35)*softness));
    mask=max(mask,.65*lobe(maskPos,vec2(-.37,.17)-offset,vec2(.38,.5)*softness));
    vec2 edge=1.0-smoothstep(vec2(.72),vec2(1.0),abs(vCorner));
    float opacity=clamp(cloud*vSmoke.x*3.0,0.0,1.0)*mask*edge.x*edge.y*vColor.a;
    if(opacity<.001)discard;
    FragColor=vec4(vColor.rgb,opacity);
}
