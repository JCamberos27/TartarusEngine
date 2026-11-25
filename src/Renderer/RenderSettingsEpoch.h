#pragma once
#include <cstdint>

// GatherVisible reads three of a renderable's settings live: Cast Shadows, its model, and its first material's render
// queue. The GPU scene (GpuScene.h) keeps a copy of them per slot. Code that changes one of them on a renderable that
// already exists, or changes a material's render queue, calls NoteRenderSettingsChanged(); the next
// TakeSceneSlotChanges then re-reads every slot's copy. Renderables made this frame need no call (their slots are
// read new), and a missed call is still caught by a slow sweep: max(1024, slots / 64) slots per sync, so a writer
// that doesn't call this leaves its slot stale for up to one lap (~64 frames, about a second on Forest). Safety net
// only: every writer should call it.
// Defined in SceneRenderer.cpp (TartarusCore): plain functions, so the game and editor modules share the one counter
// (an inline function's static would be one copy per module).
void NoteRenderSettingsChanged();
std::uint32_t RenderSettingsEpoch();
