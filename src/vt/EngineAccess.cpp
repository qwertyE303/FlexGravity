#include "mod/FlexGravityMod.h"

namespace {

void AppendWarning(core::ScanInfo &scan, const std::string &message) {
    scan.warnings.push_back(message);
}

// FG_CField function cells are ASCII and the editor does not truncate them.  A
// cell larger than this is treated as corrupt instead of being copied.
constexpr int kMaxCellStringBytes = 8192;

} // namespace

void FlexGravityMod::LogInfo(const char *fmt, ...) {
    if (!m_BML || !LogEnabled()) {
        return;
    }
    ILogger *logger = GetLogger();
    if (!logger) {
        return;
    }

    char buffer[2048] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    logger->Info("[FlexGravity] %s", buffer);
}

void FlexGravityMod::LogWarn(const char *fmt, ...) {
    if (!m_BML || !LogEnabled()) {
        return;
    }
    ILogger *logger = GetLogger();
    if (!logger) {
        return;
    }

    char buffer[2048] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    logger->Warn("[FlexGravity] %s", buffer);
}

void FlexGravityMod::LogAlways(const char *fmt, ...) {
    if (!m_BML) {
        return;
    }
    ILogger *logger = GetLogger();
    if (!logger) {
        return;
    }

    char buffer[2048] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    logger->Warn("[FlexGravity] %s", buffer);
}

void FlexGravityMod::Warn(const std::string &message) {
    AppendWarning(m_Scan, message);
    LogWarn("%s", message.c_str());
}

void FlexGravityMod::SendMessage(const std::string &message) {
    if (m_BML) {
        m_BML->SendIngameMessage(("[FlexGravity] " + message).c_str());
    }
}

// ---------------------------------------------------------------------------
// VT lookup helpers
// ---------------------------------------------------------------------------

CKGroup *FlexGravityMod::FindGroup(const char *name) {
    CKContext *context = Context();
    if (!context || !name) {
        return nullptr;
    }
    CKObject *object = context->GetObjectByNameAndClass(const_cast<char *>(name), CKCID_GROUP, nullptr);
    return CKGroup::Cast(object);
}

CKDataArray *FlexGravityMod::FindArray(const char *name) {
    CKContext *context = Context();
    if (!context || !name) {
        return nullptr;
    }
    CKObject *object = context->GetObjectByNameAndClass(const_cast<char *>(name), CKCID_DATAARRAY, nullptr);
    return CKDataArray::Cast(object);
}

int FlexGravityMod::FindColumn(CKDataArray *array, const char *name) {
    if (!array || !name) {
        return -1;
    }
    for (int column = 0; column < array->GetColumnCount(); ++column) {
        char *columnName = array->GetColumnName(column);
        if (columnName && _stricmp(columnName, name) == 0) {
            return column;
        }
    }
    return -1;
}

CKObject *FlexGravityMod::GetCellObject(CKDataArray *array, int row, int column) {
    if (!array || row < 0 || column < 0 || column >= array->GetColumnCount()) {
        return nullptr;
    }

    const CK_ARRAYTYPE type = array->GetColumnType(column);
    if (type == CKARRAYTYPE_OBJECT) {
        return array->GetElementObject(row, column);
    }
    if (type == CKARRAYTYPE_STRING) {
        char buffer[512] = {};
        array->GetElementStringValue(row, column, buffer);
        if (buffer[0] == '\0') {
            return nullptr;
        }
        CKContext *context = Context();
        if (!context) {
            return nullptr;
        }
        return context->GetObjectByNameAndClass(buffer, CKCID_3DENTITY, nullptr);
    }
    return nullptr;
}

float FlexGravityMod::GetCellFloat(CKDataArray *array, int row, int column, float fallback) {
    if (!array || row < 0 || column < 0 || column >= array->GetColumnCount()) {
        return fallback;
    }

    const CK_ARRAYTYPE type = array->GetColumnType(column);
    if (type == CKARRAYTYPE_FLOAT) {
        float value = fallback;
        if (array->GetElementValue(row, column, &value)) {
            return value;
        }
    } else if (type == CKARRAYTYPE_INT) {
        int value = static_cast<int>(fallback);
        if (array->GetElementValue(row, column, &value)) {
            return static_cast<float>(value);
        }
    }
    return fallback;
}

bool FlexGravityMod::GetCellInt(CKDataArray *array, int row, int column, int &value) {
    if (!array || row < 0 || column < 0 || column >= array->GetColumnCount()) {
        return false;
    }

    const CK_ARRAYTYPE type = array->GetColumnType(column);
    if (type == CKARRAYTYPE_INT) {
        int raw = value;
        if (array->GetElementValue(row, column, &raw)) {
            value = raw;
            return true;
        }
    } else if (type == CKARRAYTYPE_FLOAT) {
        // The specification says Mode is an int; a float column is an authoring
        // mistake that still round-trips instead of losing the row.
        float raw = static_cast<float>(value);
        if (array->GetElementValue(row, column, &raw)) {
            value = static_cast<int>(std::lround(static_cast<double>(raw)));
            return true;
        }
    }
    return false;
}

// Dynamic string read.  GetElementStringValue has no buffer-size parameter, so
// nullptr is used to ask for the required size (the terminating NUL included),
// then the same call fills an exactly sized buffer.  This is the same two-call
// protocol BML itself uses for DataArray strings.
bool FlexGravityMod::GetCellString(CKDataArray *array, int row, int column, std::string &out) {
    out.clear();
    if (!array || row < 0 || column < 0 || column >= array->GetColumnCount()) {
        return false;
    }
    if (array->GetColumnType(column) != CKARRAYTYPE_STRING) {
        return false;
    }

    const int length = array->GetElementStringValue(row, column, nullptr);
    if (length <= 0) {
        return true; // empty cell
    }
    if (length > kMaxCellStringBytes) {
        return false;
    }

    out.assign(static_cast<size_t>(length) + 1, '\0'); // +1: never trust the off-by-one
    array->GetElementStringValue(row, column, &out[0]);
    out.resize(std::strlen(out.c_str())); // trims whichever NUL convention is in use
    return true;
}

// One line per array instead of one line per column: a 9-column array used to
// cost 10 lines, now it costs 2 (header + this).
void FlexGravityMod::LogArrayColumns(CKDataArray *array, const char *label) {
    if (!array) {
        return;
    }

    std::string columns;
    for (int column = 0; column < array->GetColumnCount(); ++column) {
        if (!columns.empty()) {
            columns += ", ";
        }
        char *name = array->GetColumnName(column);
        columns += name ? name : "<null>";
        columns += ":";
        columns += ArrayTypeName(array->GetColumnType(column));
    }

    LogInfo("Array %s columns=%d rows=%d [%s]", label, array->GetColumnCount(),
            array->GetRowCount(), columns.c_str());
}

// ---------------------------------------------------------------------------
// Ball lookup
// ---------------------------------------------------------------------------

CK3dEntity *FlexGravityMod::FindActiveBall() {
    if (!m_BML) {
        return nullptr;
    }
    CKDataArray *currentLevel = m_BML->GetArrayByName("CurrentLevel");
    if (currentLevel) {
        const int column = FindColumn(currentLevel, "ActiveBall");
        if (column >= 0) {
            if (CK3dEntity *ball = CK3dEntity::Cast(currentLevel->GetElementObject(0, column))) {
                return ball;
            }
        }
    }

    // Fallback: CurrentLevel/ActiveBall is the documented source, but if the
    // array or column is missing the overlay would silently draw nothing.
    // Prefer a physicalized body whose name starts with "Ball", then any body
    // that was physicalized with a ball collider.
    CK3dEntity *ballCollider = nullptr;
    for (const auto &entry : m_Bodies) {
        const BodyInfo &body = entry.second;
        if (body.fixed || !body.physicalized) {
            continue;
        }
        if (m_BallRadii.find(body.objectId) == m_BallRadii.end()) {
            continue;
        }
        CK3dEntity *entity = ResolveEntity(body.objectId);
        if (!entity) {
            continue;
        }
        if (body.objectName.compare(0, 4, "Ball") == 0) {
            return entity;
        }
        if (!ballCollider) {
            ballCollider = entity;
        }
    }
    return ballCollider;
}

// Mass is only known after OnPhysicalize; the fallback is the wood ball's mass
// and is only used for the level-start diagnostic line.
float FlexGravityMod::GetBodyMass(CK3dEntity *entity, bool *found) {
    if (found) {
        *found = false;
    }
    if (!entity) {
        return 0.0f;
    }

    auto it = m_Bodies.find(entity->GetID());
    if (it != m_Bodies.end() && it->second.physicalized && it->second.mass > 0.0f) {
        if (found) {
            *found = true;
        }
        return it->second.mass;
    }
    return 2.0f;
}

// ---------------------------------------------------------------------------
// Region geometry
// ---------------------------------------------------------------------------
CK3dEntity *FlexGravityMod::ResolveEntity(CK_ID objectId) {
    CKContext *context = Context();
    if (!context || objectId == 0) {
        return nullptr;
    }
    return CK3dEntity::Cast(context->GetObject(objectId));
}

