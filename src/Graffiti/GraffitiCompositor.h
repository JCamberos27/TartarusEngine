#pragma once
#include "GraffitiPaint.h"
#include "GraffitiPiece.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Shader;

// The Graffiti compositor (docs/GRAFFITI_DESIGNER.md): draws a paint program (GraffitiPaint.h) on
// the GPU. Each layer is drawn into its own target (premultiplied linear colour + paint thickness),
// blurred where it has an outline or a shadow, and composited onto the layers below through a
// ping-pong pair that also carries height, glow and metal. The result feeds the Designer's canvas
// (PreviewTexture) and the bake (the four decal maps, read back through PBOs a frame or two later).
//
// The same program always issues the same draws in the same order with no atomics or races, so on
// one machine a re-bake is byte-identical (the smoke test checks it; the program itself is
// unit-tested on the CPU).
//
// Live strokes are incremental: BeginLive composites the layers below the one being sprayed once,
// keeps that layer's paint in a target of its own, and from then on only the new splats are drawn
// into it and the stack above re-composited.
//
// GL resources are created on first use and sized to the canvas; Release() (or the destructor)
// frees them. Main thread, GL context current. Restores the GL state it touches.
namespace Graffiti {

struct BakeImages {
    int Width = 0, Height = 0;
    // Rows top first. Albedo and Surface RGBA, Normal RGB, Aux one channel.
    std::vector<uint8_t> Albedo, Normal, Surface, Aux;
};

class Compositor {
public:
    Compositor();
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // An image's ink box and aspect (decoded, and uploaded for drawing, the first time it's asked
    // for). The paint program's MetricsFn: Metrics() bound to this compositor.
    ImageMetrics Metrics(const std::string& absolutePath);
    MetricsFn MetricsCallback();
    void ForgetImages(); // re-read every image next time (one changed on disk)
    // Bumps whenever what an image's metrics say may have changed (forgotten, or a failed read that
    // later succeeded): cached paint programs built from the old metrics are stale past it.
    uint32_t ImageEpoch() const { return m_ImageEpoch; }
    // Retries images that failed to read a while ago (bumping the epoch when one loads): call before
    // reading ImageEpoch() for a build.
    void RetryFailedImages();

    // The whole piece, from scratch.
    void Render(const Piece& piece, const Program& program);
    // Redraws only the canvas (preview and overlay) from the last composite, for edits that change
    // nothing but the preview shader's inputs (relief, show-through, the wall swatch). False when
    // there is no valid composite to redraw from: the caller then does a full Render.
    bool Repreview(const Piece& piece);

    // A live stroke on program layer `programLayer` (an index into program.Layers).
    void BeginLive(const Piece& piece, const Program& program, int programLayer);
    void LiveSplats(const Splat* splats, size_t count); // the new ones, in order
    void LiveComposite(const Piece& piece, const Program& program);
    void EndLive() { m_LiveLayer = -1; }
    bool Live() const { return m_LiveLayer >= 0; }

    // Renders the piece and starts reading the four maps back. PollBake fills `out` once they have
    // arrived (true), or with `wait` blocks until they do. A bake already in flight is finished first
    // and kept (it is still handed out by the next PollBake), never dropped: a second StartBake
    // before the first was polled replaces it, so callers poll one bake before starting the next.
    void StartBake(const Piece& piece, const Program& program);
    bool PollBake(BakeImages& out, bool wait = false);
    bool BakeInFlight() const { return m_Fence != nullptr || m_HasStash; }

    // The canvas for display: sRGB RGBA8, opaque over a wall swatch. GL rows run bottom up, so the
    // image's top is at v = 1 (draw it with uv0 (0, 1), uv1 (1, 0)).
    unsigned int PreviewTexture() const { return m_Preview; }
    // The same paint with no wall under it (sRGB, straight alpha, same orientation): the viewport
    // draws it over the wall while a stroke is live, until the re-baked decal is resident.
    unsigned int OverlayTexture() const { return m_Overlay; }
    // The canvas colour (sRGB 0..1, as shown) at canvas uv (y down): the Designer's eyedropper.
    glm::vec3 SamplePreview(const glm::vec2& uv);
    int Width() const { return m_W; }
    int Height() const { return m_H; }
    void SetWallColor(const glm::vec3& linear) { m_Wall = linear; }

    void Release();

    // GPU bytes held by the canvas targets (Statistics, tests).
    size_t GpuBytes() const;

private:
    struct Image {
        unsigned int Tex = 0;
        int Width = 0, Height = 0;
        ImageMetrics M;
        double FailedAt = -1.0; // steady-clock seconds of a failed read (retried after a short while)
    };
    struct Target { unsigned int Fbo = 0, Color = 0, Extra = 0; }; // colour + thickness / aux

    void EnsureGl();
    void EnsureTargets(int w, int h);
    void FreeTargets();
    bool ReadBack(BakeImages& out); // the PBOs into rows top first (the fence has been passed)
    void FinishInFlight();          // waits for a bake in flight and keeps its images in m_Stash
    const Image& LoadImage(const std::string& path);
    void DrawLayer(const Piece& piece, const LayerProgram& lp, const Target& into);
    void DrawSplats(const Splat* splats, size_t count, bool erase);
    void BlurLayer(const Piece& piece, const Layer& layer, unsigned int coverageTex);
    void Composite(const Piece& piece, const Layer& layer, const Target& below, const Target& layerT, const Target& dst);
    int ComposeFrom(const Piece& piece, const Program& program, int first, int last, const Target* start);
    void DrawPreview(const Piece& piece, const Target& final);
    void Fullscreen();

    std::unique_ptr<Shader> m_Splat, m_Quad, m_Fill, m_Blur, m_Composite, m_Bake, m_PreviewShader;
    unsigned int m_EmptyVao = 0, m_SplatVao = 0, m_SplatVbo = 0;
    size_t m_SplatCapacity = 0; // instances the VBO holds

    int m_W = 0, m_H = 0;
    Target m_Layer, m_LiveT, m_Below, m_C[2];
    unsigned int m_BlurFbo[4] = {}, m_BlurTex[4] = {};
    unsigned int m_PreviewFbo = 0, m_Preview = 0, m_Overlay = 0;
    unsigned int m_OutFbo = 0, m_Out[4] = {};
    unsigned int m_Pbo[4] = {};
    void* m_Fence = nullptr; // GLsync
    BakeImages m_Stash;      // a bake finished early because its targets were about to go
    bool m_HasStash = false;
    bool m_Composited = false; // m_C[m_Final] holds a valid composite (Repreview)
    uint32_t m_ImageEpoch = 0;
    int m_Final = 0;         // m_C index holding the last composite

    int m_LiveLayer = -1;
    glm::vec3 m_Wall{0.42f, 0.4f, 0.37f};
    std::unordered_map<std::string, Image> m_Images;
    std::vector<std::string> m_TextureNames; // the program's image table, as last rendered
};

// PNG bytes for the four maps (stb_image_write at a fixed compression level: deterministic). Pure
// CPU: safe on a worker.
struct BakeFiles {
    std::string Albedo, Normal, Surface, Aux;
};
BakeFiles EncodePngs(const BakeImages& images);
// Writes the four PNGs into piece.BakeDir() and then the piece's DA_Piece_<name>.decal (last, so the
// renderer's reload sees the new maps). A first bake creates the .decal from the piece; a re-bake
// updates only what the bake owns (its maps, tier, paint, glow and channels) and keeps edits made
// to it since (size, fades, category, weathering). Atomic writes; safe on a worker, where nothing
// enters the editor's undo history (the recipe is the undo state: undoing re-bakes it).
bool WriteBake(const Piece& piece, const BakeFiles& files);
// What WriteBake reads of `piece`, and nothing more: the paths (Path, Name), the canvas and paint
// settings and the weathering, with the layers (and their strokes) reduced to one stand-in layer when
// any visible one is metal. Taken when a bake starts, so a bake in flight keeps writing to the piece
// it was started on however the Designer's open piece changes meanwhile.
std::shared_ptr<Piece> SnapshotForBake(const Piece& piece);

} // namespace Graffiti
