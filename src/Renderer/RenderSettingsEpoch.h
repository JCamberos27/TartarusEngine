#pragma once
#include <atomic>
#include <cstdint>

// GatherVisible reads three of a renderable's settings live: Cast Shadows, its model, and its first material's render
// queue. The GPU scene (GpuScene.h) keeps a copy of them per slot. Code that changes one of them on a renderable that
// already exists, or changes a material's render queue, calls NoteRenderSettingsChanged(); the next
// TakeSceneSlotChanges then re-reads every slot's copy. Renderables made this frame need no call (their slots are
// read new), and a missed call is still caught by a slow sweep (a slice of the slots each frame).
inline std::atomic<std::uint32_t>& RenderSettingsEpochRef() {
    static std::atomic<std::uint32_t> epoch{0};
    return epoch;
}
inline void NoteRenderSettingsChanged() { RenderSettingsEpochRef().fetch_add(1, std::memory_order_relaxed); }
inline std::uint32_t RenderSettingsEpoch() { return RenderSettingsEpochRef().load(std::memory_order_relaxed); }
