#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "camera_channel.h"
#include "camera_layout.h"
#include "temp_dir.h"
#include "test_framework.h"

using namespace acdb;
using Result = CameraChannel::ReadResult;

namespace {

// One row per field of the Lua layout string, in order: the C type as the
// Lua ffi sees it, the element count (arrays) and the C++ offset.
struct FieldSpec {
    const char* name;
    const char* type;
    unsigned count;
    size_t offset;
};

const FieldSpec kFields[] = {
    {"magic", "uint32_t", 1, offsetof(CameraLayout, magic)},
    {"version", "uint32_t", 1, offsetof(CameraLayout, version)},
    {"seq", "uint32_t", 1, offsetof(CameraLayout, seq)},
    {"frame", "uint32_t", 1, offsetof(CameraLayout, frame)},
    {"pos", "float", 3, offsetof(CameraLayout, pos)},
    {"fwd", "float", 3, offsetof(CameraLayout, fwd)},
    {"up", "float", 3, offsetof(CameraLayout, up)},
    {"side", "float", 3, offsetof(CameraLayout, side)},
    {"fovVDeg", "float", 1, offsetof(CameraLayout, fovVDeg)},
    {"clipNear", "float", 1, offsetof(CameraLayout, clipNear)},
    {"clipFar", "float", 1, offsetof(CameraLayout, clipFar)},
    {"originShift", "float", 3, offsetof(CameraLayout, originShift)},
    {"renderW", "float", 1, offsetof(CameraLayout, renderW)},
    {"renderH", "float", 1, offsetof(CameraLayout, renderH)},
    {"flags", "uint32_t", 1, offsetof(CameraLayout, flags)},
    {"dt", "float", 1, offsetof(CameraLayout, dt)},
    {"simTimeMs", "double", 1, offsetof(CameraLayout, simTimeMs)},
};

// Size (and natural alignment) of the scalar C types the layout may use.
size_t ScalarSize(const std::string& type) {
    if (type == "uint32_t" || type == "float") return 4;
    if (type == "double") return 8;
    return 0;
}

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

}  // namespace

// The C++ struct must have exactly the offsets that natural C alignment gives
// the Lua layout string (LuaJIT's ffi.cdef lays the struct out the same way),
// so the table above is checked against a layout computed from its types.
TEST(CameraLayout_OffsetsFollowNaturalAlignmentOfTheLuaTypes) {
    size_t offset = 0;
    size_t max_align = 1;
    for (const auto& f : kFields) {
        const size_t size = ScalarSize(f.type);
        REQUIRE(size != 0);
        offset = AlignUp(offset, size);
        if (offset != f.offset) std::printf("  field %s: expected offset %zu, struct has %zu\n", f.name, offset, f.offset);
        CHECK_EQ(offset, f.offset);
        offset += size * f.count;
        if (size > max_align) max_align = size;
    }
    CHECK_EQ(AlignUp(offset, max_align), sizeof(CameraLayout));
    CHECK_EQ(max_align, alignof(CameraLayout));
}

// Compile-time values of the fixed interface (spec 6.6 flag bits).
static_assert(sizeof(CameraLayout) == 112);
static_assert(kCameraMagic == 0x47534C44u);
static_assert(kCameraVersion == 1u);
static_assert(kCamJumped == 1u && kCamPaused == 2u && kCamReplay == 4u && kCamVR == 8u && kCamTriple == 16u &&
              kCamMainMenu == 32u && kCamWriteFailed == 64u);

TEST(CameraLayout_MagicReadsDlsgInMemory) {
    const uint32_t magic = kCameraMagic;
    char tag[5] = {};
    std::memcpy(tag, &magic, 4);
    CHECK(std::strcmp(tag, "DLSG") == 0);
}

// ---------------------------------------------------------------------------
// The CSP Lua app (apps/lua/AcDlssg): its layout string, constants and write
// sequence are checked against the C++ side, so the two cannot drift apart.

namespace {

std::string ReadAppFile(const char* file) {
    return acdb_test::ReadAll(std::filesystem::path(ACDB_SOURCE_DIR) / "apps" / "lua" / "AcDlssg" / file);
}

// The body of `local LAYOUT = [[ ... ]]`, or empty.
std::string LayoutString(const std::string& lua) {
    const std::string open = "local LAYOUT = [[";
    const size_t begin = lua.find(open);
    if (begin == std::string::npos) return {};
    const size_t body = begin + open.size();
    const size_t end = lua.find("]]", body);
    if (end == std::string::npos) return {};
    return lua.substr(body, end - body);
}

struct Decl {
    std::string type;
    std::string name;
    unsigned count = 1;
};

// Splits a C struct body into declarations; an unparsable one comes back
// with an empty type and its text as the name.
std::vector<Decl> ParseDecls(const std::string& body) {
    static const std::regex decl(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*(\d+)\s*\])?\s*$)");
    std::vector<Decl> out;
    size_t start = 0;
    while (start < body.size()) {
        size_t semi = body.find(';', start);
        if (semi == std::string::npos) semi = body.size();
        const std::string text = body.substr(start, semi - start);
        start = semi + 1;
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::smatch m;
        Decl d;
        if (std::regex_match(text, m, decl)) {
            d.type = m[1];
            d.name = m[2];
            if (m[3].matched) d.count = static_cast<unsigned>(std::stoul(m[3]));
        } else {
            d.name = text;
        }
        out.push_back(d);
    }
    return out;
}

// The number assigned by `local <name> = <number>` (decimal or 0x hex), or -1.
long long LuaConstant(const std::string& lua, const std::string& name) {
    const std::regex re("local\\s+" + name + "\\s*=\\s*(0[xX][0-9A-Fa-f]+|\\d+)\\b");
    std::smatch m;
    if (!std::regex_search(lua, m, re)) return -1;
    return std::stoll(m[1].str(), nullptr, 0);
}

}  // namespace

TEST(LuaApp_LayoutStringMatchesTheStructFieldByField) {
    const std::string lua = ReadAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const std::string layout = LayoutString(lua);
    REQUIRE(!layout.empty());
    const std::vector<Decl> decls = ParseDecls(layout);
    CHECK_EQ(decls.size(), std::size(kFields));
    for (size_t i = 0; i < decls.size() && i < std::size(kFields); ++i) {
        const bool same = decls[i].type == kFields[i].type && decls[i].name == kFields[i].name &&
                          decls[i].count == kFields[i].count;
        if (!same)
            std::printf("  field %zu: Lua has '%s %s[%u]', C++ has '%s %s[%u]'\n", i, decls[i].type.c_str(),
                        decls[i].name.c_str(), decls[i].count, kFields[i].type, kFields[i].name, kFields[i].count);
        CHECK(same);
    }
}

TEST(LuaApp_ConstantsMatchTheCppSide) {
    const std::string lua = ReadAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    CHECK_EQ(LuaConstant(lua, "MAGIC"), static_cast<long long>(kCameraMagic));
    CHECK_EQ(LuaConstant(lua, "VERSION"), static_cast<long long>(kCameraVersion));
    CHECK_EQ(LuaConstant(lua, "FLAG_JUMPED"), static_cast<long long>(kCamJumped));
    CHECK_EQ(LuaConstant(lua, "FLAG_PAUSED"), static_cast<long long>(kCamPaused));
    CHECK_EQ(LuaConstant(lua, "FLAG_REPLAY"), static_cast<long long>(kCamReplay));
    CHECK_EQ(LuaConstant(lua, "FLAG_VR"), static_cast<long long>(kCamVR));
    CHECK_EQ(LuaConstant(lua, "FLAG_TRIPLE"), static_cast<long long>(kCamTriple));
    CHECK_EQ(LuaConstant(lua, "FLAG_MAIN_MENU"), static_cast<long long>(kCamMainMenu));
    CHECK_EQ(LuaConstant(lua, "FLAG_WRITE_FAILED"), static_cast<long long>(kCamWriteFailed));

    // ac.writeMemoryMappedFile takes the name without "Local\" (CSP SDK,
    // common/ac_extras_connectmmf.lua) and persists the mapping.
    std::smatch m;
    REQUIRE(std::regex_search(lua, m, std::regex(R"(local\s+SECTION_NAME\s*=\s*'([^']+)')")));
    const std::string name = m[1];
    const std::wstring full = L"Local\\" + std::wstring(name.begin(), name.end());
    CHECK(full == kCameraSectionName);
    CHECK(lua.find("ac.writeMemoryMappedFile(SECTION_NAME, LAYOUT, true)") != std::string::npos);
}

// publish() must keep the order the C++ reader relies on and that
// LuaStyleWrite below reproduces: seq odd, barrier, magic/version/frame,
// pcall(fill), write-failed flag on error, barrier, seq even.
TEST(LuaApp_PublishFollowsTheSeqlockWriterOrder) {
    const std::string lua = ReadAppFile("AcDlssg.lua");
    REQUIRE(!lua.empty());
    const size_t begin = lua.find("local function publish()");
    REQUIRE(begin != std::string::npos);
    const size_t end = lua.find("\nend", begin);
    REQUIRE(end != std::string::npos);
    const std::string body = lua.substr(begin, end - begin);
    const char* steps[] = {
        "local s = bit.band(bit.bor(mmf.seq, 1), 0x7FFFFFFF)",
        "mmf.seq = s",
        "memoryBarrier()",
        "mmf.magic = MAGIC",
        "mmf.version = VERSION",
        "mmf.frame = frameCounter",
        "pcall(fill)",
        "mmf.flags = bit.bor(mmf.flags, FLAG_WRITE_FAILED)",
        "memoryBarrier()",
        "mmf.seq = s + 1",
    };
    size_t at = 0;
    for (const char* step : steps) {
        const size_t found = body.find(step, at);
        if (found == std::string::npos) std::printf("  publish(): '%s' missing or out of order\n", step);
        CHECK(found != std::string::npos);
        if (found != std::string::npos) at = found + std::strlen(step);
    }
}

TEST(LuaApp_ManifestLoadsTheAppWithAC) {
    const std::string ini = ReadAppFile("manifest.ini");
    REQUIRE(!ini.empty());
    const size_t core = ini.find("[CORE]");
    REQUIRE(core != std::string::npos);
    CHECK(std::regex_search(ini.substr(core), std::regex(R"(\n\s*LAZY\s*=\s*NONE\b)")));
    CHECK(ini.find("[WINDOW_") != std::string::npos);
}

// ---------------------------------------------------------------------------
// CameraChannel

namespace {

// A section name of its own for every test, so that a test never touches the
// section of a game that runs at the same time.
std::wstring TestSectionName() {
    static std::atomic<unsigned> counter{0};
    return L"Local\\AcDlssg.Camera.test." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(counter++);
}

// CSP's side of the section: ac.writeMemoryMappedFile creates or opens the
// named section with PAGE_READWRITE and maps sizeof(layout) bytes for read and
// write (spec 4, "CSP Lua apps").
class CspSide {
public:
    explicit CspSide(const std::wstring& name, DWORD size = sizeof(CameraLayout)) {
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());
        if (handle_) mmf_ = static_cast<CameraLayout*>(MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(CameraLayout)));
    }
    ~CspSide() {
        if (mmf_) UnmapViewOfFile(mmf_);
        if (handle_) CloseHandle(handle_);
    }
    CspSide(const CspSide&) = delete;
    CspSide& operator=(const CspSide&) = delete;

    CameraLayout* mmf() const { return mmf_; }

private:
    HANDLE handle_ = nullptr;
    CameraLayout* mmf_ = nullptr;
};

template <class T>
T Load(T& field) {
    return std::atomic_ref<T>(field).load(std::memory_order_relaxed);
}
template <class T>
void Store(T& field, T value) {
    std::atomic_ref<T>(field).store(value, std::memory_order_relaxed);
}
// ac.memoryBarrier().
void Barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }

void StoreVec(float (&dst)[3], const float (&src)[3]) {
    for (int i = 0; i < 3; ++i) Store(dst[i], src[i]);
}

// publish() of apps/lua/AcDlssg/AcDlssg.lua, statement by statement. The
// fields of `values` other than magic, version, seq and frame are what fill()
// writes; with fill_fails, fill() raises an error after writing pos.
void LuaStyleWrite(CameraLayout* m, uint32_t* frame_counter, const CameraLayout& values, bool fill_fails = false) {
    const uint32_t s = (Load(m->seq) | 1u) & 0x7FFFFFFFu;  // bit.band(bit.bor(mmf.seq, 1), 0x7FFFFFFF)
    Store(m->seq, s);
    Barrier();
    *frame_counter = *frame_counter + 1;
    Store(m->magic, kCameraMagic);
    Store(m->version, kCameraVersion);
    Store(m->frame, *frame_counter);
    // pcall(fill)
    StoreVec(m->pos, values.pos);
    if (!fill_fails) {
        StoreVec(m->fwd, values.fwd);
        StoreVec(m->up, values.up);
        StoreVec(m->side, values.side);
        Store(m->fovVDeg, values.fovVDeg);
        Store(m->clipNear, values.clipNear);
        Store(m->clipFar, values.clipFar);
        StoreVec(m->originShift, values.originShift);
        Store(m->renderW, values.renderW);
        Store(m->renderH, values.renderH);
        Store(m->flags, values.flags);
        Store(m->dt, values.dt);
        Store(m->simTimeMs, values.simTimeMs);
    } else {
        Store(m->flags, Load(m->flags) | static_cast<uint32_t>(kCamWriteFailed));
    }
    Barrier();
    Store(m->seq, s + 1);
}

CameraLayout SampleValues() {
    CameraLayout v = {};
    const float pos[3] = {101.5f, 2.25f, -3003.0f};
    const float fwd[3] = {0.0f, 0.0f, 1.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float side[3] = {-1.0f, 0.0f, 0.0f};
    const float shift[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(v.pos, pos, sizeof(pos));
    std::memcpy(v.fwd, fwd, sizeof(fwd));
    std::memcpy(v.up, up, sizeof(up));
    std::memcpy(v.side, side, sizeof(side));
    std::memcpy(v.originShift, shift, sizeof(shift));
    v.fovVDeg = 56.0f;
    v.clipNear = 0.1f;
    v.clipFar = 5000.0f;
    v.renderW = 1707.0f;
    v.renderH = 960.0f;
    v.flags = kCamReplay;
    v.dt = 1.0f / 48.0f;
    v.simTimeMs = 123456.75;
    return v;
}

bool SameFields(const CameraLayout& a, const CameraLayout& b) {
    return a.magic == b.magic && a.version == b.version && a.seq == b.seq && a.frame == b.frame &&
           std::memcmp(a.pos, b.pos, sizeof(a.pos)) == 0 && std::memcmp(a.fwd, b.fwd, sizeof(a.fwd)) == 0 &&
           std::memcmp(a.up, b.up, sizeof(a.up)) == 0 && std::memcmp(a.side, b.side, sizeof(a.side)) == 0 &&
           a.fovVDeg == b.fovVDeg && a.clipNear == b.clipNear && a.clipFar == b.clipFar &&
           std::memcmp(a.originShift, b.originShift, sizeof(a.originShift)) == 0 && a.renderW == b.renderW &&
           a.renderH == b.renderH && a.flags == b.flags && a.dt == b.dt && a.simTimeMs == b.simTimeMs;
}

// NtQuerySection(SectionBasicInformation): the size the section was created with.
LONGLONG SectionSize(const std::wstring& name) {
    struct SectionBasicInformation {
        PVOID BaseAddress;
        ULONG AllocationAttributes;
        LARGE_INTEGER MaximumSize;
    };
    using NtQuerySectionFn = LONG(NTAPI*)(HANDLE, int, PVOID, SIZE_T, PSIZE_T);
    const auto query = reinterpret_cast<NtQuerySectionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySection")));
    if (!query) return -1;
    HANDLE h = OpenFileMappingW(SECTION_QUERY | FILE_MAP_READ, FALSE, name.c_str());
    if (!h) return -2;
    SectionBasicInformation info = {};
    const LONG status = query(h, 0, &info, sizeof(info), nullptr);
    CloseHandle(h);
    return status < 0 ? -3 : info.MaximumSize.QuadPart;
}

}  // namespace

TEST(CameraChannel_DefaultSectionIsLocalV1) {
    CHECK(CameraChannel::Get().SectionName() == L"Local\\AcDlssg.Camera.v1");
    CHECK(CameraChannel().SectionName() == L"Local\\AcDlssg.Camera.v1");
}

TEST(CameraChannel_ReadBeforeCreateIsNoSection) {
    CameraChannel ch(TestSectionName().c_str());
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::NoSection);
}

TEST(CameraChannel_CreateMakesA4096ByteSection) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CHECK(error.empty());
    CHECK_EQ(SectionSize(name), static_cast<LONGLONG>(kCameraSectionSize));
}

TEST(CameraChannel_CreateIsIdempotent) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::Ok);
}

// CSP may run the Lua app before the DLL's bootstrap: the DLL then opens the
// section CSP created, with CSP's size, and reads what the app wrote.
TEST(CameraChannel_CreateOpensTheSectionCspCreatedFirst) {
    const std::wstring name = TestSectionName();
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    const CameraLayout values = SampleValues();
    LuaStyleWrite(csp.mmf(), &frame, values);

    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CameraLayout out = {};
    REQUIRE(ch.Read(&out) == Result::Ok);
    CHECK_EQ(out.frame, 1u);
    CHECK_EQ(out.pos[2], values.pos[2]);
}

TEST(CameraChannel_CreateFailsWhenTheNameIsTakenByAnotherObjectType) {
    const std::wstring name = TestSectionName();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    REQUIRE(mutex != nullptr);
    CameraChannel ch(name.c_str());
    std::string error;
    CHECK(!ch.Create(&error));
    CHECK(error.find("CreateFileMappingW") != std::string::npos);
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::NoSection);
    CloseHandle(mutex);
}

TEST(CameraChannel_ZeroedSectionIsNotWritten) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CameraLayout out = {};
    out.frame = 77;
    CHECK(ch.Read(&out) == Result::NotWritten);
    CHECK_EQ(out.frame, 77u);  // untouched unless Ok
}

TEST(CameraChannel_CompleteWriteReadsBackEveryField) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 41;
    const CameraLayout values = SampleValues();
    LuaStyleWrite(csp.mmf(), &frame, values);

    CameraLayout expected = values;
    expected.magic = kCameraMagic;
    expected.version = kCameraVersion;
    expected.seq = 2;
    expected.frame = 42;
    CameraLayout out = {};
    REQUIRE(ch.Read(&out) == Result::Ok);
    CHECK(SameFields(out, expected));
}

TEST(CameraChannel_OddSeqIsTorn) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    // A writer stopped between its two seq stores (or still writing): the
    // reader gives up after its two attempts instead of waiting.
    csp.mmf()->seq = 3;
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::Torn);
}

TEST(CameraChannel_WriteFailedFlagIsReported) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    LuaStyleWrite(csp.mmf(), &frame, SampleValues(), /*fill_fails=*/true);
    // The failed write still leaves seq even: the record is stable, but invalid.
    CHECK_EQ(csp.mmf()->seq % 2, 0u);
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::WriteFailed);
    // The next good write clears the flag, because fill() rewrites flags.
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CHECK(ch.Read(&out) == Result::Ok);
    CHECK_EQ(out.frame, 3u);
    CHECK_EQ(out.flags & kCamWriteFailed, 0u);
}

TEST(CameraChannel_WrongMagicOrVersionIsNotWritten) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CameraLayout out = {};
    csp.mmf()->magic = kCameraMagic + 1;
    CHECK(ch.Read(&out) == Result::NotWritten);
    csp.mmf()->magic = kCameraMagic;
    csp.mmf()->version = kCameraVersion + 1;
    CHECK(ch.Read(&out) == Result::NotWritten);
    // A record of another layout says nothing about write failures either.
    csp.mmf()->flags = kCamWriteFailed;
    CHECK(ch.Read(&out) == Result::NotWritten);
}

// The writer forces parity instead of incrementing blindly (spec 6.6): a seq
// left odd by an interrupted write (an app reload mid-write) becomes even
// again after the next complete write, and seq stays at or below 2^31 so the
// Lua bit operations never produce negative numbers.
TEST(CameraChannel_WriterParityForcingRecoversAndWraps) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    uint32_t frame = 0;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    csp.mmf()->seq = 5;  // interrupted write
    CameraLayout out = {};
    CHECK(ch.Read(&out) == Result::Torn);
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CHECK_EQ(csp.mmf()->seq, 6u);
    CHECK(ch.Read(&out) == Result::Ok);

    csp.mmf()->seq = 0x7FFFFFFEu;
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CHECK_EQ(csp.mmf()->seq, 0x80000000u);
    CHECK(ch.Read(&out) == Result::Ok);
    LuaStyleWrite(csp.mmf(), &frame, SampleValues());
    CHECK_EQ(csp.mmf()->seq, 2u);
    CHECK(ch.Read(&out) == Result::Ok);
    CHECK_EQ(out.frame, 4u);
}

namespace {

// Racing writer for the seqlock tests: every field of write k is derived from
// k, so a snapshot mixing two writes is detectable.
constexpr uint32_t kFloatKeyMask = 0xFFFFF;  // exact in a float

CameraLayout RaceValues(uint32_t k) {
    CameraLayout v = {};
    const float f = static_cast<float>(k & kFloatKeyMask);
    for (int i = 0; i < 3; ++i) {
        v.pos[i] = f + static_cast<float>(i);
        v.fwd[i] = f + static_cast<float>(10 + i);
        v.up[i] = f + static_cast<float>(20 + i);
        v.side[i] = f + static_cast<float>(30 + i);
        v.originShift[i] = f + static_cast<float>(40 + i);
    }
    v.fovVDeg = f + 50.0f;
    v.clipNear = f + 51.0f;
    v.clipFar = f + 52.0f;
    v.renderW = f + 53.0f;
    v.renderH = f + 54.0f;
    v.flags = k & 0x1Fu;  // never kCamWriteFailed
    v.dt = f + 55.0f;
    v.simTimeMs = static_cast<double>(k);
    return v;
}

// True if every field of the snapshot belongs to the same write.
bool Consistent(const CameraLayout& s) {
    if (s.magic != kCameraMagic || s.version != kCameraVersion) return false;
    CameraLayout v = RaceValues(s.frame);
    v.magic = s.magic;
    v.version = s.version;
    v.seq = s.seq;
    v.frame = s.frame;
    return SameFields(s, v);
}

// Writes back to back, or with a pause of 0..max_pause-1 spins after each
// write, so that reads also land between writes and not only inside them.
class RaceWriter {
public:
    explicit RaceWriter(CameraLayout* mmf, unsigned max_pause = 0) : mmf_(mmf) {
        thread_ = std::thread([this, max_pause] {
            uint32_t frame = 0;
            uint32_t rng = 12345;
            while (!stop_.load(std::memory_order_relaxed)) {
                LuaStyleWrite(mmf_, &frame, RaceValues(frame + 1));
                writes_.store(frame, std::memory_order_relaxed);
                if (max_pause) {
                    rng = rng * 1664525u + 1013904223u;
                    for (unsigned i = (rng >> 8) % max_pause; i > 0; --i) YieldProcessor();
                }
            }
        });
        while (writes_.load(std::memory_order_relaxed) == 0) std::this_thread::yield();
    }
    ~RaceWriter() {
        stop_.store(true);
        thread_.join();
    }
    RaceWriter(const RaceWriter&) = delete;
    RaceWriter& operator=(const RaceWriter&) = delete;

    uint32_t Writes() const { return writes_.load(std::memory_order_relaxed); }

private:
    CameraLayout* mmf_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> writes_{0};
    std::thread thread_;
};

}  // namespace

// A writer thread doing exactly what the Lua app does races the reader for
// millions of reads: an Ok snapshot must never mix two writes.
TEST(CameraChannel_ConcurrentWriterNeverYieldsATornOkSnapshot) {
    const std::wstring name = TestSectionName();
    CameraChannel ch(name.c_str());
    std::string error;
    REQUIRE(ch.Create(&error));
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);

    constexpr int kReads = 3000000;
    int ok = 0, torn = 0, bad = 0, other = 0;
    {
        RaceWriter writer(csp.mmf(), /*max_pause=*/256);
        CameraLayout out = {};
        for (int i = 0; i < kReads; ++i) {
            const Result r = ch.Read(&out);
            if (r == Result::Ok) {
                ++ok;
                if (!Consistent(out)) ++bad;
            } else if (r == Result::Torn) {
                ++torn;
            } else {
                ++other;
            }
        }
        std::printf("  %d reads during %u writes: %d ok, %d torn, %d inconsistent ok, %d other\n", kReads,
                    writer.Writes(), ok, torn, bad, other);
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(other, 0);
    CHECK(ok > 0);
}

// The oracle above can see tearing: the same race read without the seqlock
// checks does produce mixed snapshots, so the zero above means something.
TEST(CameraChannel_RaceOracleDetectsAnUnprotectedCopy) {
    const std::wstring name = TestSectionName();
    CspSide csp(name);
    REQUIRE(csp.mmf() != nullptr);
    CspSide reader_view(name);
    REQUIRE(reader_view.mmf() != nullptr);

    int mixed = 0;
    long reads = 0;
    {
        RaceWriter writer(csp.mmf());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        CameraLayout* src = reader_view.mmf();
        while (mixed == 0 && std::chrono::steady_clock::now() < deadline) {
            for (int i = 0; i < 1000; ++i, ++reads) {
                CameraLayout s = {};
                s.magic = Load(src->magic);
                s.version = Load(src->version);
                s.seq = Load(src->seq);
                s.frame = Load(src->frame);
                for (int j = 0; j < 3; ++j) {
                    s.pos[j] = Load(src->pos[j]);
                    s.fwd[j] = Load(src->fwd[j]);
                    s.up[j] = Load(src->up[j]);
                    s.side[j] = Load(src->side[j]);
                    s.originShift[j] = Load(src->originShift[j]);
                }
                s.fovVDeg = Load(src->fovVDeg);
                s.clipNear = Load(src->clipNear);
                s.clipFar = Load(src->clipFar);
                s.renderW = Load(src->renderW);
                s.renderH = Load(src->renderH);
                s.flags = Load(src->flags);
                s.dt = Load(src->dt);
                s.simTimeMs = Load(src->simTimeMs);
                if (!Consistent(s)) ++mixed;
            }
        }
    }
    std::printf("  %d mixed snapshots in %ld unprotected reads\n", mixed, reads);
    CHECK(mixed > 0);
}

// ---------------------------------------------------------------------------
// CameraLatch

namespace {
CameraLayout WithFrame(uint32_t frame) {
    CameraLayout s = {};
    s.frame = frame;
    return s;
}
}  // namespace

TEST(CameraLatch_FirstLatchIsOnlyABaseline) {
    CameraLatch latch;
    CHECK(!latch.Latch(WithFrame(100)));
    CHECK(latch.Latch(WithFrame(101)));
}

TEST(CameraLatch_OnlyAGreaterFrameIsFresh) {
    CameraLatch latch;
    latch.Latch(WithFrame(10));
    CHECK(latch.Latch(WithFrame(11)));   // the app wrote during this frame
    CHECK(!latch.Latch(WithFrame(11)));  // it did not write again
    CHECK(latch.Latch(WithFrame(15)));   // frames without a capture in between
    CHECK(!latch.Latch(WithFrame(15)));
}

TEST(CameraLatch_LowerFrameIsStaleThenRebases) {
    CameraLatch latch;
    latch.Latch(WithFrame(500));
    CHECK(!latch.Latch(WithFrame(3)));  // a writer that restarted its counter
    CHECK(latch.Latch(WithFrame(4)));   // one frame later it counts again
}

TEST(CameraLatch_ResetStartsOver) {
    CameraLatch latch;
    latch.Latch(WithFrame(1));
    CHECK(latch.Latch(WithFrame(2)));
    latch.Reset();
    CHECK(!latch.Latch(WithFrame(3)));
    CHECK(latch.Latch(WithFrame(4)));
}

// Per-frame sequence: the app writes once per frame before the scene render,
// the bridge latches at the captured evaluate of the same frame.
TEST(CameraLatch_TypicalFrameSequence) {
    CameraLatch latch;
    uint32_t written = 0;
    int fresh = 0;
    for (int frame = 0; frame < 10; ++frame) {
        const bool app_wrote = frame != 4 && frame != 7;  // the app missed frames 4 and 7
        if (app_wrote) ++written;
        const bool is_fresh = latch.Latch(WithFrame(written));
        if (frame == 0 || !app_wrote) {
            CHECK(!is_fresh);
        } else {
            CHECK(is_fresh);
        }
        if (is_fresh) ++fresh;
    }
    CHECK_EQ(fresh, 7);
}
