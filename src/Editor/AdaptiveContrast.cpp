#include "AdaptiveContrast.h"
#include "gl.h"

// Max side of the sampled patch, matching the fixed-size PBOs in AsyncLuminanceReadback.
static constexpr int kMaxLumPatch = 64;

float SampleTextureLuminance(AsyncLuminanceReadback& rb, unsigned int colorTex, int texW, int texH,
                             ImVec2 imgPos, ImVec2 imgSize, ImVec2 centerScreen, float boxPx) {
    // Async readback via a ping-ponged pair of PBOs (#178): rather than glReadPixels straight into
    // client memory (which stalls the GPU pipeline until the transfer finishes), this call kicks
    // off a non-blocking read into one PBO — glReadPixels returns immediately when a buffer is
    // bound to GL_PIXEL_PACK_BUFFER — and, in the same call, maps+consumes whatever the OTHER PBO
    // was loaded with by the PREVIOUS kickoff (one throttled ~100ms tick earlier). The result is
    // therefore a frame or two stale, which is invisible: it only steers a slowly-eased HUD tint.
    float result = -1.0f;

    // 1) Consume the other slot's pending result from the previous call, if any.
    const int readSlot = 1 - rb.Next;
    if (rb.Pending[readSlot] > 0) {
        if (void* ptr = glMapNamedBuffer(rb.Pbo[readSlot], GL_READ_ONLY)) {
            const unsigned char* px = (const unsigned char*)ptr;
            const int n = rb.Pending[readSlot];
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
                sum += 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
            result = (float)(sum / (n * 255.0)); // 0 = black behind the box, 1 = white
            glUnmapNamedBuffer(rb.Pbo[readSlot]);
        }
        rb.Pending[readSlot] = 0;
    }

    if (colorTex == 0 || texW < 1 || texH < 1 || imgSize.x < 1.0f || imgSize.y < 1.0f) return result;

    // screen box -> fraction of the displayed image -> texels (GL bottom-left origin). Handles
    // the Game view too, where the on-screen image is letterboxed and a different size than the
    // framebuffer it samples.
    const float sx = texW / imgSize.x, sy = texH / imgSize.y;
    int rw = (int)(boxPx * sx), rh = (int)(boxPx * sy);
    int rx = (int)((centerScreen.x - imgPos.x - boxPx * 0.5f) * sx);
    int ry = (int)((imgSize.y - ((centerScreen.y - imgPos.y) + boxPx * 0.5f)) * sy);
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > texW) rw = texW - rx;
    if (ry + rh > texH) rh = texH - ry;
    if (rw > kMaxLumPatch) { rx += (rw - kMaxLumPatch) / 2; rw = kMaxLumPatch; }
    if (rh > kMaxLumPatch) { ry += (rh - kMaxLumPatch) / 2; rh = kMaxLumPatch; }
    if (rx < 0 || ry < 0 || rw < 1 || rh < 1) return result;

    // 2) Kick off this call's read into the OTHER slot, for a future call to consume.
    const int writeSlot = readSlot;
    if (rb.Pbo[writeSlot] == 0) {
        glCreateBuffers(1, &rb.Pbo[writeSlot]);
        glNamedBufferStorage(rb.Pbo[writeSlot], kMaxLumPatch * kMaxLumPatch * 4, nullptr, GL_MAP_READ_BIT);
    }

    if (rb.SampleFbo == 0) glGenFramebuffers(1, &rb.SampleFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rb.SampleFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, rb.Pbo[writeSlot]);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); // offset 0 into the bound PBO — async
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFbo);

    rb.Pending[writeSlot] = rw * rh;
    rb.Next = readSlot; // next call reads what we just wrote and writes into what we just read

    return result;
}
