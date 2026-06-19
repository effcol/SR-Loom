// LFPReader.cpp -- see header.
#include "LFPReader.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>

using namespace srw;

namespace
{
    // Each chunk begins with 8 bytes: 4-char ASCII type + the standard
    // PNG-style "0D 0A 1A 0A" binary-safety footer that Lytro borrowed.
    constexpr uint8_t kFileSig[8]    = { 0x89,'L','F','P', 0x0D,0x0A,0x1A,0x0A };
    constexpr uint8_t kMetadataSig[8]= { 0x89,'L','F','M', 0x0D,0x0A,0x1A,0x0A };
    constexpr uint8_t kContentSig[8] = { 0x89,'L','F','C', 0x0D,0x0A,0x1A,0x0A };

    // Sha1 identifier field is always 80 bytes, null-padded after the
    // "sha1-<40 hex>" prefix. Header sig + length + sha1 = 8 + 8 + 80 = 96.
    constexpr size_t kSha1FieldSize = 80;

    // Read big-endian uint64 from buf[0..8). Lytro picked BE -- the
    // PNG-style chunk format does too, this isn't an accident.
    uint64_t ReadBE64(const uint8_t* buf)
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | buf[i];
        return v;
    }

    // Slurp an entire file. Returns empty vector on failure.
    std::vector<uint8_t> ReadFile(const std::string& path)
    {
        std::vector<uint8_t> data;
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return data;
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0)
        {
            data.resize((size_t)sz);
            const size_t got = fread(data.data(), 1, (size_t)sz, f);
            if (got != (size_t)sz) data.clear();
        }
        fclose(f);
        return data;
    }

    // sha1 identifier is null-padded -- find the null terminator (or use
    // the full field width) to get the string.
    std::string ReadSha1Id(const uint8_t* buf)
    {
        const char* s = reinterpret_cast<const char*>(buf);
        size_t n = 0;
        while (n < kSha1FieldSize && s[n]) ++n;
        return std::string(s, n);
    }
}

bool LFPReader::LoadFromFile(const std::string& path)
{
    m_chunks.clear();
    m_bySha1.clear();

    std::vector<uint8_t> file = ReadFile(path);
    if (file.size() < 16) return false;

    // File header: LFP magic + 8-byte version field.
    if (memcmp(file.data(), kFileSig, 8) != 0)
        return false;
    {
        LFPChunk fh;
        fh.type = LFPChunk::File;
        fh.data.assign(file.begin() + 8, file.begin() + 16);
        m_chunks.push_back(std::move(fh));
    }

    // Walk subsequent chunks. Each one is 8-byte sig + 8-byte BE length +
    // 80-byte sha1 + length bytes of payload, then padded up to a 16-byte
    // boundary (Lytro's "chunks always start aligned" convention).
    size_t pos = 16;
    while (pos + 16 <= file.size())
    {
        // Identify chunk type from magic.
        LFPChunk::Type type = LFPChunk::Content;
        if      (memcmp(file.data() + pos, kMetadataSig, 8) == 0) type = LFPChunk::Metadata;
        else if (memcmp(file.data() + pos, kContentSig,  8) == 0) type = LFPChunk::Content;
        else
        {
            // Unknown magic -- either we got out of sync or there's a
            // chunk type we don't know about. Log and stop; the chunks
            // collected so far are still usable.
            Log("LFPReader: unknown chunk magic at offset %zu in %s", pos, path.c_str());
            break;
        }
        pos += 8;

        if (pos + 8 > file.size()) break;
        const uint64_t dataLen = ReadBE64(file.data() + pos);
        pos += 8;

        if (pos + kSha1FieldSize > file.size()) break;
        const std::string sha1 = ReadSha1Id(file.data() + pos);
        pos += kSha1FieldSize;

        if (pos + dataLen > file.size())
        {
            Log("LFPReader: chunk %s claims %llu bytes but file has %zu remaining",
                sha1.c_str(), (unsigned long long)dataLen, file.size() - pos);
            break;
        }

        LFPChunk c;
        c.type = type;
        c.sha1 = sha1;
        c.data.assign(file.begin() + pos, file.begin() + pos + (size_t)dataLen);
        pos += (size_t)dataLen;

        // Align to next 16-byte boundary -- Lytro pads the trailing area
        // of each chunk with zeros up to the boundary.
        const size_t aligned = (pos + 15) & ~static_cast<size_t>(15);
        pos = aligned;

        if (!sha1.empty())
            m_bySha1[sha1] = m_chunks.size();
        m_chunks.push_back(std::move(c));
    }

    Log("LFPReader: parsed %zu chunks from %s (file size %zu)",
        m_chunks.size(), path.c_str(), file.size());
    return !m_chunks.empty();
}

std::string LFPReader::TopLevelJson() const
{
    for (const auto& c : m_chunks)
        if (c.type == LFPChunk::Metadata) return c.AsString();
    return {};
}

const LFPChunk* LFPReader::FindBySha1(const std::string& sha1) const
{
    auto it = m_bySha1.find(sha1);
    if (it == m_bySha1.end()) return nullptr;
    return &m_chunks[it->second];
}

std::vector<std::string> LFPReader::AllJson() const
{
    std::vector<std::string> out;
    for (const auto& c : m_chunks)
        if (c.type == LFPChunk::Metadata)
            out.push_back(c.AsString());
    return out;
}

// ---------------------------------------------------------------------------
// Tiny JSON helpers. LFP's JSON is well-formed and Lytro consistently writes
// string values in double quotes, so a substring search for `"key" : "value"`
// is reliable enough for our needs. Spec compliance not attempted.
// ---------------------------------------------------------------------------

namespace
{
    // Skip whitespace (incl. \n / \t -- which Lytro uses to indent).
    size_t SkipWs(const std::string& s, size_t p)
    {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
            ++p;
        return p;
    }

    // From the first `"key"` occurrence onward, find the colon + the string
    // value following it (if it IS a string). Returns empty when not found
    // or when the value isn't a string literal. Mutates outNext to the
    // position right after the consumed value so the caller can continue
    // searching for further occurrences.
    bool MatchKeyString(const std::string& json, const std::string& key,
                        size_t fromPos, std::string& outValue, size_t& outNext)
    {
        const std::string needle = "\"" + key + "\"";
        size_t kp = json.find(needle, fromPos);
        while (kp != std::string::npos)
        {
            size_t p = SkipWs(json, kp + needle.size());
            if (p < json.size() && json[p] == ':')
            {
                p = SkipWs(json, p + 1);
                if (p < json.size() && json[p] == '"')
                {
                    const size_t valStart = p + 1;
                    const size_t valEnd   = json.find('"', valStart);
                    if (valEnd != std::string::npos)
                    {
                        outValue = json.substr(valStart, valEnd - valStart);
                        outNext  = valEnd + 1;
                        return true;
                    }
                }
            }
            // Wasn't the right shape; advance past this occurrence and try
            // again -- some other key in the file may share a substring.
            kp = json.find(needle, kp + 1);
        }
        outNext = json.size();
        return false;
    }
}

std::string srw::LFPJsonFindString(const std::string& json, const std::string& key)
{
    std::string value;
    size_t next = 0;
    return MatchKeyString(json, key, 0, value, next) ? value : std::string{};
}

std::vector<std::string> srw::LFPJsonFindAllStrings(const std::string& json,
                                                     const std::string& key)
{
    std::vector<std::string> out;
    size_t pos = 0;
    std::string v;
    while (MatchKeyString(json, key, pos, v, pos))
        out.push_back(std::move(v));
    return out;
}

const srw::LFPChunk* srw::LFPFindRawImageChunk(const LFPReader& r)
{
    // Scan the top JSON for all imageRef occurrences and pick whichever
    // sha1 resolves to the largest data chunk. The raw plenoptic image
    // is the only multi-megabyte payload in a well-formed LFP; small
    // imageRefs are JPEG thumbnails / refocus previews.
    const std::string top = r.TopLevelJson();
    if (top.empty()) return nullptr;
    const std::vector<std::string> refs = LFPJsonFindAllStrings(top, "imageRef");
    const LFPChunk* best = nullptr;
    for (const auto& sha : refs)
    {
        const LFPChunk* c = r.FindBySha1(sha);
        if (!c) continue;
        if (!best || c->data.size() > best->data.size()) best = c;
    }
    return best;
}

bool srw::LFPJsonFindNumber(const std::string& json, const std::string& key, double& out)
{
    const std::string needle = "\"" + key + "\"";
    size_t kp = json.find(needle);
    while (kp != std::string::npos)
    {
        size_t p = SkipWs(json, kp + needle.size());
        if (p < json.size() && json[p] == ':')
        {
            p = SkipWs(json, p + 1);
            // Allow optional sign + digits + optional decimal + optional exponent.
            // Lytro emits things like "-0.00028155732434242963791" and
            // "1.3999999761581419596e-06" so we want strtod-grade parsing.
            if (p < json.size() && (json[p] == '-' || json[p] == '+' ||
                                    (json[p] >= '0' && json[p] <= '9') ||
                                    json[p] == '.'))
            {
                char* end = nullptr;
                const double v = std::strtod(json.c_str() + p, &end);
                if (end && end != json.c_str() + p)
                {
                    out = v;
                    return true;
                }
            }
        }
        kp = json.find(needle, kp + 1);
    }
    return false;
}

bool srw::LFPParseCalibration(const std::string& json, LFPCalibration& out)
{
    // The JSON nests fields several levels deep but every key we want is
    // uniquely named within the document, so the flat-search helpers work
    // (e.g. there's only one "lensPitch", one "focalLength", etc).
    double v = 0;

    // --- Sensor + bit packing ---
    if (LFPJsonFindNumber(json, "width",  v)) out.sensorWidth  = (int)v;
    if (LFPJsonFindNumber(json, "height", v)) out.sensorHeight = (int)v;
    if (LFPJsonFindNumber(json, "bitsPerPixel", v)) out.bitsPerPixel = (int)v;
    {
        const std::string endian = LFPJsonFindString(json, "endianness");
        out.bigEndian = (endian == "big");
    }
    if (LFPJsonFindNumber(json, "pixelPitch", v)) out.pixelPitchM = v;
    out.bayerTile      = LFPJsonFindString(json, "tile");        if (out.bayerTile.empty())      out.bayerTile = "r,gr:gb,b";
    out.upperLeftPixel = LFPJsonFindString(json, "upperLeftPixel"); if (out.upperLeftPixel.empty()) out.upperLeftPixel = "b";

    // --- Black / white levels (the "black" + "white" objects each carry
    // r/gr/gb/b sub-fields). Each sub-key is searched ANCHORED at the
    // outer key (not chained off the previous match) -- Illum and F01
    // emit the sub-keys in different orders within the object, so chained
    // search loses Gb / B on Illum's "gr, r, b, gb" ordering.
    auto findFourFromAnchor = [&](const std::string& after, double& a, double& b,
                                   double& c, double& d) {
        const size_t anchor = json.find("\"" + after + "\"");
        if (anchor == std::string::npos) return false;
        auto pullKey = [&](const std::string& key, double& dst) {
            const std::string nk = "\"" + key + "\"";
            size_t kp = json.find(nk, anchor);
            if (kp == std::string::npos) return false;
            size_t p = SkipWs(json, kp + nk.size());
            if (p >= json.size() || json[p] != ':') return false;
            p = SkipWs(json, p + 1);
            char* end = nullptr;
            const double val = std::strtod(json.c_str() + p, &end);
            if (end == json.c_str() + p) return false;
            dst = val;
            return true;
        };
        const bool gotR  = pullKey("r",  a);
        const bool gotGr = pullKey("gr", b);
        const bool gotGb = pullKey("gb", c);
        const bool gotB  = pullKey("b",  d);
        return gotR && gotGr && gotGb && gotB;
    };
    findFourFromAnchor("black",            out.blackR, out.blackGR, out.blackGB, out.blackB);
    findFourFromAnchor("white",            out.whiteR, out.whiteGR, out.whiteGB, out.whiteB);
    findFourFromAnchor("whiteBalanceGain", out.wbR,    out.wbGR,    out.wbGB,    out.wbB);
    // Illum: prefer algorithms.awb.computed.gain over image.color.whiteBalanceGain.
    // The schema documents image.color.* as "color processing for thumbnails/
    // playback, NOT applied to the sensor image" while algorithms.awb.computed
    // is the actual as-shot AWB. Plenopticam and LFToolbox both use this on
    // Illum (B01). On F01 only image.color.whiteBalanceGain exists -- left
    // intact above. The anchor "awb" lands on algorithms.awb; sub-keys r/gr/gb/b
    // are then searched forward from there and land inside the .computed.gain
    // sub-object.
    {
        double aR = 0, aGR = 0, aGB = 0, aB = 0;
        if (findFourFromAnchor("awb", aR, aGR, aGB, aB))
        {
            out.wbR = aR; out.wbGR = aGR; out.wbGB = aGB; out.wbB = aB;
        }
    }

    // --- Colour matrix. F01 uses key "ccmRgbToSrgbArray", Illum uses
    // "ccm" (inside the color block). Try the F01 name first; if not
    // found, fall back to "ccm". Either way it's a 9-number array.
    auto pullCcmArray = [&](const std::string& key) -> bool {
        const std::string nk = "\"" + key + "\"";
        size_t kp = json.find(nk);
        if (kp == std::string::npos) return false;
        size_t p = SkipWs(json, kp + nk.size());
        if (p >= json.size() || json[p] != ':') return false;
        p = SkipWs(json, p + 1);
        if (p >= json.size() || json[p] != '[') return false;
        ++p;
        for (int i = 0; i < 9; ++i)
        {
            p = SkipWs(json, p);
            char* end = nullptr;
            const double val = std::strtod(json.c_str() + p, &end);
            if (end == json.c_str() + p) return i == 9;
            out.ccmRgbToSrgb[i] = val;
            p = (size_t)(end - json.c_str());
            p = SkipWs(json, p);
            if (p < json.size() && json[p] == ',') ++p;
        }
        return true;
    };
    if (!pullCcmArray("ccmRgbToSrgbArray"))
        pullCcmArray("ccm");

    if (LFPJsonFindNumber(json, "gamma", v)) out.gamma = v;

    // --- Main lens ---
    if (LFPJsonFindNumber(json, "focalLength", v)) out.focalLengthM = v;
    if (LFPJsonFindNumber(json, "fNumber",     v)) out.fNumber      = v;
    // exitPupilOffset has nested "z" -- pull from after the key.
    {
        const std::string nk = "\"exitPupilOffset\"";
        size_t kp = json.find(nk);
        if (kp != std::string::npos)
        {
            size_t p = json.find("\"z\"", kp);
            if (p != std::string::npos)
            {
                p = SkipWs(json, p + 3);
                if (p < json.size() && json[p] == ':')
                {
                    p = SkipWs(json, p + 1);
                    char* end = nullptr;
                    const double val = std::strtod(json.c_str() + p, &end);
                    if (end != json.c_str() + p) out.exitPupilOffsetZ = val;
                }
            }
        }
    }

    // --- Microlens array ---
    out.mlaTiling = LFPJsonFindString(json, "tiling");
    if (out.mlaTiling.empty()) out.mlaTiling = "hexUniformRowMajor";
    if (LFPJsonFindNumber(json, "lensPitch", v)) out.mlaLensPitchM = v;
    if (LFPJsonFindNumber(json, "rotation",  v)) out.mlaRotationRad = v;
    // scaleFactor + sensorOffset are nested {x,y[,z]} -- pull via sub-key
    // searches anchored at the outer key for safety.
    auto pullNestedXYZ = [&](const std::string& outer,
                              double& x, double& y, double& z) {
        const std::string nk = "\"" + outer + "\"";
        size_t kp = json.find(nk);
        if (kp == std::string::npos) return;
        auto pullSub = [&](const std::string& sub, double& dst, size_t fromPos) {
            const std::string sn = "\"" + sub + "\"";
            size_t sp = json.find(sn, fromPos);
            if (sp == std::string::npos) return fromPos;
            size_t p = SkipWs(json, sp + sn.size());
            if (p >= json.size() || json[p] != ':') return fromPos;
            p = SkipWs(json, p + 1);
            char* end = nullptr;
            const double val = std::strtod(json.c_str() + p, &end);
            if (end == json.c_str() + p) return fromPos;
            dst = val;
            return (size_t)(end - json.c_str());
        };
        size_t after = kp;
        after = pullSub("x", x, after);
        after = pullSub("y", y, after);
        after = pullSub("z", z, after);
    };
    pullNestedXYZ("scaleFactor",  out.mlaScaleFactorX,
                                  out.mlaScaleFactorY, v /*unused z*/);
    pullNestedXYZ("sensorOffset", out.mlaSensorOffsetXM,
                                  out.mlaSensorOffsetYM, out.mlaSensorOffsetZM);

    out.cameraMake  = LFPJsonFindString(json, "make");
    out.cameraModel = LFPJsonFindString(json, "model");

    // --- analogGain (per-channel ISO gain) ---
    findFourFromAnchor("analogGain",
                       out.analogGainR, out.analogGainGr,
                       out.analogGainGb, out.analogGainB);

    // --- normalizedResponses[0]: per-camera sensor RGB response. The
    // JSON form is "normalizedResponses": [ { ..., "cct": 5100, "r": ...,
    // "b": ... } ]. Anchor on the field name + ":" so we land on the
    // array entry and then pull the four sub-keys (already done by
    // findFourFromAnchor) plus the CCT.
    {
        double nR = 1, nGR = 1, nGB = 1, nB = 1;
        if (findFourFromAnchor("normalizedResponses", nR, nGR, nGB, nB))
        {
            out.sensorNormR = nR; out.sensorNormGr = nGR;
            out.sensorNormGb = nGB; out.sensorNormB = nB;
            // Pull the CCT sub-key from the same anchor.
            const size_t a = json.find("\"normalizedResponses\"");
            if (a != std::string::npos)
            {
                const size_t cp = json.find("\"cct\"", a);
                if (cp != std::string::npos)
                {
                    size_t p = SkipWs(json, cp + 5);
                    if (p < json.size() && json[p] == ':')
                    {
                        p = SkipWs(json, p + 1);
                        char* end = nullptr;
                        const double cv = std::strtod(json.c_str() + p, &end);
                        if (end != json.c_str() + p) out.sensorNormCct = cv;
                    }
                }
            }
        }
    }

    // --- perCcm[]: list of {cct, ccm[9]} entries. Walk the array.
    // Implementation: find "perCcm", then advance through nested objects,
    // pulling one ccm[9] + cct per inner object. Stops on ']' for perCcm.
    {
        const std::string nk = "\"perCcm\"";
        size_t kp = json.find(nk);
        if (kp != std::string::npos)
        {
            size_t p = SkipWs(json, kp + nk.size());
            if (p < json.size() && json[p] == ':')
            {
                p = SkipWs(json, p + 1);
                if (p < json.size() && json[p] == '[')
                {
                    ++p;
                    while (true)
                    {
                        p = SkipWs(json, p);
                        if (p >= json.size() || json[p] == ']') break;
                        if (json[p] != '{') { ++p; continue; }
                        // Find this object's closing brace.
                        size_t depth = 1, e = p + 1;
                        while (e < json.size() && depth > 0)
                        {
                            if (json[e] == '{') ++depth;
                            else if (json[e] == '}') --depth;
                            ++e;
                        }
                        if (depth != 0) break;
                        const std::string sub = json.substr(p, e - p);
                        // Pull ccm[9] + cct out of `sub`.
                        LFPCalibration::PerCcm entry{};
                        const size_t ap = sub.find("\"ccm\"");
                        if (ap != std::string::npos)
                        {
                            size_t q = SkipWs(sub, ap + 5);
                            if (q < sub.size() && sub[q] == ':')
                            {
                                q = SkipWs(sub, q + 1);
                                if (q < sub.size() && sub[q] == '[')
                                {
                                    ++q;
                                    for (int i = 0; i < 9; ++i)
                                    {
                                        q = SkipWs(sub, q);
                                        char* end = nullptr;
                                        const double val = std::strtod(sub.c_str() + q, &end);
                                        if (end == sub.c_str() + q) break;
                                        entry.ccm[i] = val;
                                        q = (size_t)(end - sub.c_str());
                                        q = SkipWs(sub, q);
                                        if (q < sub.size() && sub[q] == ',') ++q;
                                    }
                                }
                            }
                        }
                        const size_t cp = sub.find("\"cct\"");
                        if (cp != std::string::npos)
                        {
                            size_t q = SkipWs(sub, cp + 5);
                            if (q < sub.size() && sub[q] == ':')
                            {
                                q = SkipWs(sub, q + 1);
                                char* end = nullptr;
                                const double val = std::strtod(sub.c_str() + q, &end);
                                if (end != sub.c_str() + q) entry.cct = val;
                            }
                        }
                        if (entry.cct > 0) out.perCcm.push_back(entry);
                        p = e;
                        p = SkipWs(json, p);
                        if (p < json.size() && json[p] == ',') ++p;
                    }
                }
            }
        }
        // Sort perCcm by CCT ascending for clean interpolation.
        std::sort(out.perCcm.begin(), out.perCcm.end(),
                  [](const LFPCalibration::PerCcm& a, const LFPCalibration::PerCcm& b){
                      return a.cct < b.cct;
                  });
    }

    // --- algorithms.awb.computed.cct: the scene CCT used to select /
    // interpolate the perCcm. The "awb" anchor lands on algorithms.awb,
    // then we search forward for "cct". (settings.whiteBalance.cct also
    // exists earlier in the doc with a similar value, but the computed
    // one is what Lytro's pipeline uses.)
    {
        const size_t a = json.find("\"awb\"");
        if (a != std::string::npos)
        {
            const size_t cp = json.find("\"cct\"", a);
            if (cp != std::string::npos)
            {
                size_t p = SkipWs(json, cp + 5);
                if (p < json.size() && json[p] == ':')
                {
                    p = SkipWs(json, p + 1);
                    char* end = nullptr;
                    const double cv = std::strtod(json.c_str() + p, &end);
                    if (end != json.c_str() + p) out.awbCct = cv;
                }
            }
        }
    }

    if (out.cameraModel == "ILLUM")
    {
        // Illum: identity CCM, no metadata WB, post-demosaic Gray-World
        // AWB instead. Why this combo:
        //   - Metadata image.color.ccm and plenopticam's generic CCM
        //     both produce a magenta/blue tint via their negative G
        //     off-diagonals when multiplied by Lytro's typical WB
        //     (R~1.37, B~1.53). Identity CCM sidesteps that entirely.
        //   - Metadata WB pumps R and B above G even when the scene is
        //     genuinely neutral, because Lytro's WB encodes both the
        //     per-camera sensor response AND the scene-illuminant shift
        //     in one gain -- and our pipeline doesn't have the
        //     percentile-renormalize step their renderer has to absorb
        //     that. Skipping it + computing WB from the demosaiced
        //     scene means (Gray-World) avoids the cast entirely.
        //   - Gray-World runs at the end of LFPDemosaicPlenopticAware
        //     and assumes "scene average is grey" -- a fair default for
        //     ordinary photography. Sunsets / strong-tint scenes lose
        //     their cast, which is the price of the simplicity.
        // Illum: full Lytro pipeline (WB on raw -> perCcm CCM in
        // ProPhoto-D50 space -> Blitzen1_1 contrast spline on luma ->
        // Bradford-adapted ProPhoto-D50 -> sRGB-D65 -> sRGB gamma).
        // The CCM and spline values come from disassembly of
        // liblytro-glfe-1.0.so (Illum's on-camera renderer).
        // sensorNorm reset (WB already encodes per-camera response).
        out.sensorNormR = out.sensorNormGr = out.sensorNormGb = out.sensorNormB = 1.0;

        Log("LFPParseCalibration: Illum -- full Lytro pipeline "
            "(perCcm@%.0fK + spline + ProPhoto->sRGB)", out.awbCct);
    }

    if (!out.IsUsable())
    {
        Log("LFPParseCalibration: missing required fields -- "
            "sensor=%dx%d bpp=%d pitch=%g mla=%g focal=%g f=%g",
            out.sensorWidth, out.sensorHeight, out.bitsPerPixel,
            out.pixelPitchM, out.mlaLensPitchM, out.focalLengthM, out.fNumber);
        return false;
    }
    return true;
}

// ===========================================================================
// Phase 2: raw Bayer unpack
// ===========================================================================

std::vector<uint16_t> srw::LFPUnpackBayer(const std::vector<uint8_t>& packed,
                                           const LFPCalibration& cal)
{
    const size_t pixelCount = (size_t)cal.sensorWidth * (size_t)cal.sensorHeight;
    std::vector<uint16_t> out;
    out.reserve(pixelCount);

    if (cal.bitsPerPixel == 12 && cal.bigEndian)
    {
        // F01 packing: 2 pixels per 3 bytes, big-endian.
        //   pixel0 = byte0<<4 | byte1>>4    (high 8 + high 4 bits)
        //   pixel1 = (byte1&0x0F)<<8 | byte2 (low 4 + 8 bits)
        const size_t triplets = packed.size() / 3;
        for (size_t i = 0; i < triplets && out.size() < pixelCount; ++i)
        {
            const uint8_t b0 = packed[i * 3 + 0];
            const uint8_t b1 = packed[i * 3 + 1];
            const uint8_t b2 = packed[i * 3 + 2];
            out.push_back((uint16_t)((b0 << 4) | (b1 >> 4)));
            if (out.size() < pixelCount)
                out.push_back((uint16_t)(((b1 & 0x0F) << 8) | b2));
        }
    }
    else if (cal.bitsPerPixel == 10 && !cal.bigEndian)
    {
        // Illum packing: 4 pixels per 5 bytes, little-endian. The 5th
        // byte carries the high 2 bits of each of the prior 4 pixels:
        //   pixel_i = byte[i] | ((byte[4] >> (i*2)) & 0x03) << 8
        const size_t quintets = packed.size() / 5;
        for (size_t i = 0; i < quintets && out.size() < pixelCount; ++i)
        {
            const uint8_t* q = &packed[i * 5];
            const uint8_t hi = q[4];
            for (int p = 0; p < 4 && out.size() < pixelCount; ++p)
            {
                const uint16_t v = (uint16_t)q[p] | (uint16_t)(((hi >> (p * 2)) & 0x03) << 8);
                out.push_back(v);
            }
        }
    }
    else
    {
        Log("LFPUnpackBayer: unsupported format bpp=%d %s",
            cal.bitsPerPixel, cal.bigEndian ? "BE" : "LE");
        return {};
    }

    // Some Illum captures pack slightly more bytes than the active sensor
    // area requires; some pack slightly less (trailing padding). Pad with
    // black or truncate so the caller always sees exactly sensorW*sensorH.
    if (out.size() < pixelCount)
    {
        Log("LFPUnpackBayer: packed buffer short -- got %zu of %zu pixels, padding with zero",
            out.size(), pixelCount);
        out.resize(pixelCount, 0);
    }
    else if (out.size() > pixelCount)
    {
        out.resize(pixelCount);
    }
    return out;
}

// ===========================================================================
// Microlens-array geometry
// ===========================================================================

namespace
{
    constexpr double kSqrt3Over2 = 0.86602540378443864676;

    // Sensor-pixel coords (top-left origin, sub-pixel) for microlens (col, row).
    // Flat-top hex grid: odd rows offset by +pitch/2 along U.
    void MicrolensCentre(int col, int row, const srw::LFPCalibration& cal,
                         double& outX, double& outY)
    {
        const double pitchM   = cal.mlaLensPitchM;
        // Position in MLA coords (metres, MLA-local origin)
        double mx = col * pitchM + ((row & 1) ? 0.5 * pitchM : 0.0);
        double my = row * pitchM * kSqrt3Over2;
        // Apply scale (anisotropic) then rotation about MLA origin.
        mx *= cal.mlaScaleFactorX;
        my *= cal.mlaScaleFactorY;
        const double cr = cos(cal.mlaRotationRad);
        const double sr = sin(cal.mlaRotationRad);
        const double rx = cr * mx - sr * my;
        const double ry = sr * mx + cr * my;
        // Translate by MLA-origin offset relative to sensor centre, then
        // shift to sensor top-left origin (sensor centre = W/2, H/2).
        const double sxMeters = rx + cal.mlaSensorOffsetXM;
        const double syMeters = ry + cal.mlaSensorOffsetYM;
        const double pxFromCentre = sxMeters / cal.pixelPitchM;
        const double pyFromCentre = syMeters / cal.pixelPitchM;
        outX = pxFromCentre + 0.5 * cal.sensorWidth;
        outY = pyFromCentre + 0.5 * cal.sensorHeight;
    }
}

void srw::LFPMicrolensCentreSensor(int col, int row, const LFPCalibration& cal,
                                    double& outSensorX, double& outSensorY)
{
    MicrolensCentre(col, row, cal, outSensorX, outSensorY);
}

void srw::LFPMicrolensGridDims(const LFPCalibration& cal,
                                int& outCols, int& outRows)
{
    // Conservative grid: shrink so the outermost lenses' radii stay inside
    // the sensor active area. radius = pitch * 0.5 in U; rows step by
    // pitch * sqrt(3)/2 in V.
    const double pitchPx = cal.mlaLensPitchM / cal.pixelPitchM;
    const double radius  = 0.5 * pitchPx;
    const double rowStep = pitchPx * kSqrt3Over2;

    // Find max col / row by stepping outward from MLA origin until a lens
    // centre falls within `radius` of the edge.
    auto fitsAt = [&](int col, int row) {
        double cx, cy;
        MicrolensCentre(col, row, cal, cx, cy);
        return cx > radius && cx < cal.sensorWidth - radius
            && cy > radius && cy < cal.sensorHeight - radius;
    };
    int maxCol = 0;
    while (fitsAt(maxCol + 1, 0)) ++maxCol;
    int maxRow = 0;
    while (fitsAt(0, maxRow + 1)) ++maxRow;
    // The MLA origin sits roughly at the centre of the sensor, but offsets
    // are small (~few microns). Lenses also extend in negative col/row
    // directions; we treat (cols, rows) as the full extent across the
    // sensor, with col=0 at the leftmost-row=0 visible lens.
    int minCol = 0;
    while (fitsAt(minCol - 1, 0)) --minCol;
    int minRow = 0;
    while (fitsAt(0, minRow - 1)) --minRow;
    outCols = maxCol - minCol + 1;
    outRows = maxRow - minRow + 1;
}

// ===========================================================================
// Bayer phase
// ===========================================================================

int srw::LFPBayerPhase(int x, int y, const LFPCalibration& cal)
{
    // Tile layout for "r,gr:gb,b" (the only one Lytro emits): the 2x2 tile
    // when upperLeftPixel = "r" places R at (0,0), Gr at (1,0), Gb at
    // (0,1), B at (1,1). upperLeftPixel shifts the tile origin so sensor
    // (0,0) maps onto the named channel.
    int offX = 0, offY = 0;
    if      (cal.upperLeftPixel == "r")  { offX = 0; offY = 0; }
    else if (cal.upperLeftPixel == "gr") { offX = 1; offY = 0; }
    else if (cal.upperLeftPixel == "gb") { offX = 0; offY = 1; }
    else /* "b" or fallback */            { offX = 1; offY = 1; }
    const int tx = ((x + offX) & 1);
    const int ty = ((y + offY) & 1);
    // Per the tile layout: phase[tx][ty] = R(0)/Gb(2)/Gr(1)/B(3).
    static constexpr int kPhase[2][2] = { {0, 2}, {1, 3} };
    return kPhase[tx][ty];
}

// ===========================================================================
// Phase 3 verify: extract a single sub-aperture view as RGB8
// ===========================================================================

std::vector<uint8_t> srw::LFPExtractSubApertureView(
    const std::vector<uint16_t>& bayer,
    const LFPCalibration& cal,
    float apertureU, float apertureV,
    int& outW, int& outH)
{
    LFPMicrolensGridDims(cal, outW, outH);
    if (outW <= 0 || outH <= 0) { outW = outH = 0; return {}; }

    // Sample radius inside each microlens: half the lens pitch in pixels,
    // a touch shrunken so we don't fall off the lens at the edge of the
    // aperture (lens vignettes hard past ~0.9 of the radius).
    const double pitchPx = cal.mlaLensPitchM / cal.pixelPitchM;
    const double radiusPx = 0.42 * pitchPx;   // ~42% of pitch -> safely inside

    // Compute the (col, row) offset of (0, 0) in the output grid -- the
    // grid wraps from the leftmost-visible lens up to the rightmost. We
    // need to know what (col, row) corresponds to output (0, 0).
    int gridMinCol = 0, gridMinRow = 0;
    auto fits = [&](int col, int row) {
        double cx, cy;
        MicrolensCentre(col, row, cal, cx, cy);
        const double r = 0.5 * pitchPx;
        return cx > r && cx < cal.sensorWidth - r
            && cy > r && cy < cal.sensorHeight - r;
    };
    while (fits(gridMinCol - 1, 0)) --gridMinCol;
    while (fits(0, gridMinRow - 1)) --gridMinRow;

    // Per-channel normalisation: subtract black, divide by (white - black),
    // apply WB gain. CCM intentionally NOT applied here (it's the part the
    // user can decide on once they see the verify dump looks right).
    const double whiteR  = (std::max)(1.0, cal.whiteR  - cal.blackR);
    const double whiteGr = (std::max)(1.0, cal.whiteGR - cal.blackGR);
    const double whiteGb = (std::max)(1.0, cal.whiteGB - cal.blackGB);
    const double whiteB  = (std::max)(1.0, cal.whiteB  - cal.blackB);

    // Normalise raw at sensor pixel -> linear [0, 1] for its Bayer phase.
    auto sampleRaw = [&](int sx, int sy) -> std::pair<int, double> {
        if (sx < 0 || sx >= cal.sensorWidth || sy < 0 || sy >= cal.sensorHeight)
            return { -1, 0.0 };
        const int phase = LFPBayerPhase(sx, sy, cal);
        const double raw = (double)bayer[(size_t)sy * cal.sensorWidth + sx];
        double linear = 0.0;
        switch (phase)
        {
        case 0: linear = std::clamp((raw - cal.blackR)  / whiteR,  0.0, 1.0) * cal.wbR;  break;
        case 1: linear = std::clamp((raw - cal.blackGR) / whiteGr, 0.0, 1.0) * cal.wbGR; break;
        case 2: linear = std::clamp((raw - cal.blackGB) / whiteGb, 0.0, 1.0) * cal.wbGB; break;
        case 3: linear = std::clamp((raw - cal.blackB)  / whiteB,  0.0, 1.0) * cal.wbB;  break;
        }
        return { phase, linear };
    };

    std::vector<uint8_t> rgb((size_t)outW * outH * 3, 0);

    for (int oy = 0; oy < outH; ++oy)
    {
        for (int ox = 0; ox < outW; ++ox)
        {
            const int col = ox + gridMinCol;
            const int row = oy + gridMinRow;
            double cx, cy;
            MicrolensCentre(col, row, cal, cx, cy);
            // Aperture sample point within the lens (lens-local pixel coords).
            const double sampleX = cx + apertureU * radiusPx;
            const double sampleY = cy + apertureV * radiusPx;

            // Demosaic: find one nearest R, one G (avg of nearest Gr+Gb),
            // and one nearest B by scanning a small neighbourhood (3x3)
            // around the sample point. Always picking same-lens pixels
            // since 3x3 < pitchPx; cross-lens contamination is impossible.
            double rSum = 0, gSum = 0, bSum = 0;
            int    rN = 0, gN = 0, bN = 0;
            const int isx = (int)floor(sampleX);
            const int isy = (int)floor(sampleY);
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto [phase, v] = sampleRaw(isx + dx, isy + dy);
                    if (phase < 0) continue;
                    if      (phase == 0) { rSum += v; ++rN; }
                    else if (phase == 3) { bSum += v; ++bN; }
                    else                 { gSum += v; ++gN; }
                }
            }
            if (rN == 0 || gN == 0 || bN == 0) continue;   // off-edge / missing channel
            double r = rSum / rN;
            double g = gSum / gN;
            double b = bSum / bN;

            // Apply CCM (camera RGB -> sRGB linear). The CCM has negative
            // entries -- it's a 3x3 rotation between colour-space bases.
            // Without this, Lytro raw RGB has heavy magenta/pink cast.
            const double r2 = cal.ccmRgbToSrgb[0]*r + cal.ccmRgbToSrgb[1]*g + cal.ccmRgbToSrgb[2]*b;
            const double g2 = cal.ccmRgbToSrgb[3]*r + cal.ccmRgbToSrgb[4]*g + cal.ccmRgbToSrgb[5]*b;
            const double b2 = cal.ccmRgbToSrgb[6]*r + cal.ccmRgbToSrgb[7]*g + cal.ccmRgbToSrgb[8]*b;
            r = r2; g = g2; b = b2;

            // sRGB encode via the calibration's gamma (Lytro stores ~1/2.4,
            // so this is gamma -> sRGB encode in one step).
            const double g_ = cal.gamma > 0.0 ? cal.gamma : 0.4167;
            r = pow((std::max)(0.0, (std::min)(1.0, r)), g_);
            g = pow((std::max)(0.0, (std::min)(1.0, g)), g_);
            b = pow((std::max)(0.0, (std::min)(1.0, b)), g_);

            const size_t off = ((size_t)oy * outW + ox) * 3;
            // (std::min) -- parens dodge the legacy <windows.h> min/max macros
            rgb[off + 0] = (uint8_t)(std::min)(255, (int)(r * 255.0 + 0.5));
            rgb[off + 1] = (uint8_t)(std::min)(255, (int)(g * 255.0 + 0.5));
            rgb[off + 2] = (uint8_t)(std::min)(255, (int)(b * 255.0 + 0.5));
        }
    }
    return rgb;
}

// ===========================================================================
// Plenoptic-aware full-sensor demosaic
// ===========================================================================

namespace
{
    // Inverse hex-grid math: sensor pixel (sx, sy) -> nearest microlens
    // (col, row). Mirror of MicrolensCentre but going the other way.
    inline void SensorToNearestLens(int sx, int sy, const srw::LFPCalibration& cal,
                                     int& outCol, int& outRow)
    {
        // 1) Sensor pixel -> sensor-centre-relative pixels (matches MLA-origin math)
        const double pxFromC = sx - 0.5 * cal.sensorWidth;
        const double pyFromC = sy - 0.5 * cal.sensorHeight;
        // 2) Subtract MLA origin offset (metres -> pixels)
        const double mxPx = pxFromC - cal.mlaSensorOffsetXM / cal.pixelPitchM;
        const double myPx = pyFromC - cal.mlaSensorOffsetYM / cal.pixelPitchM;
        // 3) Undo MLA rotation (rotate by -mlaRotation)
        const double cr = cos(-cal.mlaRotationRad);
        const double sr = sin(-cal.mlaRotationRad);
        const double rx = cr * mxPx - sr * myPx;
        const double ry = sr * mxPx + cr * myPx;
        // 4) Undo scale
        const double mx = (cal.mlaScaleFactorX != 0.0) ? rx / cal.mlaScaleFactorX : rx;
        const double my = (cal.mlaScaleFactorY != 0.0) ? ry / cal.mlaScaleFactorY : ry;
        // 5) Snap to nearest hex grid (in MLA coords, expressed in pixels)
        const double pitchPx = cal.mlaLensPitchM / cal.pixelPitchM;
        const double rowStep = pitchPx * kSqrt3Over2;
        // Round row first, then col (col depends on row's parity for hex offset).
        const int row = (int)std::lround(my / rowStep);
        const double colOffset = (row & 1) ? 0.5 * pitchPx : 0.0;
        const int col = (int)std::lround((mx - colOffset) / pitchPx);
        outCol = col;
        outRow = row;
    }

    // Normalise raw sensor value [0, 2^bpp-1] to linear [0, 1] for the
    // given Bayer phase. Subtract black, divide by (white - black), divide
    // by the per-channel sensor response (so the CCM operates on neutral
    // primaries), apply white-balance gain. Returns the value in linear
    // space (pre-CCM, pre-gamma). On F01 sensorNorm* default to 1.0 so
    // that step is a no-op there.
    //
    // wbScale parameter lets the caller disable per-channel WB without
    // re-mutating the calibration: Illum's perCcm entries are already
    // scene-WB-baked (row sums = (1,1,1)) so applying metadata WB on top
    // would double-correct and push white toward magenta. Pass false to
    // skip WB; the post-CCM result then renders neutral on a white input.
    inline double NormalizeRaw(uint16_t raw, int phase, const srw::LFPCalibration& cal, bool applyWb)
    {
        double bl = 0, wh = 1, wb = 1, nr = 1;
        switch (phase)
        {
        case 0: bl = cal.blackR;  wh = (std::max)(1.0, cal.whiteR  - cal.blackR);  wb = cal.wbR;  nr = cal.sensorNormR;  break;
        case 1: bl = cal.blackGR; wh = (std::max)(1.0, cal.whiteGR - cal.blackGR); wb = cal.wbGR; nr = cal.sensorNormGr; break;
        case 2: bl = cal.blackGB; wh = (std::max)(1.0, cal.whiteGB - cal.blackGB); wb = cal.wbGB; nr = cal.sensorNormGb; break;
        case 3: bl = cal.blackB;  wh = (std::max)(1.0, cal.whiteB  - cal.blackB);  wb = cal.wbB;  nr = cal.sensorNormB;  break;
        }
        const double v = ((double)raw - bl) / wh;
        const double n = (nr > 1e-6) ? (v / nr) : v;
        const double clipped = (std::max)(0.0, (std::min)(1.0, n));
        return applyWb ? clipped * wb : clipped;
    }

    // Interpolate the per-CCT CCM table at the scene's awbCct. Picks the
    // bracketing pair, lerps element-wise. If awbCct is below the lowest
    // or above the highest entry, clamps to the nearest. Returns false
    // and leaves outCcm untouched if perCcm is empty (caller falls back
    // to cal.ccmRgbToSrgb).
    inline bool InterpolatePerCcm(const srw::LFPCalibration& cal, double outCcm[9])
    {
        if (cal.perCcm.empty()) return false;
        const double cct = cal.awbCct > 0 ? cal.awbCct : cal.sensorNormCct;
        // Clamp to endpoints.
        if (cct <= cal.perCcm.front().cct)
        {
            for (int i = 0; i < 9; ++i) outCcm[i] = cal.perCcm.front().ccm[i];
            return true;
        }
        if (cct >= cal.perCcm.back().cct)
        {
            for (int i = 0; i < 9; ++i) outCcm[i] = cal.perCcm.back().ccm[i];
            return true;
        }
        for (size_t i = 0; i + 1 < cal.perCcm.size(); ++i)
        {
            const auto& a = cal.perCcm[i];
            const auto& b = cal.perCcm[i + 1];
            if (cct >= a.cct && cct <= b.cct)
            {
                const double t = (cct - a.cct) / (std::max)(1e-6, b.cct - a.cct);
                for (int k = 0; k < 9; ++k)
                    outCcm[k] = a.ccm[k] * (1.0 - t) + b.ccm[k] * t;
                return true;
            }
        }
        return false;
    }
}

std::vector<uint8_t> srw::LFPDemosaicPlenopticAware(
    const std::vector<uint16_t>& bayer,
    const LFPCalibration& inCal)
{
    const LFPCalibration& cal = inCal;
    const int W = cal.sensorWidth;
    const int H = cal.sensorHeight;
    if ((int)bayer.size() < W * H) return {};

    // Build per-pixel (col, row) lens-membership lookup. Threaded by row
    // for speed (linear-time per pixel but with sin/cos/divides inside).
    std::vector<uint32_t> lensId((size_t)W * H, 0);
    auto buildLensIds = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; ++y)
        {
            uint32_t* rowOut = &lensId[(size_t)y * W];
            for (int x = 0; x < W; ++x)
            {
                int col = 0, row = 0;
                SensorToNearestLens(x, y, cal, col, row);
                const uint16_t cu = (uint16_t)(int16_t)(std::max)(-32768, (std::min)(32767, col));
                const uint16_t ru = (uint16_t)(int16_t)(std::max)(-32768, (std::min)(32767, row));
                rowOut[x] = ((uint32_t)cu << 16) | ru;
            }
        }
    };

    // Run NW threads on row ranges.
    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    const int nThreads = (int)(std::min)(hw, 16u);
    {
        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        const int rowsPer = (H + nThreads - 1) / nThreads;
        for (int t = 0; t < nThreads; ++t)
        {
            const int yBegin = t * rowsPer;
            const int yEnd   = (std::min)(H, yBegin + rowsPer);
            if (yBegin >= yEnd) break;
            threads.emplace_back(buildLensIds, yBegin, yEnd);
        }
        for (auto& th : threads) th.join();
    }

    std::vector<uint8_t> rgb((size_t)W * H * 3, 0);
    const double gamma = cal.gamma > 0.0 ? cal.gamma : 0.4167;

    // Build the effective CCM. Illum: interpolate perCcm[] at the scene
    // CCT (algorithms.awb.computed.cct). F01 / no-perCcm: use the
    // metadata CCM as-is.
    double effCcm[9];
    const bool gotInterp = InterpolatePerCcm(cal, effCcm);
    if (!gotInterp)
    {
        for (int i = 0; i < 9; ++i) effCcm[i] = cal.ccmRgbToSrgb[i];
    }
    else
    {
        Log("LFPDemosaic: CCM interpolated @ %.0fK from %zu entries: "
            "[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]",
            cal.awbCct, cal.perCcm.size(),
            effCcm[0], effCcm[1], effCcm[2],
            effCcm[3], effCcm[4], effCcm[5],
            effCcm[6], effCcm[7], effCcm[8]);
    }

    const bool applyWb = true;
    const double postWbR = 1.0;
    const double postWbG = 1.0;
    const double postWbB = 1.0;

    // Illum-only: ProPhoto-RGB intermediate + Lytro Blitzen1_1 contrast
    // spline + Bradford-adapted ProPhoto-D50 -> sRGB-D65. The Lytro
    // CCMs are sensor-RGB -> ProPhoto-D50; treating them as sRGB
    // directly was the source of all our magenta woes. With this
    // matrix chain the G channel stays positive and saturation is
    // restored correctly.
    //
    // Bradford ProPhoto-D50 -> sRGB-D65 (Lindbloom canonical
    // composition, verified via the disassembly research):
    //   M_pp_to_srgb = M_xyz_to_srgb_d65 @ M_bradford_d50_to_d65 @ M_pp_to_xyz_d50
    static constexpr double kProPhotoToSrgb[9] = {
         2.03407576, -0.72733405, -0.30674175,
        -0.22881313,  1.23173007, -0.00291686,
        -0.00856977, -0.15328662,  1.16185641
    };
    // ProPhoto luma weights (only R and G contribute meaningfully).
    static constexpr double kPpLumaR = 0.2880402;
    static constexpr double kPpLumaG = 0.7118741;
    static constexpr double kPpLumaB = 0.0000857;

    // Lytro Blitzen1_1 contrast spline LUT (1024 entries). The 13
    // (x, y) control points come from disassembling Illum's renderer
    // (liblytro-glfe-1.0.so, function InitializeContrastSplineControl
    // Points_Blitzen1_1 at .so offset 0x263370). Slight-S curve:
    // shadows go down (y<x), highlights go up (y>x).
    const bool isIllum = (cal.cameraModel == "ILLUM");
    double splineLut[1024];
    if (isIllum)
    {
        static constexpr double kSpline[13][2] = {
            {0.000, 0.000}, {0.010, 0.004}, {0.030, 0.020}, {0.050, 0.045},
            {0.076, 0.088}, {0.108, 0.143}, {0.150, 0.206}, {0.350, 0.490},
            {0.540, 0.705}, {0.625, 0.786}, {0.865, 0.955}, {0.924, 0.981},
            {1.000, 1.000}
        };
        for (int i = 0; i < 1024; ++i)
        {
            const double y = (double)i / 1023.0;
            int k = 0;
            while (k < 12 && kSpline[k + 1][0] < y) ++k;
            if (k >= 12) { splineLut[i] = kSpline[12][1]; continue; }
            const double x0 = kSpline[k][0],     y0 = kSpline[k][1];
            const double x1 = kSpline[k + 1][0], y1 = kSpline[k + 1][1];
            const double t = (y - x0) / (x1 - x0);
            splineLut[i] = y0 + t * (y1 - y0);
        }
    }

    auto demosaicRange = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                const uint32_t myId = lensId[(size_t)y * W + x];
                const int myPhase = LFPBayerPhase(x, y, cal);
                const double myVal = NormalizeRaw(bayer[(size_t)y * W + x], myPhase, cal, applyWb);

                double rSum = 0, gSum = 0, bSum = 0;
                int    rN = 0, gN = 0, bN = 0;
                if      (myPhase == 0)  { rSum += myVal; ++rN; }
                else if (myPhase == 3)  { bSum += myVal; ++bN; }
                else                    { gSum += myVal; ++gN; }

                // 7x7 same-lens neighbour blend. Illum's microlens regions
                // are ~14 px diameter so 7x7 fits inside the lens; F01's
                // are ~10 px so 7x7 spills past the edge in places but
                // the same-lens mask filters spilled samples out (the
                // useful sample count drops to whatever stays inside the
                // lens). 49 samples vs the 25 of the previous 5x5 gives
                // ~2x more per Bayer channel and visibly less grain on
                // Illum without measurable CPU cost on modern desktops.
                for (int dy = -3; dy <= 3; ++dy)
                {
                    const int ny = y + dy;
                    if ((unsigned)ny >= (unsigned)H) continue;
                    for (int dx = -3; dx <= 3; ++dx)
                    {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        if ((unsigned)nx >= (unsigned)W) continue;
                        if (lensId[(size_t)ny * W + nx] != myId) continue;
                        const int p = LFPBayerPhase(nx, ny, cal);
                        const double v = NormalizeRaw(bayer[(size_t)ny * W + nx], p, cal, applyWb);
                        if      (p == 0) { rSum += v; ++rN; }
                        else if (p == 3) { bSum += v; ++bN; }
                        else             { gSum += v; ++gN; }
                    }
                }

                const double r0 = rN > 0 ? rSum / rN : 0.0;
                const double g0 = gN > 0 ? gSum / gN : 0.0;
                const double b0 = bN > 0 ? bSum / bN : 0.0;

                // CCM (sensor RGB -> ProPhoto for Illum, sensor RGB ->
                // sRGB-linear for F01). Either way the matrix form is
                // the same; only the downstream interpretation differs.
                double r = effCcm[0] * r0 + effCcm[1] * g0 + effCcm[2] * b0;
                double g = effCcm[3] * r0 + effCcm[4] * g0 + effCcm[5] * b0;
                double b = effCcm[6] * r0 + effCcm[7] * g0 + effCcm[8] * b0;
                r *= postWbR; g *= postWbG; b *= postWbB;

                if (isIllum)
                {
                    // r/g/b are now ProPhoto-D50 linear. Apply contrast
                    // spline on luma -- scale RGB by spline(Y)/Y so
                    // hue is preserved.
                    const double Y = kPpLumaR * r + kPpLumaG * g + kPpLumaB * b;
                    if (Y > 1e-4)
                    {
                        const double Yc = (std::min)(1.0, (std::max)(0.0, Y));
                        const int li = (int)(Yc * 1023.0);
                        const double scale = splineLut[li] / Y;
                        r *= scale; g *= scale; b *= scale;
                    }
                    // ProPhoto-D50 -> sRGB-D65 (Bradford-adapted).
                    const double sr = kProPhotoToSrgb[0]*r + kProPhotoToSrgb[1]*g + kProPhotoToSrgb[2]*b;
                    const double sg = kProPhotoToSrgb[3]*r + kProPhotoToSrgb[4]*g + kProPhotoToSrgb[5]*b;
                    const double sb = kProPhotoToSrgb[6]*r + kProPhotoToSrgb[7]*g + kProPhotoToSrgb[8]*b;
                    r = (std::max)(0.0, (std::min)(1.0, sr));
                    g = (std::max)(0.0, (std::min)(1.0, sg));
                    b = (std::max)(0.0, (std::min)(1.0, sb));
                }
                else
                {
                    r = (std::max)(0.0, (std::min)(1.0, r));
                    g = (std::max)(0.0, (std::min)(1.0, g));
                    b = (std::max)(0.0, (std::min)(1.0, b));
                }
                r = pow(r, gamma);
                g = pow(g, gamma);
                b = pow(b, gamma);

                const size_t off = ((size_t)y * W + x) * 3;
                rgb[off + 0] = (uint8_t)(std::min)(255, (int)(r * 255.0 + 0.5));
                rgb[off + 1] = (uint8_t)(std::min)(255, (int)(g * 255.0 + 0.5));
                rgb[off + 2] = (uint8_t)(std::min)(255, (int)(b * 255.0 + 0.5));
            }
        }
    };

    {
        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        const int rowsPer = (H + nThreads - 1) / nThreads;
        for (int t = 0; t < nThreads; ++t)
        {
            const int yBegin = t * rowsPer;
            const int yEnd   = (std::min)(H, yBegin + rowsPer);
            if (yBegin >= yEnd) break;
            threads.emplace_back(demosaicRange, yBegin, yEnd);
        }
        for (auto& th : threads) th.join();
    }

    // No post-pass needed: the per-pixel ProPhoto + spline + matrix
    // chain in the demosaic loop handles colour correctly. Kept the
    // empty branch as a hook in case future cameras need one.
    if (false && cal.cameraModel == "ILLUM")
    {
        uint64_t sumR = 0, sumG = 0, sumB = 0, nSamp = 0;
        for (int y = 0; y < H; y += 4)
        {
            for (int x = 0; x < W; x += 4)
            {
                const size_t o = ((size_t)y * W + x) * 3;
                sumR += rgb[o + 0];
                sumG += rgb[o + 1];
                sumB += rgb[o + 2];
                ++nSamp;
            }
        }
        const double mR = nSamp > 0 ? (double)sumR / nSamp : 1.0;
        const double mG = nSamp > 0 ? (double)sumG / nSamp : 1.0;
        const double mB = nSamp > 0 ? (double)sumB / nSamp : 1.0;
        const double target = (mR + mG + mB) / 3.0;
        const double sR = (std::max)(0.5, (std::min)(2.0, target / (std::max)(1.0, mR)));
        const double sG = (std::max)(0.5, (std::min)(2.0, target / (std::max)(1.0, mG)));
        const double sB = (std::max)(0.5, (std::min)(2.0, target / (std::max)(1.0, mB)));
        Log("LFPDemosaic: Illum Gray-World -- means R%.1f G%.1f B%.1f -> "
            "scales R%.3f G%.3f B%.3f", mR, mG, mB, sR, sG, sB);

        constexpr double kSatBoost = 2.0;

        auto applyRange = [&](int yBegin, int yEnd) {
            for (int y = yBegin; y < yEnd; ++y)
            {
                for (int x = 0; x < W; ++x)
                {
                    const size_t o = ((size_t)y * W + x) * 3;
                    double r = rgb[o + 0] * sR;
                    double g = rgb[o + 1] * sG;
                    double b = rgb[o + 2] * sB;
                    r = (std::min)(255.0, r);
                    g = (std::min)(255.0, g);
                    b = (std::min)(255.0, b);
                    const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    r = Y + (r - Y) * kSatBoost;
                    g = Y + (g - Y) * kSatBoost;
                    b = Y + (b - Y) * kSatBoost;
                    rgb[o + 0] = (uint8_t)(std::min)(255.0, (std::max)(0.0, r) + 0.5);
                    rgb[o + 1] = (uint8_t)(std::min)(255.0, (std::max)(0.0, g) + 0.5);
                    rgb[o + 2] = (uint8_t)(std::min)(255.0, (std::max)(0.0, b) + 0.5);
                }
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        const int rowsPer = (H + nThreads - 1) / nThreads;
        for (int t = 0; t < nThreads; ++t)
        {
            const int yBegin = t * rowsPer;
            const int yEnd   = (std::min)(H, yBegin + rowsPer);
            if (yBegin >= yEnd) break;
            threads.emplace_back(applyRange, yBegin, yEnd);
        }
        for (auto& th : threads) th.join();
    }

    return rgb;
}

std::vector<uint8_t> srw::LFPExtractSubApertureViewFromRgb(
    const std::vector<uint8_t>& sensorRgb,
    const LFPCalibration& cal,
    float apertureU, float apertureV,
    int& outW, int& outH)
{
    LFPMicrolensGridDims(cal, outW, outH);
    if (outW <= 0 || outH <= 0 || (int)sensorRgb.size() < cal.sensorWidth * cal.sensorHeight * 3)
    {
        outW = outH = 0;
        return {};
    }

    const double pitchPx  = cal.mlaLensPitchM / cal.pixelPitchM;
    const double radiusPx = 0.42 * pitchPx;   // safely inside the lens

    // Same grid origin as LFPExtractSubApertureView so dims line up.
    int gridMinCol = 0, gridMinRow = 0;
    auto fits = [&](int col, int row) {
        double cx, cy;
        MicrolensCentre(col, row, cal, cx, cy);
        const double r = 0.5 * pitchPx;
        return cx > r && cx < cal.sensorWidth - r
            && cy > r && cy < cal.sensorHeight - r;
    };
    while (fits(gridMinCol - 1, 0)) --gridMinCol;
    while (fits(0, gridMinRow - 1)) --gridMinRow;

    const int W = cal.sensorWidth;
    const int H = cal.sensorHeight;
    auto sampleRgb = [&](double sx, double sy, double& outR, double& outG, double& outB) {
        // Bilinear sample of demosaiced RGB.
        const int x0 = (int)floor(sx); const int y0 = (int)floor(sy);
        const double fx = sx - x0;     const double fy = sy - y0;
        auto safe = [&](int x, int y, int c) -> double {
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return 0.0;
            return (double)sensorRgb[((size_t)y * W + x) * 3 + c];
        };
        for (int c = 0; c < 3; ++c)
        {
            const double v00 = safe(x0,   y0,   c);
            const double v10 = safe(x0+1, y0,   c);
            const double v01 = safe(x0,   y0+1, c);
            const double v11 = safe(x0+1, y0+1, c);
            const double v   = v00 * (1-fx)*(1-fy) + v10 * fx*(1-fy)
                              + v01 * (1-fx)*fy    + v11 * fx*fy;
            if      (c == 0) outR = v;
            else if (c == 1) outG = v;
            else             outB = v;
        }
    };

    std::vector<uint8_t> out((size_t)outW * outH * 3, 0);

    // Aperture-stop integration: average a small ring of samples around
    // the aperture sample point. Models a slightly larger aperture stop;
    // perceptually smooths per-microlens noise that would otherwise show
    // up as grid artefacts. 5x5 = 25 samples per output pixel -- modest
    // CPU cost since this runs once at load.
    constexpr int kIntegRadius = 2;   // -2..+2 = 25 samples per output pixel
    constexpr int kIntegSamples = (kIntegRadius * 2 + 1) * (kIntegRadius * 2 + 1);

    for (int oy = 0; oy < outH; ++oy)
    {
        for (int ox = 0; ox < outW; ++ox)
        {
            const int col = ox + gridMinCol;
            const int row = oy + gridMinRow;
            double cx, cy;
            MicrolensCentre(col, row, cal, cx, cy);
            const double sampleX = cx + apertureU * radiusPx;
            const double sampleY = cy + apertureV * radiusPx;
            double rSum = 0, gSum = 0, bSum = 0;
            for (int dy = -kIntegRadius; dy <= kIntegRadius; ++dy)
            {
                for (int dx = -kIntegRadius; dx <= kIntegRadius; ++dx)
                {
                    double r, g, b;
                    sampleRgb(sampleX + dx, sampleY + dy, r, g, b);
                    rSum += r; gSum += g; bSum += b;
                }
            }
            const double r = rSum / kIntegSamples;
            const double g = gSum / kIntegSamples;
            const double b = bSum / kIntegSamples;
            const size_t off = ((size_t)oy * outW + ox) * 3;
            out[off + 0] = (uint8_t)(std::min)(255, (int)(r + 0.5));
            out[off + 1] = (uint8_t)(std::min)(255, (int)(g + 0.5));
            out[off + 2] = (uint8_t)(std::min)(255, (int)(b + 0.5));
        }
    }
    return out;
}

// stb_image symbols live in SRWeaver.cpp -- forward-declare just what
// we need to decode the embedded preview JPG + ESLF PNG.
extern "C" unsigned char* stbi_load(char const* filename, int* x, int* y,
                                     int* channels_in_file, int desired_channels);
extern "C" unsigned char* stbi_load_from_memory(unsigned char const* buffer,
                                                 int len, int* x, int* y,
                                                 int* channels_in_file,
                                                 int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);
extern "C" const char* stbi_failure_reason();

namespace {
    // Per-channel histogram matching. Builds CDFs of `ours` and `ref`,
    // then for each value v in ours finds the smallest v' in ref where
    // ref_cdf[v'] >= ours_cdf[v]. Applies the resulting LUT to `ours`
    // in place. Standard image-processing technique for "make image A
    // look like image B colour-wise" -- works whether or not A and B
    // are spatially aligned, since it only looks at value distributions.
    //
    // We use it to colour-grade our plenoptic demosaic output against
    // the embedded Lytro JPG preview: same scene, same camera, but the
    // JPG was rendered by Lytro's full pipeline (per-camera WhiteImage,
    // perCcm interpolation, contrast spline) which we can't reproduce
    // without per-body calibration. Matching histograms transfers those
    // colour decisions to our render without needing the underlying
    // pipeline.
    //
    // Note: per-channel matching can shift hue slightly (each channel
    // is remapped independently). Joint 3-channel matching avoids that
    // but is far more complex. For our use case the simpler approach
    // is good enough and very fast.
    void HistogramMatchPerChannel(std::vector<uint8_t>& ours, size_t oursPixels,
                                   const std::vector<uint8_t>& ref, size_t refPixels)
    {
        for (int c = 0; c < 3; ++c)
        {
            uint64_t histOurs[256] = {0};
            uint64_t histRef[256]  = {0};
            for (size_t i = 0; i < oursPixels; ++i) ++histOurs[ours[i * 3 + c]];
            for (size_t i = 0; i < refPixels;  ++i) ++histRef[ref [i * 3 + c]];

            double cdfOurs[256], cdfRef[256];
            uint64_t accO = 0, accR = 0;
            const double invO = oursPixels > 0 ? 1.0 / oursPixels : 0.0;
            const double invR = refPixels  > 0 ? 1.0 / refPixels  : 0.0;
            for (int v = 0; v < 256; ++v) { accO += histOurs[v]; cdfOurs[v] = accO * invO; }
            for (int v = 0; v < 256; ++v) { accR += histRef [v]; cdfRef [v] = accR * invR; }

            uint8_t lut[256];
            int j = 0;
            for (int v = 0; v < 256; ++v)
            {
                while (j < 255 && cdfRef[j] < cdfOurs[v]) ++j;
                lut[v] = (uint8_t)j;
            }
            for (size_t i = 0; i < oursPixels; ++i)
                ours[i * 3 + c] = lut[ours[i * 3 + c]];
        }
    }
}

bool srw::LFPLoadDemosaicedSensor(const std::string& path,
                                    std::vector<uint8_t>& outSensorRgb,
                                    LFPCalibration& outCal)
{
    LFPReader r;
    if (!r.LoadFromFile(path)) { Log("LFPLoadDemosaicedSensor: parse %s failed", path.c_str()); return false; }
    const std::string metaRef = LFPJsonFindString(r.TopLevelJson(), "metadataRef");
    const LFPChunk* mc = r.FindBySha1(metaRef);
    const LFPChunk* ic = LFPFindRawImageChunk(r);
    if (!mc || !ic) { Log("LFPLoadDemosaicedSensor: refs missing in %s", path.c_str()); return false; }
    if (!LFPParseCalibration(mc->AsString(), outCal)) return false;
    std::vector<uint16_t> bayer = LFPUnpackBayer(ic->data, outCal);
    if (bayer.empty()) return false;
    outSensorRgb = LFPDemosaicPlenopticAware(bayer, outCal);
    if (outSensorRgb.empty()) return false;

    // Colour-correction step: if an embedded JPG preview exists,
    // histogram-match our demosaic output against it. This transfers
    // Lytro's pipeline colour science (WB + perCcm + contrast spline)
    // to our output without needing the per-camera calibration that
    // would let us reproduce that pipeline. For LFR files this is
    // basically free (the JPG is always present). For F01 LFP files
    // it's a small refinement on already-good colour. If no preview
    // is found (rare), we keep our existing Gray-World output.
    std::vector<uint8_t> jpgBytes;
    if (LFPExtractEmbeddedPreview(path, jpgBytes))
    {
        int jw = 0, jh = 0, jc = 0;
        unsigned char* jpgRgb = stbi_load_from_memory(
            jpgBytes.data(), (int)jpgBytes.size(), &jw, &jh, &jc, 3);
        if (jpgRgb && jw > 0 && jh > 0)
        {
            std::vector<uint8_t> ref(jpgRgb, jpgRgb + (size_t)jw * jh * 3);
            stbi_image_free(jpgRgb);
            const size_t oursPx = (size_t)outCal.sensorWidth * outCal.sensorHeight;
            const size_t refPx  = (size_t)jw * jh;
            HistogramMatchPerChannel(outSensorRgb, oursPx, ref, refPx);
            Log("LFPLoadDemosaicedSensor: colour histogram-matched against "
                "embedded %dx%d preview", jw, jh);
        }
        else
        {
            if (jpgRgb) stbi_image_free(jpgRgb);
            Log("LFPLoadDemosaicedSensor: embedded preview decode failed, "
                "using Gray-World colour");
        }
    }
    return true;
}

bool srw::IsLytroEslfPng(const std::string& path)
{
    auto endsWith = [](const std::string& s, const std::string& suffix) {
        if (s.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); ++i)
        {
            const char a = s[s.size() - suffix.size() + i];
            const char b = suffix[i];
            const char la = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
            const char lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
            if (la != lb) return false;
        }
        return true;
    };
    if (endsWith(path, "_eslf.png")) return true;
    // qs14x14 pattern: contains "_qs14x14" before ".png"
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string stem = path.substr(0, dot);
    return stem.find("_qs14x14") != std::string::npos
        && endsWith(path, ".png");
}

bool srw::LFPLoadEslfAsSensorRgb(const std::string& path,
                                   std::vector<uint8_t>& outSensorRgb,
                                   LFPCalibration& outCal)
{
    // Load as RGBA to preserve the microlens-disc alpha mask. Lytro's
    // ESLF PNGs make the gaps between round microlenses transparent
    // (alpha=0); the LFPRenderer shader uses that alpha as a per-
    // sample weight during aperture-stop integration so the dark/
    // transparent gap regions don't bleed into the output.
    int w = 0, h = 0, c = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!pixels)
    {
        Log("LFPLoadEslfAsSensorRgb: stbi_load failed on %s: %s",
            path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "?");
        return false;
    }
    outSensorRgb.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);

    // Synthesise an Illum-typical calibration. The MLA values come
    // from inspecting the calibration of multiple Illum LFRs in this
    // repo's test set -- they're consistent across bodies within
    // small (sub-micron) tolerances. mlaSensorOffset is set to 0
    // since the ESLF PNG is already cropped to the active microlens
    // region (which is centered on the MLA origin).
    outCal = LFPCalibration{};
    outCal.sensorWidth   = w;
    outCal.sensorHeight  = h;
    outCal.bitsPerPixel  = 16;
    outCal.bigEndian     = false;
    outCal.pixelPitchM   = 1.4e-6;
    outCal.bayerTile     = "r,gr:gb,b";
    outCal.upperLeftPixel= "gr";
    outCal.cameraMake    = "Lytro, Inc.";
    outCal.cameraModel   = "ILLUM-ESLF";   // distinct so the demosaic
                                            // post-pass skips the
                                            // Gray-World branch (colour
                                            // is already correct).
    outCal.focalLengthM       = 0.030;    // typical mid-zoom
    outCal.fNumber            = 2.0;      // typical Illum
    outCal.exitPupilOffsetZ   = 0.317;    // matches the test LFRs
    outCal.mlaTiling          = "hexUniformRowMajor";
    outCal.mlaLensPitchM      = 20.0e-6;  // Illum MLA
    outCal.mlaRotationRad     = 0.0;
    outCal.mlaScaleFactorX    = 1.0;
    outCal.mlaScaleFactorY    = 1.000255; // matches test LFRs
    outCal.mlaSensorOffsetXM  = 0.0;
    outCal.mlaSensorOffsetYM  = 0.0;
    outCal.mlaSensorOffsetZM  = 40.0e-6;
    // Identity CCM + WB -- the PNG is already colour-correct.
    for (int i = 0; i < 9; ++i) outCal.ccmRgbToSrgb[i] = 0.0;
    outCal.ccmRgbToSrgb[0] = outCal.ccmRgbToSrgb[4] = outCal.ccmRgbToSrgb[8] = 1.0;
    outCal.wbR = outCal.wbGR = outCal.wbGB = outCal.wbB = 1.0;
    outCal.gamma = 1.0;   // PNG values are already sRGB-encoded

    Log("LFPLoadEslfAsSensorRgb: loaded %dx%d ESLF PNG from %s",
        w, h, path.c_str());
    return true;
}

// Probe just the camera model from an LFR/LFP -- parses the metadata
// JSON chunk only, no raw decode. Fast (<10 ms) so the load-path can
// decide between embedded-preview and plenoptic render without doing
// the expensive demosaic first.
std::string srw::LFPProbeCameraModel(const std::string& path)
{
    LFPReader r;
    if (!r.LoadFromFile(path)) return "";
    const std::string metaRef = LFPJsonFindString(r.TopLevelJson(), "metadataRef");
    const LFPChunk* mc = r.FindBySha1(metaRef);
    if (!mc) return "";
    return LFPJsonFindString(mc->AsString(), "model");
}

// Extract the first embedded JPG (FF D8 FF ... FF D9) from an LFR/LFP.
// Lytro stores a colour-accurate preview as the first imageRef chunk;
// the LFP container's chunk envelope doesn't interfere because JPG's
// own framing is self-terminating. We scan the file rather than walking
// the chunk table since the JPG is always present and its sha1 isn't
// referenced by a single canonical key.
bool srw::LFPExtractEmbeddedPreview(const std::string& path,
                                     std::vector<uint8_t>& outJpgBytes)
{
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) { Log("LFPExtractEmbeddedPreview: open %s failed", path.c_str()); return false; }
    std::fseek(f, 0, SEEK_END);
    const size_t fileLen = (size_t)std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(fileLen);
    if (std::fread(buf.data(), 1, fileLen, f) != fileLen)
    {
        std::fclose(f);
        Log("LFPExtractEmbeddedPreview: short read on %s", path.c_str());
        return false;
    }
    std::fclose(f);

    // Find FF D8 FF then end at FF D9. Skip the first 100 bytes (LFP
    // container header) to avoid any false starts in chunk magic.
    size_t start = std::string::npos;
    for (size_t i = 100; i + 3 < buf.size(); ++i)
    {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8 && buf[i + 2] == 0xFF)
        {
            start = i;
            break;
        }
    }
    if (start == std::string::npos)
    {
        Log("LFPExtractEmbeddedPreview: no JPG SOI found in %s", path.c_str());
        return false;
    }
    size_t end = std::string::npos;
    for (size_t j = start + 3; j + 1 < buf.size(); ++j)
    {
        if (buf[j] == 0xFF && buf[j + 1] == 0xD9)
        {
            end = j + 2;
            break;
        }
    }
    if (end == std::string::npos)
    {
        Log("LFPExtractEmbeddedPreview: no JPG EOI after SOI at %zu in %s",
            start, path.c_str());
        return false;
    }
    outJpgBytes.assign(buf.begin() + start, buf.begin() + end);
    Log("LFPExtractEmbeddedPreview: %zu bytes extracted from offset %zu in %s",
        outJpgBytes.size(), start, path.c_str());
    return true;
}

bool srw::LFPLoadAsStereoSBS(const std::string& path,
                              std::vector<uint8_t>& outRgba,
                              int& outW, int& outH,
                              float apertureL, float apertureR)
{
    LFPReader r;
    if (!r.LoadFromFile(path))
    {
        Log("LFPLoadAsStereoSBS: container parse failed for %s", path.c_str());
        return false;
    }

    const std::string top = r.TopLevelJson();
    const std::string metaRef = LFPJsonFindString(top, "metadataRef");
    if (metaRef.empty())
    {
        Log("LFPLoadAsStereoSBS: no metadataRef in %s", path.c_str());
        return false;
    }
    const LFPChunk* mc = r.FindBySha1(metaRef);
    const LFPChunk* ic = LFPFindRawImageChunk(r);
    if (!mc || !ic)
    {
        Log("LFPLoadAsStereoSBS: metadata or raw image chunk missing in %s", path.c_str());
        return false;
    }

    LFPCalibration cal;
    if (!LFPParseCalibration(mc->AsString(), cal))
    {
        Log("LFPLoadAsStereoSBS: calibration parse failed for %s", path.c_str());
        return false;
    }

    std::vector<uint16_t> bayer = LFPUnpackBayer(ic->data, cal);
    if (bayer.empty())
    {
        Log("LFPLoadAsStereoSBS: bayer unpack failed for %s", path.c_str());
        return false;
    }

    // Plenoptic-aware full-sensor demosaic. Pre-rendering CCM + gamma + WB
    // means sub-aperture extraction is just a bilinear texture read.
    std::vector<uint8_t> sensorRgb = LFPDemosaicPlenopticAware(bayer, cal);
    if (sensorRgb.empty())
    {
        Log("LFPLoadAsStereoSBS: demosaic failed for %s", path.c_str());
        return false;
    }
    // Raw bayer + lens-id lookup can be freed now -- ~80MB on Illum.
    std::vector<uint16_t>().swap(bayer);

    // Extract two sub-aperture views and tile into a single SBS RGBA buffer.
    int lW = 0, lH = 0, rW = 0, rH = 0;
    std::vector<uint8_t> left  = LFPExtractSubApertureViewFromRgb(sensorRgb, cal, apertureL, 0.0f, lW, lH);
    std::vector<uint8_t> right = LFPExtractSubApertureViewFromRgb(sensorRgb, cal, apertureR, 0.0f, rW, rH);
    if (left.empty() || right.empty() || lW != rW || lH != rH)
    {
        Log("LFPLoadAsStereoSBS: view extract failed (L %dx%d  R %dx%d)", lW, lH, rW, rH);
        return false;
    }

    outW = lW * 2;
    outH = lH;
    outRgba.assign((size_t)outW * outH * 4, 255);   // alpha = 255

    // Stitch: row-by-row, copy left view to [0, lW) and right view to [lW, 2*lW).
    for (int y = 0; y < lH; ++y)
    {
        for (int x = 0; x < lW; ++x)
        {
            const size_t srcOff = ((size_t)y * lW + x) * 3;
            const size_t dstL   = ((size_t)y * outW + x) * 4;
            const size_t dstR   = ((size_t)y * outW + (x + lW)) * 4;
            outRgba[dstL + 0] = left [srcOff + 0];
            outRgba[dstL + 1] = left [srcOff + 1];
            outRgba[dstL + 2] = left [srcOff + 2];
            outRgba[dstL + 3] = 255;
            outRgba[dstR + 0] = right[srcOff + 0];
            outRgba[dstR + 1] = right[srcOff + 1];
            outRgba[dstR + 2] = right[srcOff + 2];
            outRgba[dstR + 3] = 255;
        }
    }
    Log("LFPLoadAsStereoSBS: %s -> %dx%d SBS (camera %s, aperture L=%.2f R=%.2f)",
        path.c_str(), outW, outH, cal.cameraModel.c_str(), apertureL, apertureR);
    return true;
}
