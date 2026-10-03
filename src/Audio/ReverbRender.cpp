#include "ReverbRender.h"

#include "ReverbFdn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int RunReverbRender(int argc, char** argv) {
    // argv[0] is the exe; the flag may sit anywhere: the three paths follow it.
    int at = 0;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--reverb-render") at = i;
    if (at == 0 || at + 3 >= argc) {
        std::fprintf(stderr, "usage: --reverb-render <in.f32> <params.txt> <out.f32> [sampleRate]\n");
        return 2;
    }
    const char *inPath = argv[at + 1], *parPath = argv[at + 2], *outPath = argv[at + 3];
    const int rate = at + 4 < argc ? std::atoi(argv[at + 4]) : 48000;
    std::ifstream fi(inPath, std::ios::binary);
    if (!fi) {
        std::fprintf(stderr, "cannot open %s\n", inPath);
        return 2;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
    std::vector<float> in(raw.size() / sizeof(float));
    if (!in.empty()) std::memcpy(in.data(), raw.data(), in.size() * sizeof(float));
    struct Change { double T; ReverbParams P; };
    std::vector<Change> changes;
    {
        std::ifstream fp(parPath);
        double t;
        float a, b, c, d, e, f;
        while (fp >> t >> a >> b >> c >> d >> e >> f) changes.push_back({t, {a, b, c, d, e, f}});
    }
    ReverbFdn fdn(rate);
    if (!changes.empty()) fdn.Reset(changes.front().P); // starts in its first space
    std::vector<float> out(in.size(), 0.0f);
    const int total = (int)(in.size() / 2);
    const int block = 480; // the audio callback's size at 48 kHz
    size_t next = 0;
    for (int pos = 0; pos < total; pos += block) {
        const double now = (double)pos / (double)rate;
        while (next < changes.size() && changes[next].T <= now) fdn.SetTarget(changes[next++].P);
        const int n = pos + block <= total ? block : total - pos;
        fdn.Process(in.data() + 2 * (size_t)pos, out.data() + 2 * (size_t)pos, n);
    }
    std::ofstream fo(outPath, std::ios::binary);
    if (!fo) return 2;
    fo.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)(out.size() * sizeof(float)));
    return 0;
}
