#pragma once

// `TartarusEngine.exe --reverb-render <in.f32> <params.txt> <out.f32> [sampleRate]`: the engine's reverb (ReverbFdn, the same
// code the audio callback runs) over a recorded send mix, for tools/mix_npc_video.py. in / out are raw interleaved stereo
// float32; params.txt has one line per parameter change: `<time s> <room> <decay s> <damping> <predelay ms> <wet> <early/late>`
// (sorted by time; the DSP glides between them exactly as it does live). Output is the wet signal, same length. Exit code 0 = ok.
int RunReverbRender(int argc, char** argv);
