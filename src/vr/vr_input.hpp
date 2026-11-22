#pragma once

#include "vr_math.hpp"

#include <cstdint>

namespace SWE1RVR {

struct VRInputState {
    uint16_t buttons = 0;
    float stickX = 0.0f;
    float stickY = 0.0f;
    bool leftGripValid = false;
    Pose leftGripPose{};
    bool rightGripValid = false;
    Pose rightGripPose{};
    bool recenterRequested = false;
};

#if defined(__ANDROID__)
bool vrInputInitialize(void *xrInstance, void *xrSession);
void vrInputShutdown();
void vrInputSync(void *xrSession, void *baseSpace, int64_t predictedDisplayTime);
bool vrInputConsumeRecenterRequest();
void vrInputUpdateHaptics(void *xrSession, bool rumbleActive);
VRInputState getVRInputSnapshot();
#endif

} // namespace SWE1RVR
