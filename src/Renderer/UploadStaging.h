#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// Background asset uploads without the main-thread memcpy (AssetLibrary::PumpAsync). A big texture or mesh used to go
// up in one glTextureSubImage2D / glNamedBufferStorage from client memory: the driver copies the whole thing before
// the call returns, 4-9 ms for a 4K map or a dense skinned mesh, and Play's first seconds on the night scenes hitched.
//
// Now the bytes are copied on a worker thread into a persistently mapped staging ring (one GL buffer, write-only,
// coherent), and the main thread only issues the GL copy out of it (a pixel-unpack upload or glCopyNamedBufferSubData),
// which the driver queues as a DMA. A region is reused once a fence placed after its copy has passed.
//
// Main thread only, except the copy itself (StageCopy runs it on the worker; the worker touches no GL). Off with
// Project Settings > Quality > Async uploads or TARTARUS_NO_ASYNC_UPLOAD=1: everything uploads straight from memory, as
// before. A request bigger than three quarters of the ring, or one that does not fit right now, also goes the old way.
namespace UploadStaging {

// One step of a pending upload: finished, did some work, or waiting on the copy worker (try again next pump).
enum class Step { Done, Progress, Waiting };

bool Enabled();

// One staged copy: Ready() once the worker has written the bytes.
struct Region {
    std::size_t Offset = 0, Size = 0;
    unsigned char* Ptr = nullptr;
    std::shared_ptr<std::atomic<bool>> Copied;
    bool Valid() const { return Ptr != nullptr; }
    bool Ready() const { return Copied && Copied->load(std::memory_order_acquire); }
};

// Space for `size` bytes (offset aligned to 256), or an invalid region when staging is off or the ring is full.
Region Reserve(std::size_t size);
// Queues `fill(dst)` on the copy worker (the caller keeps the source alive until Ready()).
void StageCopy(Region& r, std::function<void(unsigned char* dst)> fill);
// After the GL commands that read the region are issued: fences it, and it is recycled once the GPU is past them.
void Retire(Region& r);
// Drops a region nothing will read (its owner is going away): waits for the worker's copy, then retires it.
void Abandon(Region& r);
// Runs `f` on the copy worker - for freeing an upload's big CPU buffers (returning tens of MB to the OS took
// milliseconds on the main thread). `f` must touch no GL.
void Dispose(std::function<void()> f);
// The ring buffer's GL name (bind as GL_PIXEL_UNPACK_BUFFER / copy source). 0 when staging is off.
unsigned Buffer();
// Creates the ring now (pinning 128 MiB takes a while): the first pump calls it, at startup, so the first load in
// Play doesn't pay it.
void Warm();
// Recycles the regions whose fences have passed. Called by Reserve, and once per pump.
void Poll();

// Testable ring arithmetic (unit tests): where a `size`-byte block goes in a ring of `capacity` bytes whose live
// span runs from `tail` to `head` (head == tail: empty unless `full`). Returns ~0 when it doesn't fit.
std::size_t RingFit(std::size_t capacity, std::size_t head, std::size_t tail, bool empty, std::size_t size);
}
