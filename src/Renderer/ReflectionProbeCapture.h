#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "ProbeSH.h"

class HdrTarget;
class IblProbe;
class ReflectionProbeArray;
struct RenderFrameContext;

// Room captures for Reflection Probes (ReflectionProbeComponent::CaptureScene).
//
// ReflectionProbeArray alone only parallax-corrects the sky's specular cube, so indoor metal reflected the sky. This
// renders the scene itself around each capturing probe - six 90-degree faces at kFaceSize, opaque geometry + sky +
// lights, no transparents, particles, view model or post (RenderFrameContext::ProbeCapture) - and GGX-prefilters
// each into one layer of a cube-map array with the sky specular cube's mip layout (IblProbe::PrefilterIntoArray), so
// the model shader samples a captured probe exactly like the sky one (uProbeCubes / uProbeLayer).
//
// Never per frame. Sync() compares the capturing probes' volumes with what was captured and queues only what
// changed: a new scene (or the first frames of one) captures everything, a moved probe recaptures once it has
// stopped moving, and RequestRecapture() (the Inspector's button) redoes them all. Step() spreads the work like the
// sky's amortized IBL bake: per frame at most one face is rendered and one face prefiltered, pipelined over two
// capture cubes, so a probe costs ~6 frames and the whole set ~6 * probes + 6.
class ReflectionProbeCapture {
public:
    static constexpr int kFaceSize = 128;
    static constexpr int kSettleFrames = 3;        // a fresh probe set waits this many frames (the sky IBL bakes first)
    static constexpr double kMoveSettleSeconds = 0.3; // a moved probe waits until it has been still this long

    ReflectionProbeCapture();
    ~ReflectionProbeCapture();
    ReflectionProbeCapture(const ReflectionProbeCapture&) = delete;
    ReflectionProbeCapture& operator=(const ReflectionProbeCapture&) = delete;

    // After probes.Update(world), every frame: plans captures and hands the array (and each ready probe's layer) to
    // `probes`. environmentReady: the sky's IBL is baked (a capture taken before it would hold a black sky).
    void Sync(ReflectionProbeArray& probes, bool environmentReady, double nowSeconds);
    bool Busy() const;
    // One frame's share of the work. `draw` renders the scene for a context into the bound target (main.cpp's
    // drawScene). Leaves the default framebuffer bound; the caller restores its viewport.
    void Step(IblProbe& ibl, ReflectionProbeArray& probes, const std::function<void(const RenderFrameContext&)>& draw);
    // Every capturing probe again (the Inspector's Recapture button). Static so the editor needs no handle.
    static void RequestRecapture();

    int ReadyCount() const;
    int SlotCount() const { return (int)m_Slots.size(); }
    void Release();

    // The view matrix of cube face `face` (GL order +X -X +Y -Y +Z -Z) seen from `pos`, oriented so a render of it
    // copies straight into that face of a GL cube map (the IBL bake's face convention).
    static glm::mat4 FaceView(const glm::vec3& pos, int face);
    static glm::mat4 FaceProj();

    // Which of `want` must be (re)captured against what `have` holds, by slot: true = queue it. Exposed for the tests.
    struct Volume { glm::vec3 Center, HalfSize; };
    static std::vector<bool> Changed(const std::vector<Volume>& have, const std::vector<Volume>& want);

private:
    struct Slot { Volume Vol; bool Ready = false; ProbeSH::Coeffs SH{}; };
    struct Job { int Slot = -1; int Face = 0; int Buf = 0; bool Done = false; };
    void EnsureArray(int layers);
    void EnsureCaptureCubes();

    std::vector<Slot> m_Slots;           // index = cube-array layer
    std::vector<int> m_Queue;            // slots waiting for a capture, in order
    std::vector<Volume> m_Pending;       // the last volumes seen, while they settle
    int m_PendingFrames = 0;
    double m_PendingSince = 0.0;
    Job m_Cap, m_Pre;                    // the face being rendered / the face being prefiltered
    unsigned int m_Array = 0;
    int m_ArrayLayers = 0;
    unsigned int m_CaptureCube[2] = {0, 0}; // RGBA16F, full mip chain, the prefilter's input
    std::unique_ptr<HdrTarget> m_Target;    // one face's render target (single-sample)
};
