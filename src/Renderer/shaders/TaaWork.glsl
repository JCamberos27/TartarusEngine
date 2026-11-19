// TAA's working space (Karis 2014): colours compressed by their brightest channel before the resolve blends them,
// so a lone hot pixel can't dominate; TaaSharpen expands back. The two must stay exact inverses.
vec3 ToWork(vec3 c) { c = max(c, vec3(0.0)); return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 FromWork(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
