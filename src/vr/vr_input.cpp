#include "vr_input.hpp"

#if defined(__ANDROID__)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <openxr/openxr.h>

namespace SWE1RVR {

namespace {

constexpr uint16_t N64_BTN_A = 0x8000;
constexpr uint16_t N64_BTN_B = 0x4000;
constexpr uint16_t N64_BTN_Z = 0x2000;
constexpr uint16_t N64_BTN_START = 0x1000;
constexpr uint16_t N64_BTN_R = 0x0010;
constexpr uint16_t N64_BTN_C_UP = 0x0008;
constexpr uint16_t N64_BTN_C_DOWN = 0x0004;
constexpr uint16_t N64_BTN_C_LEFT = 0x0002;
constexpr uint16_t N64_BTN_C_RIGHT = 0x0001;

float applyDeadzone(float v, float deadzone = 0.12f) {
    const float absV = std::fabs(v);
    if (absV <= deadzone) {
        return 0.0f;
    }
    const float scaled = (absV - deadzone) / (1.0f - deadzone);
    return (v < 0.0f) ? -scaled : scaled;
}

} // namespace

class VRInputManager {
public:
    bool initialize(XrInstance instance, XrSession session) {
        shutdown();
        if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
            return false;
        }

        if (XR_FAILED(xrStringToPath(instance, "/user/hand/left", &handPaths_[0])) ||
            XR_FAILED(xrStringToPath(instance, "/user/hand/right", &handPaths_[1]))) {
            return false;
        }

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "swe1r_gameplay");
        std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "SWE1R Gameplay");
        setInfo.priority = 0;
        if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &actionSet_))) {
            return false;
        }

        auto makeAction = [&](XrActionType type, const char *name, const char *loc,
                              uint32_t subactionCount, const XrPath *subactions, XrAction *out) -> bool {
            XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
            ci.actionType = type;
            std::snprintf(ci.actionName, sizeof(ci.actionName), "%s", name);
            std::snprintf(ci.localizedActionName, sizeof(ci.localizedActionName), "%s", loc);
            ci.countSubactionPaths = subactionCount;
            ci.subactionPaths = subactions;
            return XR_SUCCEEDED(xrCreateAction(actionSet_, &ci, out));
        };

        if (!makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Steer Stick", 0, nullptr, &leftStick_) ||
            !makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Camera Stick", 0, nullptr, &rightStick_) ||
            !makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_val", "Trigger", 2, handPaths_.data(), &trigger_) ||
            !makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip_val", "Grip", 2, handPaths_.data(), &grip_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_a", "Accelerate / Confirm", 0, nullptr, &btnA_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_b", "Brake / Back", 0, nullptr, &btnB_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_x", "Look Back", 0, nullptr, &btnX_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_y", "Repair", 0, nullptr, &btnY_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_cam", "Camera View", 0, nullptr, &btnCam_) ||
            !makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "btn_menu", "Pause", 0, nullptr, &btnMenu_) ||
            !makeAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand Grip Pose", 2, handPaths_.data(), &gripPose_) ||
            !makeAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_out", "Haptics", 2, handPaths_.data(), &haptics_)) {
            shutdown();
            return false;
        }

        std::vector<XrActionSuggestedBinding> bindings;
        auto addBinding = [&](XrAction act, const char *pathStr) {
            XrPath p = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance, pathStr, &p))) {
                bindings.push_back({act, p});
            }
        };

        addBinding(leftStick_, "/user/hand/left/input/thumbstick");
        addBinding(rightStick_, "/user/hand/right/input/thumbstick");
        addBinding(trigger_, "/user/hand/left/input/trigger/value");
        addBinding(trigger_, "/user/hand/right/input/trigger/value");
        addBinding(grip_, "/user/hand/left/input/squeeze/value");
        addBinding(grip_, "/user/hand/right/input/squeeze/value");
        addBinding(btnA_, "/user/hand/right/input/a/click");
        addBinding(btnB_, "/user/hand/right/input/b/click");
        addBinding(btnX_, "/user/hand/left/input/x/click");
        addBinding(btnY_, "/user/hand/left/input/y/click");
        addBinding(btnCam_, "/user/hand/left/input/thumbstick/click");
        addBinding(btnCam_, "/user/hand/right/input/thumbstick/click");
        addBinding(btnMenu_, "/user/hand/left/input/menu/click");
        addBinding(gripPose_, "/user/hand/left/input/grip/pose");
        addBinding(gripPose_, "/user/hand/right/input/grip/pose");
        addBinding(haptics_, "/user/hand/left/output/haptic");
        addBinding(haptics_, "/user/hand/right/output/haptic");

        XrPath profilePath = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profilePath))) {
            shutdown();
            return false;
        }

        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profilePath;
        suggested.suggestedBindings = bindings.data();
        suggested.countSuggestedBindings = uint32_t(bindings.size());
        if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested))) {
            std::printf("[SWE1R-VR] Warning: xrSuggestInteractionProfileBindings failed\n");
        }

        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            spaceInfo.action = gripPose_;
            spaceInfo.subactionPath = handPaths_[h];
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateActionSpace(session, &spaceInfo, &gripSpaces_[h]))) {
                gripSpaces_[h] = XR_NULL_HANDLE;
            }
        }

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &actionSet_;
        if (XR_FAILED(xrAttachSessionActionSets(session, &attachInfo))) {
            shutdown();
            return false;
        }

        initialized_ = true;
        return true;
    }

    void shutdown() {
        for (XrSpace &s : gripSpaces_) {
            if (s != XR_NULL_HANDLE) {
                xrDestroySpace(s);
                s = XR_NULL_HANDLE;
            }
        }
        if (actionSet_ != XR_NULL_HANDLE) {
            xrDestroyActionSet(actionSet_);
            actionSet_ = XR_NULL_HANDLE;
        }
        leftStick_ = rightStick_ = trigger_ = grip_ = btnA_ = btnB_ = btnX_ = btnY_ = btnCam_ = btnMenu_ = gripPose_ = haptics_ = XR_NULL_HANDLE;
        initialized_ = false;
        bothGripHoldStart_ = 0;
        bothGripTriggered_ = false;
        std::scoped_lock<std::mutex> lock(stateMutex_);
        state_ = {};
    }

    void sync(XrSession session, XrSpace baseSpace, XrTime predictedDisplayTime) {
        if (!initialized_ || session == XR_NULL_HANDLE) {
            return;
        }

        XrActiveActionSet activeSet{actionSet_, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        if (XR_FAILED(xrSyncActions(session, &syncInfo))) {
            return;
        }

        auto getVec2 = [&](XrAction act) -> XrVector2f {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = act;
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive) {
                return st.currentState;
            }
            return {0.0f, 0.0f};
        };

        auto getFloat = [&](XrAction act, XrPath subaction) -> float {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = act;
            gi.subactionPath = subaction;
            XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive) {
                return st.currentState;
            }
            return 0.0f;
        };

        auto getBool = [&](XrAction act) -> bool {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = act;
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive) {
                return st.currentState != XR_FALSE;
            }
            return false;
        };

        const XrVector2f lStick = getVec2(leftStick_);
        const XrVector2f rStick = getVec2(rightStick_);
        const float lTrig = getFloat(trigger_, handPaths_[0]);
        const float rTrig = getFloat(trigger_, handPaths_[1]);
        const float lGrip = getFloat(grip_, handPaths_[0]);
        const float rGrip = getFloat(grip_, handPaths_[1]);

        uint16_t btns = 0;
        if (rTrig > 0.15f || getBool(btnA_)) btns |= N64_BTN_A;
        if (lTrig > 0.15f)                   btns |= N64_BTN_Z;
        if (getBool(btnB_))                  btns |= N64_BTN_B;
        if (getBool(btnX_))                  btns |= N64_BTN_C_DOWN;
        if (getBool(btnY_))                  btns |= N64_BTN_R;
        if (getBool(btnCam_))                btns |= N64_BTN_C_UP;
        if (getBool(btnMenu_))               btns |= N64_BTN_START;
        if (lGrip > 0.50f)                   btns |= N64_BTN_C_LEFT;
        if (rGrip > 0.50f)                   btns |= N64_BTN_C_RIGHT;

        if (rStick.x < -0.5f) btns |= N64_BTN_C_LEFT;
        if (rStick.x >  0.5f) btns |= N64_BTN_C_RIGHT;
        if (rStick.y >  0.5f) btns |= N64_BTN_C_UP;
        if (rStick.y < -0.5f) btns |= N64_BTN_C_DOWN;

        bool triggerRecenter = false;
        if (lGrip > 0.70f && rGrip > 0.70f && predictedDisplayTime > 0) {
            if (bothGripHoldStart_ == 0) {
                bothGripHoldStart_ = predictedDisplayTime;
            } else if (!bothGripTriggered_ && (predictedDisplayTime - bothGripHoldStart_) >= 600000000LL) {
                bothGripTriggered_ = true;
                triggerRecenter = true;
            }
        } else {
            bothGripHoldStart_ = 0;
            bothGripTriggered_ = false;
        }

        VRInputState next{};
        next.buttons = btns;
        next.stickX = std::clamp(applyDeadzone(lStick.x), -1.0f, 1.0f);
        next.stickY = std::clamp(applyDeadzone(lStick.y), -1.0f, 1.0f);
        next.recenterRequested = triggerRecenter;

        if (baseSpace != XR_NULL_HANDLE && predictedDisplayTime > 0) {
            for (int h = 0; h < 2; ++h) {
                if (gripSpaces_[h] == XR_NULL_HANDLE) {
                    continue;
                }
                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(gripSpaces_[h], baseSpace, predictedDisplayTime, &loc))) {
                    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    if ((loc.locationFlags & need) == need) {
                        Pose pose{};
                        pose.orientation = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
                        pose.position = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
                        if (h == 0) {
                            next.leftGripValid = true;
                            next.leftGripPose = pose;
                        } else {
                            next.rightGripValid = true;
                            next.rightGripPose = pose;
                        }
                    }
                }
            }
        }

        std::scoped_lock<std::mutex> lock(stateMutex_);
        const bool keepRecenter = state_.recenterRequested || next.recenterRequested;
        state_ = next;
        state_.recenterRequested = keepRecenter;
    }

    bool consumeRecenterRequest() {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        const bool requested = state_.recenterRequested;
        state_.recenterRequested = false;
        return requested;
    }

    void updateHaptics(XrSession session, bool rumbleActive) {
        if (!initialized_ || session == XR_NULL_HANDLE || haptics_ == XR_NULL_HANDLE) {
            return;
        }
        if (rumbleActive) {
            XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
            vib.duration = 50000000LL; // 50ms pulse refreshed while active
            vib.frequency = 160.0f;
            vib.amplitude = 0.55f;
            for (int h = 0; h < 2; ++h) {
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = haptics_;
                hi.subactionPath = handPaths_[h];
                xrApplyHapticFeedback(session, &hi, reinterpret_cast<const XrHapticBaseHeader *>(&vib));
            }
        } else if (lastRumbleActive_) {
            for (int h = 0; h < 2; ++h) {
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = haptics_;
                hi.subactionPath = handPaths_[h];
                xrStopHapticFeedback(session, &hi);
            }
        }
        lastRumbleActive_ = rumbleActive;
    }

    VRInputState getSnapshot() const {
        std::scoped_lock<std::mutex> lock(stateMutex_);
        return state_;
    }

private:
    bool initialized_ = false;
    bool lastRumbleActive_ = false;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths_{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> gripSpaces_{XR_NULL_HANDLE, XR_NULL_HANDLE};

    XrAction leftStick_ = XR_NULL_HANDLE;
    XrAction rightStick_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE;
    XrAction grip_ = XR_NULL_HANDLE;
    XrAction btnA_ = XR_NULL_HANDLE;
    XrAction btnB_ = XR_NULL_HANDLE;
    XrAction btnX_ = XR_NULL_HANDLE;
    XrAction btnY_ = XR_NULL_HANDLE;
    XrAction btnCam_ = XR_NULL_HANDLE;
    XrAction btnMenu_ = XR_NULL_HANDLE;
    XrAction gripPose_ = XR_NULL_HANDLE;
    XrAction haptics_ = XR_NULL_HANDLE;

    XrTime bothGripHoldStart_ = 0;
    bool bothGripTriggered_ = false;

    mutable std::mutex stateMutex_;
    VRInputState state_{};
};

static VRInputManager g_vrInputManager;

bool vrInputInitialize(void *xrInstance, void *xrSession) {
    return g_vrInputManager.initialize(reinterpret_cast<XrInstance>(xrInstance), reinterpret_cast<XrSession>(xrSession));
}

void vrInputShutdown() {
    g_vrInputManager.shutdown();
}

void vrInputSync(void *xrSession, void *baseSpace, int64_t predictedDisplayTime) {
    g_vrInputManager.sync(reinterpret_cast<XrSession>(xrSession), reinterpret_cast<XrSpace>(baseSpace), XrTime(predictedDisplayTime));
}

bool vrInputConsumeRecenterRequest() {
    return g_vrInputManager.consumeRecenterRequest();
}

void vrInputUpdateHaptics(void *xrSession, bool rumbleActive) {
    g_vrInputManager.updateHaptics(reinterpret_cast<XrSession>(xrSession), rumbleActive);
}

VRInputState getVRInputSnapshot() {
    return g_vrInputManager.getSnapshot();
}

} // namespace SWE1RVR

#endif // __ANDROID__
