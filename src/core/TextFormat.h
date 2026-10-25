#pragma once

#include <cstdio>
#include <string>

#include <VxVector.h>

namespace fg::core {

inline std::string BoolText(bool value) {
    return value ? "true" : "false";
}

inline std::string FormatVec(const VxVector &value) {
    char buffer[128] = {};
    snprintf(buffer, sizeof(buffer), "(%.4f, %.4f, %.4f)", value.x, value.y, value.z);
    return std::string(buffer);
}

} // namespace fg::core
