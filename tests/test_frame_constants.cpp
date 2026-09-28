// FrameConstants: sl::Constants from the CSP camera snapshots (spec 6.7).
// Expected values are computed here independently of frame_constants.cpp:
// closed forms where they exist, otherwise straight dot products in double.
#include <sl_consts.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "camera_layout.h"
#include "frame_constants.h"
#include "test_framework.h"

using namespace acdb;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

struct V3 {
    double x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 a) { return Scale(a, 1.0 / std::sqrt(Dot(a, a))); }
V3 FromSl(const sl::float3& v) { return {v.x, v.y, v.z}; }
V3 FromArray(const float (&v)[3]) { return {v[0], v[1], v[2]}; }
void ToArray(V3 v, float (&out)[3]) {
    out[0] = static_cast<float>(v.x);
    out[1] = static_cast<float>(v.y);
    out[2] = static_cast<float>(v.z);
}
double Det(V3 r, V3 u, V3 f) { return Dot(r, Cross(u, f)); }

struct V4 {
    double x, y, z, w;
};
struct M4 {
    double m[4][4];
};
M4 Identity() {
    M4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
}
M4 FromSl(const sl::float4x4& a) {
    M4 r{};
    for (uint32_t i = 0; i < 4; ++i) {
        r.m[i][0] = a[i].x;
        r.m[i][1] = a[i].y;
        r.m[i][2] = a[i].z;
        r.m[i][3] = a[i].w;
    }
    return r;
}
M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
// Row vector times matrix.
V4 Mul(V4 v, const M4& a) {
    const double in[4] = {v.x, v.y, v.z, v.w};
    double o[4] = {};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) o[j] += in[k] * a.m[k][j];
    return {o[0], o[1], o[2], o[3]};
}

// Reports the largest difference when two matrices differ by more than tol.
bool MatNear(const M4& a, const M4& b, double tol, const char* what) {
    double worst = 0;
    int wi = 0, wj = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const double d = std::fabs(a.m[i][j] - b.m[i][j]);
            if (!(d <= worst)) {
                worst = d;
                wi = i;
                wj = j;
            }
        }
    if (worst <= tol) return true;
    std::printf("  %s: [%d][%d] is %.9g, expected %.9g\n", what, wi, wj, a.m[wi][wj], b.m[wi][wj]);
    return false;
}
bool VecNear(V3 a, V3 b, double tol) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol;
}

// The standard non-reversed D3D perspective (row vectors) and its inverse.
M4 Perspective(double fovYRad, double aspect, double n, double f) {
    const double ys = 1.0 / std::tan(fovYRad / 2), xs = ys / aspect, a = f / (f - n);
    M4 r{};
    r.m[0][0] = xs;
    r.m[1][1] = ys;
    r.m[2][2] = a;
    r.m[2][3] = 1;
    r.m[3][2] = -n * a;
    return r;
}
M4 PerspectiveInverse(double fovYRad, double aspect, double n, double f) {
    const double ys = 1.0 / std::tan(fovYRad / 2), xs = ys / aspect;
    M4 r{};
    r.m[0][0] = 1 / xs;
    r.m[1][1] = 1 / ys;
    r.m[2][3] = -(f - n) / (n * f);
    r.m[3][2] = 1;
    r.m[3][3] = 1 / n;
    return r;
}

// A CSP-like snapshot: side = cross(fwd, up), which is the screen's right in
// AC's right-handed world. fwd and up are stored as given.
CameraLayout MakeCamera(V3 pos, V3 fwd, V3 up, float fovDeg = 60, float n = 0.1f, float f = 1000,
                        float w = 1920, float h = 1080) {
    CameraLayout c{};
    c.magic = kCameraMagic;
    c.version = kCameraVersion;
    c.seq = 2;
    c.frame = 100;
    ToArray(pos, c.pos);
    ToArray(fwd, c.fwd);
    ToArray(up, c.up);
    ToArray(Normalize(Cross(fwd, up)), c.side);
    c.fovVDeg = fovDeg;
    c.clipNear = n;
    c.clipFar = f;
    c.renderW = w;
    c.renderH = h;
    c.dt = 1.0f / 60;
    c.simTimeMs = 12345.0;
    return c;
}

// Looking along +z with y up; side = (-1,0,0) is the screen's right.
CameraLayout AxisCamera(V3 pos, float fovDeg, float n, float f, float w, float h) {
    return MakeCamera(pos, {0, 0, 1}, {0, 1, 0}, fovDeg, n, f, w, h);
}

// Turned by angle (radians) towards the screen's right from AxisCamera:
// fwd = cos * (0,0,1) + sin * (-1,0,0).
CameraLayout YawedCamera(V3 pos, double angle, float fovDeg, float n, float f, float w, float h) {
    return MakeCamera(pos, {-std::sin(angle), 0, std::cos(angle)}, {0, 1, 0}, fovDeg, n, f, w, h);
}

CaptureParams CspCapture() {
    CaptureParams p;
    p.jitterX = 0.25f;
    p.jitterY = -0.125f;
    p.mvScaleX = -1920;
    p.mvScaleY = -1080;
    p.renderW = 1920;
    p.renderH = 1080;
    p.createFlags = dlss_create_flags::kMVLowRes;  // what CSP passes
    p.ngxReset = false;
    return p;
}

ConstantsInput Input(const CameraLayout* cur, const CameraLayout* prev) {
    ConstantsInput in;
    in.cur = cur;
    in.prev = prev;
    in.capture = CspCapture();
    in.prevFrameHadInputs = true;
    return in;
}

// Clip-space position of a world point, computed with dot products: view x
// along the screen's right (cross(fwd, up) in AC's world), y along up
// (orthogonalized), z along fwd.
V4 WorldToClip(const CameraLayout& c, V3 p) {
    const V3 f = Normalize(FromArray(c.fwd));
    const V3 upIn = FromArray(c.up);
    const V3 u = Normalize(Sub(upIn, Scale(f, Dot(upIn, f))));
    const V3 r = Cross(f, u);
    const V3 d = Sub(p, FromArray(c.pos));
    const V4 view = {Dot(d, r), Dot(d, u), Dot(d, f), 1};
    return Mul(view, Perspective(c.fovVDeg * kPi / 180, static_cast<double>(c.renderW) / c.renderH, c.clipNear,
                                 c.clipFar));
}

bool NdcNear(V4 a, V4 b, double tol) {
    return std::fabs(a.x / a.w - b.x / b.w) <= tol && std::fabs(a.y / a.w - b.y / b.w) <= tol &&
           std::fabs(a.z / a.w - b.z / b.w) <= tol;
}

bool Build(const ConstantsInput& in, sl::Constants* out) {
    std::string why;
    const bool ok = BuildFrameConstants(in, out, &why);
    if (!ok) std::printf("  BuildFrameConstants failed: %s\n", why.c_str());
    return ok;
}

// Expects a refusal whose reason contains keyword, with *out untouched.
bool Refuses(const ConstantsInput& in, const char* keyword) {
    sl::Constants out;
    out.cameraNear = 123.0f;
    std::string why;
    if (BuildFrameConstants(in, &out, &why)) {
        std::printf("  accepted input that should fail with '%s'\n", keyword);
        return false;
    }
    if (why.find(keyword) == std::string::npos) {
        std::printf("  reason '%s' does not mention '%s'\n", why.c_str(), keyword);
        return false;
    }
    if (out.cameraNear != 123.0f) {
        std::printf("  *out was written on failure\n");
        return false;
    }
    return true;
}

}  // namespace

TEST(FC_AxisCameraGivesTheExpectedMatrices) {
    // fov 90, aspect 2, near 1, far 101: xs = 0.5, ys = 1, f/(f-n) = 1.01.
    const CameraLayout cam = AxisCamera({0, 0, 0}, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    REQUIRE(Build(in, &c));

    M4 viewToClip{};
    viewToClip.m[0][0] = 0.5;
    viewToClip.m[1][1] = 1;
    viewToClip.m[2][2] = 1.01;
    viewToClip.m[2][3] = 1;
    viewToClip.m[3][2] = -1.01;
    CHECK(MatNear(FromSl(c.cameraViewToClip), viewToClip, 1e-6, "cameraViewToClip"));

    M4 clipToView{};
    clipToView.m[0][0] = 2;
    clipToView.m[1][1] = 1;
    clipToView.m[2][3] = -100.0 / 101.0;
    clipToView.m[3][2] = 1;
    clipToView.m[3][3] = 1;
    CHECK(MatNear(FromSl(c.clipToCameraView), clipToView, 1e-6, "clipToCameraView"));

    // The same camera twice: no motion.
    CHECK(MatNear(FromSl(c.clipToPrevClip), Identity(), 1e-6, "clipToPrevClip"));
    CHECK(MatNear(FromSl(c.prevClipToClip), Identity(), 1e-6, "prevClipToClip"));

    CHECK(VecNear(FromSl(c.cameraPos), {0, 0, 0}, 0));
    CHECK(VecNear(FromSl(c.cameraFwd), {0, 0, 1}, 1e-7));
    CHECK(VecNear(FromSl(c.cameraUp), {0, 1, 0}, 1e-7));
    CHECK(VecNear(FromSl(c.cameraRight), {-1, 0, 0}, 1e-7));  // side as given
    CHECK_EQ(c.cameraNear, 1.0f);
    CHECK_EQ(c.cameraFar, 101.0f);
    CHECK(std::fabs(c.cameraFOV - kPi / 2) < 1e-7);
    CHECK_EQ(c.cameraAspectRatio, 2.0f);
}

TEST(FC_ClipToCameraViewIsTheInverseOfCameraViewToClip) {
    const CameraLayout cam = AxisCamera({10, 2, -30}, 47.5f, 0.05f, 25000, 2560, 1440);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    CHECK(MatNear(Mul(FromSl(c.cameraViewToClip), FromSl(c.clipToCameraView)), Identity(), 1e-5, "P*Pinv"));
    CHECK(MatNear(Mul(FromSl(c.clipToCameraView), FromSl(c.cameraViewToClip)), Identity(), 1e-5, "Pinv*P"));
}

TEST(FC_EveryRequiredFieldIsSetAndEveryBooleanIsExplicit) {
    const CameraLayout cam = AxisCamera({1, 2, 3}, 60, 0.1f, 1000, 1920, 1080);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    REQUIRE(Build(in, &c));

    // What Streamline's validateCommonConstants (sl.common) warns about.
    for (const sl::float4x4* m : {&c.cameraViewToClip, &c.clipToCameraView, &c.clipToPrevClip, &c.prevClipToClip})
        for (uint32_t i = 0; i < 4; ++i) {
            const sl::float4& r = (*m)[i];
            CHECK(r.x != sl::INVALID_FLOAT && r.y != sl::INVALID_FLOAT && r.z != sl::INVALID_FLOAT &&
                  r.w != sl::INVALID_FLOAT);
        }
    for (const sl::float2* v : {&c.jitterOffset, &c.mvecScale, &c.cameraPinholeOffset})
        CHECK(v->x != sl::INVALID_FLOAT && v->y != sl::INVALID_FLOAT);
    for (const sl::float3* v : {&c.cameraPos, &c.cameraUp, &c.cameraRight, &c.cameraFwd})
        CHECK(v->x != sl::INVALID_FLOAT && v->y != sl::INVALID_FLOAT && v->z != sl::INVALID_FLOAT);
    for (float v : {c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio}) CHECK(v != sl::INVALID_FLOAT);

    CHECK_EQ(c.depthInverted, sl::Boolean::eFalse);
    CHECK_EQ(c.cameraMotionIncluded, sl::Boolean::eTrue);
    CHECK_EQ(c.motionVectors3D, sl::Boolean::eFalse);
    CHECK_EQ(c.reset, sl::Boolean::eFalse);
    CHECK_EQ(c.orthographicProjection, sl::Boolean::eFalse);
    CHECK_EQ(c.motionVectorsDilated, sl::Boolean::eFalse);
    CHECK_EQ(c.motionVectorsJittered, sl::Boolean::eFalse);

    CHECK_EQ(c.cameraPinholeOffset.x, 0.0f);
    CHECK_EQ(c.cameraPinholeOffset.y, 0.0f);
    // Left at Streamline's "not provided" defaults on purpose.
    CHECK_EQ(c.clipToLensClip[0].x, sl::INVALID_FLOAT);
    CHECK_EQ(c.motionVectorsInvalidValue, sl::INVALID_FLOAT);
    CHECK_EQ(c.minRelativeLinearDepthObjectSeparation, 40.0f);
}

TEST(FC_FovIsVerticalAndInRadians) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    CHECK(std::fabs(c.cameraFOV - kPi / 3) < 1e-6);
    // Vertical: ys = 1/tan(30 deg); horizontal is ys / aspect.
    CHECK(std::fabs(c.cameraViewToClip[1].y - 1.0 / std::tan(kPi / 6)) < 1e-5);
    CHECK(std::fabs(c.cameraViewToClip[0].x - (1.0 / std::tan(kPi / 6)) * 1080.0 / 1920.0) < 1e-5);
    CHECK(std::fabs(c.cameraAspectRatio - 1920.0 / 1080.0) < 1e-6);
}

TEST(FC_ClipToPrevClipRoundTrips) {
    // Rotation about a skewed axis, translation far from the origin, and a
    // change of FOV, aspect and clip planes between the frames.
    const CameraLayout prev = MakeCamera({4000, 35, -2500}, {0.3, -0.1, 0.9}, {0.05, 1, 0.1}, 55, 0.2f, 8000, 1707,
                                         960);
    const CameraLayout cur = MakeCamera({4000.75, 35.1, -2499.5}, {0.25, -0.05, 0.95}, {-0.05, 1, 0.02}, 58, 0.15f,
                                        9000, 1600, 900);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    CHECK(MatNear(Mul(FromSl(c.prevClipToClip), FromSl(c.clipToPrevClip)), Identity(), 1e-5, "prev*cur"));
    CHECK(MatNear(Mul(FromSl(c.clipToPrevClip), FromSl(c.prevClipToClip)), Identity(), 1e-5, "cur*prev"));
}

TEST(FC_SidewaysTranslationGivesTheClosedForm) {
    // The camera moved d along its right (world -x) between the frames, far
    // from the origin. In view space v_prev = v_cur + (d,0,0), so
    // clipToPrevClip = I except [2][0] = -d*xs*(f-n)/(n*f) and [3][0] = d*xs/n.
    const double d = 2, n = 1, f = 101, xs = 0.5;
    const CameraLayout prev = AxisCamera({4000, 30, -2500}, 90, 1, 101, 200, 100);
    const CameraLayout cur = AxisCamera({4000 - d, 30, -2500}, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    M4 expected = Identity();
    expected.m[2][0] = -d * xs * (f - n) / (n * f);
    expected.m[3][0] = d * xs / n;
    CHECK(MatNear(FromSl(c.clipToPrevClip), expected, 1e-5, "clipToPrevClip"));
}

TEST(FC_ForwardTranslationGivesTheClosedForm) {
    // The camera moved d forward (+z): v_prev = v_cur + (0,0,d), so
    // clipToPrevClip = I except [2][2] = 1-d/n, [3][2] = d*f/(n*(f-n)),
    // [2][3] = -d*(f-n)/(n*f), [3][3] = 1+d/n.
    const double d = 0.5, n = 1, f = 101;
    const CameraLayout prev = AxisCamera({4000, 30, -2500}, 90, 1, 101, 200, 100);
    const CameraLayout cur = AxisCamera({4000, 30, -2500 + d}, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    M4 expected = Identity();
    expected.m[2][2] = 1 - d / n;
    expected.m[3][2] = d * f / (n * (f - n));
    expected.m[2][3] = -d * (f - n) / (n * f);
    expected.m[3][3] = 1 + d / n;
    CHECK(MatNear(FromSl(c.clipToPrevClip), expected, 1e-5, "clipToPrevClip"));
}

TEST(FC_YawGivesTheExpectedClipToPrevClip) {
    // The camera turned by t towards its right. In view space
    // v_prev = v_cur * Ry with rows (c,0,-s), (0,1,0), (s,0,c), so
    // clipToPrevClip = Pinv * Ry * P.
    const double t = 10 * kPi / 180, cs = std::cos(t), sn = std::sin(t);
    const CameraLayout prev = AxisCamera({12, 1.5, -40}, 90, 1, 101, 200, 100);
    const CameraLayout cur = YawedCamera({12, 1.5, -40}, t, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants c;
    REQUIRE(Build(in, &c));

    M4 ry = Identity();
    ry.m[0][0] = cs;
    ry.m[0][2] = -sn;
    ry.m[2][0] = sn;
    ry.m[2][2] = cs;
    const M4 p = Perspective(kPi / 2, 2, 1, 101);
    const M4 pinv = PerspectiveInverse(kPi / 2, 2, 1, 101);
    CHECK(MatNear(FromSl(c.clipToPrevClip), Mul(Mul(pinv, ry), p), 1e-5, "clipToPrevClip"));

    // What is in the centre now was to the right before: ndc x = xs*tan(t).
    const V4 centre = Mul(V4{0, 0, 10, 1}, p);
    const V4 before = Mul(centre, FromSl(c.clipToPrevClip));
    CHECK(std::fabs(before.x / before.w - 0.5 * std::tan(t)) < 1e-6);
    CHECK(std::fabs(before.y / before.w) < 1e-6);
}

TEST(FC_ProjectedPointsMapThroughClipToPrevClip) {
    const CameraLayout prev = MakeCamera({-3200, 120, 4100}, {0.7, -0.2, -0.6}, {0.1, 1, 0.05}, 50, 0.1f, 5000, 1707,
                                         960);
    const CameraLayout cur = MakeCamera({-3199.25, 120.05, 4099.5}, {0.72, -0.18, -0.58}, {0.12, 1, 0.03}, 52, 0.1f,
                                        5000, 1707, 960);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    const M4 toPrev = FromSl(c.clipToPrevClip), toCur = FromSl(c.prevClipToClip);
    const V3 curFwd = Normalize(FromArray(cur.fwd));
    const V3 curRight = Normalize(Cross(FromArray(cur.fwd), FromArray(cur.up)));
    const V3 curUp = Normalize(FromArray(cur.up));
    int bad = 0;
    for (double depth : {0.5, 3.0, 20.0, 150.0, 1200.0})
        for (double sx : {-0.4, 0.0, 0.3})
            for (double sy : {-0.2, 0.0, 0.25}) {
                const V3 p = Add(FromArray(cur.pos), Add(Scale(curFwd, depth),
                                                         Add(Scale(curRight, sx * depth), Scale(curUp, sy * depth))));
                const V4 cc = WorldToClip(cur, p), cp = WorldToClip(prev, p);
                if (!NdcNear(Mul(cc, toPrev), cp, 1e-4) || !NdcNear(Mul(cp, toCur), cc, 1e-4)) {
                    ++bad;
                    std::printf("  point at depth %g, (%g, %g) does not map\n", depth, sx, sy);
                }
            }
    CHECK_EQ(bad, 0);
}

TEST(FC_OrthonormalizesASkewedBasisKeepingFwd) {
    CameraLayout cam = AxisCamera({5, 6, 7}, 60, 0.1f, 1000, 1920, 1080);
    const V3 fwdIn = {0.02, -0.01, 2.0}, upIn = {0.1, 1.2, 0.05}, sideIn = {-0.9, 0.05, -0.03};
    ToArray(fwdIn, cam.fwd);
    ToArray(upIn, cam.up);
    ToArray(sideIn, cam.side);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    const V3 r = FromSl(c.cameraRight), u = FromSl(c.cameraUp), f = FromSl(c.cameraFwd);

    // Gram-Schmidt keeping fwd: fwd only normalized, up made orthogonal to fwd.
    const V3 fIn = Normalize(FromArray(cam.fwd));
    const V3 uExp = Normalize(Sub(FromArray(cam.up), Scale(fIn, Dot(FromArray(cam.up), fIn))));
    CHECK(VecNear(f, fIn, 1e-6));
    CHECK(VecNear(u, uExp, 1e-6));
    CHECK(std::fabs(Dot(r, r) - 1) < 1e-6 && std::fabs(Dot(u, u) - 1) < 1e-6 && std::fabs(Dot(f, f) - 1) < 1e-6);
    CHECK(std::fabs(Dot(r, u)) < 1e-6 && std::fabs(Dot(r, f)) < 1e-6 && std::fabs(Dot(u, f)) < 1e-6);
    // The signs of the inputs survive: right is where side points.
    CHECK(Dot(r, FromArray(cam.side)) > 0.99 * std::sqrt(Dot(FromArray(cam.side), FromArray(cam.side))));
    CHECK(Dot(u, FromArray(cam.up)) > 0);
    // AC's world is right-handed: right = fwd x up, determinant -1.
    CHECK(std::fabs(Det(r, u, f) + 1) < 1e-6);
    CHECK(VecNear(r, Cross(f, u), 1e-6));
}

TEST(FC_ResetRules) {
    const CameraLayout prev = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    CameraLayout cur = AxisCamera({0, 0, 0.5}, 60, 0.1f, 1000, 1920, 1080);
    sl::Constants c;

    ConstantsInput in = Input(&cur, &prev);
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eFalse);

    in.capture.ngxReset = true;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eTrue);

    in = Input(&cur, &prev);
    in.prevFrameHadInputs = false;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eTrue);

    // Only the current frame's jump counts.
    CameraLayout jumpedPrev = prev;
    jumpedPrev.flags = kCamJumped;
    in = Input(&cur, &jumpedPrev);
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eFalse);

    cur.flags = kCamJumped;
    in = Input(&cur, &prev);
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eTrue);

    // Other flags do not reset (the presenter handles paused and menus).
    cur.flags = kCamPaused | kCamReplay | kCamMainMenu;
    in = Input(&cur, &prev);
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eFalse);
}

TEST(FC_NoPreviousCameraResetsAndUsesTheCurrentOne) {
    const CameraLayout cur = AxisCamera({100, 5, 100}, 60, 0.1f, 1000, 1920, 1080);
    ConstantsInput in = Input(&cur, nullptr);
    sl::Constants c;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.reset, sl::Boolean::eTrue);
    CHECK(MatNear(FromSl(c.clipToPrevClip), Identity(), 1e-6, "clipToPrevClip"));
    CHECK(MatNear(FromSl(c.prevClipToClip), Identity(), 1e-6, "prevClipToClip"));
}

TEST(FC_MotionVectorScaleAndJitter) {
    // CSP: MV.Scale = -(render size) gives {-1,-1}.
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1707, 960);
    ConstantsInput in = Input(&cam, &cam);
    in.capture.mvScaleX = -1707;
    in.capture.mvScaleY = -960;
    in.capture.renderW = 1707;
    in.capture.renderH = 960;
    sl::Constants c;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.mvecScale.x, -1.0f);
    CHECK_EQ(c.mvecScale.y, -1.0f);
    CHECK_EQ(c.jitterOffset.x, 0.25f);   // NGX pixels, passed through
    CHECK_EQ(c.jitterOffset.y, -0.125f);

    in.capture.mvScaleX = 2;
    in.capture.mvScaleY = 4;
    in.capture.renderW = 4;
    in.capture.renderH = 16;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.mvecScale.x, 0.5f);
    CHECK_EQ(c.mvecScale.y, 0.25f);
}

TEST(FC_DepthAndJitterFlagsComeFromTheCreateFlags) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    ConstantsInput in = Input(&cam, &cam);
    sl::Constants c;
    in.capture.createFlags = dlss_create_flags::kMVLowRes;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.depthInverted, sl::Boolean::eFalse);
    CHECK_EQ(c.motionVectorsJittered, sl::Boolean::eFalse);

    in.capture.createFlags = dlss_create_flags::kMVLowRes | dlss_create_flags::kDepthInverted;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.depthInverted, sl::Boolean::eTrue);
    CHECK_EQ(c.motionVectorsJittered, sl::Boolean::eFalse);

    in.capture.createFlags = dlss_create_flags::kMVJittered;
    REQUIRE(Build(in, &c));
    CHECK_EQ(c.depthInverted, sl::Boolean::eFalse);
    CHECK_EQ(c.motionVectorsJittered, sl::Boolean::eTrue);

}

// NVSDK_NGX_DLSS_Feature_Flags in the DLSS SDK's nvsdk_ngx_defs.h.
static_assert(dlss_create_flags::kIsHDR == 1u);
static_assert(dlss_create_flags::kMVLowRes == 2u);
static_assert(dlss_create_flags::kMVJittered == 4u);
static_assert(dlss_create_flags::kDepthInverted == 8u);

TEST(FC_FlipHandednessMirrorsTheView) {
    const double t = 10 * kPi / 180;
    const CameraLayout prev = AxisCamera({12, 1.5, -40}, 90, 1, 101, 200, 100);
    const CameraLayout yawed = YawedCamera({12, 1.5, -40}, t, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&yawed, &prev);
    in.options.flipHandedness = true;
    sl::Constants c;
    REQUIRE(Build(in, &c));
    // cameraRight is -side, and the yaw now reads as a turn to the left.
    CHECK(VecNear(FromSl(c.cameraRight), Scale(FromArray(yawed.side), -1), 1e-6));
    CHECK(std::fabs(Det(FromSl(c.cameraRight), FromSl(c.cameraUp), FromSl(c.cameraFwd)) - 1) < 1e-6);
    const M4 p = Perspective(kPi / 2, 2, 1, 101);
    const V4 before = Mul(Mul(V4{0, 0, 10, 1}, p), FromSl(c.clipToPrevClip));
    CHECK(std::fabs(before.x / before.w + 0.5 * std::tan(t)) < 1e-6);

    // Motion along the view axis is the same either way.
    const CameraLayout ahead = AxisCamera({12, 1.5, -39.5}, 90, 1, 101, 200, 100);
    ConstantsInput straight = Input(&ahead, &prev);
    sl::Constants plain, flipped;
    REQUIRE(Build(straight, &plain));
    straight.options.flipHandedness = true;
    REQUIRE(Build(straight, &flipped));
    CHECK(MatNear(FromSl(flipped.clipToPrevClip), FromSl(plain.clipToPrevClip), 1e-6, "clipToPrevClip"));
    CHECK(MatNear(FromSl(flipped.cameraViewToClip), FromSl(plain.cameraViewToClip), 0, "cameraViewToClip"));
}

TEST(FC_NegateSideFlipsOnlyCameraRight) {
    const double t = 7 * kPi / 180;
    const CameraLayout prev = MakeCamera({300, 4, 900}, {0.2, 0, 1}, {0, 1, 0}, 60, 0.1f, 2000, 1920, 1080);
    const CameraLayout cur = YawedCamera({300.5, 4, 900.2}, t, 62, 0.1f, 2000, 1920, 1080);
    ConstantsInput in = Input(&cur, &prev);
    sl::Constants plain, negated;
    REQUIRE(Build(in, &plain));
    in.options.negateSide = true;
    REQUIRE(Build(in, &negated));
    CHECK(VecNear(FromSl(negated.cameraRight), Scale(FromSl(plain.cameraRight), -1), 0));
    CHECK(VecNear(FromSl(negated.cameraUp), FromSl(plain.cameraUp), 0));
    CHECK(VecNear(FromSl(negated.cameraFwd), FromSl(plain.cameraFwd), 0));
    CHECK(VecNear(FromSl(negated.cameraPos), FromSl(plain.cameraPos), 0));
    CHECK(std::fabs(Det(FromSl(negated.cameraRight), FromSl(negated.cameraUp), FromSl(negated.cameraFwd)) - 1) <
          1e-6);
    CHECK(MatNear(FromSl(negated.cameraViewToClip), FromSl(plain.cameraViewToClip), 0, "cameraViewToClip"));
    CHECK(MatNear(FromSl(negated.clipToCameraView), FromSl(plain.clipToCameraView), 0, "clipToCameraView"));
    CHECK(MatNear(FromSl(negated.clipToPrevClip), FromSl(plain.clipToPrevClip), 0, "clipToPrevClip"));
    CHECK(MatNear(FromSl(negated.prevClipToClip), FromSl(plain.prevClipToClip), 0, "prevClipToClip"));
}

TEST(FC_RefusesNullInput) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    CHECK(Refuses(Input(nullptr, &cam), "cur"));
    ConstantsInput in = Input(&cam, &cam);
    std::string why;
    CHECK(!BuildFrameConstants(in, nullptr, &why));
    CHECK(!why.empty());
    // A null why is allowed.
    CHECK(!BuildFrameConstants(Input(nullptr, nullptr), nullptr, nullptr));
}

TEST(FC_RefusesAZeroRenderSize) {
    CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    CameraLayout bad = cam;
    bad.renderW = 0;
    CHECK(Refuses(Input(&bad, &cam), "render size"));
    bad = cam;
    bad.renderH = -1080;
    CHECK(Refuses(Input(&bad, &cam), "render size"));
    ConstantsInput in = Input(&cam, &cam);
    in.capture.renderW = 0;
    CHECK(Refuses(in, "render size"));
    in = Input(&cam, &cam);
    in.capture.renderH = 0;
    CHECK(Refuses(in, "render size"));
}

TEST(FC_RefusesBadClipPlanes) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    for (const auto& [n, f] : {std::pair{1.0f, 1.0f}, std::pair{10.0f, 5.0f}, std::pair{0.0f, 100.0f},
                               std::pair{-0.1f, 100.0f}}) {
        CameraLayout bad = cam;
        bad.clipNear = n;
        bad.clipFar = f;
        const bool refused = Refuses(Input(&bad, &cam), "clipNear");
        if (!refused) std::printf("  near %g, far %g\n", n, f);
        CHECK(refused);
    }
}

TEST(FC_RefusesFovOutsideTheOpenRange) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    for (float fov : {0.0f, 180.0f, -30.0f, 200.0f}) {
        CameraLayout bad = cam;
        bad.fovVDeg = fov;
        CHECK(Refuses(Input(&bad, &cam), "fovVDeg"));
    }
    CameraLayout ok = cam;
    ok.fovVDeg = 179.0f;
    sl::Constants c;
    CHECK(Build(Input(&ok, &cam), &c));
}

TEST(FC_RefusesNonFiniteValues) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    const auto withCur = [&](auto&& edit) {
        CameraLayout bad = cam;
        edit(bad);
        return Refuses(Input(&bad, &cam), "finite");
    };
    CHECK(withCur([](CameraLayout& c) { c.pos[0] = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.pos[2] = kInf; }));
    CHECK(withCur([](CameraLayout& c) { c.fwd[2] = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.up[1] = kInf; }));
    CHECK(withCur([](CameraLayout& c) { c.side[0] = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.fovVDeg = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.clipNear = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.clipFar = kInf; }));
    CHECK(withCur([](CameraLayout& c) { c.renderW = kNaN; }));
    CHECK(withCur([](CameraLayout& c) { c.renderH = kInf; }));

    ConstantsInput in = Input(&cam, &cam);
    in.capture.jitterX = kNaN;
    CHECK(Refuses(in, "finite"));
    in = Input(&cam, &cam);
    in.capture.jitterY = kInf;
    CHECK(Refuses(in, "finite"));
    in = Input(&cam, &cam);
    in.capture.mvScaleX = kNaN;
    CHECK(Refuses(in, "MV scale"));

    // A given prev is checked too.
    CameraLayout badPrev = cam;
    badPrev.pos[1] = kNaN;
    CHECK(Refuses(Input(&cam, &badPrev), "prev"));
}

TEST(FC_RefusesAZeroMotionVectorScale) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    ConstantsInput in = Input(&cam, &cam);
    in.capture.mvScaleX = 0;
    CHECK(Refuses(in, "MV scale"));
    in = Input(&cam, &cam);
    in.capture.mvScaleY = 0;
    CHECK(Refuses(in, "MV scale"));
}

TEST(FC_RefusesADegenerateBasis) {
    const CameraLayout cam = AxisCamera({0, 0, 0}, 60, 0.1f, 1000, 1920, 1080);
    const auto withCur = [&](V3 fwd, V3 up, V3 side) {
        CameraLayout bad = cam;
        ToArray(fwd, bad.fwd);
        ToArray(up, bad.up);
        ToArray(side, bad.side);
        return Refuses(Input(&bad, &cam), "basis");
    };
    CHECK(withCur({0, 0, 0}, {0, 1, 0}, {-1, 0, 0}));        // zero fwd
    CHECK(withCur({0, 0, 1}, {0, 0, 0}, {-1, 0, 0}));        // zero up
    CHECK(withCur({0, 0, 1}, {0, 1, 0}, {0, 0, 0}));         // zero side
    CHECK(withCur({0, 0, 1}, {0, 0, 3}, {-1, 0, 0}));        // up parallel to fwd
    CHECK(withCur({0, 0, 1}, {0, 0, -2}, {-1, 0, 0}));       // up antiparallel to fwd
    CHECK(withCur({0, 0, 1}, {0, 1, 0}, {0, 0.7, 0.7}));     // side in the fwd/up plane
    CHECK(withCur({0, 0, 1}, {0, 1, 0}, {1e-9, 1, 0}));      // side along up

    CameraLayout badPrev = cam;
    ToArray({0, 1, 0}, badPrev.up);
    ToArray({0, 1, 0}, badPrev.fwd);
    CHECK(Refuses(Input(&cam, &badPrev), "prev"));
}

TEST(FC_FormatConstantsForLog) {
    const CameraLayout cam = AxisCamera({1.5, 2, -3}, 90, 1, 101, 200, 100);
    ConstantsInput in = Input(&cam, &cam);
    in.prevFrameHadInputs = false;
    sl::Constants c;
    REQUIRE(Build(in, &c));
    const std::string s = FormatConstantsForLog(c);
    for (const char* part : {"pos=(1.5, 2, -3)", "right=(-1, 0, 0)", "up=(0, 1, 0)", "fwd=(0, 0, 1)",
                             "det=-1.000", "near=1", "far=101", "fovY=90.000deg", "aspect=2.0000",
                             "jitter=(0.25, -0.125)", "mvecScale=(-1, -1)", "depthInverted=0", "mvJittered=0",
                             "reset=1", "viewToClip=[", "clipToView=[", "clipToPrevClip=[", "prevClipToClip=["}) {
        if (s.find(part) == std::string::npos) {
            std::printf("  '%s' missing in: %s\n", part, s.c_str());
            CHECK(false);
        }
    }
    CHECK(s.find('\n') == std::string::npos);
}
