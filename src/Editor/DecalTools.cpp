#include "DecalTools.h"
#include "../Renderer/DecalPalette.h" // DecalSites

#include <algorithm>
#include <cmath>

namespace DecalTools {
namespace {

constexpr float kPi = 3.14159265358979f;

// Any horizontal unit vector across `n` (a surface tangent).
glm::vec3 TangentOf(const glm::vec3& n) {
    const glm::vec3 ref = std::abs(n.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(ref, n));
}

glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float l2 = glm::dot(v, v);
    return l2 > 1e-12f ? v * (1.0f / std::sqrt(l2)) : fallback;
}

} // namespace

uint32_t Mix(uint32_t a, uint32_t b) {
    uint32_t z = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

// --- BrushStroke -------------------------------------------------------------------------------
void BrushStroke::Begin(uint32_t seed, const BrushSettings& settings) {
    m_S = settings;
    m_S.Spacing = std::max(m_S.Spacing, 0.02f);
    m_S.Radius = std::max(m_S.Radius, 0.0f);
    m_S.Density = std::clamp(m_S.Density, 0.0f, 8.0f);
    m_Seed = seed ? seed : 1u;
    m_Index = 0;
    m_Placed = 0;
    m_Started = false;
    m_Carry = 0.0f;
}

int BrushStroke::Emit(const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir, BrushDab* out, int cap) {
    Rng rng(Mix(m_Seed, m_Index++));
    int count = (int)m_S.Density;
    if (rng.Unit() < m_S.Density - (float)count) ++count;
    const glm::vec3 n = m_S.AlignToSurface ? normal : m_FirstNormal;
    const glm::vec3 t = TangentOf(n), b = glm::cross(n, t);
    int made = 0;
    for (int i = 0; i < count && made < cap; ++i) {
        // Uniform over the disk: the radius is the root of the roll.
        const float r = m_S.Radius * std::sqrt(rng.Unit()), a = rng.Unit() * 2.0f * kPi;
        BrushDab d;
        d.Point = point + t * (r * std::cos(a)) + b * (r * std::sin(a));
        d.Normal = n;
        d.Right = t;
        d.Direction = dir;
        d.Spin = glm::radians(rng.Signed() * m_S.RotationJitter);
        if (m_S.FollowStroke && glm::dot(dir, dir) > 0.5f)
            d.Spin += std::atan2(glm::dot(dir, b), glm::dot(dir, t)) - 0.5f * kPi; // decal +Y along the heading
        d.Scale = std::max(m_S.Size * (1.0f + rng.Signed() * m_S.SizeJitter), 0.05f);
        d.PickRoll = rng.Unit();
        d.Opacity = std::clamp(m_S.Opacity, 0.0f, 1.0f);
        d.Seed = rng.Next() & 0x7FFFFFFFu;
        out[made++] = d;
    }
    m_Placed += made;
    return made;
}

int BrushStroke::Advance(const glm::vec3& point, const glm::vec3& normal, BrushDab* out, int cap) {
    cap = std::min(cap, kMaxPerAdvance);
    const glm::vec3 n = SafeNormalize(normal, glm::vec3(0, 1, 0));
    if (!m_Started) {
        m_Started = true;
        m_Last = point;
        m_LastNormal = n;
        m_FirstNormal = n;
        m_Carry = 0.0f;
        return cap > 0 ? Emit(point, n, glm::vec3(0.0f), out, cap) : 0;
    }
    const glm::vec3 seg = point - m_Last;
    const float len = glm::length(seg);
    if (len < 1e-5f) return 0;
    const glm::vec3 dir = seg / len;

    int made = 0;
    float at = m_S.Spacing - m_Carry; // distance along this segment to the next dab
    float lastDab = -m_Carry;
    while (at <= len && made < cap) {
        const float u = at / len;
        const glm::vec3 p = m_Last + seg * u;
        const glm::vec3 nn = SafeNormalize(glm::mix(m_LastNormal, n, u), n);
        made += Emit(p, nn, dir, out + made, cap - made);
        lastDab = at;
        at += m_S.Spacing;
    }
    // A jump longer than the cap allows drops the rest of the segment rather than carrying a debt.
    m_Carry = at <= len ? 0.0f : len - lastDab;
    m_Last = point;
    m_LastNormal = n;
    return made;
}

// --- Auto-grime --------------------------------------------------------------------------------
namespace {

constexpr int kMaxRows = 18;
constexpr int kMaxCols = 64;

enum class Depth : uint8_t { Wall, Deep, Shallow, Other };

struct Column {
    Depth State[kMaxRows];
    glm::vec3 Point[kMaxRows];
    bool Door = false, Window = false, Pipe = false;
    float SillH = 0.0f, DoorTopH = 0.0f, PipeH = 0.0f; // metres above the floor
};

} // namespace

GrimeSummary PlanGrime(const GrimeProbe& probe, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                       const GrimeSettings& settings, uint32_t seed, std::vector<GrimeSite>& out) {
    out.clear();
    GrimeSummary sum;
    Rng rng(Mix(seed, 0x6A1Du));
    const glm::vec3 centre = 0.5f * (boundsMin + boundsMax), half = 0.5f * (boundsMax - boundsMin);
    const float density = std::clamp(settings.Density, 0.0f, 2.0f);
    const float spacing = std::max(settings.Spacing, 0.25f);

    auto cast = [&](const glm::vec3& o, const glm::vec3& d, float maxT, GrimeHit& h) {
        ++sum.Rays;
        if (!probe(o, d, maxT, h)) return false;
        if (glm::dot(h.Normal, d) > 0.0f) h.Normal = -h.Normal; // always the side facing the ray
        return true;
    };

    // --- Floor and ceiling: the lowest floor and highest ceiling hit from five spots, so a table or
    // a lamp under them doesn't move the lines.
    float floorY = boundsMin.y, ceilY = boundsMax.y;
    {
        static const float kX[5] = {0.0f, -0.5f, 0.5f, -0.5f, 0.5f}, kZ[5] = {0.0f, -0.5f, -0.5f, 0.5f, 0.5f};
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 5; ++i) {
            const glm::vec3 p(centre.x + kX[i] * half.x, centre.y, centre.z + kZ[i] * half.z);
            GrimeHit h;
            if (cast(p, glm::vec3(0, -1, 0), half.y + 0.5f, h) && h.Normal.y > 0.7f) lo = std::min(lo, h.Point.y);
        }
        // Up from just above the floor, far enough for a whole storey: a selected floor slab alone
        // has bounds too thin to reach its ceiling.
        for (int i = 0; i < 5; ++i) {
            const glm::vec3 p(centre.x + kX[i] * half.x, lo < 1e29f ? lo + 0.5f : centre.y, centre.z + kZ[i] * half.z);
            GrimeHit h;
            if (cast(p, glm::vec3(0, 1, 0), std::max(half.y + 0.5f, 4.5f), h) && h.Normal.y < -0.7f) hi = std::max(hi, h.Point.y);
        }
        if (lo < 1e29f) floorY = lo;
        if (hi > -1e29f) ceilY = hi;
        if (ceilY - floorY < 1.0f) { floorY = boundsMin.y; ceilY = std::max(boundsMax.y, floorY + 2.4f); }
    }
    sum.FloorY = floorY;
    sum.CeilingY = ceilY;
    const float height = ceilY - floorY;

    auto add = [&](uint32_t kind, glm::vec3 p, const glm::vec3& n, const glm::vec3& along, float scale = 1.0f) {
        GrimeSite s;
        s.Point = p;
        s.Normal = n;
        s.Along = along;
        s.Kind = kind;
        s.Height = p.y - floorY;
        s.Scale = scale;
        out.push_back(s);
    };

    // --- Walls: four sides, each a row of columns of rays from the centre line outward.
    const int rows = std::clamp((int)std::ceil((height - 0.3f) / 0.25f) + 1, 2, kMaxRows);
    const float rowStep = (height - 0.3f) / (float)(rows - 1);
    auto rowH = [&](int k) { return 0.15f + rowStep * (float)k; }; // metres above the floor
    const glm::vec3 up(0, 1, 0);
    static const glm::vec3 kDirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

    for (int w = 0; w < 4; ++w) {
        const glm::vec3 d = kDirs[w];
        const bool alongX = d.x != 0.0f;                           // the wall runs along z
        const glm::vec3 lat = alongX ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        const float halfLat = alongX ? half.z : half.x;
        const float halfDepth = alongX ? half.x : half.z;
        const float centreLat = alongX ? centre.z : centre.x;
        const int cols = std::clamp((int)std::ceil(2.0f * halfLat / spacing), 2, kMaxCols);
        const float cell = 2.0f * halfLat / (float)cols;
        const float maxT = halfDepth + 0.8f;

        static thread_local Column cl[kMaxCols];
        static thread_local float depth[kMaxCols][kMaxRows];
        static thread_local bool hitWall[kMaxCols][kMaxRows];
        glm::vec3 wallNormal(-d.x, 0.0f, -d.z);
        std::vector<float> baseline;
        baseline.reserve((size_t)cols * 4);

        for (int i = 0; i < cols; ++i) {
            const float l = centreLat - halfLat + (float)(i + 0.5f) * cell;
            for (int k = 0; k < rows; ++k) {
                glm::vec3 o = centre;
                o.y = floorY + rowH(k);
                o += lat * (l - glm::dot(centre, lat));
                GrimeHit h;
                const bool hit = cast(o, d, maxT, h);
                hitWall[i][k] = hit && glm::dot(h.Normal, -d) > 0.75f;
                depth[i][k] = hit ? h.T : 1e9f;
                cl[i].Point[k] = hit ? h.Point : o + d * maxT;
                if (hitWall[i][k] && rowH(k) > 0.6f && rowH(k) < 1.8f) baseline.push_back(h.T);
                if (hitWall[i][k]) wallNormal = h.Normal;
            }
        }
        if (baseline.empty()) continue; // no wall on this side
        std::nth_element(baseline.begin(), baseline.begin() + baseline.size() / 2, baseline.end());
        const float t0 = baseline[baseline.size() / 2];
        const glm::vec3 n = SafeNormalize(wallNormal, -d);
        const glm::vec3 along = SafeNormalize(glm::cross(up, n), lat);

        // Classify every row of every column against the wall's own depth.
        for (int i = 0; i < cols; ++i) {
            Column& c = cl[i];
            c.Door = c.Window = c.Pipe = false;
            for (int k = 0; k < rows; ++k) {
                const float t = depth[i][k];
                if (t > t0 + 0.10f) c.State[k] = Depth::Deep;
                else if (hitWall[i][k] && std::abs(t - t0) <= 0.10f) c.State[k] = Depth::Wall;
                else if (t < t0 - 0.04f && t > t0 - 0.45f) c.State[k] = Depth::Shallow;
                else c.State[k] = hitWall[i][k] ? Depth::Wall : Depth::Other;
            }
            // Open from the floor up to a metre = a door (or an arch).
            int low = 0, lowDeep = 0;
            for (int k = 0; k < rows && rowH(k) <= 0.9f; ++k, ++low) lowDeep += c.State[k] == Depth::Deep;
            if (low >= 2 && lowDeep == low) {
                c.Door = true;
                int top = 0;
                while (top + 1 < rows && c.State[top + 1] == Depth::Deep) ++top;
                c.DoorTopH = rowH(top);
            } else {
                // Open higher up, two rows or more = a window; the sill is its lowest open row.
                for (int k = 1; k + 1 < rows; ++k)
                    if (rowH(k) >= 0.45f && c.State[k] == Depth::Deep && c.State[k + 1] == Depth::Deep &&
                        c.State[k - 1] != Depth::Deep) { c.Window = true; c.SillH = rowH(k); break; }
            }
            for (int k = 0; k < rows && !c.Door; ++k)
                if (c.State[k] == Depth::Shallow) { c.Pipe = true; c.PipeH = rowH(k); break; }
        }

        // A point on the wall behind column i at `h` metres above the floor.
        auto wallPoint = [&](int i, float h) {
            int best = -1;
            float bestD = 1e9f;
            for (int k = 0; k < rows; ++k)
                if (cl[i].State[k] == Depth::Wall && std::abs(rowH(k) - h) < bestD) { bestD = std::abs(rowH(k) - h); best = k; }
            glm::vec3 p = best >= 0 ? cl[i].Point[best] : glm::vec3(0.0f);
            if (best < 0) {
                p = centre + lat * (centreLat - halfLat + (float)(i + 0.5f) * cell - glm::dot(centre, lat)) + d * t0;
            }
            p.y = floorY + h;
            return p;
        };

        // Which side of this wall's doors the handle is on (hand grime gathers there).
        const bool handleHigh = (Mix(seed, 0xD00Du + (uint32_t)w) & 1u) != 0u;
        const float dens = std::min(density, 1.0f) + 0.25f * std::max(density - 1.0f, 0.0f); // frames: more, not 2x
        for (int i = 0; i < cols; ++i) {
            const Column& c = cl[i];
            const bool doorBefore = i > 0 && cl[i - 1].Door, doorAfter = i + 1 < cols && cl[i + 1].Door;
            const int winNb = (i > 0 && cl[i - 1].Window) ? i - 1 : (i + 1 < cols && cl[i + 1].Window) ? i + 1 : -1;
            if (!c.Door) {
                if (rng.Unit() < 0.85f * density) add(DecalSites::Base, wallPoint(i, rng.Range(0.05f, 0.30f)), n, along);
                // Water creeping down from the ceiling line: sparse, the ceiling isn't a blanket.
                if (rng.Unit() < 0.12f * density) add(DecalSites::Ceiling, wallPoint(i, height - rng.Range(0.10f, 0.35f)), n, along);
            }
            // Under a sill: two or three drips, spread along it and run down the wall.
            if (c.Window && !c.Door && c.SillH > 0.5f) {
                const int drips = rng.Unit() < 0.5f * dens ? 3 : 2;
                for (int j = 0; j < drips; ++j) {
                    const float h = std::max(0.25f, c.SillH - rng.Range(0.10f, 0.65f));
                    add(DecalSites::Window, wallPoint(i, h) + lat * rng.Range(-0.4f, 0.4f), n, along, rng.Range(0.5f, 0.95f));
                }
            }
            // Beside a window: grime up the jamb.
            if (!c.Window && !c.Door && winNb >= 0 && cl[winNb].SillH > 0.5f && rng.Unit() < 0.8f * dens)
                add(DecalSites::Window, wallPoint(i, cl[winNb].SillH + rng.Range(0.0f, 0.8f)) + lat * ((float)(winNb - i) * cell * 0.3f),
                    n, along, rng.Range(0.45f, 0.75f));
            if (c.Door && c.DoorTopH + 0.4f < height && rng.Unit() < 0.7f * density)
                add(DecalSites::Door, wallPoint(i, c.DoorTopH + rng.Range(0.20f, 0.35f)), n, along);
            if (!c.Door && (doorBefore || doorAfter)) {
                const glm::vec3 toDoor = lat * ((doorAfter ? 1.0f : -1.0f) * cell * 0.3f); // hug the frame
                if (doorAfter == handleHigh) {
                    // The handle side: hand-dirt at grabbing height, and a kick scuff low down.
                    add(DecalSites::Door, wallPoint(i, rng.Range(0.85f, 1.0f)) + toDoor, n, along, rng.Range(0.45f, 0.7f));
                    add(DecalSites::Door, wallPoint(i, rng.Range(1.18f, 1.4f)) + toDoor * 0.6f, n, along, rng.Range(0.4f, 0.6f));
                    if (rng.Unit() < 0.8f * dens)
                        add(DecalSites::Door, wallPoint(i, rng.Range(0.12f, 0.35f)) + toDoor, n, along, rng.Range(0.55f, 0.85f));
                } else if (rng.Unit() < 0.8f * dens) {
                    add(DecalSites::Door, wallPoint(i, rng.Range(0.4f, std::min(1.9f, height - 0.5f))) + toDoor, n, along,
                        rng.Range(0.5f, 0.8f));
                }
            }
            if (c.Pipe && rng.Unit() < 0.9f * density)
                add(DecalSites::Pipe, wallPoint(i, std::max(0.2f, c.PipeH - rng.Range(0.30f, 0.45f))), n, along);
        }

        // Open wall: a few irregular patches (a big one with small satellites round it), clean wall between.
        {
            const int patches = (int)std::floor(2.0f * halfLat * density / 8.0f + rng.Unit());
            for (int pIdx = 0; pIdx < patches; ++pIdx) {
                int ci = -1;
                for (int tries = 0; tries < 8 && ci < 0; ++tries) {
                    const int t = (int)(rng.Next() % (uint32_t)cols);
                    if (!cl[t].Door && !cl[t].Window) ci = t;
                }
                if (ci < 0) break;
                const float h = rng.Range(0.8f, std::max(0.9f, height - 0.6f));
                add(DecalSites::Wall, wallPoint(ci, h), n, along, rng.Range(1.25f, 1.8f));
                const int sats = 1 + (int)(rng.Next() % 3u);
                for (int j = 0; j < sats; ++j) {
                    const float off = rng.Signed() * rng.Range(0.6f, 1.5f);
                    const int sj = ci + (int)std::lround(off / cell);
                    if (sj < 0 || sj >= cols || cl[sj].Door || cl[sj].Window) continue;
                    const float sh = std::clamp(h + rng.Signed() * 0.7f, 0.4f, height - 0.3f);
                    add(DecalSites::Wall, wallPoint(sj, sh) + lat * (off - (float)(sj - ci) * cell), n, along, rng.Range(0.3f, 0.65f));
                }
            }
        }

        // Corners: from the wall's two ends, along the wall at a base, a middle and a ceiling height.
        for (int end = 0; end < 2; ++end) {
            const int i = end == 0 ? 0 : cols - 1;
            const glm::vec3 toEnd = end == 0 ? -lat : lat;
            for (int s = 0; s < 3; ++s) {
                const float h = s == 0 ? 0.20f : s == 1 ? rng.Range(0.7f, std::max(0.8f, height - 0.7f)) : height - 0.20f;
                const glm::vec3 wp = wallPoint(i, h);
                GrimeHit ch;
                if (!cast(wp + n * 0.06f, toEnd, cell + 0.6f, ch)) continue;
                if (std::abs(glm::dot(ch.Normal, n)) >= 0.45f) continue; // not a wall across this one
                if (rng.Unit() < 0.8f * density) {
                    glm::vec3 p = ch.Point;
                    p.y = floorY + h;
                    add(DecalSites::Corner, p, n, along);
                }
            }
        }
    }

    // --- Ceiling and floor patches on a coarse grid.
    {
        const float step = spacing * 1.6f;
        const int nx = std::clamp((int)std::ceil(2.0f * half.x / step), 1, 24), nz = std::clamp((int)std::ceil(2.0f * half.z / step), 1, 24);
        for (int gz = 0; gz < nz; ++gz)
            for (int gx = 0; gx < nx; ++gx) {
                const float x = boundsMin.x + (gx + 0.5f) * (2.0f * half.x / nx), z = boundsMin.z + (gz + 0.5f) * (2.0f * half.z / nz);
                GrimeHit h;
                // Ceiling stains only near the walls and corners, where the water comes in.
                const float edge = std::min(std::min(x - boundsMin.x, boundsMax.x - x), std::min(z - boundsMin.z, boundsMax.z - z));
                if (edge < 1.1f && rng.Unit() < 0.25f * density && cast(glm::vec3(x, floorY + 1.0f, z), up, height + 0.5f, h) &&
                    h.Normal.y < -0.7f && h.Point.y > ceilY - 0.5f)
                    add(DecalSites::Ceiling, h.Point, h.Normal, TangentOf(h.Normal));
                if (rng.Unit() < 0.25f * density && cast(glm::vec3(x, ceilY - 0.3f, z), -up, height + 0.5f, h) &&
                    h.Normal.y > 0.7f && h.Point.y < floorY + 0.4f)
                    add(DecalSites::Floor, h.Point, h.Normal, TangentOf(h.Normal));
            }
    }

    // --- Tidy: only the asked-for kinds, none on top of another of its kind, then the cap.
    out.erase(std::remove_if(out.begin(), out.end(), [&](const GrimeSite& s) { return !(settings.Kinds & s.Kind); }), out.end());
    std::vector<GrimeSite> kept;
    kept.reserve(out.size());
    for (const GrimeSite& s : out) {
        bool crowded = false;
        for (const GrimeSite& k : kept) {
            // Small decals (a satellite, a hand smudge) may sit closer together than full-size ones.
            const float r = 0.30f * std::clamp(std::min(k.Scale, s.Scale), 0.5f, 1.0f);
            if (k.Kind == s.Kind && glm::dot(k.Point - s.Point, k.Point - s.Point) < r * r &&
                glm::dot(k.Normal, s.Normal) > 0.9f) { crowded = true; break; }
        }
        if (!crowded) kept.push_back(s);
    }
    if ((int)kept.size() > settings.MaxSites && settings.MaxSites >= 0) {
        // Keep a seeded random subset, in generation order.
        std::vector<uint32_t> order(kept.size());
        for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
        for (uint32_t i = 0; i < (uint32_t)settings.MaxSites; ++i)
            std::swap(order[i], order[i + rng.Next() % ((uint32_t)order.size() - i)]);
        order.resize((size_t)settings.MaxSites);
        std::sort(order.begin(), order.end());
        std::vector<GrimeSite> cut;
        cut.reserve(order.size());
        for (uint32_t i : order) cut.push_back(kept[i]);
        kept.swap(cut);
    }
    out.swap(kept);
    return sum;
}

} // namespace DecalTools
