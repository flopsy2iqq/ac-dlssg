#pragma once
// Capture slots: a ring of 3 slots, each with one depth and one motion-vector
// texture shared from D3D11 to D3D12 (spec 6.4 "Capture slots", 6.5 "Copying
// into slot N mod 3").
//
// Every texture is created on D3D11 with MISC_SHARED | MISC_SHARED_NTHANDLE,
// shared with IDXGIResource1::CreateSharedHandle and opened with
// ID3D12Device::OpenSharedHandle (the other direction failed with
// E_INVALIDARG on the reference machine, spec 6.4). Each texture is sized
// from its own source texture's D3D11_TEXTURE2D_DESC, never from the back
// buffer or the NGX size scalars.
//  - depth: R32_FLOAT, BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE, written by
//    a cs_5_0 blit (compiled at build time) from a typed SRV of the source;
//  - motion vectors: the source's typed format (a TYPELESS source gets the
//    typed format of its family, see MvecFormat), BIND_SHADER_RESOURCE,
//    written by a copy of subresource 0.
//
// Threading: everything runs on the render thread (CSP's immediate context).
// Nothing here waits on or signals a fence; the caller does (spec 7 step 2).
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acdb {

// How a depth source is read (pure; see CaptureSlots::Copy). srv is the
// typed view format of the depth channel and family the typeless format of
// the source's family; both UNKNOWN for a format that is no depth format.
struct DepthFormatInfo {
    DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN;
};
DepthFormatInfo DepthSourceFormat(DXGI_FORMAT source);

// The typed format of the motion-vector slot texture (pure): a typed source
// format is kept; R16G16, R32G32, R16G16B16A16 and R32G32B32A32 TYPELESS map
// to their FLOAT format; UNKNOWN (refused) for other typeless formats and for
// depth formats.
DXGI_FORMAT TypedMvecFormat(DXGI_FORMAT source);

class CaptureSlots {
public:
    static constexpr uint32_t kSlots = 3;

    // Creates the depth-blit compute shader on device11. The slots start empty;
    // Recreate fills one. Both devices must be on the same adapter.
    static std::unique_ptr<CaptureSlots> Create(ID3D11Device* device11, ID3D12Device* device12, std::string* error);
    ~CaptureSlots();

    // True when the slot has textures and both sources' descs equal the descs
    // recorded at the slot's last Recreate: format, Width, Height and
    // SampleDesc. False for an empty slot, a bad index or a null source.
    bool Matches(uint32_t slot, ID3D11Resource* depth, ID3D11Resource* mvec) const;

    // Releases the slot's textures and creates them for these sources. The
    // caller guarantees the GPU no longer uses the slot (spec 6.4: wait until
    // progress has passed the last frame that tagged it). Also drops the
    // cached source views, so old source textures are not kept alive.
    // Refused (false, the slot left empty): a bad index, a source that is not
    // a single-array-slice Texture2D, an MSAA source, a depth format without a
    // mapping (see Copy), a TYPELESS motion-vector format without a typed
    // mapping, a source from another device.
    bool Recreate(uint32_t slot, ID3D11Resource* depth, ID3D11Resource* mvec, std::string* error);

    // Copies the sources into the slot on ctx, which must be the immediate
    // context of device11 (a deferred context is refused). The slot must
    // Match the sources. No fence is signalled here.
    //  - motion vectors: CopySubresourceRegion of subresource 0, which is
    //    CopyResource for CSP's single-mip textures and also takes a source
    //    with more mips;
    //  - depth: cs_5_0 blit, Dispatch((w+7)/8, (h+7)/8), from a typed SRV of
    //    the source to the slot's R32_FLOAT UAV. SRV format by source format:
    //      R32_TYPELESS, R32_FLOAT, D32_FLOAT        -> R32_FLOAT
    //      R24G8_TYPELESS, D24_UNORM_S8_UINT         -> R24_UNORM_X8_TYPELESS
    //      R16_TYPELESS, R16_UNORM, D16_UNORM        -> R16_UNORM
    //      R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT   -> R32_FLOAT_X8X24_TYPELESS
    //    A source without BIND_SHADER_RESOURCE (every typed depth format) is
    //    first copied into a cached typeless texture of its family that has one.
    //  - saved and restored around the blit: the compute shader (with its class
    //    instances), CS SRV slot 0 and CS UAV slot 0 (re-bound with an initial
    //    count of -1, which keeps its hidden counter). The blit uses no
    //    constant buffer and no sampler.
    //  - a depth source bound as the output-merger depth-stencil view cannot be
    //    read (D3D11 forces the SRV to NULL). Then the depth-stencil view is
    //    unbound for the blit and re-bound afterwards, with the same render
    //    targets and the output-merger UAVs kept.
    bool Copy(ID3D11DeviceContext* ctx, uint32_t slot, ID3D11Resource* depth, ID3D11Resource* mvec,
              std::string* error);

    // The slot's D3D12 resources (COMMON state; nullptr while the slot is
    // empty). Owned by the slots; valid until the slot's next Recreate.
    ID3D12Resource* Depth12(uint32_t slot) const;
    ID3D12Resource* Mvec12(uint32_t slot) const;
    // The motion-vector texture's typed format (UNKNOWN while empty).
    DXGI_FORMAT MvecFormat(uint32_t slot) const;

    // Releases the source-view cache, and with it the references it holds
    // to CSP's depth textures (review finding F4). The slots stay; the next
    // Copy builds the view again. Same thread rules as Copy.
    void DropSourceViews();

    // Added for the owner and the tests.
    bool HasTextures(uint32_t slot) const;        // an empty slot can be created without waiting on the GPU
    ID3D11Texture2D* Depth11(uint32_t slot) const;  // the D3D11 side of Depth12
    ID3D11Texture2D* Mvec11(uint32_t slot) const;   // the D3D11 side of Mvec12
    size_t CachedSourceViews() const;              // entries in the source-view cache
    uint32_t DsvUnbinds() const;                   // copies that had to unbind the OM depth-stencil view

private:
    CaptureSlots();
    struct Slot;
    struct SourceView;
    bool Valid(uint32_t slot) const;
    SourceView* ViewFor(ID3D11Texture2D* depth, std::string* error);

    Microsoft::WRL::ComPtr<ID3D11Device> device11_;
    Microsoft::WRL::ComPtr<ID3D12Device> device12_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> blit_;
    std::unique_ptr<Slot[]> slots_;
    std::vector<SourceView> views_;
    uint64_t copies_ = 0;
    uint32_t dsv_unbinds_ = 0;
};

}  // namespace acdb
