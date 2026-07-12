#pragma once
#include <cstdint>
namespace aidl::android::hardware::graphics::common {
enum class ChromaSiting : int64_t { NONE = 0, UNKNOWN = 1, SITED_INTERSTITIAL = 2, COSITED_HORIZONTAL = 3 };
}
