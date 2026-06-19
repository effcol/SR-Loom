// lfp_view.cpp -- end-to-end Phase 3 verification. Loads an LFP file,
// parses calibration, unpacks the raw Bayer sensor data, extracts one
// sub-aperture view via the microlens grid + plenoptic sampling, and
// writes the result as a PPM image (open with IrfanView / GIMP / etc).
//
// Usage:
//   lfp_view.exe <file.lfp> <output.ppm> [apertureU] [apertureV]
//
// apertureU / apertureV default to (0, 0) = centre view. Range -1..+1.
// Try (-0.7, 0) and (+0.7, 0) to see the left/right stereo extremes.
//
// Build:
//   cl /std:c++17 /EHsc tools/lfp_view.cpp src/LFPReader.cpp /I src
#include "../src/LFPReader.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace srw;

static bool WritePPM(const std::string& path, int w, int h,
                     const std::vector<uint8_t>& rgb)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: %s <file.lfp> <output.ppm> [apertureU] [apertureV]\n", argv[0]);
        return 1;
    }
    const std::string in  = argv[1];
    const std::string out = argv[2];
    const float u = (argc > 3) ? (float)std::atof(argv[3]) : 0.0f;
    const float v = (argc > 4) ? (float)std::atof(argv[4]) : 0.0f;

    LFPReader r;
    if (!r.LoadFromFile(in)) { std::printf("parse failed\n"); return 2; }

    const std::string top = r.TopLevelJson();
    const std::string metaRef = LFPJsonFindString(top, "metadataRef");
    if (metaRef.empty()) { std::printf("missing metadataRef\n"); return 3; }
    const LFPChunk* mc = r.FindBySha1(metaRef);
    const LFPChunk* ic = LFPFindRawImageChunk(r);
    if (!mc || !ic) { std::printf("ref not found in file\n"); return 4; }
    std::printf("raw image chunk: %s (%zu bytes)\n", ic->sha1.c_str(), ic->data.size());

    LFPCalibration cal;
    if (!LFPParseCalibration(mc->AsString(), cal))
    {
        std::printf("calibration parse failed (unusable)\n");
        return 5;
    }
    std::printf("camera %s %s, sensor %dx%d %dbpp %s, mla pitch %.2fum, ap %.2fmm\n",
                cal.cameraMake.c_str(), cal.cameraModel.c_str(),
                cal.sensorWidth, cal.sensorHeight, cal.bitsPerPixel,
                cal.bigEndian ? "BE" : "LE",
                cal.mlaLensPitchM * 1e6, cal.ApertureDiameterM() * 1e3);

    auto bayer = LFPUnpackBayer(ic->data, cal);
    std::printf("unpacked %zu pixels (%zu expected)\n",
                bayer.size(), (size_t)cal.sensorWidth * cal.sensorHeight);

    int cols = 0, rows = 0;
    LFPMicrolensGridDims(cal, cols, rows);
    std::printf("MLA grid usable: %d x %d microlenses\n", cols, rows);

    int w = 0, h = 0;
    auto rgb = LFPExtractSubApertureView(bayer, cal, u, v, w, h);
    std::printf("sub-aperture view (u=%.2f v=%.2f): %d x %d\n", u, v, w, h);

    if (!WritePPM(out, w, h, rgb))
    {
        std::printf("write %s failed\n", out.c_str());
        return 6;
    }
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), rgb.size() + 32);
    return 0;
}
