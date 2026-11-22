#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SWE1RVR {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Pose {
    Quat orientation{};
    Vec3 position{};
};

struct Fov {
    float angleLeft = -0.8f;
    float angleRight = 0.8f;
    float angleUp = 0.8f;
    float angleDown = -0.8f;
};

struct Mat4 {
    float m[4][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
};

inline Vec3 operator+(const Vec3 &a, const Vec3 &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3 &a, const Vec3 &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3 &a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

inline float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline Quat conjugateQuat(const Quat &q) {
    return {-q.x, -q.y, -q.z, q.w};
}

inline Quat normalizeQuat(const Quat &q) {
    const float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq <= 1e-12f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    return {q.x * invLen, q.y * invLen, q.z * invLen, q.w * invLen};
}

inline Quat mulQuat(const Quat &a, const Quat &b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline Vec3 rotateVecByQuat(const Quat &q, const Vec3 &v) {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 uv = cross(qv, v);
    const Vec3 uuv = cross(qv, uv);
    return v + (uv * (2.0f * q.w)) + (uuv * 2.0f);
}

inline Mat4 matMulRowVector(const Mat4 &a, const Mat4 &b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[r][k] * b.m[k][c];
            }
            out.m[r][c] = sum;
        }
    }
    return out;
}

inline void transformRowVec4(const float v[4], const Mat4 &mat, float out[4]) {
    for (int c = 0; c < 4; ++c) {
        out[c] = v[0] * mat.m[0][c] + v[1] * mat.m[1][c] + v[2] * mat.m[2][c] + v[3] * mat.m[3][c];
    }
}

inline bool isPerspectiveProjRowVector(const Mat4 &p) {
    return std::isfinite(p.m[2][3]) && (p.m[2][3] < -0.5f) && (std::fabs(p.m[3][3]) < 0.25f);
}

inline bool extractNearFar(const Mat4 &n64Proj, float &outNear, float &outFar, float nearMin = 0.05f) {
    if (!isPerspectiveProjRowVector(n64Proj)) {
        outNear = std::max(nearMin, 0.1f);
        outFar = std::max(10000.0f, outNear + 10.0f);
        return false;
    }

    const float p23 = n64Proj.m[2][3]; // Typically -1.0 for standard N64 perspective.
    const float A = n64Proj.m[2][2] / (-p23);
    const float B = n64Proj.m[3][2] / (-p23);
    const float denomNear = A - 1.0f;
    const float denomFar = A + 1.0f;

    if (std::fabs(denomNear) < 1e-6f || std::fabs(denomFar) < 1e-6f || !std::isfinite(A) || !std::isfinite(B)) {
        outNear = std::max(nearMin, 0.1f);
        outFar = std::max(10000.0f, outNear + 10.0f);
        return false;
    }

    const float rawNear = B / denomNear;
    const float rawFar = B / denomFar;
    if (!std::isfinite(rawNear) || !std::isfinite(rawFar) || rawNear <= 0.0f || rawFar <= rawNear) {
        outNear = std::max(nearMin, 0.1f);
        outFar = std::max(10000.0f, outNear + 10.0f);
        return false;
    }

    outNear = std::max(rawNear, nearMin);
    outFar = std::max(rawFar, outNear + 10.0f);
    return true;
}

inline Mat4 makeAsymmetricPerspectiveRowVector(const Fov &fov, float nearZ, float farZ) {
    const float l = std::tan(fov.angleLeft);
    const float r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp);
    const float d = std::tan(fov.angleDown);

    const float invW = 1.0f / std::max(r - l, 1e-5f);
    const float invH = 1.0f / std::max(u - d, 1e-5f);
    const float invDepth = 1.0f / std::max(farZ - nearZ, 1e-4f);

    Mat4 out{};
    for (int rIdx = 0; rIdx < 4; ++rIdx) {
        for (int cIdx = 0; cIdx < 4; ++cIdx) {
            out.m[rIdx][cIdx] = 0.0f;
        }
    }

    out.m[0][0] = 2.0f * invW;
    out.m[1][1] = 2.0f * invH;
    out.m[2][0] = (r + l) * invW;
    out.m[2][1] = (u + d) * invH;
    out.m[2][2] = -(farZ + nearZ) * invDepth;
    out.m[2][3] = -1.0f;
    out.m[3][2] = -2.0f * farZ * nearZ * invDepth;
    return out;
}

inline Pose makeRecenterOrigin(const Pose &headPose) {
    const Quat q = normalizeQuat(headPose.orientation);
    const Vec3 fwd = rotateVecByQuat(q, {0.0f, 0.0f, -1.0f});
    float yaw = 0.0f;
    if ((fwd.x * fwd.x + fwd.z * fwd.z) > 1e-4f) {
        yaw = std::atan2(-fwd.x, -fwd.z);
    } else {
        const Vec3 right = rotateVecByQuat(q, {1.0f, 0.0f, 0.0f});
        yaw = std::atan2(-right.z, right.x);
    }

    const float halfYaw = 0.5f * yaw;
    Pose origin{};
    origin.orientation = {0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw)};
    origin.position = headPose.position;
    return origin;
}

inline Pose computeEyeRelativePose(const Pose &eyePose, const Pose &recenterOrigin) {
    const Quat invOrigin = conjugateQuat(normalizeQuat(recenterOrigin.orientation));
    Pose rel{};
    rel.orientation = normalizeQuat(mulQuat(invOrigin, normalizeQuat(eyePose.orientation)));
    rel.position = rotateVecByQuat(invOrigin, eyePose.position - recenterOrigin.position);
    return rel;
}

inline Mat4 makeEyeViewTransformRowVector(const Pose &relEyePose, float worldUnitsPerMeter, bool enable6Dof = true) {
    const Quat q = normalizeQuat(relEyePose.orientation);
    const Vec3 ex = rotateVecByQuat(q, {1.0f, 0.0f, 0.0f});
    const Vec3 ey = rotateVecByQuat(q, {0.0f, 1.0f, 0.0f});
    const Vec3 ez = rotateVecByQuat(q, {0.0f, 0.0f, 1.0f});
    const Vec3 pos = enable6Dof ? (relEyePose.position * worldUnitsPerMeter) : Vec3{0.0f, 0.0f, 0.0f};

    Mat4 camToEye{};
    camToEye.m[0][0] = ex.x;
    camToEye.m[1][0] = ex.y;
    camToEye.m[2][0] = ex.z;
    camToEye.m[3][0] = -dot(pos, ex);

    camToEye.m[0][1] = ey.x;
    camToEye.m[1][1] = ey.y;
    camToEye.m[2][1] = ey.z;
    camToEye.m[3][1] = -dot(pos, ey);

    camToEye.m[0][2] = ez.x;
    camToEye.m[1][2] = ez.y;
    camToEye.m[2][2] = ez.z;
    camToEye.m[3][2] = -dot(pos, ez);

    camToEye.m[0][3] = 0.0f;
    camToEye.m[1][3] = 0.0f;
    camToEye.m[2][3] = 0.0f;
    camToEye.m[3][3] = 1.0f;
    return camToEye;
}

class LowPassVec3 {
public:
    explicit LowPassVec3(float alpha = 0.35f) : alpha_(std::clamp(alpha, 0.01f, 1.0f)) {}

    void reset() {
        initialized_ = false;
        value_ = {};
    }

    Vec3 filter(const Vec3 &sample) {
        if (!initialized_) {
            value_ = sample;
            initialized_ = true;
            return value_;
        }
        value_.x = value_.x + alpha_ * (sample.x - value_.x);
        value_.y = value_.y + alpha_ * (sample.y - value_.y);
        value_.z = value_.z + alpha_ * (sample.z - value_.z);
        return value_;
    }

private:
    float alpha_;
    bool initialized_ = false;
    Vec3 value_{};
};

} // namespace SWE1RVR
