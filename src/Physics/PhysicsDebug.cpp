#include "PhysicsInternal.h"

namespace PhysicsWorld::detail {
// --- #185 debug tooling: settings + history that OUTLIVE a Play session -----------------
// The editor sets these from the Physics panel; they must survive Play->Stop->Play, so they
// can't live in PhysicsState.
unsigned g_DebugDrawFlags = 0;
bool     g_QueryRecording = false;
} // namespace PhysicsWorld::detail

namespace {
constexpr float kContactFade = 1.8f;   // bounce sparks linger ~2 s
} // namespace

namespace PhysicsWorld::detail {
ContactViz g_Contacts[kContactRing];
int g_ContactHead = 0, g_ContactCount = 0;
} // namespace PhysicsWorld::detail

namespace {
constexpr float kQueryFade = 0.5f;
} // namespace

namespace PhysicsWorld::detail {
QueryRecord g_Queries[kQueryRing];
int g_QueryHead = 0, g_QueryCount = 0;

void PushContactViz(const PxVec3& p, const PxVec3& n, float impulse, bool impact) {
    ContactViz& c = g_Contacts[g_ContactHead];
    c = ContactViz{}; c.p = p; c.n = n; c.impulse = impulse; c.impact = impact;
    g_ContactHead = (g_ContactHead + 1) % kContactRing;
    if (g_ContactCount < kContactRing) ++g_ContactCount;
}

void RecordQuery(const QueryRecord& q) {
    if (!g_QueryRecording) return;
    g_Queries[g_QueryHead] = q;
    g_QueryHead = (g_QueryHead + 1) % kQueryRing;
    if (g_QueryCount < kQueryRing) ++g_QueryCount;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
// --- Navigation geometry -----------------------------------------------------------------

int CollectStaticGeometry(std::vector<float>& verts, std::vector<int>& tris, bool includeKinematic) {
    if (!g_State || !g_State->scene) return 0;
    const int before = (int)tris.size() / 3;
    PxActorTypeFlags types = PxActorTypeFlag::eRIGID_STATIC;
    if (includeKinematic) types |= PxActorTypeFlag::eRIGID_DYNAMIC;
    const PxU32 n = g_State->scene->getNbActors(types);
    std::vector<PxActor*> actors(n);
    g_State->scene->getActors(types, actors.data(), n);
    auto addVert = [&](const PxVec3& v) {
        verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
        return (int)verts.size() / 3 - 1;
    };
    auto addBox = [&](const PxTransform& pose, const PxVec3& h) {
        int base = -1;
        for (int i = 0; i < 8; ++i) {
            const PxVec3 c((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
            const int v = addVert(pose.transform(c));
            if (i == 0) base = v;
        }
        // Faces wound counter-clockwise seen from outside (Recast wants up-facing floors CCW from above).
        static const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                     {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
        for (const auto& t : f) { tris.push_back(base + t[0]); tris.push_back(base + t[1]); tris.push_back(base + t[2]); }
    };
    std::vector<PxShape*> shapes;
    for (PxActor* a : actors) {
        auto* ra = a->is<PxRigidActor>();
        if (!ra) continue;
        if (auto* dyn = a->is<PxRigidDynamic>()) {
            if (!(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) continue;
            if (g_State->npcActors.count(ra) || ra == PlayerActor()) continue;
        }
        shapes.resize(ra->getNbShapes());
        ra->getShapes(shapes.data(), (PxU32)shapes.size());
        const PxTransform actorPose = ra->getGlobalPose();
        for (PxShape* sh : shapes) {
            if (sh->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) continue;
            if (!(sh->getFlags() & PxShapeFlag::eSIMULATION_SHAPE)) continue;
            if (BodyPartKindOf(sh->userData) == kBodyPartRagdoll) continue; // a frozen corpse's part (FreezeRagdoll): not ground
            const PxTransform pose = actorPose * sh->getLocalPose();
            const PxGeometry& g = sh->getGeometry();
            switch (g.getType()) {
            case PxGeometryType::eBOX:
                addBox(pose, static_cast<const PxBoxGeometry&>(g).halfExtents);
                break;
            case PxGeometryType::eSPHERE: {
                const float r = static_cast<const PxSphereGeometry&>(g).radius;
                addBox(pose, PxVec3(r));
                break;
            }
            case PxGeometryType::eCAPSULE: {
                const auto& c = static_cast<const PxCapsuleGeometry&>(g);
                addBox(pose, PxVec3(c.halfHeight + c.radius, c.radius, c.radius));
                break;
            }
            case PxGeometryType::eCONVEXMESH: {
                const auto& c = static_cast<const PxConvexMeshGeometry&>(g);
                const PxConvexMesh* m = c.convexMesh;
                const PxVec3* mv = m->getVertices();
                const PxU8* ib = m->getIndexBuffer();
                const int base = (int)verts.size() / 3;
                const PxMat33 sc = c.scale.toMat33();
                for (PxU32 i = 0; i < m->getNbVertices(); ++i) addVert(pose.transform(sc * mv[i]));
                for (PxU32 p = 0; p < m->getNbPolygons(); ++p) {
                    PxHullPolygon poly;
                    if (!m->getPolygonData(p, poly) || poly.mNbVerts < 3) continue;
                    const PxU8* idx = ib + poly.mIndexBase;
                    // The polygon's outward plane normal says which way it faces: wind it to match.
                    const PxVec3 out = pose.rotate(sc.getInverse().getTranspose() * PxVec3(poly.mPlane[0], poly.mPlane[1], poly.mPlane[2]));
                    for (PxU32 k = 1; k + 1 < poly.mNbVerts; ++k) {
                        const int i0 = base + idx[0], i1 = base + idx[k], i2 = base + idx[k + 1];
                        const PxVec3 a0(verts[3 * i0], verts[3 * i0 + 1], verts[3 * i0 + 2]);
                        const PxVec3 a1(verts[3 * i1], verts[3 * i1 + 1], verts[3 * i1 + 2]);
                        const PxVec3 a2(verts[3 * i2], verts[3 * i2 + 1], verts[3 * i2 + 2]);
                        const bool flip = (a1 - a0).cross(a2 - a0).dot(out) < 0.0f;
                        tris.push_back(i0); tris.push_back(flip ? i2 : i1); tris.push_back(flip ? i1 : i2);
                    }
                }
                break;
            }
            case PxGeometryType::eTRIANGLEMESH: {
                const auto& t = static_cast<const PxTriangleMeshGeometry&>(g);
                const PxTriangleMesh* m = t.triangleMesh;
                const PxVec3* mv = m->getVertices();
                const int base = (int)verts.size() / 3;
                const PxMat33 sc = t.scale.toMat33();
                for (PxU32 i = 0; i < m->getNbVertices(); ++i) addVert(pose.transform(sc * mv[i]));
                const bool b16 = m->getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
                const void* ib = m->getTriangles();
                for (PxU32 i = 0; i < m->getNbTriangles(); ++i) {
                    int i0, i1, i2;
                    if (b16) { const PxU16* x = static_cast<const PxU16*>(ib) + 3 * i; i0 = x[0]; i1 = x[1]; i2 = x[2]; }
                    else     { const PxU32* x = static_cast<const PxU32*>(ib) + 3 * i; i0 = (int)x[0]; i1 = (int)x[1]; i2 = (int)x[2]; }
                    // Recast walks on triangles whose (v1-v0) x (v2-v0) points up; whichever way a mesh was
                    // wound, its floors are made to face up (an underside under solid geometry is never reached).
                    const PxVec3 a0(verts[3 * (base + i0)], verts[3 * (base + i0) + 1], verts[3 * (base + i0) + 2]);
                    const PxVec3 a1(verts[3 * (base + i1)], verts[3 * (base + i1) + 1], verts[3 * (base + i1) + 2]);
                    const PxVec3 a2(verts[3 * (base + i2)], verts[3 * (base + i2) + 1], verts[3 * (base + i2) + 2]);
                    const bool flip = (a1 - a0).cross(a2 - a0).y < 0.0f;
                    tris.push_back(base + i0);
                    tris.push_back(base + (flip ? i2 : i1));
                    tris.push_back(base + (flip ? i1 : i2));
                }
                break;
            }
            default: break; // planes, heightfields: none in the engine's scenes yet
            }
        }
    }
    return (int)tris.size() / 3 - before;
}
} // namespace PhysicsWorld

namespace PhysicsWorld {
namespace {
// ==================================================================================
// #185 debug tooling — a small, general-use visual debugger
// ==================================================================================

PxVec3 WorldCOM(const PxRigidDynamic* d) {
    return d->getGlobalPose().transform(d->getCMassLocalPose().p);
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld {
PhysicsDebugStats GetDebugStats() {
    PhysicsDebugStats s;
    if (!g_State || !g_State->scene) return s;
    s.Active = true;
    s.Substeps = g_State->lastSubsteps;
    s.StepMillis = g_State->lastStepMillis;
    s.TimeScale = Time::EffectiveTimeScale();
    s.DynamicBodies = (int)g_State->dynamics.size();
    s.KinematicBodies = (int)g_State->kinematics.size();
    s.StaticActors = (int)g_State->staticByEntity.size();
    for (auto& [b, e] : g_State->dynamics) (b->isSleeping() ? s.AsleepBodies : s.AwakeBodies)++;
    s.ContactsThisFrame = (int)g_State->contactEvents.size();
    s.TriggerOverlaps = (int)g_State->triggerOverlaps.size() + (int)g_State->playerTriggers.size();
    s.Joints = (int)g_State->joints.size();
    for (PxJoint* j : g_State->joints)
        if (j && (j->getConstraintFlags() & PxConstraintFlag::eBROKEN)) s.BrokenJoints++;
    const PxVec3 g = g_State->scene->getGravity();
    s.Gravity[0] = g.x; s.Gravity[1] = g.y; s.Gravity[2] = g.z;
    s.FixedStep = FixedStep();
    s.FrameIndex = g_State->frameIndex;
    return s;
}

void     SetDebugDrawFlags(unsigned flags) { g_DebugDrawFlags = flags; }
unsigned GetDebugDrawFlags()               { return g_DebugDrawFlags; }

// Build this frame's debug geometry as pre-coloured, pre-faded RGBA line segments. The renderer
// draws them additively, so alpha is the glow/fade. 7 floats/vertex, 2 verts/line.
int CopyDebugLines(float* out, int maxLines) {
    if (!g_State || !g_State->scene) return 0;
    int n = 0;
    auto L = [&](const PxVec3& a, const PxVec3& b, float r, float g, float bl, float al) {
        if (out && n < maxLines) {
            float* p = out + n * 14;
            p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=r; p[4]=g; p[5]=bl; p[6]=al;
            p[7]=b.x; p[8]=b.y; p[9]=b.z; p[10]=r; p[11]=g; p[12]=bl; p[13]=al;
        }
        ++n;
    };
    auto basis = [](const PxVec3& d, PxVec3& u, PxVec3& v) {
        const PxVec3 a = (std::fabs(d.x) < 0.9f) ? PxVec3(1,0,0) : PxVec3(0,1,0);
        u = d.cross(a); const float m = u.magnitude();
        u = (m > 1e-4f) ? u * (1.0f / m) : PxVec3(0,1,0);
        v = d.cross(u);
    };
    auto arrow = [&](const PxVec3& from, const PxVec3& to, float r, float g, float bl, float al) {
        L(from, to, r, g, bl, al);
        PxVec3 d = to - from; const float len = d.magnitude();
        if (len < 1e-4f) return;
        d = d * (1.0f / len);
        PxVec3 u, v; basis(d, u, v);
        const float h = std::min(0.18f, len * 0.35f);
        for (int s = -1; s <= 1; s += 2) {
            L(to, to - d * h + u * (h * 0.5f * (float)s), r, g, bl, al);
            L(to, to - d * h + v * (h * 0.5f * (float)s), r, g, bl, al);
        }
    };
    auto ring = [&](const PxVec3& c, const PxVec3& u, const PxVec3& v, float rad, int seg,
                    float r, float g, float bl, float al) {
        PxVec3 prev = c + u * rad;
        for (int i = 1; i <= seg; ++i) {
            const float a = (float)i / (float)seg * 6.2831853f;
            const PxVec3 p = c + (u * std::cos(a) + v * std::sin(a)) * rad;
            L(prev, p, r, g, bl, al); prev = p;
        }
    };
    const unsigned f = g_DebugDrawFlags;

    // Palette (pure, high-contrast): contacts + raycasts red (#FF0000); velocity + sleep green
    // (#00FF00). Impulse / speed reads through the fade alpha, not hue, so colour stays saturated.
    const float GR = 0.0f, GG = 1.0f, GB = 0.0f;   // green — velocity, sleep
    const float RR = 1.0f, RG = 0.0f, RB = 0.0f;   // red   — contacts, raycasts

    // --- Contacts & impacts: a fading spark + impulse-scaled normal arrow; firm hits get an
    //     expanding ring. Lingers ~2 s (kContactFade).
    if (f & PDD_Contacts) {
        for (int i = 0; i < g_ContactCount; ++i) {
            const ContactViz& c = g_Contacts[i];
            if (c.age >= kContactFade) continue;
            const float k  = 1.0f - c.age / kContactFade;   // 1 -> 0
            const float al = k;                              // linear fade — stays visible longer
            const float sz = (c.impact ? 0.22f : 0.10f) * (0.55f + 0.45f * k);
            L(c.p - PxVec3(sz,0,0), c.p + PxVec3(sz,0,0), RR, RG, RB, al);
            L(c.p - PxVec3(0,sz,0), c.p + PxVec3(0,sz,0), RR, RG, RB, al);
            L(c.p - PxVec3(0,0,sz), c.p + PxVec3(0,0,sz), RR, RG, RB, al);
            arrow(c.p, c.p + c.n * (0.25f + std::min(1.2f, c.impulse * 0.08f)), RR, RG, RB, al);
            if (c.impact) {
                PxVec3 u, v; basis(c.n, u, v);
                // Shockwave ring: expands modestly, holds full brightness until the last third.
                const float rr = sz * 2.6f * (1.0f + (1.0f - k) * 1.4f);
                const float ra = std::min(1.0f, k * 2.2f);   // only fade near the end
                ring(c.p, u, v, rr,        18, RR, RG, RB, ra);
                ring(c.p, u, v, rr * 0.88f, 18, RR, RG, RB, ra);
            }
        }
    }

    // --- Raycasts / sweeps: bright travelled segment, faint continuation on a miss, a small
    //     burst + normal arrow on a hit. Overlap queries draw as three rings. All red.
    if (f & PDD_Raycasts) {
        for (int i = 0; i < g_QueryCount; ++i) {
            const QueryRecord& q = g_Queries[i];
            if (q.age >= kQueryFade) continue;
            const float k  = 1.0f - q.age / kQueryFade;
            const float al = 0.20f + 0.80f * k;
            if (q.kind == 2) {
                const PxVec3 x(1,0,0), y(0,1,0), z(0,0,1);
                ring(q.origin, x, y, q.radius, 24, RR, RG, RB, al);
                ring(q.origin, y, z, q.radius, 24, RR, RG, RB, al);
                ring(q.origin, z, x, q.radius, 24, RR, RG, RB, al);
                continue;
            }
            const PxVec3 end = q.hit ? q.hitPoint : q.origin + q.dir * q.dist;
            L(q.origin, end, RR, RG, RB, al);
            if (!q.hit) {
                L(end, q.origin + q.dir * q.dist, RR, RG, RB, al * 0.28f);
            } else {
                const float s = 0.13f;
                L(q.hitPoint - PxVec3(s,0,0), q.hitPoint + PxVec3(s,0,0), RR, RG, RB, al);
                L(q.hitPoint - PxVec3(0,s,0), q.hitPoint + PxVec3(0,s,0), RR, RG, RB, al);
                L(q.hitPoint - PxVec3(0,0,s), q.hitPoint + PxVec3(0,0,s), RR, RG, RB, al);
                arrow(q.hitPoint, q.hitPoint + q.hitNormal * 0.5f, RR, RG, RB, al);
            }
        }
    }

    // --- Velocity: a green arrow from each awake body's centre of mass.
    if (f & PDD_Velocity) {
        for (auto& [b, e] : g_State->dynamics) {
            if (b->isSleeping()) continue;
            const PxVec3 v = b->getLinearVelocity();
            if (v.magnitude() < 0.15f) continue;
            const PxVec3 cm = WorldCOM(b);
            arrow(cm, cm + v * 0.16f, GR, GG, GB, 0.95f);
        }
    }

    // --- Sleep: a dim green "z" hovering over each sleeping body.
    if (f & PDD_Sleep) {
        for (auto& [b, e] : g_State->dynamics) {
            if (!b->isSleeping()) continue;
            const PxVec3 t = b->getGlobalPose().p + PxVec3(0, 0.5f, 0);
            L(t + PxVec3(-0.08f, 0.12f, 0), t + PxVec3(0.08f, 0.12f, 0), GR, GG, GB, 0.5f);
            L(t + PxVec3( 0.08f, 0.12f, 0), t + PxVec3(-0.08f, 0.0f, 0), GR, GG, GB, 0.5f);
            L(t + PxVec3(-0.08f, 0.0f, 0),  t + PxVec3(0.08f, 0.0f, 0),  GR, GG, GB, 0.5f);
        }
    }

    // --- The custom character mover's last move (the Player's and every soldier's): what it slid along, the floor, the
    // steps it tried.
    auto drawMove = [&](const MoverDebug& d) {
        for (const auto& pl : d.Planes) {
            const float r = pl.Kind == 2 ? 1.0f : 0.0f, g = pl.Kind == 1 ? 1.0f : 0.3f, b = pl.Kind == 0 ? 1.0f : 0.0f;
            const PxVec3 at = ToPx(pl.Point), nrm = ToPx(pl.Normal);
            PxVec3 u, v;
            basis(nrm, u, v);
            ring(at, u, v, 0.12f, 12, r, g, b, 0.9f);
            arrow(at, at + nrm * 0.25f, r, g, b, 0.9f);
        }
        for (size_t i = 0; i + 1 < d.Creases.size(); i += 2) arrow(ToPx(d.Creases[i]), ToPx(d.Creases[i + 1]), 1.0f, 1.0f, 0.0f, 1.0f);
        for (const auto& st : d.Steps) {
            const PxVec3 at = ToPx(st.At);
            L(at, at + PxVec3(0.0f, std::max(st.Headroom, 0.01f), 0.0f), st.Taken ? 0.0f : 1.0f, st.Taken ? 1.0f : 0.0f, 0.0f, 1.0f);
        }
        for (size_t i = 0; i + 1 < d.Nudges.size(); i += 2) arrow(ToPx(d.Nudges[i]), ToPx(d.Nudges[i + 1]), 0.0f, 1.0f, 1.0f, 1.0f);
        if (d.Floor.Found) {
            const PxVec3 at = ToPx(d.Floor.Hit.Point);
            const float r = d.Floor.Walkable ? 0.0f : 1.0f, g = d.Floor.Walkable ? 1.0f : d.Floor.Perched ? 0.5f : 0.0f;
            ring(at, PxVec3(1, 0, 0), PxVec3(0, 0, 1), 0.06f, 10, r, g, 0.0f, 1.0f);
            L(at, ToPx(d.Feet), r, g, 0.0f, 0.6f);
        }
    };
    if (f & PDD_Movers) {
        if (g_State->playerBody) drawMove(g_State->playerBody->Debug);
        for (const auto& npc : g_State->npcs)
            if (npc.body) drawMove(npc.body->Debug);
    }
    return n;
}

void SetQueryRecording(bool on) { g_QueryRecording = on; if (!on) { g_QueryCount = 0; g_QueryHead = 0; } }
bool GetQueryRecording()        { return g_QueryRecording; }
} // namespace PhysicsWorld
