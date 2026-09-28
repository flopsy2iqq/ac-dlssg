// FrameConstants (spec 6.7).
//
// Which way CSP's camera side points. The Lua SDK does not say (ac_apps/
// lib.lua: "cameraLook: Points forward", cameraUp and cameraSide without a
// word), so the default rests on how scripts use the vectors (paths under
// the game's extension folder, CSP build installed on 2026-09-28):
//  - CSP's own code builds side vectors as cross(look, up):
//    internal/lua-shared/sim/ghost.lua:156, the csp-traffic-tool's
//    TrafficCar.lua:323, cockpit camera "True Motion" cockpit.lua:2189. A
//    published app uses ac.getCameraSide() interchangeably with
//    cross(look, up) (github.com/sug44/FpvDroneForAC src/drone.lua:64/154).
//    So side = cross(fwd, up); the open question is only which way that
//    points on screen.
//  - The screen's right: an app moves the camera by +cameraSide on the Right
//    arrow key and by -cameraSide on Left (github.com/Sahneisttoll/
//    AssettoLuaStuff apps/SahneExtra/extras/replay.lua:92-97). Track maps
//    place world (x, z) at ((x + X_OFFSET) / SCALE_FACTOR,
//    (z + Z_OFFSET) / SCALE_FACTOR) on map.png with image y downwards
//    (apps/lua/TrafficTool/src/ui/PedestrianPhoneUI.lua:1174), which is an
//    unmirrored top-down view only if cross(look, up) is the screen's right.
//    CSP's chaser cameras name cross(dir, up) "carRight"
//    (lua/chaser-camera/arcade-mode/camera.lua:37, kirbycam:117).
//  - Against it: ghost.lua:166 sets up = cross(look, wheelFR - wheelFL),
//    which points up only if cross(look, up) points to the car's left. It
//    may be that AC's car matrices use the other sign than its camera.
// Default: side = cross(fwd, up) is the screen's right, so AC's world is
// right-handed and the view basis (right, up, fwd) has determinant -1. The
// matrices only need view x to be the screen's right; if the in-game check
// shows a mirrored reprojection, FrameConstantsOptions::flipHandedness is the
// fix.
#include "frame_constants.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace acdb {
namespace {

constexpr double kPi = 3.14159265358979323846;
// A vector shorter than kMinLength counts as zero. After Gram-Schmidt removes
// its parts along the vectors before it, a vector must keep at least
// kMinResidual of its length, or it was (nearly) parallel to them.
constexpr double kMinLength = 1e-6;
constexpr double kMinResidual = 1e-3;

struct Vec {
    double x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec FromArray(const float (&v)[3]) { return {v[0], v[1], v[2]}; }
sl::float3 ToSl(Vec v) { return sl::float3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }

struct Mat {
    double m[4][4] = {};
};
Mat Mul(const Mat& a, const Mat& b) {
    Mat r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
sl::float4x4 ToSl(const Mat& a) {
    sl::float4x4 r;
    for (uint32_t i = 0; i < 4; ++i) {
        r.setRow(i, sl::float4(static_cast<float>(a.m[i][0]), static_cast<float>(a.m[i][1]),
                               static_cast<float>(a.m[i][2]), static_cast<float>(a.m[i][3])));
    }
    return r;
}

// One validated snapshot: position, the orthonormal view axes in world space
// (right = the screen's right), and the projection parameters.
struct Camera {
    Vec pos;
    Vec right, up, fwd;
    double fovY = 0, aspect = 0, n = 0, f = 0;
};

bool Fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    return false;
}

std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

bool Finite(const float (&v)[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// Checks one snapshot and builds its basis with Gram-Schmidt that keeps fwd:
// fwd normalized, up made orthogonal to fwd, side made orthogonal to both.
// Each keeps its sign. mirror uses -side as the screen's right.
bool MakeCamera(const CameraLayout& s, const char* who, bool mirror, Camera* out, std::string* why) {
    const std::string w = std::string(who) + ": ";
    struct Named {
        const char* name;
        const float (*v)[3];
    };
    for (const Named& n : {Named{"pos", &s.pos}, Named{"fwd", &s.fwd}, Named{"up", &s.up}, Named{"side", &s.side}})
        if (!Finite(*n.v)) return Fail(why, w + n.name + " is not finite");
    const std::pair<const char*, float> scalars[] = {{"fovVDeg", s.fovVDeg}, {"clipNear", s.clipNear},
                                                     {"clipFar", s.clipFar},  {"renderW", s.renderW},
                                                     {"renderH", s.renderH}};
    for (const auto& [name, v] : scalars)
        if (!std::isfinite(v)) return Fail(why, w + name + " is not finite");
    if (!(s.renderW > 0 && s.renderH > 0))
        return Fail(why, w + "render size " + Num(s.renderW) + "x" + Num(s.renderH) + " is not positive");
    if (!(s.fovVDeg > 0 && s.fovVDeg < 180)) return Fail(why, w + "fovVDeg " + Num(s.fovVDeg) + " is outside (0, 180)");
    if (!(s.clipNear > 0 && s.clipNear < s.clipFar))
        return Fail(why, w + "clipNear " + Num(s.clipNear) + " must be > 0 and below clipFar " + Num(s.clipFar));

    const Vec fwdIn = FromArray(s.fwd), upIn = FromArray(s.up), sideIn = FromArray(s.side) * (mirror ? -1.0 : 1.0);
    const double fwdLen = Length(fwdIn), upLen = Length(upIn), sideLen = Length(sideIn);
    if (fwdLen < kMinLength) return Fail(why, w + "degenerate basis: fwd is zero");
    if (upLen < kMinLength) return Fail(why, w + "degenerate basis: up is zero");
    if (sideLen < kMinLength) return Fail(why, w + "degenerate basis: side is zero");
    const Vec f = fwdIn * (1.0 / fwdLen);
    const Vec u0 = upIn - f * Dot(upIn, f);
    const double u0Len = Length(u0);
    if (u0Len < kMinResidual * upLen) return Fail(why, w + "degenerate basis: up is parallel to fwd");
    const Vec u = u0 * (1.0 / u0Len);
    const Vec r0 = sideIn - f * Dot(sideIn, f) - u * Dot(sideIn, u);
    const double r0Len = Length(r0);
    if (r0Len < kMinResidual * sideLen) return Fail(why, w + "degenerate basis: side lies in the fwd/up plane");

    out->pos = FromArray(s.pos);
    out->fwd = f;
    out->up = u;
    out->right = r0 * (1.0 / r0Len);
    out->fovY = s.fovVDeg * kPi / 180.0;
    out->aspect = static_cast<double>(s.renderW) / s.renderH;
    out->n = s.clipNear;
    out->f = s.clipFar;
    return true;
}

// Standard non-reversed D3D perspective for row vectors: depth 0 at n, 1 at f.
Mat Perspective(const Camera& c) {
    const double ys = 1.0 / std::tan(c.fovY / 2), xs = ys / c.aspect, a = c.f / (c.f - c.n);
    Mat r;
    r.m[0][0] = xs;
    r.m[1][1] = ys;
    r.m[2][2] = a;
    r.m[2][3] = 1;
    r.m[3][2] = -c.n * a;
    return r;
}

// Its exact inverse.
Mat PerspectiveInverse(const Camera& c) {
    const double ys = 1.0 / std::tan(c.fovY / 2), xs = ys / c.aspect;
    Mat r;
    r.m[0][0] = 1 / xs;
    r.m[1][1] = 1 / ys;
    r.m[2][3] = -(c.f - c.n) / (c.n * c.f);
    r.m[3][2] = 1;
    r.m[3][3] = 1 / c.n;
    return r;
}

// View space of `from` to view space of `to`, camera-centred like Streamline's
// calcCameraToPrevCamera: only the difference of the positions enters, in
// double, so large world coordinates cost no precision.
Mat ViewToView(const Camera& from, const Camera& to) {
    const Vec fromAxes[3] = {from.right, from.up, from.fwd};
    const Vec toAxes[3] = {to.right, to.up, to.fwd};
    const Vec offset = from.pos - to.pos;
    Mat r;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) r.m[i][j] = Dot(fromAxes[i], toAxes[j]);
        r.m[3][j] = Dot(offset, toAxes[j]);
    }
    r.m[3][3] = 1;
    return r;
}

sl::Boolean Bool(bool v) { return v ? sl::Boolean::eTrue : sl::Boolean::eFalse; }

// A float Streamline can use: finite, and not sl::INVALID_FLOAT (FLT_MAX),
// which it reads as "not provided".
bool Usable(float v) { return std::isfinite(v) && v != sl::INVALID_FLOAT; }

// Finite but extreme input (a subnormal near plane or FOV, an extreme aspect,
// FLT_MAX anywhere) passes the input checks and still overflows a float in
// the result. Names the first field that is not usable.
const char* FirstUnusableField(const sl::Constants& c) {
    const struct {
        const char* name;
        const sl::float4x4* m;
    } matrices[] = {{"cameraViewToClip", &c.cameraViewToClip},
                    {"clipToCameraView", &c.clipToCameraView},
                    {"clipToPrevClip", &c.clipToPrevClip},
                    {"prevClipToClip", &c.prevClipToClip}};
    for (const auto& [name, m] : matrices)
        for (uint32_t i = 0; i < 4; ++i)
            if (!Usable((*m)[i].x) || !Usable((*m)[i].y) || !Usable((*m)[i].z) || !Usable((*m)[i].w)) return name;
    const struct {
        const char* name;
        const sl::float2* v;
    } pairs[] = {{"jitterOffset", &c.jitterOffset}, {"mvecScale", &c.mvecScale}};
    for (const auto& [name, v] : pairs)
        if (!Usable(v->x) || !Usable(v->y)) return name;
    const struct {
        const char* name;
        const sl::float3* v;
    } vectors[] = {{"cameraPos", &c.cameraPos},
                   {"cameraUp", &c.cameraUp},
                   {"cameraRight", &c.cameraRight},
                   {"cameraFwd", &c.cameraFwd}};
    for (const auto& [name, v] : vectors)
        if (!Usable(v->x) || !Usable(v->y) || !Usable(v->z)) return name;
    const std::pair<const char*, float> scalars[] = {{"cameraNear", c.cameraNear},
                                                     {"cameraFar", c.cameraFar},
                                                     {"cameraFOV", c.cameraFOV},
                                                     {"cameraAspectRatio", c.cameraAspectRatio}};
    for (const auto& [name, v] : scalars)
        if (!Usable(v)) return name;
    return nullptr;
}

}  // namespace

bool BuildFrameConstants(const ConstantsInput& in, sl::Constants* out, std::string* why) {
    if (!out) return Fail(why, "no output constants");
    if (!in.cur) return Fail(why, "cur: no camera snapshot");

    const bool mirror = in.options.flipHandedness;
    Camera cur, prev;
    if (!MakeCamera(*in.cur, "cur", mirror, &cur, why)) return false;
    if (in.prev) {
        if (!MakeCamera(*in.prev, "prev", mirror, &prev, why)) return false;
    } else {
        prev = cur;
    }

    const CaptureParams& cap = in.capture;
    if (!std::isfinite(cap.jitterX) || !std::isfinite(cap.jitterY))
        return Fail(why, "capture: jitter (" + Num(cap.jitterX) + ", " + Num(cap.jitterY) + ") is not finite");
    if (!std::isfinite(cap.mvScaleX) || !std::isfinite(cap.mvScaleY) || cap.mvScaleX == 0 || cap.mvScaleY == 0)
        return Fail(why, "capture: MV scale (" + Num(cap.mvScaleX) + ", " + Num(cap.mvScaleY) +
                             ") is zero or not finite");
    if (cap.renderW == 0 || cap.renderH == 0)
        return Fail(why, "capture: render size " + Num(cap.renderW) + "x" + Num(cap.renderH) + " is zero");

    sl::Constants c;
    const Mat viewToClip = Perspective(cur), clipToView = PerspectiveInverse(cur);
    c.cameraViewToClip = ToSl(viewToClip);
    c.clipToCameraView = ToSl(clipToView);
    c.clipToPrevClip = ToSl(Mul(Mul(clipToView, ViewToView(cur, prev)), Perspective(prev)));
    c.prevClipToClip = ToSl(Mul(Mul(PerspectiveInverse(prev), ViewToView(prev, cur)), viewToClip));

    c.jitterOffset = sl::float2(cap.jitterX, cap.jitterY);
    c.mvecScale = sl::float2(static_cast<float>(static_cast<double>(cap.mvScaleX) / cap.renderW),
                             static_cast<float>(static_cast<double>(cap.mvScaleY) / cap.renderH));
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = ToSl(cur.pos);
    c.cameraUp = ToSl(cur.up);
    c.cameraRight = ToSl(in.options.negateSide ? cur.right * -1.0 : cur.right);
    c.cameraFwd = ToSl(cur.fwd);
    c.cameraNear = in.cur->clipNear;
    c.cameraFar = in.cur->clipFar;
    c.cameraFOV = static_cast<float>(cur.fovY);
    c.cameraAspectRatio = static_cast<float>(cur.aspect);

    c.depthInverted = Bool((cap.createFlags & dlss_create_flags::kDepthInverted) != 0);
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = Bool(cap.ngxReset || (in.cur->flags & kCamJumped) != 0 || !in.prevFrameHadInputs || !in.prev);
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = Bool((cap.createFlags & dlss_create_flags::kMVJittered) != 0);

    if (const char* field = FirstUnusableField(c))
        return Fail(why, std::string("result: ") + field + " overflows a float or equals sl::INVALID_FLOAT");
    *out = c;
    return true;
}

std::string FormatConstantsForLog(const sl::Constants& c) {
    std::string s;
    char buf[160];
    // + 0.0 prints -0 as 0.
    const auto vec3 = [&](const char* name, const sl::float3& v) {
        std::snprintf(buf, sizeof(buf), "%s=(%g, %g, %g) ", name, v.x + 0.0, v.y + 0.0, v.z + 0.0);
        s += buf;
    };
    const auto mat = [&](const char* name, const sl::float4x4& m) {
        s += name;
        s += "=[";
        for (uint32_t i = 0; i < 4; ++i) {
            std::snprintf(buf, sizeof(buf), "%s[%.6g %.6g %.6g %.6g]", i ? " " : "", m[i].x + 0.0, m[i].y + 0.0,
                          m[i].z + 0.0, m[i].w + 0.0);
            s += buf;
        }
        s += "] ";
    };
    vec3("pos", c.cameraPos);
    vec3("right", c.cameraRight);
    vec3("up", c.cameraUp);
    vec3("fwd", c.cameraFwd);
    const Vec r = {c.cameraRight.x, c.cameraRight.y, c.cameraRight.z};
    const Vec u = {c.cameraUp.x, c.cameraUp.y, c.cameraUp.z};
    const Vec f = {c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z};
    const Vec uxf = {u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
    std::snprintf(buf, sizeof(buf), "det=%.3f near=%g far=%g fovY=%.3fdeg aspect=%.4f ", Dot(r, uxf) + 0.0,
                  c.cameraNear, c.cameraFar, c.cameraFOV * 180.0 / kPi, c.cameraAspectRatio);
    s += buf;
    std::snprintf(buf, sizeof(buf),
                  "jitter=(%g, %g) mvecScale=(%g, %g) depthInverted=%d mvJittered=%d cameraMotionIncluded=%d "
                  "reset=%d ",
                  c.jitterOffset.x + 0.0, c.jitterOffset.y + 0.0, c.mvecScale.x + 0.0, c.mvecScale.y + 0.0,
                  static_cast<int>(c.depthInverted), static_cast<int>(c.motionVectorsJittered),
                  static_cast<int>(c.cameraMotionIncluded), static_cast<int>(c.reset));
    s += buf;
    mat("viewToClip", c.cameraViewToClip);
    mat("clipToView", c.clipToCameraView);
    mat("clipToPrevClip", c.clipToPrevClip);
    mat("prevClipToClip", c.prevClipToClip);
    s.pop_back();  // the trailing space
    return s;
}

}  // namespace acdb
