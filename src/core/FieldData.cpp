#include "core/FieldData.h"

namespace fg::core {

const char *SourceKindName(SourceInfo::Kind kind) {
    switch (kind) {
        case SourceInfo::KindMonopole: return "M";
        case SourceInfo::KindLine: return "L";
        default: return "S";
    }
}

} // namespace fg::core
