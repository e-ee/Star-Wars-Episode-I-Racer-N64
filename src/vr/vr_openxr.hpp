#pragma once

#include "vr_config.hpp"

#include <cstdint>

namespace SWE1RVR {

bool initializeAndroid();
void shutdown();
bool isActive();
VRConfig &getConfig();
void getInputState(uint16_t &outButtons, float &outStickX, float &outStickY);
void setRumble(bool enable);
void requestRecenter();

} // namespace SWE1RVR
