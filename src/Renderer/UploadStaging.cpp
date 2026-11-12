#include "UploadStaging.h"
#include "gl.h"
#include "ProjectSettings.h"
#include "Core/Env.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace UploadStaging {
namespace {
constexpr std::size_t kCapacity = 128ull << 20;          // pinned, write-combined system memory
constexpr std::size_t kLargest = kCapacity / 4 * 3;       // a 4K RGBA8 map (64 MiB) still fits
constexpr std::size_t kAlign = 256;

struct Live {
    std::size_t Offset, End;
    std::uint64_t Batch = 0; // the fence batch it was retired in
    bool Retired = false;
};

struct BatchFence {
    std::uint64_t Batch;
    GLsync Sync;
};

struct Ring {
    GLuint Buffer = 0;
    unsigned char* Base = nullptr;
    bool Failed = false;
    std::size_t Head = 0;
    std::deque<Live> Spans;
    std::deque<BatchFence> Fences; // oldest first
    std::uint64_t OpenBatch = 1;   // regions retired now join this batch; Fence() closes it
    std::uint64_t PassedBatch = 0; // every batch up to this one is known done on the GPU
    bool OpenUsed = false;
};
Ring& R() {
    static Ring* r = new Ring(); // never destroyed: the GL context is gone by the time statics are
    return *r;
}

// The copy worker: a FIFO of fills, run one at a time. Never joined (it sleeps on the condition at exit).
struct Worker {
    std::mutex M;
    std::condition_variable Cv;
    std::deque<std::function<void()>> Q;
    Worker() {
        std::thread([this] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock<std::mutex> lock(M);
                    Cv.wait(lock, [this] { return !Q.empty(); });
                    f = std::move(Q.front());
                    Q.pop_front();
                }
                f();
            }
        }).detach();
    }
    void Push(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lock(M);
            Q.push_back(std::move(f));
        }
        Cv.notify_one();
    }
};
Worker& W() {
    static Worker* w = new Worker();
    return *w;
}

bool EnvOff() {
    static const bool off = Env::Flag(Env::Var::NoAsyncUpload);
    return off;
}

bool EnsureRing() {
    Ring& r = R();
    if (r.Base || r.Failed) return !r.Failed;
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glCreateBuffers(1, &r.Buffer);
    glNamedBufferStorage(r.Buffer, (GLsizeiptr)kCapacity, nullptr, flags);
    r.Base = static_cast<unsigned char*>(glMapNamedBufferRange(r.Buffer, 0, (GLsizeiptr)kCapacity, flags));
    if (!r.Base) {
        std::fprintf(stderr, "[UploadStaging] could not map the staging ring - uploading from memory as before\n");
        glDeleteBuffers(1, &r.Buffer);
        r.Buffer = 0;
        r.Failed = true;
        return false;
    }
    return true;
}
} // namespace

std::size_t RingFit(std::size_t capacity, std::size_t head, std::size_t tail, bool empty, std::size_t size) {
    if (size == 0 || size > capacity) return ~std::size_t(0);
    if (empty) return 0;
    if (head == tail) return ~std::size_t(0); // full
    const std::size_t at = (head + kAlign - 1) / kAlign * kAlign;
    if (head > tail) {
        if (at <= capacity && capacity - at >= size) return at;
        return size <= tail ? 0 : ~std::size_t(0); // wrap to the start, up to the oldest live span
    }
    return (at <= tail && tail - at >= size) ? at : ~std::size_t(0);
}

bool Enabled() {
    return !EnvOff() && ProjectSettings::Quality().AsyncUploads && !R().Failed;
}

void Fence() {
    Ring& r = R();
    if (!r.OpenUsed) return;
    r.Fences.push_back({r.OpenBatch, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
    ++r.OpenBatch;
    r.OpenUsed = false;
}

void Poll() {
    Fence();
    Ring& r = R();
    while (!r.Spans.empty()) {
        Live& s = r.Spans.front();
        if (!s.Retired) break;
        // One query per fence, not per region: a batch's regions all pass together.
        while (s.Batch > r.PassedBatch && !r.Fences.empty()) {
            BatchFence& f = r.Fences.front();
            const GLenum st = glClientWaitSync(f.Sync, 0, 0);
            if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) break;
            glDeleteSync(f.Sync);
            r.PassedBatch = f.Batch;
            r.Fences.pop_front();
        }
        if (s.Batch > r.PassedBatch) break;
        r.Spans.pop_front();
    }
    if (r.Spans.empty()) r.Head = 0;
}

bool Stageable(std::size_t size) {
    return Enabled() && size > 0 && size <= kLargest && R().Base != nullptr;
}

Region Reserve(std::size_t size) {
    Region out;
    if (!Enabled() || size == 0 || size > kLargest || !EnsureRing()) return out;
    Ring& r = R();
    const auto fit = [&] {
        const std::size_t tail = r.Spans.empty() ? 0 : r.Spans.front().Offset;
        return RingFit(kCapacity, r.Head, tail, r.Spans.empty(), size);
    };
    std::size_t at = fit();
    if (at == ~std::size_t(0)) { // full: recycle what the GPU is done with (a driver round trip), then try again
        Poll();
        at = fit();
    }
    if (at == ~std::size_t(0)) return out;
    r.Spans.push_back({at, at + size});
    r.Head = at + size;
    out.Offset = at;
    out.Size = size;
    out.Ptr = r.Base + at;
    out.Copied = std::make_shared<std::atomic<bool>>(false);
    return out;
}

void StageCopy(Region& r, std::function<void(unsigned char* dst)> fill) {
    if (!r.Valid()) return;
    unsigned char* dst = r.Ptr;
    std::shared_ptr<std::atomic<bool>> done = r.Copied;
    W().Push([dst, done, fill = std::move(fill)] {
        fill(dst);
        done->store(true, std::memory_order_release);
    });
}

void Dispose(std::function<void()> f) { W().Push(std::move(f)); }

void Retire(Region& reg) {
    if (!reg.Valid()) return;
    Ring& r = R();
    for (Live& s : r.Spans)
        if (s.Offset == reg.Offset && !s.Retired) {
            s.Batch = r.OpenBatch;
            s.Retired = true;
            r.OpenUsed = true;
            break;
        }
    reg = Region{};
}

unsigned Buffer() { return R().Buffer; }

void Warm() {
    if (Enabled()) EnsureRing();
}
} // namespace UploadStaging

void UploadStaging::Abandon(Region& reg) {
    if (!reg.Valid()) return;
    while (!reg.Ready()) std::this_thread::yield(); // the worker is mid-copy into it: let it finish first
    Retire(reg);
}
