#include "mod/FlexGravityMod.h"

namespace fg::vt {

std::string SafeName(CKObject *object) {
    if (!object) {
        return "<null>";
    }
    CKSTRING name = object->GetName();
    return name && *name ? std::string(name) : std::string("<unnamed>");
}

std::string IdText(CKObject *object) {
    if (!object) {
        return "0";
    }
    return std::to_string(static_cast<unsigned long>(object->GetID()));
}

std::string ClassText(CKObject *object) {
    if (!object) {
        return "class=0";
    }
    return "class=" + std::to_string(static_cast<int>(object->GetClassID()));
}

std::string ObjectText(CKObject *object) {
    if (!object) {
        return "<null>";
    }
    return SafeName(object) + " (id=" + IdText(object) + ", " + ClassText(object) + ")";
}

const char *ArrayTypeName(CK_ARRAYTYPE type) {
    switch (type) {
        case CKARRAYTYPE_INT: return "int";
        case CKARRAYTYPE_FLOAT: return "float";
        case CKARRAYTYPE_STRING: return "string";
        case CKARRAYTYPE_OBJECT: return "object";
        case CKARRAYTYPE_PARAMETER: return "parameter";
        default: return "unknown";
    }
}

// The Virtools 2.1 SDK headers are older than the engine shipped with
// Ballance, so every virtual call into CKCurve is guarded.  A failed probe
// must never take the game down.
int SafeCurvePointCount(CKCurve *curve) {
    __try {
        return curve->GetControlPointCount();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

CKCurvePoint *SafeCurveControlPoint(CKCurve *curve, int index) {
    __try {
        return curve->GetControlPoint(index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

CKCurve *SafeCurveCast(CK3dEntity *entity) {
    __try {
        return CKCurve::Cast(entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafeWorldPosition(CK3dEntity *entity, VxVector &out) {
    __try {
        out = entity->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = VxVector(0.0f, 0.0f, 0.0f);
        return false;
    }
}

int SafeCurveIsOpen(CKCurve *curve) {
    __try {
        return curve->IsOpen() ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

namespace {

bool HasPlaceholderPrefixBytes(const std::string &raw) {
    return raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0x01 && raw[1] == '_' &&
           raw[2] == '_';
}

// Object names come from the Virtools object table in the system ANSI/GBK code
// page; the log is UTF-8.  ASCII names pass through unchanged.
std::string AnsiToUtf8(const std::string &input) {
    if (input.empty()) {
        return input;
    }
    const int length = static_cast<int>(input.size());
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, input.c_str(), length, nullptr, 0);
    if (wideLength <= 0) {
        return input;
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, input.c_str(), length, &wide[0], wideLength) <= 0) {
        return input;
    }
    const int utf8Length =
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return input;
    }
    std::string out(static_cast<size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLength, &out[0], utf8Length, nullptr,
                            nullptr) <= 0) {
        return input;
    }
    return out;
}

std::string SanitizeControlBytes(const std::string &text) {
    std::string out = text;
    for (char &c : out) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 0x20 && value != '\t') {
            c = '?';
        }
    }
    return out;
}

} // namespace

bool HasPlaceholderPrefix(const std::string &raw) {
    return HasPlaceholderPrefixBytes(raw);
}

std::string StripPlaceholderPrefix(const std::string &raw) {
    return HasPlaceholderPrefixBytes(raw) ? raw.substr(3) : raw;
}

std::string DisplayName(const std::string &raw) {
    return SanitizeControlBytes(AnsiToUtf8(StripPlaceholderPrefix(raw)));
}

std::string DisplayObjectName(CKObject *object) {
    if (!object) {
        return "<null>";
    }
    CKSTRING name = object->GetName();
    return name && *name ? DisplayName(std::string(name)) : std::string("<unnamed>");
}

} // namespace fg::vt

