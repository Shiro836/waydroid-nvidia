#pragma once
#include <aidl/android/hardware/graphics/common/XyColor.h>
namespace aidl::android::hardware::graphics::common {
struct Smpte2086 { XyColor primaryRed; XyColor primaryGreen; XyColor primaryBlue; XyColor whitePoint; float maxLuminance = 0.f; float minLuminance = 0.f; };
}
