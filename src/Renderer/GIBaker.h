#pragma once

#include "GIData.h"
#include <entt/entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GIVolumes;
class HdrTarget;
class World;
struct RenderFrameContext;

// --bake-gi <scene>: fills the scene's GI Volumes (GIVolumeComponent) with baked bounce light and writes their .gi
// file (GIData.h). Runs inside the normal frame loop, a slice per frame, through the same scene draw the reflection
// probes' room captures use (RenderFrameContext::ProbeCapture + GIBake).
//
// One PASS per light circuit named by any volume (plus the Outside when a volume asks for it). A pass lights only
// that circuit - LightSwitch turns every other circuit off, the scene's other lights are parked, the sky ambient is
// zero, and the sky itself is black unless it is the Outside pass - waits a few frames for the lights and shadows to
// settle, then at every probe of every volume with that layer renders six small cube faces of the room's radiance,
// reads them back and projects them to L1 SH (GI::Projector). Bounce two repeats the pass with bounce one's result
// shining as GI (GIVolumes::SetOverride + SetSolo), so the probes see light that has bounced twice.
//
// The first pass also marks probes that see too many back faces of one-sided surfaces (inside a wall or a sofa):
// their light is replaced by their valid neighbours' (GI::FillInvalid) in every layer.
class GIBaker {
public:
    struct Options {
        std::string Only;           // bake only volumes whose name contains this ("" = all)
        int FaceSize = 32;          // cube face texels
        int Bounces = 2;
        float InvalidBackfaces = 0.2f; // a probe seeing more than this share of back faces (by solid angle) is invalid
        float Emissive = 0.0f;      // emission kept in the captures (the light itself already lights the room)
        int ProbesPerFrame = 24;    // the frame's own views and shadows cost the same however many probes it does
        float ClipMargin = 8.0f;    // metres round the volumes that the captures still draw (the yard through the windows)
        int SettleFrames = 8;       // frames after a lighting change before capturing
        std::string SkipLightPrefix = "Bounce "; // hand-placed fakes of the bounce (house_lights.py FILL_I): never baked
    };

    GIBaker();
    ~GIBaker();

    // Plans the bake from the world's GI Volumes. False (with Error()) when there is nothing to bake.
    bool Begin(World& world, const Options& opt);
    // One frame's share. `draw` renders the scene for a context into the bound target; `moveCamera` puts the view the
    // light gather and the shadow slots are chosen from at a point. Returns true once everything is written (or failed).
    bool Step(World& world, GIVolumes& gi, const std::function<void(const RenderFrameContext&)>& draw,
              const std::function<void(const glm::vec3&)>& moveCamera);
    bool Failed() const { return !m_Error.empty(); }
    const std::string& Error() const { return m_Error; }
    // Progress 0..1 and a line for the log.
    float Progress() const;

    // The SH of one probe's six read-back faces (RGBA float, face-major, GL face order), and the share of its solid angle
    // that was marked a back face (alpha < 0.5). Exposed for the tests.
    static GI::ProbeSH ProjectFaces(const float* rgba, int faceSize, float* backfaceShare);

private:
    struct VolumeJob {
        entt::entity Entity = entt::null;
        std::string Name, BakeFile;
        GI::Box Bounds;
        glm::ivec3 Dims{0};
        std::vector<std::string> Layers; // circuit names, "" = Outside
        std::vector<bool> Valid;         // per probe, from the first pass
        bool ValidKnown = false;
    };
    struct Pass {
        std::string Circuit;
        int Bounce = 0;
    };
    void ApplyLighting(World& world, const Pass& p);
    void RestoreLighting(World& world);
    bool Finish(World& world, GIVolumes& gi);
    int ProbeCount() const;

    Options m_Opt;
    std::vector<VolumeJob> m_Volumes;
    std::vector<Pass> m_Passes;
    // Per bounce, the volumes' bakes as they fill (the last bounce is what's written).
    std::vector<std::shared_ptr<GI::BakeFile>> m_Bakes;
    int m_Pass = -1, m_Vol = 0, m_Probe = 0, m_Settle = 0;
    long long m_Done = 0, m_Total = 0;
    std::unique_ptr<HdrTarget> m_Target;
    std::vector<float> m_Faces;
    unsigned int m_Atlas = 0;            // this frame's faces side by side, read back once
    int m_AtlasW = 0, m_AtlasH = 0;
    std::vector<float> m_AtlasPixels;
    std::string m_Error;
    bool m_Finished = false;
    // Lighting to put back.
    std::vector<entt::entity> m_Parked;   // lights the bake deactivated
    float m_SkyAmbient = 0.0f;
    bool m_LightingSaved = false;
    double m_StartTime = 0.0;
    glm::vec3 m_ClipMin{0.0f}, m_ClipMax{0.0f};
};
