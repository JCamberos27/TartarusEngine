#pragma once
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Thumbnails for Asset Browser entries that aren't loaded (the browser lists every asset file on
// disk, like Unity's Project window, but only loads one when it's used). Loading a texture just to
// draw a 64px tile means decoding and uploading the whole image - seconds and ~100 MB of VRAM
// each for a pack of 4K maps - so instead:
//
//   Image  decoded on a worker thread, box-filtered into a kSize square (aspect kept, transparent
//          border), cached in Library/thumbnails (ThumbnailCache) so the next launch skips the
//          decode. Only the 128px result is uploaded, on the main thread (GL), a few per frame.
//   Cached the last thumbnail ThumbnailCache holds for a model or material, whatever its key: a
//          possibly stale preview until the asset loads and renders a current one. Nothing to
//          decode when there's none - the tile keeps its type glyph.
//
// Get() queues on first ask and returns 0 until the thumbnail is ready. Kept in an LRU; the most
// recently asked-for thumbnails are made first, so the tiles on screen fill in before ones
// scrolled past.
class AssetThumbnailLoader {
public:
    static constexpr int kSize = 128;
    enum class Source { Image, Cached };

    struct Thumb {
        unsigned int Tex = 0; // GL texture, 0 while pending or when there's no thumbnail
        bool FlipV = false;   // cached model/material renders are stored bottom-up
    };

    AssetThumbnailLoader();
    ~AssetThumbnailLoader(); // joins the workers; GL textures must already be freed (Clear)

    Thumb Get(const std::string& path, Source source);
    // Main thread, once a frame: uploads at most `maxUploads` finished thumbnails.
    void Pump(int maxUploads);
    // The file changed: drop its thumbnail so the next Get() makes a new one.
    void Invalidate(const std::string& path);
    // Frees every GL texture (needs the GL context). Work still queued finishes, unused.
    void Clear();

private:
    struct Job { std::string Path; Source From; unsigned Ticket; };
    struct Result { std::string Path; std::vector<unsigned char> Pixels; bool FlipV; unsigned Ticket; };
    struct Entry { Thumb T; bool Pending = true; unsigned Ticket = 0; std::list<std::string>::iterator Lru; };

    void Worker();
    void Remove(std::unordered_map<std::string, Entry>::iterator it);

    std::unordered_map<std::string, Entry> m_Entries; // main thread only
    std::list<std::string> m_Lru;                     // front = most recently used
    unsigned m_NextTicket = 1;                        // a result is used only by the request that asked for it

    std::mutex m_Mutex; // guards everything below
    std::condition_variable m_Wake;
    std::deque<Job> m_Jobs; // front = newest request
    std::vector<Result> m_Done;
    bool m_Stop = false;
    std::vector<std::thread> m_Threads;
};
