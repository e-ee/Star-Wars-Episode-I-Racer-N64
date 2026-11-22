#include "vr_math.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace SWE1RVR;

namespace {

int g_failures = 0;

bool nearEq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

void testNearFarAndProjectionCorners() {
    Fov symFov{-0.78539816f, 0.78539816f, 0.78539816f, -0.78539816f}; // +/- 45 deg
    const float n = 2.0f;
    const float f = 4000.0f;
    Mat4 proj = makeAsymmetricPerspectiveRowVector(symFov, n, f);

    float extNear = 0.0f;
    float extFar = 0.0f;
    CHECK(extractNearFar(proj, extNear, extFar, 0.05f), "extractNearFar should succeed for perspective");
    CHECK(nearEq(extNear, n, 1e-2f), "symmetric extracted near matches input near");
    CHECK(nearEq(extFar, f, 2.0f), "symmetric extracted far matches input far");

    // Verify tiny N64 near plane gets clamped up to nearMin (0.05) while preserving far
    Mat4 tinyProj = makeAsymmetricPerspectiveRowVector(symFov, 0.01f, 200.0f);
    CHECK(extractNearFar(tinyProj, extNear, extFar, 0.05f), "extractNearFar succeeds for tiny near");
    CHECK(nearEq(extNear, 0.05f, 1e-5f), "tiny near plane clamped to nearMin");
    CHECK(nearEq(extFar, 200.0f, 1.0f), "far plane preserved when near clamped");

    // Asymmetric Quest-style FOV corner mapping check
    Fov asymFov{-0.95f, 0.70f, 0.85f, -0.80f};
    const float an = 0.25f;
    const float af = 500.0f;
    Mat4 asymProj = makeAsymmetricPerspectiveRowVector(asymFov, an, af);
    const float l = std::tan(asymFov.angleLeft);
    const float r = std::tan(asymFov.angleRight);
    const float u = std::tan(asymFov.angleUp);
    const float d = std::tan(asymFov.angleDown);

    float nearTopLeft[4] = {l * an, u * an, -an, 1.0f};
    float clipNear[4]{};
    transformRowVec4(nearTopLeft, asymProj, clipNear);
    CHECK(clipNear[3] > 0.0f, "near clip W is positive");
    CHECK(nearEq(clipNear[0] / clipNear[3], -1.0f), "near top-left X maps to -1");
    CHECK(nearEq(clipNear[1] / clipNear[3], +1.0f), "near top-left Y maps to +1");
    CHECK(nearEq(clipNear[2] / clipNear[3], -1.0f), "near top-left Z maps to -1 (N64/GL depth)");

    float farBottomRight[4] = {r * af, d * af, -af, 1.0f};
    float clipFar[4]{};
    transformRowVec4(farBottomRight, asymProj, clipFar);
    CHECK(clipFar[3] > 0.0f, "far clip W is positive");
    CHECK(nearEq(clipFar[0] / clipFar[3], +1.0f, 1e-3f), "far bottom-right X maps to +1");
    CHECK(nearEq(clipFar[1] / clipFar[3], -1.0f, 1e-3f), "far bottom-right Y maps to -1");
    CHECK(nearEq(clipFar[2] / clipFar[3], +1.0f, 1e-3f), "far bottom-right Z maps to +1 (N64/GL depth)");
}

void testEyeViewSignsAndRecenter() {
    // 1. Right-eye +X IPD offset moves centered forward point to -X in eye space.
    Pose rightEye{};
    rightEye.position = {0.032f, 0.0f, 0.0f};
    Mat4 rightView = makeEyeViewTransformRowVector(rightEye, 10.0f, true);
    float ptAhead[4] = {0.0f, 0.0f, -20.0f, 1.0f};
    float eyePt[4]{};
    transformRowVec4(ptAhead, rightView, eyePt);
    CHECK(nearEq(eyePt[0], -0.32f), "right eye IPD shifts ahead object left (-X)");
    CHECK(nearEq(eyePt[1], 0.0f), "right eye IPD preserves Y");
    CHECK(nearEq(eyePt[2], -20.0f), "right eye IPD preserves Z");

    // 2. Yaw-left (+90 deg around +Y) moves object originally straight ahead (-Z) to +X (right side of eye).
    const float s = std::sin(0.78539816f); // sin(45 deg)
    const float c = std::cos(0.78539816f); // cos(45 deg)
    Pose yawLeft{};
    yawLeft.orientation = {0.0f, s, 0.0f, c};
    Mat4 yawView = makeEyeViewTransformRowVector(yawLeft, 10.0f, true);
    transformRowVec4(ptAhead, yawView, eyePt);
    CHECK(nearEq(eyePt[0], +20.0f, 1e-3f), "yaw-left puts ahead object at +X in eye space");
    CHECK(nearEq(eyePt[2], 0.0f, 1e-3f), "yaw-left puts ahead object at Z=0");

    // 3. Pitch-up (+30 deg around +X) moves object originally at horizon (-Z) downward (-Y in eye space).
    const float sp = std::sin(0.26179939f); // sin(15 deg)
    const float cp = std::cos(0.26179939f); // cos(15 deg)
    Pose pitchUp{};
    pitchUp.orientation = {sp, 0.0f, 0.0f, cp};
    Mat4 pitchView = makeEyeViewTransformRowVector(pitchUp, 10.0f, true);
    transformRowVec4(ptAhead, pitchView, eyePt);
    CHECK(eyePt[1] < -9.5f && nearEq(eyePt[1], -10.0f, 1e-2f), "pitch-up moves ahead object down (-Y)");
    CHECK(eyePt[2] < -17.0f, "pitch-up keeps ahead object in front (-Z)");

    // 4. Recenter cancels initial head yaw and head translation while preserving pitch/roll + eye IPD.
    Pose seatedHead{};
    seatedHead.position = {1.5f, 1.1f, -0.4f};
    // Compose yaw(+60 deg) * pitch(+15 deg)
    const float sy = std::sin(0.52359878f);
    const float cy = std::cos(0.52359878f);
    const Quat yawQ{0.0f, sy, 0.0f, cy};
    const Quat pitchQ{std::sin(0.13089969f), 0.0f, 0.0f, std::cos(0.13089969f)};
    seatedHead.orientation = mulQuat(yawQ, pitchQ);

    Pose origin = makeRecenterOrigin(seatedHead);
    Pose rel = computeEyeRelativePose(seatedHead, origin);
    CHECK(nearEq(rel.position.x, 0.0f) && nearEq(rel.position.y, 0.0f) && nearEq(rel.position.z, 0.0f),
          "recenter zeroes head position");
    const Vec3 relFwd = rotateVecByQuat(rel.orientation, {0.0f, 0.0f, -1.0f});
    CHECK(nearEq(relFwd.x, 0.0f, 1e-3f), "recenter zeroes initial yaw from forward vector");
    CHECK(relFwd.y > 0.25f, "recenter preserves head pitch upward");

    // 5. Matrix multiplication associativity with an existing N64 view translation (world origin at z=-100).
    Mat4 n64View{};
    n64View.m[3][2] = -100.0f;
    Mat4 combined = matMulRowVector(n64View, rightView);
    float localOrigin[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    transformRowVec4(localOrigin, combined, eyePt);
    CHECK(nearEq(eyePt[0], -0.32f) && nearEq(eyePt[2], -100.0f),
          "combined N64 view * camToEye transforms world point cleanly");
}

void testLowPassFilter() {
    LowPassVec3 filter(0.5f);
    Vec3 a = filter.filter({10.0f, 0.0f, 0.0f});
    CHECK(nearEq(a.x, 10.0f), "first sample passes through unsmoothed");
    Vec3 b = filter.filter({0.0f, 0.0f, 0.0f});
    CHECK(nearEq(b.x, 5.0f), "second sample blends by alpha=0.5");
}

} // namespace

int main() {
    testNearFarAndProjectionCorners();
    testEyeViewSignsAndRecenter();
    testLowPassFilter();

    if (g_failures == 0) {
        std::printf("vr_math_test: ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "vr_math_test: %d FAILED\n", g_failures);
    return 1;
}
