#pragma once

#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>

class HdrTarget;
class Shader;
class World;

// Puddle-only screen-space reflections (World::PuddleSsr, Render Settings > Rain Wetness).
//
// The rain's standing water (ModelFragment.glsl ApplyWetness) is a mirror, but its reflection came from the probes /
// sky only, so the night streets' puddles never showed the lamps and facades above them. Per view, between the depth
// pre-pass and the opaque pass, a half-res compute (PuddleSsr.comp.glsl) traces the reflected ray of every puddle
// pixel against this frame's depth and fetches last frame's HDR colour where it lands (reprojected). The model shader
// reads the result on unit 18 and replaces its probe / sky reflection with it on puddle pixels by its confidence.
//
// Cost control: pixels off the puddle noise mask or not on flat ground leave after one depth fetch; 20 linear steps +
// 5 refine steps on the rest. Needs the depth pre-pass (Project Settings > Quality); without it, no SSR.
class PuddleSsr {
public:
    PuddleSsr();
    ~PuddleSsr();
    PuddleSsr(const PuddleSsr&) = delete;
    PuddleSsr& operator=(const PuddleSsr&) = delete;

    static constexpr int kSteps = 20;
    static constexpr float kMaxDistance = 40.0f;

    // The half-res result this view's opaque pass may read (the trace below fills it before that pass), or 0 when
    // there's no SSR this frame: the first frame at a size has no previous colour to reflect.
    unsigned int Prepare(const HdrTarget* target);
    // Traces into Prepare()'s texture, and remembers this frame's view-projection for the next frame's reprojection.
    // depthTex: this frame's single-sample depth at the target's size.
    void Trace(const HdrTarget* target, unsigned int depthTex, const glm::mat4& view, const glm::mat4& proj,
               const World& world);
    void WarmUp();
    void Release();

private:
    struct View {
        unsigned int Tex = 0;
        int W = 0, H = 0; // the target's (full) size
        int Frames = 0;   // frames traced at this size; 0 = no previous colour yet
        glm::mat4 PrevViewProj{1.0f};
    };
    std::unordered_map<const HdrTarget*, View> m_Views;
    std::unique_ptr<Shader> m_Program;
    int m_Frame = 0;
};
