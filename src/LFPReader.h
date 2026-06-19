// LFPReader.h -- parse Lytro Light Field Picture (.lfp / .lfr / -stk.lfp)
// container files. The container is a sequence of self-describing chunks:
// a file header (LFP magic + version), then alternating LFM (metadata JSON,
// UTF-8) and LFC (binary content) chunks, each identified by a sha1 hash
// the JSON references. The top-level JSON sits in the first LFM chunk and
// describes which sha1 holds the raw sensor image, which holds calibration,
// which hold per-focal-plane JPGs (for -stk refocus stacks), etc.
//
// This reader is the foundation for both tiers of LFP support:
//   Tier 1 (#76): -stk focal-stack viewer (parse, find refocusStack
//                 acceleration entry, pull out each plane's JPG, present
//                 as a 2D viewer with a focus slider).
//   Tier 2 (#78): raw .lfp direct plenoptic-to-stereo render (additionally
//                 needs to decode the raw sensor LFC + parse the
//                 privateMetadata calibration LFM).
//
// Format reference: derived from byte inspection of Lytro Illum test files
// + cross-referenced against lfp-tools / lfpsplitter behavior. Lytro never
// published an authoritative spec; the format has been stable since 2012.
#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace srw
{
    // One chunk inside the LFP container. Type = the first 4 magic bytes
    // (LFP for header, LFM for metadata-JSON, LFC for binary content).
    // For LFM/LFC the sha1 is the "sha1-..." identifier from the chunk
    // header (other JSON chunks reference content by these sha1s).
    struct LFPChunk
    {
        enum Type { File, Metadata, Content };
        Type                 type = Content;
        std::string          sha1;     // "sha1-<40 hex>" or empty (for the file header)
        std::vector<uint8_t> data;     // raw chunk payload (JSON text for Metadata, bytes for Content)

        // Convenience: payload as a std::string (works for both since JSON
        // is just text and binary lookups don't care).
        std::string AsString() const
        {
            return std::string(data.begin(), data.end());
        }
    };

    class LFPReader
    {
    public:
        // Parse an LFP container from disk. On success, m_chunks holds
        // every chunk in file order and m_bySha1 maps sha1 -> chunk index
        // for content lookup. Returns false on bad magic / truncated file.
        bool LoadFromFile(const std::string& path);

        // First LFM chunk is always the "top-level" picture JSON that
        // names the other chunks via sha1 refs. Returns empty string if
        // no metadata chunk was found.
        std::string TopLevelJson() const;

        // Look up a chunk by its sha1 identifier (e.g. one referenced
        // from JSON as "sha1-031d1e..."). Returns nullptr if not found.
        const LFPChunk* FindBySha1(const std::string& sha1) const;

        // Pull every Metadata chunk's JSON (debug helper -- e.g. dump
        // them all to inspect calibration etc).
        std::vector<std::string> AllJson() const;

        const std::vector<LFPChunk>& Chunks() const { return m_chunks; }

    private:
        std::vector<LFPChunk>                       m_chunks;
        std::unordered_map<std::string, size_t>     m_bySha1;
    };

    // Tiny JSON-value helper: find the first occurrence of "key" : "value"
    // inside `json` and return the value as a std::string. Returns empty
    // string if the key isn't found. Sufficient for LFP's flat-ish refs
    // (metadataRef / privateMetadataRef / imageRef) without pulling in a
    // full JSON parser. Pattern matches "key" : "string-value" only --
    // numbers / arrays / nested objects need a real parser.
    std::string LFPJsonFindString(const std::string& json, const std::string& key);

    // All sha1 string values associated with a given key, in document order.
    // Useful for: accelerationArray refocus-stack imageRef lists, etc.
    std::vector<std::string> LFPJsonFindAllStrings(const std::string& json,
                                                    const std::string& key);

    // Find the chunk that holds the raw plenoptic sensor data. On F01 the
    // top JSON has a single imageRef and it IS the raw -- easy. On Illum
    // the top JSON has multiple imageRefs: a JPEG thumbnail (~64 KB), the
    // raw plenoptic data (~52 MB), and various accelerator/preview JPGs
    // (~300 KB-1 MB each). Picks the largest -- reliably the raw on both
    // F01 and Illum. Returns nullptr if no imageRef chunks found.
    const LFPChunk* LFPFindRawImageChunk(const LFPReader& r);

    // Numeric value associated with a JSON key. Returns true on a clean
    // parse, false if the key is missing / value isn't numeric. Handles
    // ints, floats, and exponents (Lytro uses scientific notation a lot).
    bool LFPJsonFindNumber(const std::string& json, const std::string& key, double& out);

    // ----- Lytro plenoptic calibration -----------------------------------
    //
    // Parsed from the metadataRef JSON of an LFP/LFR/LFX. Same structure
    // across Lytro F01 and Illum -- just different numbers. Units use the
    // SI metric in the source JSON (metres, etc).
    //
    // Origin convention: the MLA sensor offset is the position of the
    // microlens-array origin (the "(0,0)" microlens centre) measured FROM
    // the sensor centre, in metres. Plus a tiny rotation about z to align
    // the hex grid axes to the sensor pixel grid.
    struct LFPCalibration
    {
        // --- Sensor (raw pixel grid) ---
        int    sensorWidth   = 0;     // pixels
        int    sensorHeight  = 0;
        int    bitsPerPixel  = 0;     // 10 (Illum) or 12 (F01)
        bool   bigEndian     = false; // F01 = big, Illum = little
        double pixelPitchM   = 0.0;   // metres per pixel (1.4e-6 on both)
        // Bayer mosaic tile string: "r,gr:gb,b" (so far always this).
        // upperLeftPixel: one of {"r","gr","gb","b"} -- the Bayer phase
        // of the top-left pixel. F01 = "b", Illum = "b" in samples seen.
        std::string bayerTile        = "r,gr:gb,b";
        std::string upperLeftPixel   = "b";

        // --- Sensor calibration (per-channel black + white levels) ---
        // Raw black level: anything below this is sensor noise, subtract.
        // Raw white level: full-scale (after unpacking 10/12-bit value).
        double blackR  = 0.0, blackGR = 0.0, blackGB = 0.0, blackB = 0.0;
        double whiteR  = 4095.0, whiteGR = 4095.0, whiteGB = 4095.0, whiteB = 4095.0;

        // --- Color science (apply after demosaic) ---
        // Per-channel multiplier (camera white balance).
        double wbR     = 1.0, wbGR = 1.0, wbGB = 1.0, wbB = 1.0;
        // 3x3 row-major matrix: camera RGB -> sRGB linear. Apply, then
        // ^(gamma) to get sRGB encoded (or pass linear straight to the
        // weaver, which already wants linear).
        double ccmRgbToSrgb[9] = { 1,0,0, 0,1,0, 0,0,1 };
        double gamma           = 0.41666;

        // --- Per-camera sensor response normalization (Illum) ------------
        // Lytro Illum embeds devices.sensor.normalizedResponses[*]: the
        // per-channel sensor RGB response measured against a reference
        // illuminant. Different physical cameras have different values
        // (sample bodies in this repo: 0.774/0.730, 0.757/0.804). The
        // raw-Bayer pipeline divides each channel by its response BEFORE
        // WB so the CCM (which was tuned against a normalised sensor)
        // operates on the right colour primaries. Without this, Illum
        // gets the strong magenta/pink cast we were fighting via
        // Gray-World + CCM softening hacks. F01 leaves these at 1.0 --
        // its CCM was tuned for raw-after-WB directly.
        double sensorNormR  = 1.0, sensorNormGr = 1.0;
        double sensorNormGb = 1.0, sensorNormB  = 1.0;
        // Reference CCT the normalizedResponses were measured at -- kept
        // for diagnostics only; the values themselves are CCT-invariant.
        double sensorNormCct = 0.0;

        // --- Per-channel analog gain --------------------------------------
        // devices.sensor.analogGain. Camera-applied ISO multiplier per
        // Bayer channel. F01 has unequal gains (r=2.375, gr=gb=1.75,
        // b=2.031); Illum's are equal in samples seen but the field is
        // always present. Conservative: keep these at 1.0 for now and
        // wire them later if needed -- F01 already decodes correctly
        // without normalising, so adding it untested risks regression.
        double analogGainR  = 1.0, analogGainGr = 1.0;
        double analogGainGb = 1.0, analogGainB  = 1.0;

        // --- Per-CCT colour matrices (Illum) -----------------------------
        // devices.sensor.perCcm[] entries: each (cct, 3x3 CCM) pair. The
        // image is rendered with the CCM that matches the scene's
        // detected CCT (algorithms.awb.computed.cct), interpolated
        // between the two bracketing perCcm entries. Empty for F01 ->
        // we fall back to ccmRgbToSrgb above.
        struct PerCcm { double cct; double ccm[9]; };
        std::vector<PerCcm> perCcm;
        // Detected scene CCT. 0 = absent -> fall back to ccmRgbToSrgb.
        double awbCct = 0.0;

        // --- Main lens (the aperture / viewing parallax range) ---
        double focalLengthM       = 0.0;   // metres
        double fNumber            = 0.0;   // f-stop (focalLength / aperture)
        double exitPupilOffsetZ   = 0.0;   // metres, distance from sensor
        // Derived: aperture diameter in metres (focalLength / fNumber).
        double ApertureDiameterM() const
        {
            return fNumber > 0.0 ? focalLengthM / fNumber : 0.0;
        }

        // --- Microlens array (THE plenoptic calibration) ---
        std::string mlaTiling     = "hexUniformRowMajor";
        double mlaLensPitchM      = 0.0;   // metres between adjacent microlens centres (across U-axis)
        double mlaRotationRad     = 0.0;   // hex grid tilt about z (radians)
        double mlaScaleFactorX    = 1.0;
        double mlaScaleFactorY    = 1.0;   // typically very close to 1, accounts for sensor aspect
        double mlaSensorOffsetXM  = 0.0;   // metres -- MLA origin relative to sensor centre
        double mlaSensorOffsetYM  = 0.0;
        double mlaSensorOffsetZM  = 0.0;

        // --- Identification ---
        std::string cameraMake;
        std::string cameraModel;             // "F01" or "ILLUM"

        // Returns true if the parse populated at least the minimum set of
        // fields the renderer needs (sensor + MLA + lens + Bayer).
        bool IsUsable() const
        {
            return sensorWidth > 0 && sensorHeight > 0
                && bitsPerPixel >= 8 && pixelPitchM > 0
                && mlaLensPitchM > 0 && focalLengthM > 0 && fNumber > 0;
        }
    };

    // Parse the calibration out of an LFP metadata JSON chunk (the one
    // referenced by "metadataRef" at top level). Returns true if it found
    // enough fields to render. Tolerant of unknown/extra fields -- only
    // misses are reported via Log + IsUsable() returning false.
    bool LFPParseCalibration(const std::string& metadataJson, LFPCalibration& out);

    // -----------------------------------------------------------------------
    // Phase 2: raw sensor unpacking. Lytro packs the raw Bayer data into
    // a bit-packed byte stream -- 12 bits per pixel big-endian for F01,
    // 10 bits per pixel little-endian for Illum. Both formats unpack
    // into a per-pixel uint16 array sized sensorWidth * sensorHeight,
    // with values in [0 .. 2^bitsPerPixel-1]. No black-level subtract or
    // gain is applied -- callers do that as part of normalisation.
    // -----------------------------------------------------------------------
    std::vector<uint16_t> LFPUnpackBayer(const std::vector<uint8_t>& packed,
                                          const LFPCalibration& cal);

    // -----------------------------------------------------------------------
    // Microlens-array geometry helpers. Lytro's "hexUniformRowMajor" lays
    // out the MLA as flat-top hex: even rows aligned to col*pitch, odd
    // rows offset by +pitch/2, vertical spacing pitch*sqrt(3)/2. The MLA
    // is then scaled (slight aspect correction) + rotated by mlaRotation
    // + offset by mlaSensorOffset from the sensor centre.
    //
    // LFPMicrolensCentreSensor: world (col, row) -> sub-pixel (x, y) on
    //   the sensor's pixel grid (top-left-origin). Floating point.
    // LFPMicrolensGridDims: returns the (cols, rows) of microlenses that
    //   fit inside the sensor's active area, conservatively shrunk so
    //   no lens spills off the edge.
    // -----------------------------------------------------------------------
    void LFPMicrolensCentreSensor(int col, int row, const LFPCalibration& cal,
                                   double& outSensorX, double& outSensorY);
    void LFPMicrolensGridDims(const LFPCalibration& cal,
                               int& outCols, int& outRows);

    // -----------------------------------------------------------------------
    // Bayer phase lookup. Returns 0=R, 1=Gr, 2=Gb, 3=B for the given
    // sensor pixel based on the calibration's tile + upperLeftPixel.
    // -----------------------------------------------------------------------
    int LFPBayerPhase(int sensorX, int sensorY, const LFPCalibration& cal);

    // -----------------------------------------------------------------------
    // Phase 3 verification: extract a single sub-aperture view as 8-bit
    // sRGB pixels. apertureU / apertureV are normalised aperture coords
    // in [-1, +1] (0, 0 = centre view, looking straight through the main
    // lens). For each microlens we sample the pixel at (cx + apertureU *
    // lensRadius, cy + apertureV * lensRadius) and demosaic via nearest-
    // neighbour interpolation from same-microlens Bayer neighbours.
    // Output: tightly-packed RGB8 buffer at the MLA grid resolution.
    // Applies WB + linear gamma; CCM intentionally skipped for clarity.
    // -----------------------------------------------------------------------
    std::vector<uint8_t> LFPExtractSubApertureView(
        const std::vector<uint16_t>& bayer,
        const LFPCalibration& cal,
        float apertureU, float apertureV,
        int& outW, int& outH);

    // -----------------------------------------------------------------------
    // Plenoptic-aware full-sensor demosaic. Standard Bayer demosaic blends
    // neighbours across microlens boundaries -- but neighbouring microlenses
    // represent DIFFERENT scene points, so cross-blending contaminates the
    // angular samples (the "grain" / colour fringing visible in the
    // Phase 3 verify dumps). Fix: confine Bayer interpolation to same-lens
    // neighbours only.
    //
    // Process: builds an in-memory (col, row) lens-membership lookup per
    // sensor pixel via inverse hex-grid math, then per-pixel Bayer demosaic
    // restricted to neighbours sharing the same lens. Applies black-level
    // subtract + WB + CCM + gamma in one pass. Output: tightly-packed RGB8
    // at full sensor resolution.
    //
    // Memory: ~3 * sensorW * sensorH bytes for the RGB output + a 4-byte
    // lookup per pixel during processing (released on return). ~160 MB
    // working set for Illum, ~40 MB for F01.
    //
    // Cost: one second-ish for Illum on a desktop CPU. Cached per LFP
    // load; subsequent sub-aperture extractions read from it cheaply.
    // -----------------------------------------------------------------------
    std::vector<uint8_t> LFPDemosaicPlenopticAware(
        const std::vector<uint16_t>& bayer,
        const LFPCalibration& cal);

    // Sample a sub-aperture view from a pre-demosaiced full-sensor RGB8
    // buffer (output of LFPDemosaicPlenopticAware). Per output pixel:
    // look up the corresponding microlens centre, sample the demosaiced
    // RGB at (centre + aperture * lensRadius) bilinearly. Much cleaner
    // than the raw-Bayer LFPExtractSubApertureView path because each
    // output pixel is sampled from a properly-demosaiced source.
    std::vector<uint8_t> LFPExtractSubApertureViewFromRgb(
        const std::vector<uint8_t>& sensorRgb,
        const LFPCalibration& cal,
        float apertureU, float apertureV,
        int& outW, int& outH);

    // Phase 4-v1 convenience: load an LFP file end-to-end and produce an
    // RGBA8 side-by-side stereo pair, suitable for direct upload via the
    // existing Full-SBS pipeline. Output width = 2 * viewW, height = viewH.
    // apertureL / apertureR are the U positions in normalised aperture
    // coords (-1..+1, where 0 = centre, -0.7 / +0.7 = near-extreme L / R).
    // Both eyes get V=0 (centre vertically). Returns false on any
    // unrecoverable failure (file parse, calibration missing, etc).
    bool LFPLoadAsStereoSBS(const std::string& path,
                            std::vector<uint8_t>& outRgba,
                            int& outW, int& outH,
                            float apertureL = -0.7f, float apertureR = +0.7f);

    // Phase 4-v2: load an LFP file all the way through the calibration +
    // unpack + plenoptic demosaic stages, returning the demosaiced full-
    // sensor RGB buffer and the parsed calibration. The caller (typically
    // LFPRenderer) handles GPU upload and per-frame aperture sampling.
    // Returns false on any unrecoverable failure.
    bool LFPLoadDemosaicedSensor(const std::string& path,
                                  std::vector<uint8_t>& outSensorRgb,
                                  LFPCalibration& outCal);

    // Extract the embedded JPG preview chunk from an LFR/LFP. Lytro
    // ships a 704x480 colour-accurate preview rendered by their own
    // pipeline -- useful as a 2D fallback when our plenoptic colour
    // pipeline can't reproduce their look (Illum especially, since we
    // don't have per-camera WhiteImage calibration). Scans for the
    // first JPEG (FF D8 FF) in the file and reads through to FF D9.
    // Returns the JPG bytes; caller decodes via stb_image.
    bool LFPExtractEmbeddedPreview(const std::string& path,
                                    std::vector<uint8_t>& outJpgBytes);

    // Just the camera-model string from the LFR metadata, fast-parsed
    // (no demosaic, no calibration). For the load-path branch that
    // decides between embedded-preview (Illum) and plenoptic render
    // (F01) without doing 100+ MB of work first. Returns "" if not
    // found.
    std::string LFPProbeCameraModel(const std::string& path);

    // Load an Illum ESLF (Extended Sub-aperture Light Field) PNG --
    // these are 7574x5264, 16-bit RGBA images that ARE the Lytro-
    // rendered colour-corrected microlens array (Lytro Desktop's
    // demosaic + colour pipeline output saved as PNG, cropped to the
    // active microlens region). Loading one gives us correct colours
    // AND the per-microlens detail needed for plenoptic sub-aperture
    // sampling -- so the LFPRenderer can produce proper 3D stereo
    // with Lytro's colour science already baked in. Returns the PNG
    // contents as 8-bit RGB + a synthesised Illum calibration (per-
    // camera-body values aren't available without an accompanying
    // LFR, so we use Illum-typical defaults: 20um lens pitch, the
    // standard MLA hex tiling, etc).
    bool LFPLoadEslfAsSensorRgb(const std::string& path,
                                  std::vector<uint8_t>& outSensorRgb,
                                  LFPCalibration& outCal);

    // True if the path looks like an ESLF PNG (filename ends in
    // _eslf.png or _qs14x14*.png; these are the two naming
    // conventions used by Lytro Desktop's "Export -> Light Field
    // Image" feature).
    bool IsLytroEslfPng(const std::string& path);
}
