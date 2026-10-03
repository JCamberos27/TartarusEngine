#pragma once

#include <cmath>
#include <vector>

// A small real-input FFT for the convolution reverb (Audio/Convolution): power-of-two sizes, float, interleaved complex
// (re, im), no dependencies. A real transform of N samples runs one complex FFT of N / 2 points (iterative radix-2, the
// twiddles of every stage stored contiguously) plus the usual even / odd split.
//
// Forward(): N reals -> N / 2 + 1 complex bins. Inverse(): the N / 2 + 1 bins -> N reals, scaled by 1 / N, so
// Inverse(Forward(x)) == x and Inverse(Forward(x) * Forward(h)) is the circular convolution of x and h.
// The object holds a scratch buffer: one transform at a time per object (use one per thread).

class RealFft {
public:
    explicit RealFft(int n) : m_N(n < 8 ? 8 : n), m_M(m_N / 2) {
        int bits = 0;
        while ((1 << bits) < m_M) ++bits;
        m_Rev.resize((size_t)m_M);
        for (int i = 0; i < m_M; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            m_Rev[(size_t)i] = r;
        }
        // stage tables: for a butterfly of half size h, w[j] = exp(-2 pi i j / (2h)), j < h, stored at [h - 1 + j]
        m_Tw.resize(2 * (size_t)m_M);
        for (int h = 1; h < m_M; h <<= 1)
            for (int j = 0; j < h; ++j) {
                const double a = -3.14159265358979323846 * (double)j / (double)h;
                m_Tw[2 * (size_t)(h - 1 + j)] = (float)std::cos(a);
                m_Tw[2 * (size_t)(h - 1 + j) + 1] = (float)std::sin(a);
            }
        // the split twiddles exp(-2 pi i k / N), k = 0 .. N / 2
        m_Split.resize(2 * ((size_t)m_M + 1));
        for (int k = 0; k <= m_M; ++k) {
            const double a = -2.0 * 3.14159265358979323846 * (double)k / (double)m_N;
            m_Split[2 * (size_t)k] = (float)std::cos(a);
            m_Split[2 * (size_t)k + 1] = (float)std::sin(a);
        }
        m_Z.resize(2 * (size_t)m_M);
    }

    int Size() const { return m_N; }
    int Bins() const { return m_M + 1; }

    void Forward(const float* in, float* out) {
        const int M = m_M;
        float* z = m_Z.data();
        for (int i = 0; i < M; ++i) { // bit-reversed load of the packed pairs
            const int r = m_Rev[(size_t)i];
            z[2 * r] = in[2 * i];
            z[2 * r + 1] = in[2 * i + 1];
        }
        Butterflies(z, false);
        for (int k = 0; k <= M; ++k) {
            const int a = k == M ? 0 : k, b = k == 0 ? 0 : M - k;
            const float zr = z[2 * a], zi = z[2 * a + 1], cr = z[2 * b], ci = -z[2 * b + 1]; // Z[k], conj(Z[M - k])
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);                         // even part
            const float dr = 0.5f * (zr - cr), di = 0.5f * (zi - ci);                         // (Z - conj) / 2 ...
            const float orr = di, oi = -dr;                                                   // ... divided by i
            const float wr = m_Split[2 * (size_t)k], wi = m_Split[2 * (size_t)k + 1];
            out[2 * k] = er + (orr * wr - oi * wi);
            out[2 * k + 1] = ei + (orr * wi + oi * wr);
        }
    }

    void Inverse(const float* in, float* out) {
        const int M = m_M;
        float* z = m_Z.data();
        const float scale = 1.0f / (float)m_N;
        for (int k = 0; k < M; ++k) { // rebuild the packed spectrum Z[k] = E[k] + i O[k]
            const int b = M - k;
            const float xr = in[2 * k], xi = in[2 * k + 1], yr = in[2 * b], yi = -in[2 * b + 1]; // X[k], conj(X[M - k])
            const float er = 0.5f * (xr + yr), ei = 0.5f * (xi + yi);
            const float tr = 0.5f * (xr - yr), ti = 0.5f * (xi - yi);                            // = W * O
            const float wr = m_Split[2 * (size_t)k], wi = -m_Split[2 * (size_t)k + 1];           // conj(W)
            const float orr = tr * wr - ti * wi, oi = tr * wi + ti * wr;
            const int r = m_Rev[(size_t)k];
            z[2 * r] = er - oi;      // E + i O
            z[2 * r + 1] = ei + orr;
        }
        Butterflies(z, true);
        for (int i = 0; i < M; ++i) {
            out[2 * i] = z[2 * i] * scale * 2.0f;
            out[2 * i + 1] = z[2 * i + 1] * scale * 2.0f;
        }
    }

private:
    // In-place decimation-in-time butterflies over data already in bit-reversed order; the inverse conjugates the twiddles
    // (and leaves the 1 / M scaling to the caller).
    void Butterflies(float* z, bool inverse) const {
        const int M = m_M;
        const float sign = inverse ? -1.0f : 1.0f;
        for (int h = 1; h < M; h <<= 1) {
            const float* tw = m_Tw.data() + 2 * (size_t)(h - 1);
            for (int base = 0; base < M; base += 2 * h) {
                float* a = z + 2 * (size_t)base;
                float* b = a + 2 * (size_t)h;
                for (int j = 0; j < h; ++j) {
                    const float wr = tw[2 * j], wi = sign * tw[2 * j + 1];
                    const float br = b[2 * j], bi = b[2 * j + 1];
                    const float tr = br * wr - bi * wi, ti = br * wi + bi * wr;
                    const float ar = a[2 * j], ai = a[2 * j + 1];
                    b[2 * j] = ar - tr;
                    b[2 * j + 1] = ai - ti;
                    a[2 * j] = ar + tr;
                    a[2 * j + 1] = ai + ti;
                }
            }
        }
    }

    int m_N, m_M;
    std::vector<int> m_Rev;
    std::vector<float> m_Tw, m_Split, m_Z;
};
