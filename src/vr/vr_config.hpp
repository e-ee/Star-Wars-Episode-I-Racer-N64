#pragma once

#include <cstdint>

namespace SWE1RVR {

struct VRConfig {
    float renderScale = 0.85f;
    uint32_t rt64ResolutionMultiplier = 4;
    float targetRefreshRate = 90.0f;
    float worldUnitsPerMeter = 12.0f;
    float hudDistanceMeters = 1.8f;
    float hudWidthMeters = 1.6f;
    float menuDistanceMeters = 2.2f;
    float menuWidthMeters = 2.4f;
    int foveationLevel = 2; // 0=off, 1=low, 2=medium, 3=high
    bool enable6DofHead = true;
    bool enableNoseBobFilter = true;
    float nearPlaneMin = 0.05f;
};

} // namespace SWE1RVR
