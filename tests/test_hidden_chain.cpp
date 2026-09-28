#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gpu_test_devices.h"
#include "hidden_chain.h"
#include "internal_call.h"
#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace acdb;

namespace {

constexpr DXGI_USAGE kGameUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;

// CSP's main swap chain description (spec section 4), at the given size.
DXGI_SWAP_CHAIN_DESC1 GameDesc(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BufferUsage = kGameUsage;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return d;
}

// GPU tests skip, and say so, when no adapter works (gpu_test_devices.h).
bool GetDevices(acdb_test::GpuTestDevices* d) {
    if (acdb_test::CreateGpuTestDevices(d)) return true;
    std::printf("  SKIP: no adapter creates the test devices\n");
    return false;
}

std::unique_ptr<HiddenChain> CreateChain(const acdb_test::GpuTestDevices& d, UINT w, UINT h) {
    std::string err;
    auto chain = HiddenChain::Create(d.factory.Get(), d.device11.Get(), GameDesc(w, h), &err);
    if (!chain) std::printf("  HiddenChain::Create: %s\n", err.c_str());
    return chain;
}

// Top-level "acdb_hidden" windows of this process only: another process may
// be running these tests at the same time.
int CountHiddenWindows() {
    int count = 0;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            wchar_t cls[64] = {};
            if (pid == GetCurrentProcessId() && GetClassNameW(hwnd, cls, 64) > 0 &&
                std::wcscmp(cls, L"acdb_hidden") == 0)
                ++*reinterpret_cast<int*>(lp);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

D3D11_TEXTURE2D_DESC DescOf(ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    return desc;
}

HRESULT CreateRtv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT viewFormat, ID3D11RenderTargetView** out) {
    if (viewFormat == DXGI_FORMAT_UNKNOWN) return dev->CreateRenderTargetView(tex, nullptr, out);
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = viewFormat;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv.Texture2D.MipSlice = 0;
    return dev->CreateRenderTargetView(tex, &rtv, out);
}

// Copies tex into a staging texture and returns its RGBA8 texels, row by row.
std::vector<uint32_t> ReadTexels(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC desc = DescOf(tex);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<uint32_t> texels(static_cast<size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(&texels[static_cast<size_t>(y) * desc.Width],
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(desc.Width) * 4);
    ctx->Unmap(staging.Get(), 0);
    return texels;
}

// Number of texels whose R, G, B or A byte differs from expect by more than tol.
size_t CountMismatches(const std::vector<uint32_t>& texels, const int (&expect)[4], int tol) {
    size_t bad = 0;
    for (uint32_t t : texels) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(static_cast<int>((t >> (8 * c)) & 0xFF) - expect[c]) > tol) {
                ++bad;
                break;
            }
        }
    }
    return bad;
}

bool IsPumped(HWND hwnd) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

}  // namespace

TEST(HiddenChain_CreatesFlipDiscardChainFromGameDesc) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    auto hidden = HiddenChain::Create(d.factory.Get(), d.device11.Get(), GameDesc(1280, 720), &err);
    if (!hidden) std::printf("  error: %s\n", err.c_str());
    REQUIRE(hidden);
    CHECK(!IsInternalCall());
    REQUIRE(hidden->Buffer0() != nullptr);
    REQUIRE(hidden->Window() != nullptr);

    const D3D11_TEXTURE2D_DESC tex = DescOf(hidden->Buffer0());
    CHECK_EQ(tex.Width, 1280u);
    CHECK_EQ(tex.Height, 720u);
    CHECK_EQ(tex.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(tex.SampleDesc.Count, 1u);
    CHECK((tex.BindFlags & D3D11_BIND_RENDER_TARGET) != 0);
    CHECK((tex.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0);

    // A swap-chain buffer's parent is its swap chain.
    ComPtr<IDXGISurface> surface;
    REQUIRE(SUCCEEDED(hidden->Buffer0()->QueryInterface(IID_PPV_ARGS(&surface))));
    ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(surface->GetParent(IID_PPV_ARGS(&chain))));
    DXGI_SWAP_CHAIN_DESC1 cd{};
    REQUIRE(SUCCEEDED(chain->GetDesc1(&cd)));
    CHECK_EQ(cd.Width, 1280u);
    CHECK_EQ(cd.Height, 720u);
    CHECK_EQ(cd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(cd.BufferUsage & kGameUsage, kGameUsage);
    CHECK_EQ(cd.BufferCount, 2u);
    CHECK_EQ(cd.SwapEffect, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    CHECK_EQ(cd.Scaling, DXGI_SCALING_STRETCH);
    CHECK_EQ(cd.AlphaMode, DXGI_ALPHA_MODE_IGNORE);
    CHECK_EQ(cd.Flags, 0u);  // the game's 0x840 flags are not copied
    HWND chainHwnd = nullptr;
    CHECK(SUCCEEDED(chain->GetHwnd(&chainHwnd)));
    CHECK(chainHwnd == hidden->Window());
    ComPtr<ID3D11Device> chainDevice;
    CHECK(SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&chainDevice))));
    CHECK(chainDevice.Get() == d.device11.Get());
}

TEST(HiddenChain_Buffer0TakesUnormAndSrgbRenderTargetViews) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto hidden = CreateChain(d, 1280, 720);
    REQUIRE(hidden);

    // CSP's view (NULL description) and ReShade's two views.
    ComPtr<ID3D11RenderTargetView> csp, unorm, srgb;
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_UNKNOWN, &csp)));
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_R8G8B8A8_UNORM, &unorm)));
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &srgb)));
    CHECK(csp && unorm && srgb);

    // The premise of the hidden chain: a plain typed UNORM texture refuses
    // the sRGB view, and only a swap-chain buffer accepts both.
    D3D11_TEXTURE2D_DESC plainDesc{};
    plainDesc.Width = 1280;
    plainDesc.Height = 720;
    plainDesc.MipLevels = 1;
    plainDesc.ArraySize = 1;
    plainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    plainDesc.SampleDesc.Count = 1;
    plainDesc.Usage = D3D11_USAGE_DEFAULT;
    plainDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> plain;
    REQUIRE(SUCCEEDED(d.device11->CreateTexture2D(&plainDesc, nullptr, &plain)));
    ComPtr<ID3D11RenderTargetView> plainNull, plainSrgb;
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), plain.Get(), DXGI_FORMAT_UNKNOWN, &plainNull)));
    CHECK(FAILED(CreateRtv(d.device11.Get(), plain.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &plainSrgb)));
    CHECK(!plainSrgb);
}

TEST(HiddenChain_ClearedColourReadsBackThroughStaging) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto hidden = CreateChain(d, 1280, 720);
    REQUIRE(hidden);

    ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_UNKNOWN, &rtv)));
    const float colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    d.ctx11->ClearRenderTargetView(rtv.Get(), colour);
    std::vector<uint32_t> texels = ReadTexels(d.device11.Get(), d.ctx11.Get(), hidden->Buffer0());
    REQUIRE(texels.size() == 1280u * 720u);
    const int expectUnorm[4] = {51, 102, 153, 255};
    CHECK_EQ(CountMismatches(texels, expectUnorm, 1), 0u);

    // Through the sRGB view the same memory receives sRGB-encoded values:
    // linear 0.5 encodes to 0.7354, i.e. 188.
    ComPtr<ID3D11RenderTargetView> srgb;
    REQUIRE(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &srgb)));
    const float linear[4] = {0.5f, 0.0f, 1.0f, 1.0f};
    d.ctx11->ClearRenderTargetView(srgb.Get(), linear);
    texels = ReadTexels(d.device11.Get(), d.ctx11.Get(), hidden->Buffer0());
    REQUIRE(texels.size() == 1280u * 720u);
    const int expectSrgb[4] = {188, 0, 255, 255};
    CHECK_EQ(CountMismatches(texels, expectSrgb, 2), 0u);
}

TEST(HiddenChain_ResizeReplacesBuffer0) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto hidden = CreateChain(d, 1280, 720);
    REQUIRE(hidden);

    ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_UNKNOWN, &rtv)));
    // With a view still alive the resize fails, and the old buffer stays.
    CHECK(FAILED(hidden->Resize(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM)));
    REQUIRE(hidden->Buffer0() != nullptr);
    CHECK_EQ(DescOf(hidden->Buffer0()).Width, 1280u);

    rtv.Reset();
    d.ctx11->ClearState();
    d.ctx11->Flush();
    CHECK_EQ(hidden->Resize(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM), S_OK);
    REQUIRE(hidden->Buffer0() != nullptr);
    const D3D11_TEXTURE2D_DESC tex = DescOf(hidden->Buffer0());
    CHECK_EQ(tex.Width, 1920u);
    CHECK_EQ(tex.Height, 1080u);
    CHECK_EQ(tex.Format, DXGI_FORMAT_R8G8B8A8_UNORM);

    ComPtr<ID3D11RenderTargetView> csp, srgb;
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_UNKNOWN, &csp)));
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), hidden->Buffer0(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &srgb)));
    REQUIRE(csp);
    const float colour[4] = {1.0f, 0.0f, 0.2f, 1.0f};
    d.ctx11->ClearRenderTargetView(csp.Get(), colour);
    const std::vector<uint32_t> texels = ReadTexels(d.device11.Get(), d.ctx11.Get(), hidden->Buffer0());
    REQUIRE(texels.size() == 1920u * 1080u);
    const int expect[4] = {255, 0, 51, 255};
    CHECK_EQ(CountMismatches(texels, expect, 1), 0u);
    CHECK(!IsWindowVisible(hidden->Window()));
}

TEST(HiddenChain_WindowIsNeverShownAndIsPumpedByItsOwnThread) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto hidden = CreateChain(d, 1280, 720);
    REQUIRE(hidden);
    const HWND hwnd = hidden->Window();
    REQUIRE(IsWindow(hwnd));
    CHECK(!IsWindowVisible(hwnd));

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    CHECK((style & WS_POPUP) != 0);
    CHECK((style & WS_VISIBLE) == 0);
    CHECK((style & WS_CHILD) == 0);
    CHECK(GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow());
    wchar_t cls[64] = {};
    CHECK(GetClassNameW(hwnd, cls, 64) > 0);
    CHECK(std::wcscmp(cls, L"acdb_hidden") == 0);

    const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
    CHECK(tid != 0);
    CHECK(tid != GetCurrentThreadId());
    CHECK(IsPumped(hwnd));
    CHECK(!IsWindowVisible(hwnd));
}

TEST(HiddenChain_DestructionJoinsWindowThread) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    CHECK_EQ(CountHiddenWindows(), 0);
    // A second cycle proves the first left no thread, window or class behind.
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto hidden = CreateChain(d, 1280, 720);
        REQUIRE(hidden);
        const HWND hwnd = hidden->Window();
        const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
        REQUIRE(tid != 0);
        const HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, tid);
        REQUIRE(thread != nullptr);
        CHECK_EQ(CountHiddenWindows(), 1);
        CHECK_EQ(WaitForSingleObject(thread, 0), static_cast<DWORD>(WAIT_TIMEOUT));

        hidden.reset();
        CHECK_EQ(WaitForSingleObject(thread, 0), static_cast<DWORD>(WAIT_OBJECT_0));
        CHECK(!IsWindow(hwnd));
        CHECK_EQ(CountHiddenWindows(), 0);
        CloseHandle(thread);
    }
}

TEST(HiddenChain_SeveralChainsCoexist) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    auto a = CreateChain(d, 1280, 720);
    auto b = CreateChain(d, 640, 480);
    REQUIRE(a && b);
    CHECK(a->Window() != b->Window());
    CHECK(GetWindowThreadProcessId(a->Window(), nullptr) != GetWindowThreadProcessId(b->Window(), nullptr));
    CHECK_EQ(CountHiddenWindows(), 2);

    a.reset();
    CHECK(IsWindow(b->Window()));
    CHECK(IsPumped(b->Window()));
    auto c = CreateChain(d, 800, 600);
    REQUIRE(c);
    CHECK_EQ(CountHiddenWindows(), 2);
    ComPtr<ID3D11RenderTargetView> srgb;
    CHECK(SUCCEEDED(CreateRtv(d.device11.Get(), c->Buffer0(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &srgb)));
    srgb.Reset();
    b.reset();
    c.reset();
    CHECK_EQ(CountHiddenWindows(), 0);
}

TEST(HiddenChain_FailuresReportAndCleanUp) {
    acdb_test::GpuTestDevices d;
    if (!GetDevices(&d)) return;
    std::string err;
    CHECK(!HiddenChain::Create(nullptr, d.device11.Get(), GameDesc(1280, 720), &err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!HiddenChain::Create(d.factory.Get(), nullptr, GameDesc(1280, 720), &err));
    CHECK(!err.empty());

    // Flip-model chains reject sRGB buffer formats: the window thread must
    // still be shut down.
    err.clear();
    CHECK(!HiddenChain::Create(d.factory.Get(), d.device11.Get(),
                               GameDesc(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), &err));
    CHECK(err.find("CreateSwapChainForHwnd") != std::string::npos);
    CHECK_EQ(CountHiddenWindows(), 0);
    CHECK(!IsInternalCall());

    // A null error pointer is allowed.
    CHECK(!HiddenChain::Create(d.factory.Get(), d.device11.Get(),
                               GameDesc(1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), nullptr));
    CHECK_EQ(CountHiddenWindows(), 0);
}
