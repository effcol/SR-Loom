// Standalone tool for exercising LFPLoadAsStereoSBS + experimenting with
// the colour pipeline (CCM / WB on/off) for debugging Illum colour.
#include "../src/LFPReader.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace srw;

static bool WritePPM(const std::string& p, int w, int h,
                     const std::vector<uint8_t>& rgba, bool stride4) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const int step = stride4 ? 4 : 3;
    for (size_t i = 0; i + 2 < rgba.size(); i += step)
        std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: %s <file.lfp> <out.ppm> [noccm|nowb|none]\n", argv[0]); return 1; }
    const std::string mode = (argc > 3) ? argv[3] : "";

    LFPReader r;
    if (!r.LoadFromFile(argv[1])) { std::printf("parse fail\n"); return 2; }
    auto* mc = r.FindBySha1(LFPJsonFindString(r.TopLevelJson(), "metadataRef"));
    auto* ic = LFPFindRawImageChunk(r);
    if (!mc || !ic) return 3;
    LFPCalibration cal;
    if (!LFPParseCalibration(mc->AsString(), cal)) return 4;

    if (mode == "noccm" || mode == "none") {
        for (int i = 0; i < 9; ++i) cal.ccmRgbToSrgb[i] = (i % 4 == 0) ? 1.0 : 0.0;
        std::printf("CCM disabled (identity)\n");
    }
    if (mode == "nowb" || mode == "none") {
        cal.wbR = cal.wbGR = cal.wbGB = cal.wbB = 1.0;
        std::printf("WB disabled (all gains 1.0)\n");
    }

    auto bayer = LFPUnpackBayer(ic->data, cal);
    auto rgb = LFPDemosaicPlenopticAware(bayer, cal);
    int lW, lH, rW, rH;
    auto left  = LFPExtractSubApertureViewFromRgb(rgb, cal, -0.7f, 0.0f, lW, lH);
    auto right = LFPExtractSubApertureViewFromRgb(rgb, cal, +0.7f, 0.0f, rW, rH);
    std::vector<uint8_t> out((size_t)lW * 2 * lH * 3, 255);
    for (int y = 0; y < lH; ++y) {
        for (int x = 0; x < lW; ++x) {
            const size_t s = ((size_t)y * lW + x) * 3;
            const size_t dl = ((size_t)y * lW * 2 + x) * 3;
            const size_t dr = ((size_t)y * lW * 2 + (x + lW)) * 3;
            out[dl+0] = left[s+0];  out[dl+1] = left[s+1];  out[dl+2] = left[s+2];
            out[dr+0] = right[s+0]; out[dr+1] = right[s+1]; out[dr+2] = right[s+2];
        }
    }
    WritePPM(argv[2], lW * 2, lH, out, false);
    std::printf("wrote %s (%dx%d SBS)\n", argv[2], lW * 2, lH);
    return 0;
}
