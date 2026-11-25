import sys, pathlib
# VERIFY-ONLY (diag/w1-pieces-compare, never PR'd): does every body piece carry the driver's exact local pose on every bone
# the IK touches and every ancestor of those bones (the inputs a once-on-the-driver solve would need)? Checked at
# A = before foot IK, B = before spine + shoulders, C = end of LateUpdate. Logs every 600 frames + per-piece counts.
root = pathlib.Path(r"C:\Users\jacob\tbw1s")
def patch(rel, old, new):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    if s.count(old) != 1:
        sys.exit(f"{rel}: anchor x{s.count(old)}: {old[:70]!r}")
    p.write_text(s.replace(old, new), encoding="utf-8")

fb = "src/Game/FirstPersonBody.cpp"
patch(fb, '#include "Profiler.h"\n', '#include "Profiler.h"\n#include "Log.h"\n#include <cstring>\n#include <map>\n')
patch(fb, "void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,",
r'''namespace {
struct PieceCmp {
    unsigned long long checks = 0, bad = 0, nameBad = 0, missing = 0;
    double maxCm = 0.0, maxDeg = 0.0, maxScale = 0.0;
    std::string worstBone;
};
std::map<std::string, PieceCmp> g_PieceCmp[3];
unsigned long long g_PieceFrames = 0;
} // namespace
void FirstPersonBody::DiagComparePieces(const World& world, int at) {
    static const char* const kTouched[] = {"pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05",
                                           "thigh_l", "thigh_r", "calf_l", "calf_r", "foot_l", "foot_r", "ball_l", "ball_r",
                                           "clavicle_l", "clavicle_r", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r",
                                           "hand_l", "hand_r", "head", "neck_01"};
    const Model* drv = nullptr;
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k)
        if (m_Pieces[k] == m_Driver && m_Models[k]) drv = m_Models[k].get();
    if (!drv) return;
    const IK::Pose& dp = drv->AppliedLocalPose();
    if ((int)dp.size() != drv->NodeCount()) return;
    for (size_t k = 0; k < m_Models.size() && k < m_Pieces.size(); ++k) {
        if (!m_Models[k] || m_Models[k].get() == drv) continue;
        const Model& m = *m_Models[k];
        const IK::Pose& mp = m.AppliedLocalPose();
        if ((int)mp.size() != m.NodeCount()) continue;
        const auto* nm = world.Registry.try_get<NameComponent>(m_Pieces[k]);
        PieceCmp& pc = g_PieceCmp[at][nm ? nm->Name : std::string("piece") + std::to_string(k)];
        for (const char* b : kTouched) {
            int nd = drv->NodeIndex(Bone(b)), nk = m.NodeIndex(Bone(b));
            if (nk < 0) continue;           // the piece hasn't that bone: nothing to copy
            if (nd < 0) { ++pc.missing; continue; }
            while (nd >= 0 && nk >= 0) {    // the bone and every ancestor
                ++pc.checks;
                if (drv->NodeName(nd) != m.NodeName(nk)) { ++pc.nameBad; break; }
                const LocalTRS& a = dp[(size_t)nd];
                const LocalTRS& c = mp[(size_t)nk];
                if (std::memcmp(&a, &c, sizeof(LocalTRS)) != 0) {
                    ++pc.bad;
                    const double cm = 100.0 * glm::length(a.T - c.T);
                    const double deg = glm::degrees(2.0 * std::acos(std::min(1.0, (double)std::abs(glm::dot(a.R, c.R)))));
                    const double sc = glm::length(a.S - c.S);
                    if (cm > pc.maxCm || deg > pc.maxDeg) pc.worstBone = drv->NodeName(nd);
                    pc.maxCm = std::max(pc.maxCm, cm);
                    pc.maxDeg = std::max(pc.maxDeg, deg);
                    pc.maxScale = std::max(pc.maxScale, sc);
                }
                nd = drv->NodeParent(nd);
                nk = m.NodeParent(nk);
            }
            if ((nd < 0) != (nk < 0)) ++pc.nameBad; // chains of different depth
        }
    }
    if (at == 2 && ++g_PieceFrames % 600 == 0) {
        static const char* const kAt[3] = {"A-preFoot", "B-preSpine", "C-end"};
        for (int a = 0; a < 3; ++a)
            for (const auto& [name, pc] : g_PieceCmp[a])
                Log::Info("PIECESCMP frames=" + std::to_string(g_PieceFrames) + " at=" + kAt[a] + " piece=" + name + " checks=" +
                          std::to_string(pc.checks) + " bad=" + std::to_string(pc.bad) + " nameBad=" + std::to_string(pc.nameBad) +
                          " missing=" + std::to_string(pc.missing) + " maxCm=" + std::to_string(pc.maxCm) + " maxDeg=" +
                          std::to_string(pc.maxDeg) + " maxScale=" + std::to_string(pc.maxScale) + " worst=" + pc.worstBone);
    }
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,''')
patch(fb, "    {\n    PROFILE_SCOPE(\"FPB foot IK\");\n", "    DiagComparePieces(world, 0);\n    {\n    PROFILE_SCOPE(\"FPB foot IK\");\n")
patch(fb, "    PROFILE_SCOPE(\"FPB spine + shoulders\");\n", "    DiagComparePieces(world, 1);\n    PROFILE_SCOPE(\"FPB spine + shoulders\");\n")
patch(fb, "    PostureMetrics(world, camera, dt);\n}", "    PostureMetrics(world, camera, dt);\n    DiagComparePieces(world, 2);\n}")
patch("src/Game/FirstPersonBody.h", "    const std::string& Bone(const std::string& standard) const {",
      "    void DiagComparePieces(const World& world, int at); // VERIFY-ONLY\n    const std::string& Bone(const std::string& standard) const {")
# The same pitch sweep as #1256's verify (look up/down) under the npc-test script.
patch("src/App/PlayFrame.cpp", "        firstPersonBody.LateUpdate(world, player.Cam, gameDt,",
"""        { static double vT = 0.0; vT += gameDt; player.Cam.Pitch = 80.0f * (float)std::sin(vT * 6.2831853 / 4.0); }
        firstPersonBody.LateUpdate(world, player.Cam, gameDt,""")
print("pieces patch applied")
