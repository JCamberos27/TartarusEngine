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

// Second-order IIR section (direct form II transposed): the K-weighting filters of the loudness meter and the reverb's
// shelf / high-pass share it. Coefficients are set by the owner; Run() filters one sample.
struct Biquad {
    double B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0, Z1 = 0, Z2 = 0;
    double Run(double x) {
        const double y = B0 * x + Z1;
        Z1 = B1 * x - A1 * y + Z2;
        Z2 = B2 * x - A2 * y;
        return y;
    }
};
