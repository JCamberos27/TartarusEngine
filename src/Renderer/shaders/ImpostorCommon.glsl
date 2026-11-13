// Octahedral impostors (ImpostorBake.cpp bakes them, the impostor draw reads them): the atlas is a kFrames x kFrames
// grid of views of the object, each looking at it from the direction its grid point decodes to on the upper
// hemi-octahedron. Grid point (i, j) of N sits at p = (i, j) / (N - 1) * 2 - 1, so the outer ring of frames is the
// horizon. ImpostorBake.cpp mirrors these three functions exactly: change one, change both.

// Unit direction (y >= 0) -> hemi-octahedral point in [-1, 1]^2.
vec2 ImpostorHemiOctEncode(vec3 d) {
    d.y = max(d.y, 0.0);
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    return vec2(d.x + d.z, d.x - d.z);
}

// Hemi-octahedral point in [-1, 1]^2 -> unit direction (y >= 0).
vec3 ImpostorHemiOctDecode(vec2 p) {
    vec3 d = vec3(p.x + p.y, 0.0, p.x - p.y) * 0.5;
    d.y = 1.0 - abs(d.x) - abs(d.z);
    return normalize(d);
}

// A frame's view basis: the camera sits along `d` (object -> camera) and looks back at the centre, `right` and `up`
// span the frame's image (+x right, +y up). Straight down the y axis, right is +x.
void ImpostorFrameBasis(vec3 d, out vec3 right, out vec3 up) {
    right = abs(d.y) < 0.999 ? normalize(cross(vec3(0.0, 1.0, 0.0), d)) : vec3(1.0, 0.0, 0.0);
    up = cross(d, right);
}
