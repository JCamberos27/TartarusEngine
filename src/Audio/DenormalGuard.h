#pragma once

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <xmmintrin.h>
#define AUDIO_SSE 1
#endif

// Enables flush-to-zero / denormals-are-zero for the scope of one audio callback (decaying filter and FFT tails go subnormal).
struct DenormalGuard {
#ifdef AUDIO_SSE
    unsigned Saved;
    DenormalGuard() : Saved(_mm_getcsr()) { _mm_setcsr(Saved | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(Saved); }
#endif
};
