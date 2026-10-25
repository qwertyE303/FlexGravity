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

} // namespace fg::vt

