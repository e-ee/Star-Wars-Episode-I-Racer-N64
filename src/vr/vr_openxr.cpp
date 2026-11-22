#include "vr_openxr.hpp"

#if defined(__ANDROID__)

#include "vr_input.hpp"
#include "vr_math.hpp"

#include "common/rt64_user_configuration.h"
#include "hle/rt64_shared_queue_resources.h"
#include "hle/rt64_vr_bridge.h"
#include "render/rt64_render_worker.h"
#include "plume_vulkan.h"

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ultramodern {
void quit();
}

namespace SWE1RVR {

namespace {

uint32_t align16(uint32_t v) {
    return (v + 15U) & ~15U;
}

class OpenXRBridge final : public RT64::VRBridge {
public:
    VRConfig config{};

    bool initializeAndroid() {
        std::scoped_lock<std::mutex> lock(lifecycleMutex_);
        if (initialized_) {
            return true;
        }

        JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
        jobject localActivity = static_cast<jobject>(SDL_AndroidGetActivity());
        if (env == nullptr || localActivity == nullptr) {
            std::fprintf(stderr, "[SWE1R-VR] Missing JNIEnv or Android activity\n");
            return false;
        }

        JavaVM *vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
            env->DeleteLocalRef(localActivity);
            std::fprintf(stderr, "[SWE1R-VR] GetJavaVM failed\n");
            return false;
        }

        appActivity_ = env->NewGlobalRef(localActivity);
        env->DeleteLocalRef(localActivity);

        PFN_xrInitializeLoaderKHR pfnInitializeLoaderKHR = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                               reinterpret_cast<PFN_xrVoidFunction *>(&pfnInitializeLoaderKHR))) &&
            pfnInitializeLoaderKHR != nullptr) {
            XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            loaderInit.applicationVM = vm;
            loaderInit.applicationContext = appActivity_;
            const XrResult loaderRes = pfnInitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loaderInit));
            if (XR_FAILED(loaderRes)) {
                std::fprintf(stderr, "[SWE1R-VR] xrInitializeLoaderKHR failed: %d\n", int(loaderRes));
                return false;
            }
        }

        uint32_t extCount = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
            std::fprintf(stderr, "[SWE1R-VR] xrEnumerateInstanceExtensionProperties failed\n");
            return false;
        }
        std::vector<XrExtensionProperties> availableExts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, availableExts.data());

        auto hasExt = [&](const char *name) -> bool {
            for (const auto &e : availableExts) {
                if (std::strcmp(e.extensionName, name) == 0) {
                    return true;
                }
            }
            return false;
        };

        std::vector<const char *> enabledExts;
        enabledExts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
        enabledExts.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);

        hasRefreshRateExt_ = hasExt(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (hasRefreshRateExt_) {
            enabledExts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }
        hasPerfExt_ = hasExt(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
        if (hasPerfExt_) {
            enabledExts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
        }
        hasColorSpaceExt_ = hasExt(XR_FB_COLOR_SPACE_EXTENSION_NAME);
        if (hasColorSpaceExt_) {
            enabledExts.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
        }

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = vm;
        androidInfo.applicationActivity = appActivity_;

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = &androidInfo;
        std::snprintf(createInfo.applicationInfo.applicationName, sizeof(createInfo.applicationInfo.applicationName), "SWE1R Q3");
        createInfo.applicationInfo.applicationVersion = 1;
        std::snprintf(createInfo.applicationInfo.engineName, sizeof(createInfo.applicationInfo.engineName), "RT64");
        createInfo.applicationInfo.engineVersion = 1;
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        createInfo.enabledExtensionCount = uint32_t(enabledExts.size());
        createInfo.enabledExtensionNames = enabledExts.data();

        const XrResult instRes = xrCreateInstance(&createInfo, &xrInstance_);
        if (XR_FAILED(instRes) || xrInstance_ == XR_NULL_HANDLE) {
            std::fprintf(stderr, "[SWE1R-VR] xrCreateInstance failed: %d\n", int(instRes));
            return false;
        }

        XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
        sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (XR_FAILED(xrGetSystem(xrInstance_, &sysInfo, &systemId_))) {
            std::fprintf(stderr, "[SWE1R-VR] xrGetSystem failed\n");
            cleanupInstanceOnly();
            return false;
        }

        XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(xrInstance_, systemId_, &sysProps))) {
            std::printf("[SWE1R-VR] System: %s\n", sysProps.systemName);
        }

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(xrInstance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        if (viewCount < 2) {
            std::fprintf(stderr, "[SWE1R-VR] Expected stereo view configuration, got %u views\n", viewCount);
            cleanupInstanceOnly();
            return false;
        }
        std::vector<XrViewConfigurationView> viewConfigs(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(xrInstance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewConfigs.data());

        eyeWidth_ = std::clamp(align16(uint32_t(std::lround(viewConfigs[0].recommendedImageRectWidth * config.renderScale))), 1024U, 2048U);
        eyeHeight_ = std::clamp(align16(uint32_t(std::lround(viewConfigs[0].recommendedImageRectHeight * config.renderScale))), 1024U, 2048U);
        std::printf("[SWE1R-VR] Eye swapchain target: %ux%u (recommended %ux%u)\n",
                    eyeWidth_, eyeHeight_,
                    viewConfigs[0].recommendedImageRectWidth, viewConfigs[0].recommendedImageRectHeight);

        xrGetInstanceProcAddr(xrInstance_, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetVkReqs2_));
        xrGetInstanceProcAddr(xrInstance_, "xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreateVkInstance_));
        xrGetInstanceProcAddr(xrInstance_, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction *>(&pfnGetVkDevice2_));
        xrGetInstanceProcAddr(xrInstance_, "xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction *>(&pfnCreateVkDevice_));
        if (!pfnGetVkReqs2_ || !pfnCreateVkInstance_ || !pfnGetVkDevice2_ || !pfnCreateVkDevice_) {
            std::fprintf(stderr, "[SWE1R-VR] Missing XR_KHR_vulkan_enable2 entry points\n");
            cleanupInstanceOnly();
            return false;
        }

        XrGraphicsRequirementsVulkan2KHR vkReqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        if (XR_FAILED(pfnGetVkReqs2_(xrInstance_, systemId_, &vkReqs))) {
            std::fprintf(stderr, "[SWE1R-VR] xrGetVulkanGraphicsRequirements2KHR failed\n");
            cleanupInstanceOnly();
            return false;
        }

        plume::VulkanExternalHooks hooks{};
        hooks.headlessSwapChain = true;
        hooks.headlessWidth = eyeWidth_;
        hooks.headlessHeight = eyeHeight_;
        hooks.headlessRefreshRate = uint32_t(config.targetRefreshRate);
        hooks.createInstance = &hookCreateVulkanInstance;
        hooks.pickPhysicalDevice = &hookPickVulkanPhysicalDevice;
        hooks.createDevice = &hookCreateVulkanDevice;
        hooks.onDeviceCreated = &hookOnVulkanDeviceCreated;
        plume::SetVulkanExternalHooks(hooks);

        initialized_ = true;
        return true;
    }

    void shutdown() {
        std::scoped_lock<std::mutex> lock(lifecycleMutex_);
        RT64::SetVRBridge(nullptr);
        active_.store(false, std::memory_order_release);
        sessionRunning_ = false;
        sessionFocused_ = false;

        vrInputShutdown();

        for (int i = 0; i < 2; ++i) {
            if (eyeSwapchains_[i].handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(eyeSwapchains_[i].handle);
                eyeSwapchains_[i] = {};
            }
        }
        if (hudSwapchain_.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(hudSwapchain_.handle);
            hudSwapchain_ = {};
        }

        if (localSpace_ != XR_NULL_HANDLE) {
            xrDestroySpace(localSpace_);
            localSpace_ = XR_NULL_HANDLE;
        }
        if (viewSpace_ != XR_NULL_HANDLE) {
            xrDestroySpace(viewSpace_);
            viewSpace_ = XR_NULL_HANDLE;
        }
        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }

        cleanupInstanceOnly();
        plume::SetVulkanExternalHooks(plume::VulkanExternalHooks{});

        if (appActivity_ != nullptr) {
            if (JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv())) {
                env->DeleteGlobalRef(appActivity_);
            }
            appActivity_ = nullptr;
        }

        initialized_ = false;
    }

    bool isActive() const override {
        return active_.load(std::memory_order_acquire);
    }

    void requestRecenter() {
        recenterPending_.store(true, std::memory_order_release);
    }

    void setRumble(bool enable) {
        rumbleActive_.store(enable, std::memory_order_release);
    }

    RT64::VRViewPoseSnapshot snapshotFrameViews() override {
        std::scoped_lock<std::mutex> lock(viewMutex_);
        return latestSnapshot_;
    }

    void computeEyeMatrices(int eyeIndex, const RT64::VRViewPoseSnapshot &snapshot,
                            const float n64View[16], const float n64Proj[16],
                            float outView[16], float outProj[16]) const override {
        const int eye = std::clamp(eyeIndex, 0, 1);

        Mat4 inViewMat{};
        Mat4 inProjMat{};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                inViewMat.m[r][c] = n64View[r * 4 + c];
                inProjMat.m[r][c] = n64Proj[r * 4 + c];
            }
        }

        float nearZ = 0.1f;
        float farZ = 10000.0f;
        extractNearFar(inProjMat, nearZ, farZ, config.nearPlaneMin);

        Fov fov{
            snapshot.eyes[eye].fov[0],
            snapshot.eyes[eye].fov[1],
            snapshot.eyes[eye].fov[2],
            snapshot.eyes[eye].fov[3],
        };
        const Mat4 outProjMat = makeAsymmetricPerspectiveRowVector(fov, nearZ, farZ);

        Pose relPose{};
        relPose.orientation = {
            snapshot.eyes[eye].orientation[0],
            snapshot.eyes[eye].orientation[1],
            snapshot.eyes[eye].orientation[2],
            snapshot.eyes[eye].orientation[3],
        };
        relPose.position = {
            snapshot.eyes[eye].position[0],
            snapshot.eyes[eye].position[1],
            snapshot.eyes[eye].position[2],
        };

        const Mat4 camToEye = makeEyeViewTransformRowVector(relPose, config.worldUnitsPerMeter, config.enable6DofHead);
        const Mat4 outViewMat = matMulRowVector(inViewMat, camToEye);

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                outView[r * 4 + c] = outViewMat.m[r][c];
                outProj[r * 4 + c] = outProjMat.m[r][c];
            }
        }
    }

    void onPresentTick(RT64::RenderWorker *presentWorker, RT64::SharedQueueResources *sharedResources) override {
        if (!active_.load(std::memory_order_acquire) || session_ == XR_NULL_HANDLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }

        pollEvents();

        if (!sessionRunning_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }

        if (sessionFocused_) {
            vrInputSync(session_, localSpace_, predictedDisplayTime_);
            if (vrInputConsumeRecenterRequest()) {
                recenterPending_.store(true, std::memory_order_release);
            }
            vrInputUpdateHaptics(session_, rumbleActive_.load(std::memory_order_acquire));
        }

        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session_, &waitInfo, &frameState))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            return;
        }
        predictedDisplayTime_ = frameState.predictedDisplayTime;

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(session_, &beginInfo))) {
            return;
        }

        std::array<XrView, 2> rawViews{{
            {XR_TYPE_VIEW},
            {XR_TYPE_VIEW},
        }};
        bool haveRawViews = false;

        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = localSpace_;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        if (XR_SUCCEEDED(xrLocateViews(session_, &locateInfo, &viewState, 2, &viewCountOutput, rawViews.data())) &&
            viewCountOutput >= 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
            haveRawViews = true;
            updateTrackedSnapshot(rawViews, (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        }

        bool submitStereo = false;
        bool submitHud = false;
        RT64::VRViewPoseSnapshot submittedViews{};
        uint32_t hudImgW = 1280;
        uint32_t hudImgH = 960;

        if (frameState.shouldRender != XR_FALSE && presentWorker != nullptr && sharedResources != nullptr) {
            const int slotIdx = sharedResources->vrPublishedSlot.load(std::memory_order_acquire);
            if (slotIdx >= 0 && slotIdx < 2) {
                std::scoped_lock<std::mutex> slotLock(sharedResources->vrSlotMutex[slotIdx]);
                const auto &slot = sharedResources->vrSlots[slotIdx];
                if (slot.frameId != 0) {
                    const bool wantStereo = slot.hasStereo &&
                                            slot.eyeTargets[0] != nullptr && !slot.eyeTargets[0]->isEmpty() &&
                                            slot.eyeTargets[1] != nullptr && !slot.eyeTargets[1]->isEmpty();
                    const bool wantHud = slot.hasHud && slot.hudTarget != nullptr && !slot.hudTarget->isEmpty();

                    uint32_t eyeImgIndices[2] = {};
                    bool eyeAcquired[2] = {false, false};
                    uint32_t hudImgIndex = 0;
                    bool hudAcquired = false;

                    if (wantStereo) {
                        for (int e = 0; e < 2; ++e) {
                            eyeAcquired[e] = acquireAndWaitSwapchain(eyeSwapchains_[e], &eyeImgIndices[e]);
                        }
                    }
                    if (wantHud) {
                        hudAcquired = acquireAndWaitSwapchain(hudSwapchain_, &hudImgIndex);
                    }

                    if ((wantStereo && eyeAcquired[0] && eyeAcquired[1]) || (wantHud && hudAcquired)) {
                        presentWorker->commandList->begin();

                        bool recordedAny = false;
                        if (wantStereo && eyeAcquired[0] && eyeAcquired[1]) {
                            const bool ok0 = plume::VulkanBlitTextureToExternalImage(
                                presentWorker->commandList.get(),
                                slot.eyeTargets[0]->texture.get(),
                                slot.eyeWidth[0], slot.eyeHeight[0],
                                eyeSwapchains_[0].images[eyeImgIndices[0]],
                                eyeWidth_, eyeHeight_,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
                            const bool ok1 = plume::VulkanBlitTextureToExternalImage(
                                presentWorker->commandList.get(),
                                slot.eyeTargets[1]->texture.get(),
                                slot.eyeWidth[1], slot.eyeHeight[1],
                                eyeSwapchains_[1].images[eyeImgIndices[1]],
                                eyeWidth_, eyeHeight_,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
                            submitStereo = ok0 && ok1;
                            recordedAny = recordedAny || submitStereo;
                            submittedViews = slot.views;
                        }

                        if (wantHud && hudAcquired) {
                            const bool okHud = plume::VulkanBlitTextureToExternalImage(
                                presentWorker->commandList.get(),
                                slot.hudTarget->texture.get(),
                                slot.hudWidth, slot.hudHeight,
                                hudSwapchain_.images[hudImgIndex],
                                hudWidth_, hudHeight_,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
                            submitHud = okHud;
                            recordedAny = recordedAny || submitHud;
                            hudImgW = slot.hudWidth;
                            hudImgH = slot.hudHeight;
                        }

                        presentWorker->commandList->end();
                        if (recordedAny) {
                            presentWorker->execute();
                            presentWorker->wait();
                        }
                    }

                    for (int e = 0; e < 2; ++e) {
                        if (eyeAcquired[e]) {
                            XrSwapchainImageReleaseInfo relInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            xrReleaseSwapchainImage(eyeSwapchains_[e].handle, &relInfo);
                        }
                    }
                    if (hudAcquired) {
                        XrSwapchainImageReleaseInfo relInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        xrReleaseSwapchainImage(hudSwapchain_.handle, &relInfo);
                    }
                }
            }
        }

        std::array<XrCompositionLayerProjectionView, 2> projViews{{
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        }};
        XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::vector<const XrCompositionLayerBaseHeader *> layers;

        if (submitStereo && haveRawViews) {
            for (int e = 0; e < 2; ++e) {
                Pose relEyePose{};
                relEyePose.orientation = {
                    submittedViews.valid ? submittedViews.eyes[e].orientation[0] : 0.0f,
                    submittedViews.valid ? submittedViews.eyes[e].orientation[1] : 0.0f,
                    submittedViews.valid ? submittedViews.eyes[e].orientation[2] : 0.0f,
                    submittedViews.valid ? submittedViews.eyes[e].orientation[3] : 1.0f,
                };
                relEyePose.position = {
                    submittedViews.valid ? submittedViews.eyes[e].position[0] : (e == 0 ? -0.032f : 0.032f),
                    submittedViews.valid ? submittedViews.eyes[e].position[1] : 0.0f,
                    submittedViews.valid ? submittedViews.eyes[e].position[2] : 0.0f,
                };

                // Convert eye pose back from recentered space to LOCAL space so ATW sees exact rendered pose.
                const Pose localRenderedPose = toLocalSpacePose(relEyePose);

                projViews[e].pose.orientation = {
                    localRenderedPose.orientation.x,
                    localRenderedPose.orientation.y,
                    localRenderedPose.orientation.z,
                    localRenderedPose.orientation.w,
                };
                projViews[e].pose.position = {
                    localRenderedPose.position.x,
                    localRenderedPose.position.y,
                    localRenderedPose.position.z,
                };
                projViews[e].fov = {
                    submittedViews.valid ? submittedViews.eyes[e].fov[0] : rawViews[e].fov.angleLeft,
                    submittedViews.valid ? submittedViews.eyes[e].fov[1] : rawViews[e].fov.angleRight,
                    submittedViews.valid ? submittedViews.eyes[e].fov[2] : rawViews[e].fov.angleUp,
                    submittedViews.valid ? submittedViews.eyes[e].fov[3] : rawViews[e].fov.angleDown,
                };
                projViews[e].subImage.swapchain = eyeSwapchains_[e].handle;
                projViews[e].subImage.imageRect = {{0, 0}, {int32_t(eyeWidth_), int32_t(eyeHeight_)}};
                projViews[e].subImage.imageArrayIndex = 0;
            }
            projLayer.layerFlags = 0;
            projLayer.space = localSpace_;
            projLayer.viewCount = 2;
            projLayer.views = projViews.data();
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projLayer));
        }

        if (submitHud && hudWidth_ > 0 && hudHeight_ > 0) {
            const float hudHeightScale = (hudImgW > 0 && hudImgH > 0) ? 0.75f : 0.75f;
            if (submitStereo) {
                quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                quadLayer.space = viewSpace_;
                quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quadLayer.pose.position = {0.0f, -0.04f, -config.hudDistanceMeters};
                quadLayer.size = {config.hudWidthMeters, config.hudWidthMeters * hudHeightScale};
            } else {
                const Pose origin = getRecenterOriginSnapshot();
                const Vec3 forward = rotateVecByQuat(origin.orientation, {0.0f, 0.0f, -config.menuDistanceMeters});
                quadLayer.layerFlags = 0;
                quadLayer.space = localSpace_;
                quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quadLayer.pose.orientation = {origin.orientation.x, origin.orientation.y, origin.orientation.z, origin.orientation.w};
                quadLayer.pose.position = {
                    origin.position.x + forward.x,
                    origin.position.y + forward.y,
                    origin.position.z + forward.z,
                };
                quadLayer.size = {config.menuWidthMeters, config.menuWidthMeters * hudHeightScale};
            }
            quadLayer.subImage.swapchain = hudSwapchain_.handle;
            quadLayer.subImage.imageRect = {{0, 0}, {int32_t(hudWidth_), int32_t(hudHeight_)}};
            quadLayer.subImage.imageArrayIndex = 0;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quadLayer));
        }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = uint32_t(layers.size());
        endInfo.layers = layers.empty() ? nullptr : layers.data();

        std::mutex *queueMutex = presentWorker ? plume::VulkanGetQueueMutex(presentWorker->device, graphicsQueueFamily_, graphicsQueueIndex_) : nullptr;
        if (queueMutex != nullptr) {
            std::scoped_lock<std::mutex> qLock(*queueMutex);
            xrEndFrame(session_, &endInfo);
        } else {
            xrEndFrame(session_, &endInfo);
        }
    }

private:
    struct SwapchainState {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<VkImage> images;
    };

    static OpenXRBridge &self() {
        static OpenXRBridge s;
        return s;
    }

    static VkResult hookCreateVulkanInstance(const VkInstanceCreateInfo *createInfo, PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance *outInstance) {
        OpenXRBridge &b = self();
        XrVulkanInstanceCreateInfoKHR xrVk{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xrVk.systemId = b.systemId_;
        xrVk.pfnGetInstanceProcAddr = getInstanceProcAddr;
        xrVk.vulkanCreateInfo = createInfo;
        VkResult vkRes = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xrRes = b.pfnCreateVkInstance_(b.xrInstance_, &xrVk, outInstance, &vkRes);
        if (XR_SUCCEEDED(xrRes) && vkRes == VK_SUCCESS && outInstance != nullptr) {
            b.vkInstance_ = *outInstance;
        }
        return XR_SUCCEEDED(xrRes) ? vkRes : VK_ERROR_INITIALIZATION_FAILED;
    }

    static VkResult hookPickVulkanPhysicalDevice(VkInstance instance, VkPhysicalDevice *outPhysicalDevice) {
        OpenXRBridge &b = self();
        XrVulkanGraphicsDeviceGetInfoKHR getInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        getInfo.systemId = b.systemId_;
        getInfo.vulkanInstance = instance;
        return XR_SUCCEEDED(b.pfnGetVkDevice2_(b.xrInstance_, &getInfo, outPhysicalDevice)) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }

    static VkResult hookCreateVulkanDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *createInfo, PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkDevice *outDevice) {
        OpenXRBridge &b = self();
        XrVulkanDeviceCreateInfoKHR xrDev{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xrDev.systemId = b.systemId_;
        xrDev.pfnGetInstanceProcAddr = getInstanceProcAddr;
        xrDev.vulkanPhysicalDevice = physicalDevice;
        xrDev.vulkanCreateInfo = createInfo;
        VkResult vkRes = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xrRes = b.pfnCreateVkDevice_(b.xrInstance_, &xrDev, outDevice, &vkRes);
        return XR_SUCCEEDED(xrRes) ? vkRes : VK_ERROR_INITIALIZATION_FAILED;
    }

    static void hookOnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex) {
        self().onVulkanDeviceReady(instance, physicalDevice, device, queueFamilyIndex, queueIndex);
    }

    void onVulkanDeviceReady(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex) {
        std::scoped_lock<std::mutex> lock(lifecycleMutex_);
        if (session_ != XR_NULL_HANDLE) {
            return;
        }

        vkInstance_ = instance;
        vkPhysicalDevice_ = physicalDevice;
        vkDevice_ = device;
        graphicsQueueFamily_ = queueFamilyIndex;
        graphicsQueueIndex_ = queueIndex;

        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = vkInstance_;
        binding.physicalDevice = vkPhysicalDevice_;
        binding.device = vkDevice_;
        binding.queueFamilyIndex = graphicsQueueFamily_;
        binding.queueIndex = graphicsQueueIndex_;

        XrSessionCreateInfo sessInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessInfo.next = &binding;
        sessInfo.systemId = systemId_;
        const XrResult sessRes = xrCreateSession(xrInstance_, &sessInfo, &session_);
        if (XR_FAILED(sessRes) || session_ == XR_NULL_HANDLE) {
            std::fprintf(stderr, "[SWE1R-VR] xrCreateSession failed: %d\n", int(sessRes));
            return;
        }

        configureQuestRuntime();

        XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace.orientation.w = 1.0f;
        xrCreateReferenceSpace(session_, &localInfo, &localSpace_);

        XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
        xrCreateReferenceSpace(session_, &viewInfo, &viewSpace_);

        const VkFormat swapFormat = pickSwapchainFormat();
        for (int e = 0; e < 2; ++e) {
            if (!createSwapchain(eyeWidth_, eyeHeight_, swapFormat, eyeSwapchains_[e])) {
                std::fprintf(stderr, "[SWE1R-VR] Failed creating eye %d swapchain\n", e);
                return;
            }
        }
        if (!createSwapchain(hudWidth_, hudHeight_, swapFormat, hudSwapchain_)) {
            std::fprintf(stderr, "[SWE1R-VR] Failed creating HUD swapchain\n");
            return;
        }

        vrInputInitialize(xrInstance_, session_);
        RT64::SetVRBridge(this);
        active_.store(true, std::memory_order_release);
        std::printf("[SWE1R-VR] OpenXR session active (%ux%u per eye, format %d)\n", eyeWidth_, eyeHeight_, int(swapFormat));
    }

    void configureQuestRuntime() {
        if (hasRefreshRateExt_) {
            PFN_xrRequestDisplayRefreshRateFB pfnReqRate = nullptr;
            if (XR_SUCCEEDED(xrGetInstanceProcAddr(xrInstance_, "xrRequestDisplayRefreshRateFB",
                                                   reinterpret_cast<PFN_xrVoidFunction *>(&pfnReqRate))) &&
                pfnReqRate != nullptr) {
                pfnReqRate(session_, config.targetRefreshRate);
            }
        }
        if (hasPerfExt_) {
            PFN_xrPerfSettingsSetPerformanceLevelEXT pfnPerf = nullptr;
            if (XR_SUCCEEDED(xrGetInstanceProcAddr(xrInstance_, "xrPerfSettingsSetPerformanceLevelEXT",
                                                   reinterpret_cast<PFN_xrVoidFunction *>(&pfnPerf))) &&
                pfnPerf != nullptr) {
                pfnPerf(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
                pfnPerf(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
            }
        }
        if (hasColorSpaceExt_) {
            PFN_xrEnumerateColorSpacesFB pfnEnumCS = nullptr;
            PFN_xrSetColorSpaceFB pfnSetCS = nullptr;
            xrGetInstanceProcAddr(xrInstance_, "xrEnumerateColorSpacesFB", reinterpret_cast<PFN_xrVoidFunction *>(&pfnEnumCS));
            xrGetInstanceProcAddr(xrInstance_, "xrSetColorSpaceFB", reinterpret_cast<PFN_xrVoidFunction *>(&pfnSetCS));
            if (pfnEnumCS != nullptr && pfnSetCS != nullptr) {
                uint32_t csCount = 0;
                if (XR_SUCCEEDED(pfnEnumCS(session_, 0, &csCount, nullptr)) && csCount > 0) {
                    std::vector<XrColorSpaceFB> spaces(csCount);
                    if (XR_SUCCEEDED(pfnEnumCS(session_, csCount, &csCount, spaces.data()))) {
                        for (XrColorSpaceFB candidate : {XR_COLOR_SPACE_QUEST_FB, XR_COLOR_SPACE_REC709_FB}) {
                            if (std::find(spaces.begin(), spaces.end(), candidate) != spaces.end()) {
                                pfnSetCS(session_, candidate);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    VkFormat pickSwapchainFormat() {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        if (count > 0) {
            xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
        }
        // RT64 writes sRGB-encoded values into UNORM targets; UNORM swapchain avoids double-gamma washout.
        for (VkFormat preferred : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
            if (std::find(formats.begin(), formats.end(), int64_t(preferred)) != formats.end()) {
                return preferred;
            }
        }
        return count > 0 ? VkFormat(formats[0]) : VK_FORMAT_R8G8B8A8_UNORM;
    }

    bool createSwapchain(uint32_t w, uint32_t h, VkFormat format, SwapchainState &out) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = int64_t(format);
        ci.sampleCount = 1;
        ci.width = w;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session_, &ci, &out.handle)) || out.handle == XR_NULL_HANDLE) {
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(out.handle, 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> vkImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        xrEnumerateSwapchainImages(out.handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(vkImages.data()));
        out.images.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            out.images[i] = vkImages[i].image;
        }
        return imageCount > 0;
    }

    bool acquireAndWaitSwapchain(const SwapchainState &sc, uint32_t *outIndex) {
        if (sc.handle == XR_NULL_HANDLE || sc.images.empty()) {
            return false;
        }
        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &acq, outIndex))) {
            return false;
        }
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &waitInfo))) {
            return false;
        }
        return *outIndex < sc.images.size();
    }

    void pollEvents() {
        while (true) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const XrResult res = xrPollEvent(xrInstance_, &event);
            if (res == XR_EVENT_UNAVAILABLE || XR_FAILED(res)) {
                break;
            }
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto *sc = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
                switch (sc->state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &beginInfo))) {
                        sessionRunning_ = true;
                        recenterPending_.store(true, std::memory_order_release);
                        std::printf("[SWE1R-VR] xrBeginSession succeeded\n");
                    }
                    break;
                }
                case XR_SESSION_STATE_SYNCHRONIZED:
                    sessionFocused_ = false;
                    break;
                case XR_SESSION_STATE_VISIBLE:
                    sessionFocused_ = false;
                    break;
                case XR_SESSION_STATE_FOCUSED:
                    sessionFocused_ = true;
                    break;
                case XR_SESSION_STATE_STOPPING:
                    sessionFocused_ = false;
                    if (sessionRunning_) {
                        xrEndSession(session_);
                        sessionRunning_ = false;
                    }
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    sessionFocused_ = false;
                    sessionRunning_ = false;
                    ultramodern::quit();
                    break;
                default:
                    break;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                sessionRunning_ = false;
                ultramodern::quit();
            }
        }
    }

    void updateTrackedSnapshot(const std::array<XrView, 2> &rawViews, bool posValid) {
        Pose midHead{};
        midHead.orientation = normalizeQuat({
            rawViews[0].pose.orientation.x,
            rawViews[0].pose.orientation.y,
            rawViews[0].pose.orientation.z,
            rawViews[0].pose.orientation.w,
        });
        midHead.position = posValid ? Vec3{
            0.5f * (rawViews[0].pose.position.x + rawViews[1].pose.position.x),
            0.5f * (rawViews[0].pose.position.y + rawViews[1].pose.position.y),
            0.5f * (rawViews[0].pose.position.z + rawViews[1].pose.position.z),
        } : Vec3{0.0f, 0.0f, 0.0f};

        std::scoped_lock<std::mutex> lock(viewMutex_);
        if (!hasRecenterOrigin_ || recenterPending_.exchange(false, std::memory_order_acquire)) {
            recenterOrigin_ = makeRecenterOrigin(midHead);
            hasRecenterOrigin_ = true;
            headPosFilter_.reset();
        }

        const Vec3 headPosFiltered = config.enableNoseBobFilter ? headPosFilter_.filter(midHead.position) : midHead.position;
        const Vec3 headPosDelta = midHead.position - headPosFiltered;

        RT64::VRViewPoseSnapshot snap{};
        snap.valid = true;
        for (int e = 0; e < 2; ++e) {
            Pose eyePose{};
            eyePose.orientation = normalizeQuat({
                rawViews[e].pose.orientation.x,
                rawViews[e].pose.orientation.y,
                rawViews[e].pose.orientation.z,
                rawViews[e].pose.orientation.w,
            });
            eyePose.position = posValid ? Vec3{
                rawViews[e].pose.position.x - headPosDelta.x,
                rawViews[e].pose.position.y - headPosDelta.y,
                rawViews[e].pose.position.z - headPosDelta.z,
            } : Vec3{e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f};

            const Pose rel = computeEyeRelativePose(eyePose, recenterOrigin_);
            snap.eyes[e].orientation[0] = rel.orientation.x;
            snap.eyes[e].orientation[1] = rel.orientation.y;
            snap.eyes[e].orientation[2] = rel.orientation.z;
            snap.eyes[e].orientation[3] = rel.orientation.w;
            snap.eyes[e].position[0] = rel.position.x;
            snap.eyes[e].position[1] = rel.position.y;
            snap.eyes[e].position[2] = rel.position.z;
            snap.eyes[e].fov[0] = rawViews[e].fov.angleLeft;
            snap.eyes[e].fov[1] = rawViews[e].fov.angleRight;
            snap.eyes[e].fov[2] = rawViews[e].fov.angleUp;
            snap.eyes[e].fov[3] = rawViews[e].fov.angleDown;
        }
        latestSnapshot_ = snap;
    }

    Pose toLocalSpacePose(const Pose &relPose) const {
        std::scoped_lock<std::mutex> lock(viewMutex_);
        if (!hasRecenterOrigin_) {
            return relPose;
        }
        Pose local{};
        local.orientation = normalizeQuat(mulQuat(recenterOrigin_.orientation, relPose.orientation));
        local.position = recenterOrigin_.position + rotateVecByQuat(recenterOrigin_.orientation, relPose.position);
        return local;
    }

    Pose getRecenterOriginSnapshot() const {
        std::scoped_lock<std::mutex> lock(viewMutex_);
        if (!hasRecenterOrigin_) {
            return Pose{};
        }
        return recenterOrigin_;
    }

    void cleanupInstanceOnly() {
        if (xrInstance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(xrInstance_);
            xrInstance_ = XR_NULL_HANDLE;
        }
        systemId_ = XR_NULL_SYSTEM_ID;
    }

    friend bool SWE1RVR::initializeAndroid();
    friend void SWE1RVR::shutdown();
    friend bool SWE1RVR::isActive();
    friend VRConfig &SWE1RVR::getConfig();
    friend void SWE1RVR::requestRecenter();
    friend void SWE1RVR::setRumble(bool enable);

    std::mutex lifecycleMutex_;
    bool initialized_ = false;
    std::atomic<bool> active_{false};
    bool sessionRunning_ = false;
    bool sessionFocused_ = false;
    std::atomic<bool> recenterPending_{true};
    std::atomic<bool> rumbleActive_{false};

    jobject appActivity_ = nullptr;
    XrInstance xrInstance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrTime predictedDisplayTime_ = 0;

    bool hasRefreshRateExt_ = false;
    bool hasPerfExt_ = false;
    bool hasColorSpaceExt_ = false;

    PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVkReqs2_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR pfnCreateVkInstance_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVkDevice2_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR pfnCreateVkDevice_ = nullptr;

    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice_ = VK_NULL_HANDLE;
    VkDevice vkDevice_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = 0;
    uint32_t graphicsQueueIndex_ = 0;

    uint32_t eyeWidth_ = 1440;
    uint32_t eyeHeight_ = 1440;
    uint32_t hudWidth_ = 1280;
    uint32_t hudHeight_ = 960;
    SwapchainState eyeSwapchains_[2];
    SwapchainState hudSwapchain_;

    mutable std::mutex viewMutex_;
    bool hasRecenterOrigin_ = false;
    Pose recenterOrigin_{};
    LowPassVec3 headPosFilter_{0.35f};
    RT64::VRViewPoseSnapshot latestSnapshot_{};
};

} // namespace

bool initializeAndroid() {
    return OpenXRBridge::self().initializeAndroid();
}

void shutdown() {
    OpenXRBridge::self().shutdown();
}

bool isActive() {
    return OpenXRBridge::self().isActive();
}

VRConfig &getConfig() {
    return OpenXRBridge::self().config;
}

void getInputState(uint16_t &outButtons, float &outStickX, float &outStickY) {
    const VRInputState s = getVRInputSnapshot();
    outButtons = s.buttons;
    outStickX = s.stickX;
    outStickY = s.stickY;
}

void setRumble(bool enable) {
    OpenXRBridge::self().setRumble(enable);
}

void requestRecenter() {
    OpenXRBridge::self().requestRecenter();
}

} // namespace SWE1RVR

#else

namespace SWE1RVR {

static VRConfig g_desktopConfig{};

bool initializeAndroid() { return false; }
void shutdown() {}
bool isActive() { return false; }
VRConfig &getConfig() { return g_desktopConfig; }
void getInputState(uint16_t &outButtons, float &outStickX, float &outStickY) {
    outButtons = 0;
    outStickX = 0.0f;
    outStickY = 0.0f;
}
void setRumble(bool) {}
void requestRecenter() {}

} // namespace SWE1RVR

#endif
