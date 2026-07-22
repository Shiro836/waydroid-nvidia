#pragma once
#include <cstdint>
#include <string>
namespace aidl::android::hardware::graphics::common {
struct ExtendableType { std::string name; int64_t value = 0; };
}
