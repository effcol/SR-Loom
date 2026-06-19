// KatangaSource.h -- receives Bo3b's Katanga "VR" output as a passive listener.
// Katanga's IPC is dead simple: a 64-bit shared file mapping at
// Local\KatangaMappedFile holds a single HANDLE value, which is a DXGI shared
// handle for a D3D11 texture containing the rendered SBS frame. We poll the
// mapping for a non-zero handle, open the shared resource as our own SRV, and
// hand it straight to the weaver. No copies, no conversion: same fast path as
// a live capture in FullSBS mode. Protocol observed from VRScreenCap (MIT);
// no Katanga source code was read.
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class KatangaSource
    {
    public:
        KatangaSource() = default;
        ~KatangaSource();

        // Start watching for Katanga's named file mapping. Returns true if the
        // device was stored; the actual reception starts when Katanga arrives.
        bool Begin(ID3D11Device* device);
        void End();

        // Poll the mapping for a fresh handle and (re)open the shared texture.
        // Cheap: actual filesystem checks happen at most every ~500 ms.
        // Returns true when a valid shared texture is currently bound.
        bool Update();

        bool IsActive()    const { return m_device != nullptr; }   // watch started
        bool IsReceiving() const { return m_srv    != nullptr; }   // texture bound this frame
        ID3D11ShaderResourceView* SRV() const { return m_srv; }
        int Width()  const { return m_width;  }
        int Height() const { return m_height; }
        DXGI_FORMAT Format() const { return m_format; }

    private:
        bool TryOpenTexture(HANDLE sharedHandle);
        void ReleaseTexture();

        ID3D11Device*             m_device         = nullptr;
        // We deliberately do NOT keep a persistent handle to the named
        // mapping -- see Update() for why. Liveness comes from re-opening
        // the mapping per poll: when the publishing game exits and we
        // aren't keeping the kernel object alive ourselves, the open fails.
        uintptr_t                 m_lastHandle     = 0;         // last shared handle value seen
        ID3D11Texture2D*          m_tex            = nullptr;
        ID3D11ShaderResourceView* m_srv            = nullptr;
        int                       m_width          = 0;
        int                       m_height         = 0;
        DXGI_FORMAT               m_format         = DXGI_FORMAT_UNKNOWN;
        DWORD                     m_lastPollTick   = 0;
    };
}
