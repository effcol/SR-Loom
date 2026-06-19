// lfp_dump.cpp -- tiny CLI test that exercises LFPReader against an LFP
// file. Builds as a standalone exe (no SR Loom / D3D11 deps) for quick
// verification that the container parser handles real Lytro files.
//
// Build by hand:
//   cl /std:c++17 /EHsc tools/lfp_dump.cpp src/LFPReader.cpp /I src
// Then run e.g.:
//   lfp_dump.exe "lib\Test Images\Lytro\IMG_0007.lfp"
//
// Logs go to stdout + the same srweaver.log file SR Loom uses (Log lives
// in Common.h). Outputs: file size, chunk count, per-chunk type/size/sha1
// summary, and -- if any Metadata chunk parses as JSON containing
// imageRef/metadataRef/privateMetadataRef keys -- the resolved sha1s.

#include "../src/LFPReader.h"
#include <cstdio>
#include <string>

using namespace srw;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: %s <file.lfp>\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];

    LFPReader r;
    if (!r.LoadFromFile(path))
    {
        std::printf("LFPReader: parse failed for %s\n", path.c_str());
        return 2;
    }

    const auto& chunks = r.Chunks();
    std::printf("== %s ==\n  chunks: %zu\n", path.c_str(), chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        const auto& c = chunks[i];
        const char* tn = (c.type == LFPChunk::File)     ? "FILE"
                       : (c.type == LFPChunk::Metadata) ? "META"
                                                        : "DATA";
        std::printf("  [%zu] %s  %10zu bytes  %s\n",
                    i, tn, c.data.size(),
                    c.sha1.empty() ? "(no sha1)" : c.sha1.c_str());
    }

    // Pull the top-level JSON and resolve the first frame's three sha1 refs.
    const std::string top = r.TopLevelJson();
    if (top.empty())
    {
        std::printf("\n  no top-level JSON\n");
        return 0;
    }
    std::printf("\n  top-level JSON (%zu bytes)\n", top.size());

    auto resolve = [&](const char* key) {
        const std::string s = LFPJsonFindString(top, key);
        if (s.empty()) { std::printf("    %s: <missing>\n", key); return; }
        const LFPChunk* c = r.FindBySha1(s);
        std::printf("    %s -> %s %s (%zu bytes)\n",
                    key, s.c_str(),
                    c ? "FOUND" : "MISSING",
                    c ? c->data.size() : 0u);
    };
    resolve("metadataRef");
    resolve("privateMetadataRef");
    resolve("imageRef");

    // For -stk files, list every imageRef in document order so we can see
    // the focal-plane refocus stack (lots of entries).
    const auto allImageRefs = LFPJsonFindAllStrings(top, "imageRef");
    std::printf("\n  imageRef occurrences: %zu\n", allImageRefs.size());
    for (size_t i = 0; i < allImageRefs.size() && i < 12; ++i)
    {
        const LFPChunk* c = r.FindBySha1(allImageRefs[i]);
        std::printf("    [%zu] %s %s (%zu bytes)\n",
                    i, allImageRefs[i].c_str(),
                    c ? "FOUND" : "MISSING",
                    c ? c->data.size() : 0u);
    }
    if (allImageRefs.size() > 12)
        std::printf("    ... and %zu more\n", allImageRefs.size() - 12);

    // Parse + dump calibration if we can find a metadataRef chunk.
    // (Lytro stores secondary JSON in LFC chunks, not LFM, so we don't
    // gate on chunk->type -- the metadataRef chunk's content IS JSON
    // regardless of how Lytro tagged it.)
    const std::string metaRef = LFPJsonFindString(top, "metadataRef");
    if (!metaRef.empty())
    {
        const LFPChunk* mc = r.FindBySha1(metaRef);
        if (mc)
        {
            LFPCalibration cal;
            const bool ok = LFPParseCalibration(mc->AsString(), cal);
            std::printf("\n  calibration parse: %s\n", ok ? "OK" : "INCOMPLETE");
            std::printf("    camera        : %s %s\n",
                        cal.cameraMake.c_str(), cal.cameraModel.c_str());
            std::printf("    sensor        : %d x %d, %d bpp %s, pitch %.3f um\n",
                        cal.sensorWidth, cal.sensorHeight, cal.bitsPerPixel,
                        cal.bigEndian ? "BE" : "LE",
                        cal.pixelPitchM * 1e6);
            std::printf("    bayer         : %s, upper-left %s\n",
                        cal.bayerTile.c_str(), cal.upperLeftPixel.c_str());
            std::printf("    black levels  : R %.1f  Gr %.1f  Gb %.1f  B %.1f\n",
                        cal.blackR, cal.blackGR, cal.blackGB, cal.blackB);
            std::printf("    WB gains      : R %.4f  Gr %.4f  Gb %.4f  B %.4f\n",
                        cal.wbR, cal.wbGR, cal.wbGB, cal.wbB);
            std::printf("    lens          : f=%.3f mm, f/%.2f, exit pupil z=%.3f mm\n",
                        cal.focalLengthM * 1e3, cal.fNumber, cal.exitPupilOffsetZ * 1e3);
            std::printf("    aperture diam : %.3f mm\n",
                        cal.ApertureDiameterM() * 1e3);
            std::printf("    MLA tiling    : %s\n", cal.mlaTiling.c_str());
            std::printf("    MLA lens pitch: %.3f um (%.2f px/lens)\n",
                        cal.mlaLensPitchM * 1e6,
                        cal.pixelPitchM > 0 ? cal.mlaLensPitchM / cal.pixelPitchM : 0.0);
            std::printf("    MLA rotation  : %.6f rad (%.4f deg)\n",
                        cal.mlaRotationRad, cal.mlaRotationRad * 180.0 / 3.14159265358979);
            std::printf("    MLA scale     : x %.6f  y %.6f\n",
                        cal.mlaScaleFactorX, cal.mlaScaleFactorY);
            std::printf("    MLA offset    : x %.3f um  y %.3f um  z %.3f um\n",
                        cal.mlaSensorOffsetXM * 1e6,
                        cal.mlaSensorOffsetYM * 1e6,
                        cal.mlaSensorOffsetZM * 1e6);
            std::printf("    CCM RGB->sRGB :\n      %8.4f %8.4f %8.4f\n      %8.4f %8.4f %8.4f\n      %8.4f %8.4f %8.4f\n",
                        cal.ccmRgbToSrgb[0], cal.ccmRgbToSrgb[1], cal.ccmRgbToSrgb[2],
                        cal.ccmRgbToSrgb[3], cal.ccmRgbToSrgb[4], cal.ccmRgbToSrgb[5],
                        cal.ccmRgbToSrgb[6], cal.ccmRgbToSrgb[7], cal.ccmRgbToSrgb[8]);
            std::printf("    gamma         : %.4f\n", cal.gamma);
        }
    }

    return 0;
}
