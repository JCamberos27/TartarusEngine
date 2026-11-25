#pragma once
// TEMPORARY sizing (diag branch only, never merged): sun gather reuse while walking.
// PlayFrame (exe) reports each frame's walk segment and motion; ShadowFrame (TartarusCore.dll) keeps the state.
// A function, not inline data: inline variables are per module.
void DiagW1Walk(int seg, double metres, double degrees, double seconds);
