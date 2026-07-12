#pragma once
#include <cstdint>
#include <aidl/android/hardware/graphics/common/ExtendableType.h>
namespace aidl::android::hardware::graphics::common {
struct PlaneLayoutComponent { ExtendableType type; int64_t offsetInBits = 0; int64_t sizeInBits = 0; };
}
