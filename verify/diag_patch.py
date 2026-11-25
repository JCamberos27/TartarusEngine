import sys, pathlib
root = pathlib.Path(r"C:\Users\jacob\tbw1s")
def patch(rel, old, new, count=1):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    n = s.count(old)
    if n != count:
        sys.exit(f"{rel}: anchor found {n}x (want {count}): {old[:60]!r}")
    p.write_text(s.replace(old, new), encoding="utf-8")

# --- Model pose resolves/applies while inside FP IK
patch("src/Renderer/ModelAnimation.cpp", '#include "Log.h"\n', '#include "Log.h"\n#include "Core/DiagW1.h"\n#include <chrono>\n')
patch("src/Renderer/ModelAnimation.cpp",
      "    m_AppliedPose = pose;\n    ++m_PoseVersion;",
      "    if (DiagW1::InFpIk) ++DiagW1::Applies;\n    m_AppliedPose = pose;\n    ++m_PoseVersion;")
patch("src/Renderer/ModelAnimation.cpp",
      "void Model::ResolveAppliedPose() const {\n    m_PoseDirty = false;",
      "void Model::ResolveAppliedPose() const {\n    const auto diagT0 = std::chrono::steady_clock::now();\n"
      "    struct DiagEnd { std::chrono::steady_clock::time_point t0; size_t n; ~DiagEnd() { if (DiagW1::InFpIk) { ++DiagW1::Resolves; DiagW1::ResolveNodes += n;"
      " DiagW1::ResolveMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } } diagEnd{diagT0, m_D->Nodes.size()};\n"
      "    m_PoseDirty = false;")

# --- FP IK frame flag + counters
patch("src/App/PlayFrame.cpp", '#include "PlayFrame.h"\n', '#include "PlayFrame.h"\n#include "Core/DiagW1.h"\n')
patch("src/App/PlayFrame.cpp",
      '        PROFILE_SCOPE("First Person IK");\n',
      '        PROFILE_SCOPE("First Person IK");\n        DiagW1::InFpIk = true;\n        ++DiagW1::FpFrames;\n'
      '        struct DiagOff { ~DiagOff() { DiagW1::InFpIk = false; } } diagOff;\n')

# --- FirstPersonBody LateUpdate sub-scopes
fb = "src/Game/FirstPersonBody.cpp"
patch(fb, '#include "Profiler.h"\n', '#include "Profiler.h"\n#include "Core/DiagW1.h"\n')
patch(fb, "    if (m_Mm.Active()) {\n        m_Mm.Pose(m_Models);\n        CaptureMmClip();\n    }",
      "    if (m_Mm.Active()) {\n        PROFILE_SCOPE(\"DIAG mm pose\");\n        m_Mm.Pose(m_Models);\n        CaptureMmClip();\n    }\n"
      "    DiagW1::FpModels += m_Models.size();")
patch(fb, "        left = right = glm::vec3(0.0f);\n        if (haveShoulders) {",
      "        PROFILE_SCOPE(\"DIAG shoulderPair\");\n        ++DiagW1::ShoulderPairs;\n        left = right = glm::vec3(0.0f);\n        if (haveShoulders) {")
patch(fb, "        const float want = viewPitch < glm::radians(-10.0f) && share > 1e-3f ? share * EyeBodyPush(world, camera, eyeWorld, flat, limit) : 0.0f;",
      "        float want = 0.0f;\n        { PROFILE_SCOPE(\"DIAG EyeBodyPush\"); want = viewPitch < glm::radians(-10.0f) && share > 1e-3f ? share * EyeBodyPush(world, camera, eyeWorld, flat, limit) : 0.0f; }")
patch(fb, "        if (mmLayers && unarmed > 1e-3f) KeepHandsFromEye(",
      "        PROFILE_SCOPE(\"DIAG KeepHandsFromEye\");\n        if (mmLayers && unarmed > 1e-3f) KeepHandsFromEye(")
patch(fb, "    PostureMetrics(world, camera, dt);\n}",
      "    { PROFILE_SCOPE(\"DIAG PostureMetrics\"); PostureMetrics(world, camera, dt); }\n}")

# --- spine chain
sp = "src/Game/FirstPersonBodySpine.cpp"
patch(sp, '#include "Profiler.h"\n', '#include "Profiler.h"\n#include "Core/DiagW1.h"\n')
patch(sp, "    // Scratch kept between calls (several chains a frame, each over every piece's full pose).\n",
      "    PROFILE_SCOPE(\"DIAG RotateChain\");\n    ++DiagW1::RotateChains;\n    // Scratch kept between calls (several chains a frame, each over every piece's full pose).\n")
patch(sp, "        parents.resize(pose.size());\n        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);\n        // Only the chain",
      "        ++DiagW1::RotatePieces;\n        parents.resize(pose.size());\n        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);\n        // Only the chain")

# --- foot IK
ft = "src/Game/FirstPersonBodyFeet.cpp"
patch(ft, "        const bool rayHit = PhysicsWorld::RaycastSolid(origin, down, cfg.FootRayLength, filter, hit) && hit.Hit;",
      "        bool rayHit = false;\n        { PROFILE_SCOPE(\"DIAG foot rays\"); rayHit = PhysicsWorld::RaycastSolid(origin, down, cfg.FootRayLength, filter, hit) && hit.Hit; }")
patch(ft, "    for (const auto& mp : m_Models) {\n        Model& m = *mp;\n        thread_local IK::Pose pose;",
      "    PROFILE_SCOPE(\"DIAG foot legs\");\n    for (const auto& mp : m_Models) {\n        Model& m = *mp;\n        thread_local IK::Pose pose;")

# --- sun gather split + padded-cascade reuse simulation
sf = "src/Renderer/ShadowFrame.cpp"
patch(sf, '#include "Core/Log.h"\n', '#include "Core/Log.h"\n#include "Core/DiagW1.h"\n')
patch(sf, "    RenderableBounds(world, casterMin, casterMax);\n    shadowMap.SetSplitLambda",
      "    { PROFILE_SCOPE(\"DIAG RenderableBounds\"); RenderableBounds(world, casterMin, casterMax); }\n    shadowMap.SetSplitLambda")
patch(sf, "    casts.CellCullCount = shadowMap.Count();\n    GatherVisible(world, casts, m_Casting);\n    m_SceneWorld = &world;",
      "    casts.CellCullCount = shadowMap.Count();\n    { PROFILE_SCOPE(\"DIAG sun GatherVisible\"); GatherVisible(world, casts, m_Casting); }\n"
      "    DiagPadCheck(world, shadowMap);\n"
      "    m_SceneWorld = &world;")
patch(sf, "// The cascades fitted to the view (near planes reaching every occluder), the casters gathered once against them,\n",
      r'''// TEMPORARY sizing (diag only): would this frame's cascades fall inside the last re-gather's cascades padded by N texels?
// A miss re-gathers (here: once more, padded, to count what the padding adds). Logged every 100 calls.
void ShadowFrame::DiagPadCheck(World& world, const CascadedShadowMap& shadowMap) {
    PROFILE_SCOPE("DIAG pad check");
    static const float kPadTex[3] = {4.0f, 16.0f, 64.0f};
    static glm::mat4 base[3][CascadedShadowMap::kMaxCascades];
    static bool have[3] = {false, false, false};
    static int baseCount[3] = {0, 0, 0};
    static size_t padCount[3] = {0, 0, 0};
    thread_local std::vector<VisibleRenderable> scratch;
    const int n = shadowMap.Count();
    const float res = (float)shadowMap.Resolution();
    ++DiagW1::SunCalls;
    DiagW1::Casting += m_Casting.size();
    for (int k = 0; k < 3; ++k) {
        const float e = 2.0f * kPadTex[k] / res;
        bool hit = have[k] && baseCount[k] == n, hitXY = hit;
        for (int c = 0; c < n && hitXY; ++c) {
            const glm::mat4 inv = glm::inverse(shadowMap.LightViewProj(c));
            for (int i = 0; i < 8 && hitXY; ++i) {
                glm::vec4 w = inv * glm::vec4((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, 1.0f);
                w /= w.w;
                const glm::vec4 q = base[k][c] * w;
                if (std::abs(q.x) > 1.0f + e || std::abs(q.y) > 1.0f + e) hitXY = hit = false;
                else if (std::abs(q.z) > 1.0f + e) hit = false;
            }
        }
        if (hitXY) ++DiagW1::PadHitXY[k];
        if (hit) { ++DiagW1::PadHit[k]; DiagW1::PadCasting[k] += padCount[k]; continue; }
        have[k] = true;
        baseCount[k] = n;
        Frustum cells[CascadedShadowMap::kMaxCascades];
        const glm::mat4 s = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / (1.0f + e)));
        for (int c = 0; c < n; ++c) {
            base[k][c] = shadowMap.LightViewProj(c);
            cells[c] = Frustum::FromViewProj(s * base[k][c]);
        }
        VisibilityQuery q;
        q.ShadowCasters = true;
        q.CellCull = cells;
        q.CellCullCount = n;
        GatherVisible(world, q, scratch);
        padCount[k] = scratch.size();
        DiagW1::PadCasting[k] += padCount[k];
    }
    if (DiagW1::SunCalls % 100 == 0) {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "DIAGW1 sun calls=%llu casting=%llu | pad4 hit=%llu xy=%llu cast=%llu | pad16 hit=%llu xy=%llu cast=%llu | pad64 hit=%llu xy=%llu cast=%llu"
                      " || fp frames=%llu models=%llu applies=%llu resolves=%llu nodes=%llu resolveMs=%.3f chains=%llu chainPieces=%llu shoulderPairs=%llu",
                      (unsigned long long)DiagW1::SunCalls, (unsigned long long)DiagW1::Casting,
                      (unsigned long long)DiagW1::PadHit[0], (unsigned long long)DiagW1::PadHitXY[0], (unsigned long long)DiagW1::PadCasting[0],
                      (unsigned long long)DiagW1::PadHit[1], (unsigned long long)DiagW1::PadHitXY[1], (unsigned long long)DiagW1::PadCasting[1],
                      (unsigned long long)DiagW1::PadHit[2], (unsigned long long)DiagW1::PadHitXY[2], (unsigned long long)DiagW1::PadCasting[2],
                      (unsigned long long)DiagW1::FpFrames, (unsigned long long)DiagW1::FpModels, (unsigned long long)DiagW1::Applies,
                      (unsigned long long)DiagW1::Resolves, (unsigned long long)DiagW1::ResolveNodes, DiagW1::ResolveMs,
                      (unsigned long long)DiagW1::RotateChains, (unsigned long long)DiagW1::RotatePieces, (unsigned long long)DiagW1::ShoulderPairs);
        Log::Info(line);
    }
}

// The cascades fitted to the view (near planes reaching every occluder), the casters gathered once against them,
''')
patch("src/Renderer/ShadowFrame.h", "    void GatherSunCasters(", "    void DiagPadCheck(World& world, const CascadedShadowMap& shadowMap); // TEMPORARY sizing\n    void GatherSunCasters(")
print("patched")
