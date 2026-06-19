// lfp_raw_stats.cpp -- print min/max/mean of the unpacked Bayer data.
// Used to debug Phase 2 unpack issues (avg should be roughly mid-range
// for real photos; if it's near 0 we're either misreading the bit
// packing or losing data somewhere).
#include "../src/LFPReader.h"
#include <cstdio>
#include <algorithm>

using namespace srw;

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: %s <file.lfp>\n", argv[0]); return 1; }
    LFPReader r;
    if (!r.LoadFromFile(argv[1])) return 2;
    auto top = r.TopLevelJson();
    auto* mc = r.FindBySha1(LFPJsonFindString(top, "metadataRef"));
    auto* ic = r.FindBySha1(LFPJsonFindString(top, "imageRef"));
    if (!mc || !ic) { std::printf("missing refs\n"); return 3; }
    LFPCalibration cal;
    if (!LFPParseCalibration(mc->AsString(), cal)) return 4;
    auto raw = LFPUnpackBayer(ic->data, cal);
    if (raw.empty()) { std::printf("unpack failed\n"); return 5; }

    uint64_t sum = 0;
    uint16_t mn = 0xFFFF, mx = 0;
    int hist[1024] = {0};
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const uint16_t v = raw[i];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        const int bucket = (v >> ((cal.bitsPerPixel > 10) ? 2 : 0)) & 0x3FF;
        ++hist[bucket];
    }
    const double avg = (double)sum / raw.size();
    std::printf("%s: %d-bit, %zu pixels, min %u max %u avg %.1f\n",
                argv[1], cal.bitsPerPixel, raw.size(), mn, mx, avg);

    // First non-empty + last non-empty hist bins, so we see if data is
    // clipped at the bottom (e.g., everything below black level).
    int firstNonzero = -1, lastNonzero = -1;
    for (int i = 0; i < 1024; ++i) if (hist[i]) { firstNonzero = i; break; }
    for (int i = 1023; i >= 0; --i) if (hist[i]) { lastNonzero = i; break; }
    std::printf("  histogram extent: bins %d..%d (out of 0..1023)\n",
                firstNonzero, lastNonzero);
    std::printf("  black level: R %.0f Gr %.0f Gb %.0f B %.0f\n",
                cal.blackR, cal.blackGR, cal.blackGB, cal.blackB);
    std::printf("  white level: R %.0f Gr %.0f Gb %.0f B %.0f\n",
                cal.whiteR, cal.whiteGR, cal.whiteGB, cal.whiteB);

    // Dump first 16 raw pixels for eyeballing
    std::printf("  first 16 raw values:");
    for (int i = 0; i < 16; ++i) std::printf(" %u", raw[i]);
    std::printf("\n");
    return 0;
}
