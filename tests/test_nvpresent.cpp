// NVIDIA Smooth Motion detection (src/nvpresent.*, GitHub issue 10): on
// driver 621.01 NvPresent64.dll, the driver's own frame generation, created
// the game window's swap chain even when the NVIDIA App showed Smooth Motion
// off, and DLSS-G could not run on top of it.
#include <windows.h>

#include <string>

#include "nvpresent.h"
#include "test_framework.h"

using namespace acdb;

namespace {

using CallFn = int(__cdecl*)(int(__cdecl*)(void*), void*);

int __cdecl RecordNvPresentInStack(void* out) {
    *static_cast<int*>(out) = NvPresentInCallStack() ? 1 : 0;
    return 0;
}

}  // namespace

// A frame inside [base, base + size) counts; the end is outside.
TEST(NvPresent_StackHasModuleLooksForAFrameInTheModulesRange) {
    const ModuleRange m{0x10000, 0x2000};
    const void* inside[] = {reinterpret_cast<const void*>(0x500), reinterpret_cast<const void*>(0x11FFF)};
    const void* first[] = {reinterpret_cast<const void*>(0x10000)};
    const void* outside[] = {reinterpret_cast<const void*>(0xFFFF), reinterpret_cast<const void*>(0x12000),
                             reinterpret_cast<const void*>(0x7FF000000000), nullptr};
    CHECK(StackHasModule(inside, 2, m));
    CHECK(StackHasModule(first, 1, m));
    CHECK(!StackHasModule(outside, 4, m));
    // Only the first n frames count.
    CHECK(!StackHasModule(inside, 1, m));
    CHECK(!StackHasModule(inside, 0, m));
    CHECK(!StackHasModule(nullptr, 0, m));
    // A module that is not loaded has no range.
    CHECK(!StackHasModule(inside, 2, ModuleRange{}));
    CHECK(!StackHasModule(first, 1, ModuleRange{0x10000, 0}));
}

TEST(NvPresent_ItsWindowIsTheInvisibleWindowClass) {
    const std::wstring name = L"InvisibleWindowClassNvPresent";
    CHECK(IsNvPresentWindowClassName(name.c_str(), static_cast<int>(name.size())));
    const std::wstring longer = name + L"2";
    CHECK(!IsNvPresentWindowClassName(longer.c_str(), static_cast<int>(longer.size())));
    CHECK(!IsNvPresentWindowClassName(name.c_str(), static_cast<int>(name.size()) - 1));
    CHECK(!IsNvPresentWindowClassName(L"acsW", 4));
    CHECK(!IsNvPresentWindowClassName(nullptr, 0));
    CHECK(!IsNvPresentWindow(nullptr));
    CHECK(!IsNvPresentWindow(GetDesktopWindow()));
}

// The stack walk itself, with a stand-in module named NvPresent64.dll: a call
// that came through it sees it, a call that did not does not, and nothing is
// seen while no such module is loaded.
TEST(NvPresent_ACallThroughNvPresent64IsSeenInTheCallStack) {
    REQUIRE(GetModuleHandleW(L"NvPresent64.dll") == nullptr);  // the real one would make this test meaningless
    int seen = -1;
    RecordNvPresentInStack(&seen);
    CHECK_EQ(seen, 0);
    CHECK_EQ(NvPresentLoadedText(), std::string("NvPresent64.dll (NVIDIA Smooth Motion) is not loaded"));

    const HMODULE fake = LoadLibraryW(FAKE_NVPRESENT_PATH);
    REQUIRE(fake != nullptr);
    const auto call = reinterpret_cast<CallFn>(GetProcAddress(fake, "FakeNvPresentCall"));
    REQUIRE(call != nullptr);
    seen = -1;
    call(&RecordNvPresentInStack, &seen);
    CHECK_EQ(seen, 1);
    seen = -1;
    RecordNvPresentInStack(&seen);
    CHECK_EQ(seen, 0);
    const std::string loaded = NvPresentLoadedText();
    CHECK(loaded.rfind("NvPresent64.dll (NVIDIA Smooth Motion) is loaded from ", 0) == 0);
    CHECK(loaded.find("fake_np") != std::string::npos);
    FreeLibrary(fake);
    CHECK(GetModuleHandleW(L"NvPresent64.dll") == nullptr);
}
