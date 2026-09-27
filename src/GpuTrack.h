// GpuTrack.h -- Auto Stereo's GPU-side scroll tracking.
//
// Each frame, for every tracked picture, two tiny compute passes on the
// analyser's half-res luma image (this very frame, still on the GPU):
//   1. the mean brightness of each row around where the CPU tracker last saw
//      the picture (over the picture's columns);
//   2. a search over vertical offsets (+/- kSearch rows) for where those rows
//      best match the picture's brightness fingerprint (sum of absolute
//      differences, best of all offsets).
// The result -- the picture's vertical offset from the CPU tracker's position,
// and whether the match is good -- stays on the GPU in a small buffer that the
// crop, the placement into the woven image and the see-through mask read
// directly. No CPU round trip: picture and cut-out move together, this frame.
// The CPU tracker (on slightly older frames) still owns everything else and
// is the fallback whenever the GPU match isn't confident.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <vector>

namespace srw
{
    struct GpuTrackJob
    {
        int                       slot = 0;         // 0..kMaxSlots-1 (result index)
        RECT                      base{};           // CPU tracker's rect (analysis px)
        RECT                      view{};           // its viewport (analysis px)
        const std::vector<float>* profile = nullptr; // row fingerprint (one value per row of base)
        const std::vector<float>* colProfile = nullptr; // column fingerprint (one value per column)
    };

    class GpuTracker
    {
    public:
        static constexpr int kMaxSlots = 16;
        static constexpr int kSearch   = 96;    // analysis rows each way
        static constexpr int kMaxRows  = 1200;  // longest picture tracked (analysis rows)
        static constexpr int kSearchX  = 48;    // analysis columns each way (sideways)
        static constexpr int kMaxCols  = 2000;  // widest picture tracked

        ~GpuTracker();
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Run all jobs on this frame's luma image. Slots not in `jobs` read
        // back as "no result".
        void Run(ID3D11ShaderResourceView* luma, const std::vector<GpuTrackJob>& jobs);

        // int4 per slot: x = vertical offset (analysis rows) from the job's
        // base, y = 1 if that match is confident, z = sideways offset
        // (analysis columns), w = 1 if that one is confident too.
        ID3D11ShaderResourceView* ResultsSRV() const { return m_resSRV; }

    private:
        ID3D11Device*              m_device  = nullptr;
        ID3D11DeviceContext*       m_context = nullptr;
        ID3D11ComputeShader*       m_csRows  = nullptr;
        ID3D11ComputeShader*       m_csSad   = nullptr;
        ID3D11ComputeShader*       m_csCols  = nullptr;
        ID3D11ComputeShader*       m_csSadX  = nullptr;
        ID3D11Buffer*              m_colProfile = nullptr;   // kMaxSlots * kMaxCols floats
        ID3D11ShaderResourceView*  m_colProfileSRV = nullptr;
        ID3D11Buffer*              m_cols    = nullptr;   // kMaxSlots * (kMaxCols + 2*kSearchX) floats
        ID3D11UnorderedAccessView* m_colsUAV = nullptr;
        ID3D11Buffer*              m_cb      = nullptr;
        ID3D11Buffer*              m_profile = nullptr;   // kMaxSlots * kMaxRows floats
        ID3D11ShaderResourceView*  m_profileSRV = nullptr;
        ID3D11Buffer*              m_rows    = nullptr;   // kMaxSlots * (kMaxRows + 2*kSearch) floats
        ID3D11UnorderedAccessView* m_rowsUAV = nullptr;
        ID3D11Buffer*              m_res     = nullptr;   // kMaxSlots int4
        ID3D11UnorderedAccessView* m_resUAV  = nullptr;
        ID3D11ShaderResourceView*  m_resSRV  = nullptr;
    };
}
