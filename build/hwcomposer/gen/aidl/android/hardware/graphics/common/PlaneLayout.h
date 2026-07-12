#pragma once
#include <cstdint>
#include <vector>
#include <aidl/android/hardware/graphics/common/PlaneLayoutComponent.h>
namespace aidl::android::hardware::graphics::common {
struct PlaneLayout {
  std::vector<PlaneLayoutComponent> components;
  int64_t offsetInBytes = 0; int64_t sampleIncrementInBits = 0; int64_t strideInBytes = 0;
  int64_t widthInSamples = 0; int64_t heightInSamples = 0; int64_t totalSizeInBytes = 0;
  int64_t horizontalSubsampling = 0; int64_t verticalSubsampling = 0;
};
}
