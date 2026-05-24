// Detector.h — heuristic auto-detection of a source's stereo layout. Downscales
// the source, reads it back, and scores SBS / TAB / anaglyph.
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class Detector
    {
    public:
        Detector() = default;
        ~Detector();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Analyze the source frame; on success sets outFormat to the best guess
        // and returns true. Returns false if no stereo layout is confident.
        bool Detect(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight,
                    StereoFormat& outFormat);

    private:
        static const int kSize = 128;   // analysis resolution

        ID3D11Device*           m_device  = nullptr;
        ID3D11DeviceContext*    m_context = nullptr;
        ID3D11VertexShader*     m_vs      = nullptr;
        ID3D11PixelShader*      m_ps      = nullptr;
        ID3D11SamplerState*     m_sampler = nullptr;
        ID3D11Texture2D*        m_rt      = nullptr;  // small downscale target
        ID3D11RenderTargetView* m_rtv     = nullptr;
        ID3D11Texture2D*        m_staging = nullptr;  // CPU-readable copy
    };
}
