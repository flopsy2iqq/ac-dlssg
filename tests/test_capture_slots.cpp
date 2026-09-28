// CaptureSlots against real devices (hardware adapter, else WARP): slot
// creation, the depth blit and motion-vector copy read back on both D3D11 and
// D3D12, compute-state preservation, refusals, the source-view cache and
// release/recreate cycles. GPU tests skip with a note when no adapter creates
// both devices.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture_slots.h"
#include "depth_blit_cs.h"
#include "fence_pair.h"
#include "gpu_test_devices.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

bool GetDevices(acdb_test::GpuTestDevices* d) {
    if (acdb_test::CreateGpuTestDevices(d)) return true;
    std::printf("  SKIP: no adapter creates both a D3D11 and a D3D12 device\n");
    return false;
}

ULONG RefCount(IUnknown* obj) {
    obj->AddRef();
    return obj->Release();
}

// ------------------------------------------------------------ texture helpers

ComPtr<ID3D11Texture2D> MakeTex(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                                UINT samples = 1, UINT arraySize = 1, const void* data = nullptr,
                                UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = arraySize;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> tex;
    const HRESULT hr = device->CreateTexture2D(&desc, data ? &init : nullptr, &tex);
    if (FAILED(hr)) {
        std::printf("  CreateTexture2D(format %d, bind 0x%X, %u samples) failed: 0x%08lX\n", format, bind, samples,
                    static_cast<unsigned long>(hr));
        return nullptr;
    }
    return tex;
}

// A texture filled with data: the data goes into a bind-less texture of the
// typeless family format (initial data needs no special binding there), which
// is then copied into the target.
ComPtr<ID3D11Texture2D> MakeFilled(ID3D11Device* device, ID3D11DeviceContext* ctx, UINT w, UINT h,
                                   DXGI_FORMAT format, UINT bind, DXGI_FORMAT uploadFormat, const void* data,
                                   UINT bytesPerTexel) {
    ComPtr<ID3D11Texture2D> upload = MakeTex(device, w, h, uploadFormat, 0, 1, 1, data, w * bytesPerTexel);
    ComPtr<ID3D11Texture2D> tex = MakeTex(device, w, h, format, bind);
    if (!upload || !tex) return nullptr;
    ctx->CopyResource(tex.Get(), upload.Get());
    return tex;
}

// Tightly packed texels of subresource 0.
std::vector<uint8_t> ReadBack11(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                                UINT bytesPerTexel) {
    if (!tex) return {};
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<uint8_t> out(static_cast<size_t>(desc.Width) * desc.Height * bytesPerTexel);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * desc.Width * bytesPerTexel,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(desc.Width) * bytesPerTexel);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

// D3D12 side: the shared fence orders the D3D11 copy before the D3D12 copy
// to a readback buffer, as the presenter orders it in production.
struct Readback12 {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D12Device* device = nullptr;
    std::unique_ptr<FencePair> fences;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done;
    uint64_t done_value = 0;
    HANDLE event = nullptr;

    ~Readback12() {
        // A failed wait can leave the D3D12 queue waiting on the shared fence.
        if (fences) fences->CpuSignalShared(fences->Next() + 1000000);
        if (queue && done) {
            queue->Signal(done.Get(), ++done_value);
            if (event && SUCCEEDED(done->SetEventOnCompletion(done_value, event)))
                WaitForSingleObject(event, 2000);
        }
        if (event) CloseHandle(event);
    }

    bool Init(acdb_test::GpuTestDevices& d) {
        ctx = d.ctx11.Get();
        device = d.device12.Get();
        std::string err;
        fences = FencePair::Create(d.device12.Get(), d.device11.Get(), &err);
        if (!fences) {
            std::printf("  FencePair: %s\n", err.c_str());
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event && SUCCEEDED(d.ctx11.As(&ctx4)) &&
               SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                   IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
    }

    std::vector<uint8_t> Read(ID3D12Resource* res, UINT bytesPerTexel) {
        if (!res) return {};
        const D3D12_RESOURCE_DESC rd = res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 total = 0;
        device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&buffer))))
            return {};

        const uint64_t v = fences->Next();
        ctx4->Signal(fences->Shared11(), v);
        ctx->Flush();
        queue->Wait(fences->Shared12(), v);

        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(done.Get(), ++done_value);
        if (FAILED(done->SetEventOnCompletion(done_value, event)) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
            std::printf("  D3D12 readback timed out\n");
            return {};
        }

        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        if (FAILED(buffer->Map(0, &range, &mapped))) return {};
        const UINT w = static_cast<UINT>(rd.Width);
        std::vector<uint8_t> out(static_cast<size_t>(w) * rows * bytesPerTexel);
        for (UINT y = 0; y < rows; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * w * bytesPerTexel,
                        static_cast<const uint8_t*>(mapped) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                        static_cast<size_t>(w) * bytesPerTexel);
        const D3D12_RANGE none{0, 0};
        buffer->Unmap(0, &none);
        return out;
    }
};

// ------------------------------------------------------------ patterns

uint32_t Hash(uint32_t i) { return i * 2654435761u; }

float BitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t FloatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// Depth values in [0, 1], no denormals; the first texels are 0 and 1.
std::vector<float> FloatDepth(UINT w, UINT h) {
    std::vector<float> v(static_cast<size_t>(w) * h);
    for (uint32_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(Hash(i + 1) % 1000003u) / 1000003.0f;
    v[0] = 0.0f;
    if (v.size() > 1) v[1] = 1.0f;
    return v;
}

// R16G16_FLOAT texels: two finite halves (exponent never all ones), both signs.
std::vector<uint32_t> MvPattern(UINT w, UINT h, uint32_t seed) {
    std::vector<uint32_t> v(static_cast<size_t>(w) * h);
    for (uint32_t i = 0; i < v.size(); ++i) {
        const uint32_t x = Hash(i + seed);
        const uint32_t lo = ((x >> 3) & 0x3FFF) | ((x & 1) << 15);
        const uint32_t hi = ((x >> 17) & 0x3FFF) | ((x & 2) << 14);
        v[i] = lo | (hi << 16);
    }
    return v;
}

// Two floats within one ulp (both non-negative).
bool WithinOneUlp(float a, float b) {
    const uint32_t x = FloatBits(a);
    const uint32_t y = FloatBits(b);
    return (x > y ? x - y : y - x) <= 1;
}

std::vector<float> AsFloats(const std::vector<uint8_t>& bytes) {
    std::vector<float> f(bytes.size() / 4);
    if (!f.empty()) std::memcpy(f.data(), bytes.data(), f.size() * 4);
    return f;
}

struct Sources {
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> mvec;
    std::vector<float> expected_depth;
    std::vector<uint32_t> mv;
};

// R32_TYPELESS depth (DSV | SRV, as CSP's) and R16G16_FLOAT motion vectors.
Sources MakeCspSources(acdb_test::GpuTestDevices& d, UINT dw, UINT dh, UINT mw, UINT mh, uint32_t seed = 7) {
    Sources s;
    s.expected_depth = FloatDepth(dw, dh);
    s.mv = MvPattern(mw, mh, seed);
    s.depth = MakeFilled(d.device11.Get(), d.ctx11.Get(), dw, dh, DXGI_FORMAT_R32_TYPELESS,
                         D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R32_TYPELESS,
                         s.expected_depth.data(), 4);
    s.mvec = MakeFilled(d.device11.Get(), d.ctx11.Get(), mw, mh, DXGI_FORMAT_R16G16_FLOAT,
                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R16G16_TYPELESS,
                        s.mv.data(), 4);
    return s;
}

bool SameFloats(const std::vector<float>& got, const std::vector<float>& want) {
    if (got.size() != want.size()) {
        std::printf("  size %zu, expected %zu\n", got.size(), want.size());
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (FloatBits(got[i]) != FloatBits(want[i])) {
            std::printf("  texel %zu: 0x%08X, expected 0x%08X\n", i, FloatBits(got[i]), FloatBits(want[i]));
            return false;
        }
    }
    return true;
}

bool SameBytes(const std::vector<uint8_t>& got, const void* want, size_t bytes) {
    if (got.size() != bytes) {
        std::printf("  %zu bytes, expected %zu\n", got.size(), bytes);
        return false;
    }
    if (std::memcmp(got.data(), want, bytes) != 0) {
        for (size_t i = 0; i < bytes; ++i)
            if (got[i] != static_cast<const uint8_t*>(want)[i]) {
                std::printf("  first difference at byte %zu\n", i);
                break;
            }
        return false;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------ format tables (no GPU)

TEST(CaptureSlots_DepthSourceFormatTable) {
    struct Row {
        DXGI_FORMAT source, srv, family;
    };
    const Row rows[] = {
        {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS},
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS},
        {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS},
        {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_TYPELESS},
        {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_TYPELESS},
        {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_TYPELESS},
        {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS},
        {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS},
    };
    for (const Row& r : rows) {
        const DepthFormatInfo info = DepthSourceFormat(r.source);
        CHECK_EQ(info.srv, r.srv);
        CHECK_EQ(info.family, r.family);
    }
    for (DXGI_FORMAT f : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT,
                          DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8_UNORM}) {
        CHECK_EQ(DepthSourceFormat(f).srv, DXGI_FORMAT_UNKNOWN);
    }
}

TEST(CaptureSlots_TypedMvecFormatTable) {
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R16G16_FLOAT), DXGI_FORMAT_R16G16_FLOAT);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R32G32_FLOAT), DXGI_FORMAT_R32G32_FLOAT);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R16G16_SNORM), DXGI_FORMAT_R16G16_SNORM);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R16G16_TYPELESS), DXGI_FORMAT_R16G16_FLOAT);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R32G32_TYPELESS), DXGI_FORMAT_R32G32_FLOAT);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS), DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R32G32B32A32_TYPELESS), DXGI_FORMAT_R32G32B32A32_FLOAT);
    // No typed guess for other typeless families; depth formats are no MVs.
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS), DXGI_FORMAT_UNKNOWN);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_R32_TYPELESS), DXGI_FORMAT_UNKNOWN);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_D32_FLOAT), DXGI_FORMAT_UNKNOWN);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_D24_UNORM_S8_UINT), DXGI_FORMAT_UNKNOWN);
    CHECK_EQ(TypedMvecFormat(DXGI_FORMAT_UNKNOWN), DXGI_FORMAT_UNKNOWN);
}

// ------------------------------------------------------------ creation

TEST(CaptureSlots_DepthBlitShaderIsEmbeddedCs50) {
    static_assert(sizeof(kDepthBlitCs) > 4);
    CHECK(std::memcmp(kDepthBlitCs, "DXBC", 4) == 0);
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ComPtr<ID3D11ComputeShader> cs;
    CHECK(SUCCEEDED(d.device11->CreateComputeShader(kDepthBlitCs, sizeof(kDepthBlitCs), nullptr, &cs)));
}

TEST(CaptureSlots_CreateRejectsNullDevices) {
    std::string err;
    CHECK(CaptureSlots::Create(nullptr, nullptr, &err) == nullptr);
    CHECK(!err.empty());
    CHECK(CaptureSlots::Create(nullptr, nullptr, nullptr) == nullptr);
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    err.clear();
    CHECK(CaptureSlots::Create(d.device11.Get(), nullptr, &err) == nullptr);
    CHECK(!err.empty());
    err.clear();
    CHECK(CaptureSlots::Create(nullptr, d.device12.Get(), &err) == nullptr);
    CHECK(!err.empty());
}

TEST(CaptureSlots_CreateRefusesDevicesOnDifferentAdapters) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    if (d.warp) {
        std::printf("  SKIP: only WARP here, no second adapter\n");
        return;
    }
    ComPtr<IDXGIAdapter1> warp;
    REQUIRE(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    ComPtr<ID3D11Device> warp11;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                        &warp11, nullptr, nullptr)));
    std::string err;
    CHECK(CaptureSlots::Create(warp11.Get(), d.device12.Get(), &err) == nullptr);
    CHECK(err.find("adapter") != std::string::npos);
}

TEST(CaptureSlots_NewSlotsAreEmpty) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    if (!slots) std::printf("  error: %s\n", err.c_str());
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 16, 16, 16, 16);
    REQUIRE(s.depth && s.mvec);
    for (uint32_t i = 0; i < CaptureSlots::kSlots; ++i) {
        CHECK(!slots->HasTextures(i));
        CHECK(slots->Depth12(i) == nullptr);
        CHECK(slots->Mvec12(i) == nullptr);
        CHECK(slots->Depth11(i) == nullptr);
        CHECK(slots->Mvec11(i) == nullptr);
        CHECK_EQ(slots->MvecFormat(i), DXGI_FORMAT_UNKNOWN);
        CHECK(!slots->Matches(i, s.depth.Get(), s.mvec.Get()));
    }
    CHECK(slots->Depth12(CaptureSlots::kSlots) == nullptr);
    CHECK_EQ(slots->CachedSourceViews(), size_t{0});
}

TEST(CaptureSlots_RecreateSizesEachTextureFromItsOwnSource) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 64, 48, 32, 24);
    REQUIRE(s.depth && s.mvec);
    const bool ok = slots->Recreate(1, s.depth.Get(), s.mvec.Get(), &err);
    if (!ok) std::printf("  error: %s\n", err.c_str());
    REQUIRE(ok);
    CHECK(slots->HasTextures(1));
    CHECK(!slots->HasTextures(0));
    CHECK(!slots->HasTextures(2));
    CHECK(slots->Matches(1, s.depth.Get(), s.mvec.Get()));

    REQUIRE(slots->Depth11(1) && slots->Mvec11(1) && slots->Depth12(1) && slots->Mvec12(1));
    D3D11_TEXTURE2D_DESC dd{};
    slots->Depth11(1)->GetDesc(&dd);
    CHECK_EQ(dd.Width, 64u);
    CHECK_EQ(dd.Height, 48u);
    CHECK_EQ(dd.Format, DXGI_FORMAT_R32_FLOAT);
    CHECK_EQ(dd.MipLevels, 1u);
    CHECK_EQ(dd.ArraySize, 1u);
    CHECK_EQ(dd.SampleDesc.Count, 1u);
    CHECK_EQ(dd.BindFlags, static_cast<UINT>(D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE));
    CHECK_EQ(dd.MiscFlags, static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE));

    D3D11_TEXTURE2D_DESC md{};
    slots->Mvec11(1)->GetDesc(&md);
    CHECK_EQ(md.Width, 32u);
    CHECK_EQ(md.Height, 24u);
    CHECK_EQ(md.Format, DXGI_FORMAT_R16G16_FLOAT);
    CHECK_EQ(md.BindFlags, static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE));
    CHECK_EQ(md.MiscFlags, static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
    CHECK_EQ(slots->MvecFormat(1), DXGI_FORMAT_R16G16_FLOAT);

    const D3D12_RESOURCE_DESC d12 = slots->Depth12(1)->GetDesc();
    CHECK_EQ(d12.Width, 64u);
    CHECK_EQ(d12.Height, 48u);
    CHECK_EQ(d12.Format, DXGI_FORMAT_R32_FLOAT);
    CHECK((d12.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
    const D3D12_RESOURCE_DESC m12 = slots->Mvec12(1)->GetDesc();
    CHECK_EQ(m12.Width, 32u);
    CHECK_EQ(m12.Height, 24u);
    CHECK_EQ(m12.Format, DXGI_FORMAT_R16G16_FLOAT);
}

TEST(CaptureSlots_MatchesComparesFormatSizeAndSampleDesc) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 64, 48, 32, 24);
    REQUIRE(s.depth && s.mvec);
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));
    ID3D11Device* dev = d.device11.Get();
    const UINT dsvSrv = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    const UINT rtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // Another resource with equal descs matches (CSP ping-pongs its MVs).
    auto depth2 = MakeTex(dev, 64, 48, DXGI_FORMAT_R32_TYPELESS, dsvSrv);
    auto mvec2 = MakeTex(dev, 32, 24, DXGI_FORMAT_R16G16_FLOAT, rtSrv);
    REQUIRE(depth2 && mvec2);
    CHECK(slots->Matches(0, depth2.Get(), mvec2.Get()));

    auto depthWide = MakeTex(dev, 65, 48, DXGI_FORMAT_R32_TYPELESS, dsvSrv);
    auto depthTall = MakeTex(dev, 64, 49, DXGI_FORMAT_R32_TYPELESS, dsvSrv);
    auto depth24 = MakeTex(dev, 64, 48, DXGI_FORMAT_R24G8_TYPELESS, dsvSrv);
    auto depthMsaa = MakeTex(dev, 64, 48, DXGI_FORMAT_R32_TYPELESS, dsvSrv, 4);
    auto mvecWide = MakeTex(dev, 33, 24, DXGI_FORMAT_R16G16_FLOAT, rtSrv);
    auto mvec32 = MakeTex(dev, 32, 24, DXGI_FORMAT_R32G32_FLOAT, rtSrv);
    auto mvecMsaa = MakeTex(dev, 32, 24, DXGI_FORMAT_R16G16_FLOAT, rtSrv, 4);
    REQUIRE(depthWide && depthTall && depth24 && depthMsaa && mvecWide && mvec32 && mvecMsaa);
    CHECK(!slots->Matches(0, depthWide.Get(), s.mvec.Get()));
    CHECK(!slots->Matches(0, depthTall.Get(), s.mvec.Get()));
    CHECK(!slots->Matches(0, depth24.Get(), s.mvec.Get()));
    CHECK(!slots->Matches(0, depthMsaa.Get(), s.mvec.Get()));
    CHECK(!slots->Matches(0, s.depth.Get(), mvecWide.Get()));
    CHECK(!slots->Matches(0, s.depth.Get(), mvec32.Get()));
    CHECK(!slots->Matches(0, s.depth.Get(), mvecMsaa.Get()));
    // Depth and MV swapped: each is compared with its own recorded desc.
    CHECK(!slots->Matches(0, s.mvec.Get(), s.depth.Get()));
    CHECK(!slots->Matches(0, nullptr, s.mvec.Get()));
    CHECK(!slots->Matches(0, s.depth.Get(), nullptr));
    CHECK(!slots->Matches(1, s.depth.Get(), s.mvec.Get()));
    CHECK(!slots->Matches(CaptureSlots::kSlots, s.depth.Get(), s.mvec.Get()));
}

TEST(CaptureSlots_RecreateRefusesUnsupportedSourcesAndLeavesTheSlotEmpty) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 32, 32, 32, 32);
    REQUIRE(s.depth && s.mvec);
    ID3D11Device* dev = d.device11.Get();
    const UINT dsvSrv = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    const UINT rtSrv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    auto depthMsaa = MakeTex(dev, 32, 32, DXGI_FORMAT_R32_TYPELESS, dsvSrv, 4);
    auto mvecMsaa = MakeTex(dev, 32, 32, DXGI_FORMAT_R16G16_FLOAT, rtSrv, 4);
    auto depthColor = MakeTex(dev, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, rtSrv);
    auto mvecTypeless8 = MakeTex(dev, 32, 32, DXGI_FORMAT_R8G8B8A8_TYPELESS, rtSrv);
    auto depthArray = MakeTex(dev, 32, 32, DXGI_FORMAT_R32_TYPELESS, dsvSrv, 1, 2);
    REQUIRE(depthMsaa && mvecMsaa && depthColor && mvecTypeless8 && depthArray);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 256;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Buffer> buffer;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &buffer)));
    // A texture of another device.
    ComPtr<ID3D11Device> other;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(d.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                                        D3D11_SDK_VERSION, &other, nullptr, nullptr)));
    auto foreignDepth = MakeTex(other.Get(), 32, 32, DXGI_FORMAT_R32_TYPELESS, dsvSrv);
    REQUIRE(foreignDepth);

    struct Case {
        const char* what;
        uint32_t slot;
        ID3D11Resource* depth;
        ID3D11Resource* mvec;
    };
    const Case cases[] = {
        {"slot index", CaptureSlots::kSlots, s.depth.Get(), s.mvec.Get()},
        {"null depth", 0, nullptr, s.mvec.Get()},
        {"null mvec", 0, s.depth.Get(), nullptr},
        {"MSAA depth", 0, depthMsaa.Get(), s.mvec.Get()},
        {"MSAA mvec", 0, s.depth.Get(), mvecMsaa.Get()},
        {"colour depth", 0, depthColor.Get(), s.mvec.Get()},
        {"typeless RGBA8 mvec", 0, s.depth.Get(), mvecTypeless8.Get()},
        {"array depth", 0, depthArray.Get(), s.mvec.Get()},
        {"buffer depth", 0, buffer.Get(), s.mvec.Get()},
        {"foreign depth", 0, foreignDepth.Get(), s.mvec.Get()},
    };
    for (const Case& c : cases) {
        // Start from a valid slot so that the refusal must also empty it.
        if (c.slot < CaptureSlots::kSlots) REQUIRE(slots->Recreate(c.slot, s.depth.Get(), s.mvec.Get(), &err));
        err.clear();
        const bool ok = slots->Recreate(c.slot, c.depth, c.mvec, &err);
        if (ok || err.empty()) std::printf("  case: %s\n", c.what);
        CHECK(!ok);
        CHECK(!err.empty());
        if (c.slot < CaptureSlots::kSlots) {
            CHECK(!slots->HasTextures(c.slot));
            CHECK(slots->Depth12(c.slot) == nullptr);
            CHECK(slots->Mvec12(c.slot) == nullptr);
            CHECK_EQ(slots->MvecFormat(c.slot), DXGI_FORMAT_UNKNOWN);
        }
    }
}

// ------------------------------------------------------------ copies

TEST(CaptureSlots_CopyCspSourcesReadsBackOnBothApis) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Readback12 rb;
    REQUIRE(rb.Init(d));
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    // Odd sizes: the dispatch rounds up and the shader must stay in bounds.
    Sources s = MakeCspSources(d, 37, 21, 37, 21);
    REQUIRE(s.depth && s.mvec);
    REQUIRE(slots->Recreate(2, s.depth.Get(), s.mvec.Get(), &err));
    const bool ok = slots->Copy(d.ctx11.Get(), 2, s.depth.Get(), s.mvec.Get(), &err);
    if (!ok) std::printf("  error: %s\n", err.c_str());
    REQUIRE(ok);

    CHECK(SameFloats(AsFloats(ReadBack11(d.device11.Get(), d.ctx11.Get(), slots->Depth11(2), 4)), s.expected_depth));
    CHECK(SameBytes(ReadBack11(d.device11.Get(), d.ctx11.Get(), slots->Mvec11(2), 4), s.mv.data(), s.mv.size() * 4));
    CHECK(SameFloats(AsFloats(rb.Read(slots->Depth12(2), 4)), s.expected_depth));
    CHECK(SameBytes(rb.Read(slots->Mvec12(2), 4), s.mv.data(), s.mv.size() * 4));
}

TEST(CaptureSlots_CopyWithDifferentDepthAndMvSizes) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Readback12 rb;
    REQUIRE(rb.Init(d));
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    struct Size {
        UINT dw, dh, mw, mh;
    };
    for (const Size& z : {Size{64, 40, 32, 20}, Size{20, 30, 41, 17}}) {
        Sources s = MakeCspSources(d, z.dw, z.dh, z.mw, z.mh);
        REQUIRE(s.depth && s.mvec);
        REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));
        REQUIRE(slots->Copy(d.ctx11.Get(), 0, s.depth.Get(), s.mvec.Get(), &err));
        CHECK(SameFloats(AsFloats(rb.Read(slots->Depth12(0), 4)), s.expected_depth));
        CHECK(SameBytes(rb.Read(slots->Mvec12(0), 4), s.mv.data(), s.mv.size() * 4));
    }
}

TEST(CaptureSlots_CopyEveryDepthFormat) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Readback12 rb;
    REQUIRE(rb.Init(d));
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    enum class Kind { F32, D24S8, U16, F32S8 };
    struct Variant {
        const char* name;
        DXGI_FORMAT format;
        UINT bind;
        DXGI_FORMAT upload;
        Kind kind;
    };
    const UINT dsv = D3D11_BIND_DEPTH_STENCIL;
    const UINT dsvSrv = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    const Variant variants[] = {
        {"R32_TYPELESS dsv+srv", DXGI_FORMAT_R32_TYPELESS, dsvSrv, DXGI_FORMAT_R32_TYPELESS, Kind::F32},
        {"R32_TYPELESS dsv only", DXGI_FORMAT_R32_TYPELESS, dsv, DXGI_FORMAT_R32_TYPELESS, Kind::F32},
        {"R32_FLOAT srv", DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R32_TYPELESS, Kind::F32},
        {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT, dsv, DXGI_FORMAT_R32_TYPELESS, Kind::F32},
        {"R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS, dsvSrv, DXGI_FORMAT_R24G8_TYPELESS, Kind::D24S8},
        {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT, dsv, DXGI_FORMAT_R24G8_TYPELESS, Kind::D24S8},
        {"R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS, dsvSrv, DXGI_FORMAT_R16_TYPELESS, Kind::U16},
        {"D16_UNORM", DXGI_FORMAT_D16_UNORM, dsv, DXGI_FORMAT_R16_TYPELESS, Kind::U16},
        {"R16_UNORM srv", DXGI_FORMAT_R16_UNORM, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R16_TYPELESS, Kind::U16},
        {"R32G8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS, dsvSrv, DXGI_FORMAT_R32G8X24_TYPELESS, Kind::F32S8},
        {"D32_FLOAT_S8X24_UINT", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, dsv, DXGI_FORMAT_R32G8X24_TYPELESS, Kind::F32S8},
    };
    const UINT w = 29;
    const UINT h = 19;
    const size_t n = static_cast<size_t>(w) * h;
    Sources base = MakeCspSources(d, w, h, w, h);
    REQUIRE(base.mvec);
    for (const Variant& v : variants) {
        std::vector<uint8_t> data;
        std::vector<float> want(n);
        bool exact = true;
        UINT bpt = 4;
        const std::vector<float> f = FloatDepth(w, h);
        switch (v.kind) {
            case Kind::F32:
                data.resize(n * 4);
                std::memcpy(data.data(), f.data(), n * 4);
                want = f;
                break;
            case Kind::F32S8:
                bpt = 8;
                data.resize(n * 8);
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t stencil = Hash(static_cast<uint32_t>(i)) >> 24;
                    std::memcpy(&data[i * 8], &f[i], 4);
                    std::memcpy(&data[i * 8 + 4], &stencil, 4);
                }
                want = f;
                break;
            case Kind::D24S8:
                exact = false;
                data.resize(n * 4);
                for (size_t i = 0; i < n; ++i) {
                    uint32_t d24 = Hash(static_cast<uint32_t>(i) + 3) & 0xFFFFFF;
                    if (i == 0) d24 = 0;
                    if (i == 1) d24 = 0xFFFFFF;
                    const uint32_t texel = d24 | ((Hash(static_cast<uint32_t>(i)) >> 24) << 24);
                    std::memcpy(&data[i * 4], &texel, 4);
                    want[i] = static_cast<float>(d24 / 16777215.0);
                }
                break;
            case Kind::U16:
                exact = false;
                bpt = 2;
                data.resize(n * 2);
                for (size_t i = 0; i < n; ++i) {
                    uint16_t d16 = static_cast<uint16_t>(Hash(static_cast<uint32_t>(i) + 5) >> 16);
                    if (i == 0) d16 = 0;
                    if (i == 1) d16 = 0xFFFF;
                    std::memcpy(&data[i * 2], &d16, 2);
                    want[i] = static_cast<float>(d16 / 65535.0);
                }
                break;
        }
        auto depth = MakeFilled(d.device11.Get(), d.ctx11.Get(), w, h, v.format, v.bind, v.upload, data.data(), bpt);
        if (!depth) std::printf("  variant %s: source not created\n", v.name);
        REQUIRE(depth);
        const bool ok = slots->Recreate(1, depth.Get(), base.mvec.Get(), &err) &&
                        slots->Copy(d.ctx11.Get(), 1, depth.Get(), base.mvec.Get(), &err);
        if (!ok) std::printf("  variant %s: %s\n", v.name, err.c_str());
        CHECK(ok);
        if (!ok) continue;
        const std::vector<float> got11 = AsFloats(ReadBack11(d.device11.Get(), d.ctx11.Get(), slots->Depth11(1), 4));
        const std::vector<float> got12 = AsFloats(rb.Read(slots->Depth12(1), 4));
        bool good = got11.size() == n && got12 == got11;
        for (size_t i = 0; good && i < n; ++i) {
            good = exact ? FloatBits(got11[i]) == FloatBits(want[i]) : WithinOneUlp(got11[i], want[i]);
            if (!good)
                std::printf("  variant %s texel %zu: %.9g, expected %.9g\n", v.name, i, got11[i], want[i]);
        }
        if (!good && got11.size() == n && got12 != got11) std::printf("  variant %s: D3D12 differs\n", v.name);
        if (!good && got11.size() != n) std::printf("  variant %s: no D3D11 readback\n", v.name);
        CHECK(good);
    }
}

TEST(CaptureSlots_CopyMapsTypelessMotionVectors) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Readback12 rb;
    REQUIRE(rb.Init(d));
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 24, 16, 24, 16);
    REQUIRE(s.depth);
    auto mv = MakeFilled(d.device11.Get(), d.ctx11.Get(), 24, 16, DXGI_FORMAT_R16G16_TYPELESS,
                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R16G16_TYPELESS,
                         s.mv.data(), 4);
    REQUIRE(mv);
    REQUIRE(slots->Recreate(0, s.depth.Get(), mv.Get(), &err));
    CHECK_EQ(slots->MvecFormat(0), DXGI_FORMAT_R16G16_FLOAT);
    CHECK_EQ(slots->Mvec12(0)->GetDesc().Format, DXGI_FORMAT_R16G16_FLOAT);
    REQUIRE(slots->Copy(d.ctx11.Get(), 0, s.depth.Get(), mv.Get(), &err));
    CHECK(SameBytes(rb.Read(slots->Mvec12(0), 4), s.mv.data(), s.mv.size() * 4));
}

TEST(CaptureSlots_PingPongMotionVectorsAcrossTheRing) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Readback12 rb;
    REQUIRE(rb.Init(d));
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources a = MakeCspSources(d, 40, 24, 40, 24, 11);
    Sources b = MakeCspSources(d, 40, 24, 40, 24, 12);
    REQUIRE(a.depth && a.mvec && b.mvec);
    for (uint32_t frame = 0; frame < 7; ++frame) {
        const uint32_t slot = frame % CaptureSlots::kSlots;
        Sources& mv = (frame % 2) ? b : a;
        if (!slots->HasTextures(slot)) REQUIRE(slots->Recreate(slot, a.depth.Get(), mv.mvec.Get(), &err));
        // Both MV textures have one desc: no recreate after the first lap.
        CHECK(slots->Matches(slot, a.depth.Get(), mv.mvec.Get()));
        REQUIRE(slots->Copy(d.ctx11.Get(), slot, a.depth.Get(), mv.mvec.Get(), &err));
        CHECK(SameBytes(rb.Read(slots->Mvec12(slot), 4), mv.mv.data(), mv.mv.size() * 4));
        CHECK(SameFloats(AsFloats(rb.Read(slots->Depth12(slot), 4)), a.expected_depth));
    }
    // Motion vectors need no view; the stable depth needs one.
    CHECK_EQ(slots->CachedSourceViews(), size_t{1});
}

// A depth source without BIND_SHADER_RESOURCE is read through a cached copy;
// that copy must be refreshed on every Copy, not only when it is created.
TEST(CaptureSlots_CopyRereadsADepthSourceWithoutAShaderBinding) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ID3D11Device* dev = d.device11.Get();
    ID3D11DeviceContext* ctx = d.ctx11.Get();
    std::string err;
    auto slots = CaptureSlots::Create(dev, d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    const UINT w = 24;
    const UINT h = 20;
    Sources base = MakeCspSources(d, w, h, w, h);
    REQUIRE(base.mvec);
    std::vector<float> first = FloatDepth(w, h);
    std::vector<float> second(first.size());
    for (size_t i = 0; i < first.size(); ++i) second[i] = 1.0f - first[i];
    auto depth = MakeFilled(dev, ctx, w, h, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, DXGI_FORMAT_R32_TYPELESS,
                            first.data(), 4);
    REQUIRE(depth);
    REQUIRE(slots->Recreate(0, depth.Get(), base.mvec.Get(), &err));
    REQUIRE(slots->Copy(ctx, 0, depth.Get(), base.mvec.Get(), &err));
    CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), first));

    // CSP renders the next frame into the same depth texture.
    auto next = MakeTex(dev, w, h, DXGI_FORMAT_R32_TYPELESS, 0, 1, 1, second.data(), w * 4);
    REQUIRE(next);
    ctx->CopyResource(depth.Get(), next.Get());
    REQUIRE(slots->Copy(ctx, 0, depth.Get(), base.mvec.Get(), &err));
    CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), second));
}

// Two depth sources of one desc, used in turn without a Recreate: the view
// cache must hand out each source's own view.
TEST(CaptureSlots_CopyReadsEachOfTwoAlternatingDepthSources) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ID3D11Device* dev = d.device11.Get();
    ID3D11DeviceContext* ctx = d.ctx11.Get();
    std::string err;
    auto slots = CaptureSlots::Create(dev, d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    const UINT w = 20;
    const UINT h = 12;
    Sources a = MakeCspSources(d, w, h, w, h);
    REQUIRE(a.depth && a.mvec);
    std::vector<float> otherDepth = FloatDepth(w, h);
    for (float& v : otherDepth) v = 1.0f - v;
    auto b = MakeFilled(dev, ctx, w, h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE,
                        DXGI_FORMAT_R32_TYPELESS, otherDepth.data(), 4);
    REQUIRE(b);
    REQUIRE(slots->Recreate(0, a.depth.Get(), a.mvec.Get(), &err));
    for (int round = 0; round < 2; ++round) {
        REQUIRE(slots->Copy(ctx, 0, a.depth.Get(), a.mvec.Get(), &err));
        CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), a.expected_depth));
        REQUIRE(slots->Copy(ctx, 0, b.Get(), a.mvec.Get(), &err));
        CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), otherDepth));
    }
    CHECK_EQ(slots->CachedSourceViews(), size_t{2});
}

// ------------------------------------------------------------ refusals at copy time

TEST(CaptureSlots_CopyRefusesDeferredContext) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 16, 16, 16, 16);
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));
    ComPtr<ID3D11DeviceContext> deferred;
    REQUIRE(SUCCEEDED(d.device11->CreateDeferredContext(0, &deferred)));
    err.clear();
    CHECK(!slots->Copy(deferred.Get(), 0, s.depth.Get(), s.mvec.Get(), &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!slots->Copy(nullptr, 0, s.depth.Get(), s.mvec.Get(), &err));
    CHECK(!err.empty());
}

TEST(CaptureSlots_CopyRefusesSlotsThatDoNotMatch) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 32, 32, 32, 32);
    Sources big = MakeCspSources(d, 64, 64, 64, 64);
    REQUIRE(s.depth && s.mvec && big.depth && big.mvec);
    err.clear();
    CHECK(!slots->Copy(d.ctx11.Get(), 0, s.depth.Get(), s.mvec.Get(), &err));  // empty slot
    CHECK(!err.empty());
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));
    err.clear();
    CHECK(!slots->Copy(d.ctx11.Get(), 0, big.depth.Get(), s.mvec.Get(), &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!slots->Copy(d.ctx11.Get(), 0, s.depth.Get(), big.mvec.Get(), &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!slots->Copy(d.ctx11.Get(), CaptureSlots::kSlots, s.depth.Get(), s.mvec.Get(), &err));
    CHECK(!err.empty());
    CHECK(slots->Copy(d.ctx11.Get(), 0, s.depth.Get(), s.mvec.Get(), &err));
}

// ------------------------------------------------------------ CSP's state is untouched

TEST(CaptureSlots_CopyRestoresComputeState) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ID3D11Device* dev = d.device11.Get();
    ID3D11DeviceContext* ctx = d.ctx11.Get();
    std::string err;
    auto slots = CaptureSlots::Create(dev, d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 32, 16, 32, 16);
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));

    // Sentinel state: a compute shader, SRVs in slots 0 and 1, a counter UAV
    // (initial count 7) in slot 0 and a typed UAV in slot 1, constant buffer 0.
    ComPtr<ID3D11ComputeShader> cs;
    REQUIRE(SUCCEEDED(dev->CreateComputeShader(kDepthBlitCs, sizeof(kDepthBlitCs), nullptr, &cs)));
    auto srvTex = MakeTex(dev, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    REQUIRE(srvTex);
    ComPtr<ID3D11ShaderResourceView> srv0, srv1;
    REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(srvTex.Get(), nullptr, &srv0)));
    REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(srvTex.Get(), nullptr, &srv1)));

    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 64;
    sb.Usage = D3D11_USAGE_DEFAULT;
    sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    sb.StructureByteStride = 4;
    ComPtr<ID3D11Buffer> counterBuf;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &counterBuf)));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> uav0, uav1;
    REQUIRE(SUCCEEDED(dev->CreateUnorderedAccessView(counterBuf.Get(), &ud, &uav0)));
    auto uavTex = MakeTex(dev, 8, 8, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
    REQUIRE(uavTex);
    REQUIRE(SUCCEEDED(dev->CreateUnorderedAccessView(uavTex.Get(), nullptr, &uav1)));

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &cb)));

    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {srv0.Get(), srv1.Get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = {uav0.Get(), uav1.Get()};
    const UINT counts[] = {7, 0};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, counts);
    ID3D11Buffer* cbs[] = {cb.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);

    REQUIRE(slots->Copy(ctx, 0, s.depth.Get(), s.mvec.Get(), &err));

    ComPtr<ID3D11ComputeShader> csAfter;
    ctx->CSGetShader(&csAfter, nullptr, nullptr);
    CHECK(csAfter.Get() == cs.Get());
    ID3D11ShaderResourceView* srvAfter[2] = {};
    ctx->CSGetShaderResources(0, 2, srvAfter);
    CHECK(srvAfter[0] == srv0.Get());
    CHECK(srvAfter[1] == srv1.Get());
    for (auto* p : srvAfter)
        if (p) p->Release();
    ID3D11UnorderedAccessView* uavAfter[2] = {};
    ctx->CSGetUnorderedAccessViews(0, 2, uavAfter);
    CHECK(uavAfter[0] == uav0.Get());
    CHECK(uavAfter[1] == uav1.Get());
    for (auto* p : uavAfter)
        if (p) p->Release();
    ComPtr<ID3D11Buffer> cbAfter;
    ctx->CSGetConstantBuffers(0, 1, &cbAfter);
    CHECK(cbAfter.Get() == cb.Get());

    // The hidden counter kept its initial count.
    D3D11_BUFFER_DESC rd{};
    rd.ByteWidth = 16;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> countOut;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&rd, nullptr, &countOut)));
    ctx->CopyStructureCount(countOut.Get(), 0, uav0.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    REQUIRE(SUCCEEDED(ctx->Map(countOut.Get(), 0, D3D11_MAP_READ, 0, &m)));
    const uint32_t count = *static_cast<const uint32_t*>(m.pData);
    ctx->Unmap(countOut.Get(), 0);
    if (count != 7) std::printf("  counter %u, expected 7\n", count);
    CHECK_EQ(count, 7u);

    // And the copy itself was right under that state.
    CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), s.expected_depth));
    ctx->ClearState();
}

TEST(CaptureSlots_CopyReadsADepthBoundAsDepthStencilView) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ID3D11Device* dev = d.device11.Get();
    ID3D11DeviceContext* ctx = d.ctx11.Get();
    std::string err;
    auto slots = CaptureSlots::Create(dev, d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 32, 16, 32, 16);
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));

    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    REQUIRE(SUCCEEDED(dev->CreateDepthStencilView(s.depth.Get(), &dd, &dsv)));
    auto rtTex = MakeTex(dev, 32, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    REQUIRE(rtTex);
    ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(rtTex.Get(), nullptr, &rtv)));
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    ctx->OMSetRenderTargets(1, rtvs, dsv.Get());

    REQUIRE(slots->Copy(ctx, 0, s.depth.Get(), s.mvec.Get(), &err));
    CHECK_EQ(slots->DsvUnbinds(), 1u);

    ComPtr<ID3D11RenderTargetView> rtvAfter;
    ComPtr<ID3D11DepthStencilView> dsvAfter;
    ctx->OMGetRenderTargets(1, &rtvAfter, &dsvAfter);
    CHECK(rtvAfter.Get() == rtv.Get());
    CHECK(dsvAfter.Get() == dsv.Get());
    CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), s.expected_depth));

    // Not bound any more: no unbind.
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    REQUIRE(slots->Copy(ctx, 0, s.depth.Get(), s.mvec.Get(), &err));
    CHECK_EQ(slots->DsvUnbinds(), 1u);
    ctx->ClearState();
}

// The depth-stencil unbind must give back the whole output-merger state:
// render targets with a gap between them, and the output-merger UAVs with
// their hidden counters.
TEST(CaptureSlots_CopyKeepsTheOutputMergerStateAroundADepthStencilUnbind) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    ID3D11Device* dev = d.device11.Get();
    ID3D11DeviceContext* ctx = d.ctx11.Get();
    std::string err;
    auto slots = CaptureSlots::Create(dev, d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources s = MakeCspSources(d, 32, 16, 32, 16);
    REQUIRE(s.depth && s.mvec);
    REQUIRE(slots->Recreate(0, s.depth.Get(), s.mvec.Get(), &err));

    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    REQUIRE(SUCCEEDED(dev->CreateDepthStencilView(s.depth.Get(), &dd, &dsv)));
    auto rt0 = MakeTex(dev, 32, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    auto rt2 = MakeTex(dev, 32, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    REQUIRE(rt0 && rt2);
    ComPtr<ID3D11RenderTargetView> rtv0, rtv2;
    REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(rt0.Get(), nullptr, &rtv0)));
    REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(rt2.Get(), nullptr, &rtv2)));

    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = 64;
    sb.Usage = D3D11_USAGE_DEFAULT;
    sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    sb.StructureByteStride = 4;
    ComPtr<ID3D11Buffer> counterBuf;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &counterBuf)));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> omUav;
    REQUIRE(SUCCEEDED(dev->CreateUnorderedAccessView(counterBuf.Get(), &ud, &omUav)));

    ID3D11RenderTargetView* rtvs[] = {rtv0.Get(), nullptr, rtv2.Get()};
    ID3D11UnorderedAccessView* omUavs[] = {omUav.Get()};
    const UINT counts[] = {5};
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(3, rtvs, dsv.Get(), 3, 1, omUavs, counts);

    REQUIRE(slots->Copy(ctx, 0, s.depth.Get(), s.mvec.Get(), &err));
    CHECK_EQ(slots->DsvUnbinds(), 1u);

    ID3D11RenderTargetView* rtvAfter[3] = {};
    ComPtr<ID3D11DepthStencilView> dsvAfter;
    ID3D11UnorderedAccessView* uavAfter[1] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(3, rtvAfter, &dsvAfter, 3, 1, uavAfter);
    CHECK(rtvAfter[0] == rtv0.Get());
    CHECK(rtvAfter[1] == nullptr);
    CHECK(rtvAfter[2] == rtv2.Get());
    CHECK(dsvAfter.Get() == dsv.Get());
    CHECK(uavAfter[0] == omUav.Get());
    for (auto* p : rtvAfter)
        if (p) p->Release();
    if (uavAfter[0]) uavAfter[0]->Release();

    D3D11_BUFFER_DESC rd{};
    rd.ByteWidth = 16;
    rd.Usage = D3D11_USAGE_STAGING;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> countOut;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&rd, nullptr, &countOut)));
    ctx->CopyStructureCount(countOut.Get(), 0, omUav.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    REQUIRE(SUCCEEDED(ctx->Map(countOut.Get(), 0, D3D11_MAP_READ, 0, &m)));
    const uint32_t count = *static_cast<const uint32_t*>(m.pData);
    ctx->Unmap(countOut.Get(), 0);
    if (count != 5) std::printf("  output-merger UAV counter %u, expected 5\n", count);
    CHECK_EQ(count, 5u);

    CHECK(SameFloats(AsFloats(ReadBack11(dev, ctx, slots->Depth11(0), 4)), s.expected_depth));
    ctx->ClearState();
}

// ------------------------------------------------------------ lifetime

TEST(CaptureSlots_ViewCacheDropsStaleSources) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    Sources first = MakeCspSources(d, 16, 16, 16, 16);
    REQUIRE(first.depth && first.mvec);
    const ULONG baseRefs = RefCount(first.depth.Get());
    REQUIRE(slots->Recreate(0, first.depth.Get(), first.mvec.Get(), &err));
    REQUIRE(slots->Copy(d.ctx11.Get(), 0, first.depth.Get(), first.mvec.Get(), &err));
    CHECK_EQ(slots->CachedSourceViews(), size_t{1});
    CHECK(RefCount(first.depth.Get()) > baseRefs);  // the cached view holds the source

    // Many other depth sources of the same desc, one after another.
    for (int i = 0; i < 12; ++i) {
        Sources other = MakeCspSources(d, 16, 16, 16, 16);
        REQUIRE(other.depth);
        REQUIRE(slots->Copy(d.ctx11.Get(), 0, other.depth.Get(), first.mvec.Get(), &err));
        CHECK(slots->CachedSourceViews() <= 4);
    }
    d.ctx11->Flush();
    // The first source is no longer cached, so the cache no longer holds it.
    CHECK_EQ(RefCount(first.depth.Get()), baseRefs);

    // Recreate drops every cached view.
    REQUIRE(slots->Copy(d.ctx11.Get(), 0, first.depth.Get(), first.mvec.Get(), &err));
    CHECK(slots->CachedSourceViews() >= 1);
    REQUIRE(slots->Recreate(0, first.depth.Get(), first.mvec.Get(), &err));
    CHECK_EQ(slots->CachedSourceViews(), size_t{0});
    CHECK_EQ(RefCount(first.depth.Get()), baseRefs);
}

TEST(CaptureSlots_ViewCacheDropsASourceUnusedForAFewCopies) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
    REQUIRE(slots != nullptr);
    // After a resize CSP renders into a new depth; the old one must not stay
    // alive in the cache although the cache is far from full.
    Sources oldDepth = MakeCspSources(d, 16, 16, 16, 16);
    Sources newDepth = MakeCspSources(d, 16, 16, 16, 16);
    REQUIRE(oldDepth.depth && newDepth.depth && oldDepth.mvec);
    const ULONG baseRefs = RefCount(oldDepth.depth.Get());
    REQUIRE(slots->Recreate(0, oldDepth.depth.Get(), oldDepth.mvec.Get(), &err));
    REQUIRE(slots->Copy(d.ctx11.Get(), 0, oldDepth.depth.Get(), oldDepth.mvec.Get(), &err));
    for (int i = 0; i < 8; ++i)
        REQUIRE(slots->Copy(d.ctx11.Get(), 0, newDepth.depth.Get(), oldDepth.mvec.Get(), &err));
    CHECK_EQ(slots->CachedSourceViews(), size_t{1});
    d.ctx11->Flush();
    CHECK_EQ(RefCount(oldDepth.depth.Get()), baseRefs);
}

TEST(CaptureSlots_RecreateManyTimesReleasesEverything) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    Sources lowRes = MakeCspSources(d, 24, 16, 24, 16);
    Sources highRes = MakeCspSources(d, 48, 32, 40, 24);
    REQUIRE(lowRes.depth && lowRes.mvec && highRes.depth && highRes.mvec);
    d.ctx11->Flush();
    const ULONG dev11Refs = RefCount(d.device11.Get());
    const ULONG dev12Refs = RefCount(d.device12.Get());
    const ULONG depthRefs = RefCount(lowRes.depth.Get());
    const ULONG mvecRefs = RefCount(lowRes.mvec.Get());
    {
        std::string err;
        auto slots = CaptureSlots::Create(d.device11.Get(), d.device12.Get(), &err);
        REQUIRE(slots != nullptr);
        for (int i = 0; i < 60; ++i) {
            const uint32_t slot = static_cast<uint32_t>(i) % CaptureSlots::kSlots;
            Sources& s = (i / 3) % 2 ? highRes : lowRes;
            // The previous textures of this slot must be released by Recreate.
            ComPtr<ID3D12Resource> old12 = slots->Depth12(slot);
            ComPtr<ID3D12Resource> oldMv12 = slots->Mvec12(slot);
            ComPtr<ID3D11Texture2D> old11 = slots->Depth11(slot);
            ComPtr<ID3D11Texture2D> oldMv11 = slots->Mvec11(slot);
            REQUIRE(slots->Recreate(slot, s.depth.Get(), s.mvec.Get(), &err));
            REQUIRE(slots->Copy(d.ctx11.Get(), slot, s.depth.Get(), s.mvec.Get(), &err));
            if (old12) CHECK_EQ(RefCount(old12.Get()), 1u);
            if (oldMv12) CHECK_EQ(RefCount(oldMv12.Get()), 1u);
            if (old11) CHECK_EQ(RefCount(old11.Get()), 1u);
            if (oldMv11) CHECK_EQ(RefCount(oldMv11.Get()), 1u);
        }
        d.ctx11->Flush();
    }
    d.ctx11->Flush();
    CHECK_EQ(RefCount(lowRes.depth.Get()), depthRefs);
    CHECK_EQ(RefCount(lowRes.mvec.Get()), mvecRefs);
    CHECK_EQ(RefCount(d.device11.Get()), dev11Refs);
    CHECK_EQ(RefCount(d.device12.Get()), dev12Refs);
}
