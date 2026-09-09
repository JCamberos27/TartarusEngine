uniform vec3 uFaceForward;
uniform vec3 uFaceRight;
uniform vec3 uFaceUp;

vec3 FaceDirection(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0; // [-1,1] across the face
    return normalize(uFaceForward + p.x * uFaceRight + p.y * uFaceUp);
}
