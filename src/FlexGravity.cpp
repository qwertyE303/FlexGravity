// FlexGravity - BML+ 0.3.13 native mod for map-authored gravity fields.
//
// 0.4.0 P3:
//   - new naming: FG_U / FG_M / FG_L / FG_S fields, FG_R / FG_RG scopes
//   - FG_M monopole physics: g = G / r^2, positive G attracts, negative repels
//   - FG_L / FG_S are scanned and their geometry is parsed + logged; their
//     physics lands in P4 / P5
//   - configuration: Debug/DebugMode only, ForceScale is fixed to 66

#include <windows.h>

#include <BML/IMod.h>
#include <BML/IBML.h>
#include <BML/ICommand.h>
#include <BML/IConfig.h>
#include <BML/ILogger.h>
#include <BML/Defines.h>
#include <BML/ExecuteBB.h>
#include <BML/ScriptHelper.h>

#include <CKAll.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *kGroupU = "FG_U";
constexpr const char *kGroupM = "FG_M";
constexpr const char *kGroupL = "FG_L";
constexpr const char *kGroupS = "FG_S";
constexpr const char *kGroupR = "FG_R";

constexpr const char *kArrayU = "FG_U";
constexpr const char *kArrayM = "FG_M";
constexpr const char *kArrayL = "FG_L";
constexpr const char *kArrayS = "FG_S";
constexpr const char *kArrayR = "FG_R";
constexpr const char *kArrayRG = "FG_RG";

// IPion's fixed PSI step is 1/66 s.  SetPhysicsForce's raw value is the
// per-step impulse, so raw = mass * acceleration / 66.
constexpr float kForceScale = 66.0f;

// Only guards divisions; it is not a clamp and not a gameplay parameter.
constexpr float kTinyDistance = 1.0e-6f;

constexpr int kMaxCurvePoints = 4096;

// ---------------------------------------------------------------------------
// Parsed data
// ---------------------------------------------------------------------------

struct RegionInfo {
    VxBbox localBounds;
    std::vector<VxVector> localVertices;
    std::vector<int> localIndices;
    bool meshValid = false;
    bool bboxValid = false;
};

struct UFieldInfo {
    CK_ID objectId = 0;
    std::string objectName;
    VxVector gravity = VxVector(0.0f, 0.0f, 0.0f);
    RegionInfo region;
};

struct SourceInfo {
    enum Kind {
        KindMonopole = 0,
        KindLine = 1,
        KindSheet = 2
    };

    CK_ID objectId = 0;
    std::string objectName;
    Kind kind = KindMonopole;
    float strength = 0.0f;

    // Line: polyline in the object's local space (curve control points).
    std::vector<VxVector> localPoints;
    bool closed = false;
    int controlPointCount = 0;
    bool usedCurve = false;

    // Sheet: local-space rectangle.
    VxVector localCenter = VxVector(0.0f, 0.0f, 0.0f);
    VxVector localAxisA = VxVector(1.0f, 0.0f, 0.0f);
    VxVector localAxisB = VxVector(0.0f, 1.0f, 0.0f);
    VxVector localNormal = VxVector(0.0f, 0.0f, 1.0f);
    float halfA = 0.0f;
    float halfB = 0.0f;

    bool geometryValid = false;
};

struct BodyInfo {
    CK_ID objectId = 0;
    std::string objectName;
    float mass = 0.0f;
    bool fixed = false;
    bool physicalized = false;
};

struct ForceChannel {
    CK_ID objectId = 0;
    CKBehavior *behavior = nullptr;
    bool active = false;
    VxVector lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    float lastMass = 0.0f;
    bool needsRebuild = false;
};

struct RRowInfo {
    CK_ID regionObjectId = 0;
    std::string regionName;
    RegionInfo region;
    std::vector<CK_ID> sourceIds;
    std::vector<std::string> sourceNames;
};

struct GroupScan {
    bool present = false;
    int count = 0;
};

struct ArrayScan {
    bool present = false;
    int rows = 0;
    int columns = 0;
};

struct ScanInfo {
    bool ran = false;

    GroupScan uGroup;
    GroupScan mGroup;
    GroupScan lGroup;
    GroupScan sGroup;
    GroupScan rGroup;

    ArrayScan uArray;
    ArrayScan mArray;
    ArrayScan lArray;
    ArrayScan sArray;
    ArrayScan rArray;
    ArrayScan rgArray;

    std::vector<UFieldInfo> uFields;
    std::vector<SourceInfo> sources;
    std::vector<RRowInfo> rRows;
    std::vector<CK_ID> globalSourceIds;
    std::vector<std::string> globalSourceNames;

    std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

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

std::string BoolText(bool value) {
    return value ? "true" : "false";
}

std::string FormatVec(const VxVector &value) {
    char buffer[128] = {};
    snprintf(buffer, sizeof(buffer), "(%.4f, %.4f, %.4f)", value.x, value.y, value.z);
    return std::string(buffer);
}

float Dot(const VxVector &a, const VxVector &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

VxVector Cross(const VxVector &a, const VxVector &b) {
    return VxVector(a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x);
}

VxVector Subtract(const VxVector &a, const VxVector &b) {
    return VxVector(a.x - b.x, a.y - b.y, a.z - b.z);
}

VxVector Add(const VxVector &a, const VxVector &b) {
    return VxVector(a.x + b.x, a.y + b.y, a.z + b.z);
}

VxVector Scale(const VxVector &a, float factor) {
    return VxVector(a.x * factor, a.y * factor, a.z * factor);
}

float LengthSquared(const VxVector &a) {
    return a.x * a.x + a.y * a.y + a.z * a.z;
}

bool Normalize(const VxVector &value, VxVector &out) {
    const float lengthSquared = LengthSquared(value);
    if (lengthSquared <= 1.0e-12f) {
        out = VxVector(0.0f, 0.0f, 0.0f);
        return false;
    }
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    out = VxVector(value.x * inverse, value.y * inverse, value.z * inverse);
    return true;
}

float GetComponent(const VxVector &value, int axis) {
    switch (axis) {
        case 0: return value.x;
        case 1: return value.y;
        default: return value.z;
    }
}

void SetComponent(VxVector &value, int axis, float component) {
    switch (axis) {
        case 0: value.x = component; break;
        case 1: value.y = component; break;
        default: value.z = component; break;
    }
}

VxVector AxisVector(int axis) {
    switch (axis) {
        case 0: return VxVector(1.0f, 0.0f, 0.0f);
        case 1: return VxVector(0.0f, 1.0f, 0.0f);
        default: return VxVector(0.0f, 0.0f, 1.0f);
    }
}

bool BoundsContains(const VxBbox &box, const VxVector &point) {
    if (!box.IsValid()) {
        return false;
    }
    const float epsilon = 1.0e-4f;
    return point.x >= box.Min.x - epsilon && point.x <= box.Max.x + epsilon &&
           point.y >= box.Min.y - epsilon && point.y <= box.Max.y + epsilon &&
           point.z >= box.Min.z - epsilon && point.z <= box.Max.z + epsilon;
}

bool RayTriangle(const VxVector &origin, const VxVector &direction,
                 const VxVector &v0, const VxVector &v1, const VxVector &v2) {
    const VxVector edge1 = Subtract(v1, v0);
    const VxVector edge2 = Subtract(v2, v0);
    const VxVector p = Cross(direction, edge2);
    const float determinant = Dot(edge1, p);
    if (std::fabs(determinant) < 1.0e-7f) {
        return false;
    }

    const float inverse = 1.0f / determinant;
    const VxVector t = Subtract(origin, v0);
    const float u = Dot(t, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }

    const VxVector q = Cross(t, edge1);
    const float v = Dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }

    const float distance = Dot(edge2, q) * inverse;
    return distance > 1.0e-5f;
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

const char *SourceKindName(SourceInfo::Kind kind) {
    switch (kind) {
        case SourceInfo::KindMonopole: return "M";
        case SourceInfo::KindLine: return "L";
        default: return "S";
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

} // namespace

class FlexGravityMod;

class FlexGravityCommand final : public ICommand {
public:
    explicit FlexGravityCommand(FlexGravityMod *mod) : m_Mod(mod) {}

    std::string GetName() override { return "fg"; }
    std::string GetAlias() override { return "/fg"; }
    std::string GetDescription() override {
        return "FlexGravity console: fg debug <command>";
    }
    bool IsCheat() override { return false; }

    void Execute(IBML *bml, const std::vector<std::string> &args) override;
    const std::vector<std::string> GetTabCompletion(IBML *bml,
                                                     const std::vector<std::string> &args) override;

private:
    FlexGravityMod *m_Mod;
};

class FlexGravityMod final : public IMod {
public:
    explicit FlexGravityMod(IBML *bml) : IMod(bml) {}

    const char *GetID() override { return "FlexGravity"; }
    const char *GetVersion() override { return "0.1.0"; }
    const char *GetName() override { return "FlexGravity"; }
    const char *GetAuthor() override { return "FlexGravity"; }
    const char *GetDescription() override {
        return "Map-authored gravity fields: uniform, monopole, line and sheet.";
    }
    DECLARE_BML_VERSION

    void OnLoad() override;
    void OnUnload() override;

    void OnStartLevel() override;
    void OnPreLoadLevel() override;
    void OnPreExitLevel() override;

    void OnProcess() override;
    void OnModifyConfig(const char *category, const char *key, IProperty *prop) override;

    void OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float friction, float elasticity, float mass,
                       const char *collGroup, CKBOOL startFrozen, CKBOOL enableColl, CKBOOL calcMassCenter,
                       float linearDamp, float rotDamp, const char *collSurface, VxVector massCenter,
                       int convexCnt, CKMesh **convexMesh, int ballCnt, VxVector *ballCenter, float *ballRadius,
                       int concaveCnt, CKMesh **concaveMesh) override;
    void OnUnphysicalize(CK3dEntity *target) override;
    void OnDead() override;
    void OnPreResetLevel() override;

    void Scan(bool verbose);
    void PrintStatus(IBML *bml);
    void PrintList(IBML *bml);
    void Reload(IBML *bml);
    void PrintBall(IBML *bml);
    void PrintUStatus(IBML *bml);
    void PrintFieldStatus(IBML *bml);
    void SetManualForce(IBML *bml, const std::vector<std::string> &args);
    void ClearManualForce(IBML *bml);
    void RegisterDebugCommand();

    bool IsDebugMode() const { return m_DebugMode; }

private:
    CKContext *Context() const { return m_BML ? m_BML->GetCKContext() : nullptr; }

    void LogInfo(const char *fmt, ...);
    void LogWarn(const char *fmt, ...);
    void Warn(const std::string &message);
    void SendMessage(const std::string &message);

    CKGroup *FindGroup(const char *name);
    CKDataArray *FindArray(const char *name);
    CKObject *GetCellObject(CKDataArray *array, int row, int column);
    float GetCellFloat(CKDataArray *array, int row, int column, float fallback);
    int FindColumn(CKDataArray *array, const char *name);
    void LogArrayColumns(CKDataArray *array, const char *label);

    void ScanGroup(const char *name, GroupScan &result,
                   std::unordered_map<CK_ID, std::string> &members);
    void ScanUArray(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanSourceArray(SourceInfo::Kind kind, const char *arrayName, const char *strengthColumn,
                         ArrayScan &arrayScan, const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanRArray(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanRGArray();
    void ValidateMembership(const std::unordered_map<CK_ID, std::string> &uGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &mGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &lGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &sGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &rGroupMembers);

    const SourceInfo *FindSource(CK_ID objectId) const;

    CK3dEntity *FindActiveBall();
    CK3dEntity *ResolveEntity(CK_ID objectId);
    float GetBodyMass(CK3dEntity *entity, bool *found);
    void ApplyCurrentManualForce();
    void ApplyForceToBall(CK3dEntity *ball, const VxVector &acceleration, float mass);
    void ClearAppliedForce();
    void UpdateManualForce();

    void BuildRegionCache(CK3dEntity *entity, RegionInfo &region, const char *label);
    bool BuildLineGeometry(SourceInfo &source, CK3dEntity *entity);
    bool BuildSheetGeometry(SourceInfo &source, CK3dEntity *entity);
    std::vector<VxVector> BuildObjectSamples(CK3dEntity *entity);
    VxVector GetObjectCenter(CK3dEntity *entity);
    bool PointInRegionLocal(const RegionInfo &region, const VxVector &localPoint);
    bool ObjectTouchesRegion(const RegionInfo &region, CK_ID regionObjectId,
                             const std::vector<VxVector> &objectSamples);
    bool IsSourceActive(CK_ID objectId, const std::vector<char> &rowTouched) const;
    VxVector ComputeSourceAcceleration(const SourceInfo &source, const VxVector &center);
    VxVector ComputeSegmentAcceleration(const VxVector &a, const VxVector &b,
                                        const VxVector &center, float strength);
    VxVector ComputeSheetAcceleration(const SourceInfo &source, const VxVector &center);
    float DistanceToPolyline(const std::vector<VxVector> &points, bool closed, const VxVector &center);
    void UpdateFields();
    void ApplyChannelForce(CK3dEntity *entity, const BodyInfo &body, const VxVector &acceleration);
    void ClearChannelForce(ForceChannel &channel);
    void ClearAllFieldForces();
    void DropAllFieldForces();
    void DestroyForceChannels();
    void RebuildForceChannels();
    CKBehavior *GetOrCreateForceBehavior(ForceChannel &channel);

    ScanInfo m_Scan;
    bool m_DebugMode = false;

    std::unordered_map<CK_ID, BodyInfo> m_Bodies;

    bool m_ManualForceActive = false;
    VxVector m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    bool m_ForceApplied = false;
    CK_ID m_ForceObjectId = 0;
    bool m_MissingMassWarned = false;

    std::unordered_map<CK_ID, ForceChannel> m_ForceChannels;
    int m_LastAffectedCount = -1;
    bool m_ForceBackendFailed = false;
    bool m_PendingLineLogged = false;
    bool m_PendingSheetLogged = false;

    // While the engine is tearing the level down (reset / exit / reload) the
    // mod must not touch physics at all: executing a SetPhysicsForce behavior
    // or unsetting a force mid-teardown hangs the game's main thread.
    bool m_TearingDown = false;
    CKBehavior *m_OwnerScript = nullptr;
    bool m_PendingChannelRebuild = false;
    bool m_CommandRegistered = false;
};

namespace {

void AppendWarning(ScanInfo &scan, const std::string &message) {
    scan.warnings.push_back(message);
}

} // namespace

// ---------------------------------------------------------------------------
// Logging / messaging
// ---------------------------------------------------------------------------

void FlexGravityMod::LogInfo(const char *fmt, ...) {
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
    logger->Info("[FlexGravity] %s", buffer);
}

void FlexGravityMod::LogWarn(const char *fmt, ...) {
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

void FlexGravityMod::LogArrayColumns(CKDataArray *array, const char *label) {
    if (!array) {
        return;
    }
    LogInfo("Array %s columns=%d rows=%d", label, array->GetColumnCount(), array->GetRowCount());
    for (int column = 0; column < array->GetColumnCount(); ++column) {
        char *name = array->GetColumnName(column);
        LogInfo("  column[%d] name=\"%s\" type=%s", column,
                name ? name : "<null>",
                ArrayTypeName(array->GetColumnType(column)));
    }
}

// ---------------------------------------------------------------------------
// Region geometry
// ---------------------------------------------------------------------------

void FlexGravityMod::BuildRegionCache(CK3dEntity *entity, RegionInfo &region, const char *label) {
    region.localVertices.clear();
    region.localIndices.clear();
    region.meshValid = false;
    region.bboxValid = false;
    region.localBounds = VxBbox{};

    if (!entity) {
        Warn(std::string("Region object is missing: ") + (label ? label : "<null>"));
        return;
    }

    VxBbox bbox = entity->GetBoundingBox(TRUE);
    CKMesh *mesh = entity->GetCurrentMesh();
    if (mesh) {
        const int vertexCount = mesh->GetVertexCount();
        const int faceCount = mesh->GetFaceCount();
        if (vertexCount > 0 && faceCount > 0) {
            region.localVertices.reserve(static_cast<size_t>(vertexCount));
            for (int index = 0; index < vertexCount; ++index) {
                VxVector vertex(0.0f, 0.0f, 0.0f);
                mesh->GetVertexPosition(index, &vertex);
                region.localVertices.push_back(vertex);
            }

            region.localIndices.reserve(static_cast<size_t>(faceCount) * 3);
            for (int face = 0; face < faceCount; ++face) {
                int v0 = 0, v1 = 0, v2 = 0;
                mesh->GetFaceVertexIndex(face, v0, v1, v2);
                if (v0 >= 0 && v1 >= 0 && v2 >= 0 &&
                    v0 < vertexCount && v1 < vertexCount && v2 < vertexCount) {
                    region.localIndices.push_back(v0);
                    region.localIndices.push_back(v1);
                    region.localIndices.push_back(v2);
                }
            }

            region.meshValid = !region.localIndices.empty();
            if (bbox.IsValid()) {
                region.localBounds = bbox;
            } else {
                VxVector minimum(1.0e6f, 1.0e6f, 1.0e6f);
                VxVector maximum(-1.0e6f, -1.0e6f, -1.0e6f);
                for (const VxVector &vertex : region.localVertices) {
                    minimum.x = (std::min)(minimum.x, vertex.x);
                    minimum.y = (std::min)(minimum.y, vertex.y);
                    minimum.z = (std::min)(minimum.z, vertex.z);
                    maximum.x = (std::max)(maximum.x, vertex.x);
                    maximum.y = (std::max)(maximum.y, vertex.y);
                    maximum.z = (std::max)(maximum.z, vertex.z);
                }
                region.localBounds.SetCorners(minimum, maximum);
            }
            region.bboxValid = region.localBounds.IsValid();

            LogInfo("Region %s: mesh=%d vertices=%d faces=%d bboxValid=%s",
                    label ? label : "<null>", region.meshValid ? 1 : 0,
                    vertexCount, faceCount, BoolText(region.bboxValid).c_str());
            return;
        }
    }

    if (bbox.IsValid()) {
        region.localBounds = bbox;
        region.bboxValid = true;
        LogWarn("Region has no usable mesh; using local bounding box: %s",
                label ? label : "<null>");
    } else {
        Warn(std::string("Region has no usable mesh or bounding box: ") + (label ? label : "<null>"));
    }
}

bool FlexGravityMod::BuildLineGeometry(SourceInfo &source, CK3dEntity *entity) {
    source.localPoints.clear();
    source.controlPointCount = 0;
    source.usedCurve = false;
    source.closed = false;
    source.geometryValid = false;

    if (!entity) {
        return false;
    }

    CKCurve *curve = SafeCurveCast(entity);

    if (curve) {
        const int count = SafeCurvePointCount(curve);
        if (count >= 2 && count <= kMaxCurvePoints) {
            std::vector<VxVector> worldPoints;
            worldPoints.reserve(static_cast<size_t>(count));
            bool ok = true;
            for (int index = 0; index < count; ++index) {
                CKCurvePoint *point = SafeCurveControlPoint(curve, index);
                if (!point) {
                    ok = false;
                    break;
                }
                VxVector world(0.0f, 0.0f, 0.0f);
                if (!SafeWorldPosition(point, world)) {
                    ok = false;
                    break;
                }
                worldPoints.push_back(world);
            }

            if (ok) {
                const VxMatrix &inverse = entity->GetInverseWorldMatrix();
                for (const VxVector &world : worldPoints) {
                    source.localPoints.push_back(inverse * world);
                }
                source.controlPointCount = count;
                source.usedCurve = true;
                source.closed = SafeCurveIsOpen(curve) == 0;
                source.geometryValid = source.localPoints.size() >= 2;
            } else {
                Warn("FG_L curve control point query failed: " + SafeName(entity));
            }
        } else if (count >= 0) {
            Warn("FG_L curve has an unusable control point count: " + SafeName(entity) +
                 " count=" + std::to_string(count));
        } else {
            Warn("FG_L curve probe raised an exception; falling back to bounding box: " +
                 SafeName(entity));
        }
    }

    if (!source.geometryValid) {
        // Fallback: the longest local bounding box axis becomes the segment.
        VxBbox box = entity->GetBoundingBox(TRUE);
        if (!box.IsValid()) {
            Warn("FG_L has no usable curve or bounding box: " + SafeName(entity));
            return false;
        }

        const VxVector size = Subtract(box.Max, box.Min);
        int axis = 0;
        if (GetComponent(size, 1) > GetComponent(size, axis)) {
            axis = 1;
        }
        if (GetComponent(size, 2) > GetComponent(size, axis)) {
            axis = 2;
        }

        VxVector center = Scale(Add(box.Min, box.Max), 0.5f);
        VxVector a = center;
        VxVector b = center;
        SetComponent(a, axis, GetComponent(box.Min, axis));
        SetComponent(b, axis, GetComponent(box.Max, axis));

        source.localPoints.clear();
        source.localPoints.push_back(a);
        source.localPoints.push_back(b);
        source.geometryValid = true;
        LogWarn("FG_L is not a curve; using the longest bounding box axis: %s", SafeName(entity).c_str());
    }

    return source.geometryValid;
}

bool FlexGravityMod::BuildSheetGeometry(SourceInfo &source, CK3dEntity *entity) {
    source.geometryValid = false;
    if (!entity) {
        return false;
    }

    VxBbox box = entity->GetBoundingBox(TRUE);
    if (!box.IsValid()) {
        Warn("FG_S has no usable bounding box: " + SafeName(entity));
        return false;
    }

    const VxVector size = Subtract(box.Max, box.Min);

    int normalAxis = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (GetComponent(size, axis) < GetComponent(size, normalAxis)) {
            normalAxis = axis;
        }
    }
    const int axisA = (normalAxis + 1) % 3;
    const int axisB = (normalAxis + 2) % 3;

    const float extentA = GetComponent(size, axisA);
    const float extentB = GetComponent(size, axisB);
    const float thickness = GetComponent(size, normalAxis);

    if (extentA <= 1.0e-4f || extentB <= 1.0e-4f) {
        Warn("FG_S bounding box is degenerate: " + SafeName(entity));
        return false;
    }
    if (thickness > 0.5f * (std::max)(extentA, extentB)) {
        Warn("FG_S object does not look flat: " + SafeName(entity) + " size=" + FormatVec(size));
    }

    source.localCenter = Scale(Add(box.Min, box.Max), 0.5f);
    source.localAxisA = AxisVector(axisA);
    source.localAxisB = AxisVector(axisB);
    source.localNormal = AxisVector(normalAxis);
    source.halfA = extentA * 0.5f;
    source.halfB = extentB * 0.5f;
    source.geometryValid = true;

    return true;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void FlexGravityMod::ScanGroup(const char *name, GroupScan &result,
                               std::unordered_map<CK_ID, std::string> &members) {
    CKGroup *group = FindGroup(name);
    if (!group) {
        Warn(std::string("Group not found: ") + name);
        return;
    }

    result.present = true;
    result.count = group->GetObjectCount();
    LogInfo("Group %s: %d member(s)", name, result.count);

    for (int index = 0; index < result.count; ++index) {
        CKBeObject *beObject = group->GetObject(index);
        if (!beObject) {
            Warn(std::string("Group ") + name + " has a null member at index " + std::to_string(index));
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(beObject);
        if (!entity) {
            Warn(std::string("Group ") + name + " member is not a 3D entity: " + ObjectText(beObject));
            continue;
        }

        members[entity->GetID()] = SafeName(entity);
        LogInfo("  [%d] %s", index, ObjectText(entity).c_str());
    }
}

void FlexGravityMod::ScanUArray(const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(kArrayU);
    if (!array) {
        Warn("Array not found: FG_U");
        return;
    }

    m_Scan.uArray.present = true;
    m_Scan.uArray.rows = array->GetRowCount();
    m_Scan.uArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayU);

    int objectColumn = FindColumn(array, "Object");
    if (objectColumn < 0) {
        objectColumn = 0;
        Warn("FG_U has no column named Object; falling back to column 0");
    }

    int xColumn = FindColumn(array, "x");
    int yColumn = FindColumn(array, "y");
    int zColumn = FindColumn(array, "z");
    if (xColumn < 0 || yColumn < 0 || zColumn < 0) {
        Warn("FG_U is missing one or more x/y/z columns");
        if (xColumn < 0) xColumn = 1;
        if (yColumn < 0) yColumn = 2;
        if (zColumn < 0) zColumn = 3;
    }

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < m_Scan.uArray.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, objectColumn);
        if (!cellObject) {
            LogInfo("  FG_U row[%d]: empty object cell", row);
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(cellObject);
        if (!entity) {
            Warn("FG_U row " + std::to_string(row) + " object is not a 3D entity: " + ObjectText(cellObject));
            continue;
        }

        UFieldInfo field;
        field.objectId = entity->GetID();
        field.objectName = SafeName(entity);
        field.gravity = VxVector(GetCellFloat(array, row, xColumn, 0.0f),
                                 GetCellFloat(array, row, yColumn, 0.0f),
                                 GetCellFloat(array, row, zColumn, 0.0f));

        if (!seen.insert(field.objectId).second) {
            Warn("FG_U lists object more than once: " + field.objectName);
        }
        if (groupMembers.find(field.objectId) == groupMembers.end()) {
            Warn("FG_U object is not in the FG_U group: " + field.objectName);
        }

        BuildRegionCache(entity, field.region, field.objectName.c_str());
        m_Scan.uFields.push_back(field);
        LogInfo("  FG_U row[%d]: %s g=(%.4f, %.4f, %.4f)", row, field.objectName.c_str(),
                field.gravity.x, field.gravity.y, field.gravity.z);
    }
}

void FlexGravityMod::ScanSourceArray(SourceInfo::Kind kind, const char *arrayName,
                                     const char *strengthColumn, ArrayScan &arrayScan,
                                     const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(arrayName);
    if (!array) {
        Warn(std::string("Array not found: ") + arrayName);
        return;
    }

    arrayScan.present = true;
    arrayScan.rows = array->GetRowCount();
    arrayScan.columns = array->GetColumnCount();
    LogArrayColumns(array, arrayName);

    int objectColumn = FindColumn(array, "Object");
    if (objectColumn < 0) {
        objectColumn = 0;
        Warn(std::string(arrayName) + " has no column named Object; falling back to column 0");
    }

    int strengthIndex = FindColumn(array, strengthColumn);
    if (strengthIndex < 0) {
        strengthIndex = 1;
        Warn(std::string(arrayName) + " has no column named " + strengthColumn +
             "; falling back to column 1");
    }

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < arrayScan.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, objectColumn);
        if (!cellObject) {
            LogInfo("  %s row[%d]: empty object cell", arrayName, row);
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(cellObject);
        if (!entity) {
            Warn(std::string(arrayName) + " row " + std::to_string(row) +
                 " object is not a 3D entity: " + ObjectText(cellObject));
            continue;
        }

        SourceInfo source;
        source.objectId = entity->GetID();
        source.objectName = SafeName(entity);
        source.kind = kind;
        source.strength = GetCellFloat(array, row, strengthIndex, 0.0f);

        if (!seen.insert(source.objectId).second) {
            Warn(std::string(arrayName) + " lists object more than once: " + source.objectName);
        }
        if (groupMembers.find(source.objectId) == groupMembers.end()) {
            Warn(std::string(arrayName) + " object is not in the " + std::string(arrayName) +
                 " group: " + source.objectName);
        }

        if (kind == SourceInfo::KindLine) {
            BuildLineGeometry(source, entity);
        } else if (kind == SourceInfo::KindSheet) {
            BuildSheetGeometry(source, entity);
        } else {
            source.geometryValid = true;
        }

        m_Scan.sources.push_back(source);

        if (kind == SourceInfo::KindLine) {
            std::string first = "<none>";
            std::string last = "<none>";
            if (!source.localPoints.empty()) {
                const VxMatrix &world = entity->GetWorldMatrix();
                first = FormatVec(world * source.localPoints.front());
                last = FormatVec(world * source.localPoints.back());
            }
            LogInfo("  FG_L row[%d]: %s Lambda=%.4f curve=%d points=%d closed=%s A=%s B=%s",
                    row, source.objectName.c_str(), source.strength,
                    source.usedCurve ? 1 : 0, source.controlPointCount,
                    BoolText(source.closed).c_str(), first.c_str(), last.c_str());
        } else if (kind == SourceInfo::KindSheet) {
            const VxMatrix &world = entity->GetWorldMatrix();
            const VxVector center = world * source.localCenter;
            VxVector axisA(0.0f, 0.0f, 0.0f);
            VxVector axisB(0.0f, 0.0f, 0.0f);
            VxVector normal(0.0f, 0.0f, 0.0f);
            Normalize(world * source.localAxisA, axisA);
            Normalize(world * source.localAxisB, axisB);
            Normalize(Cross(axisA, axisB), normal);
            LogInfo("  FG_S row[%d]: %s Sigma=%.4f center=%s size=(%.4f x %.4f) normal=%s",
                    row, source.objectName.c_str(), source.strength,
                    FormatVec(center).c_str(), source.halfA * 2.0f, source.halfB * 2.0f,
                    FormatVec(normal).c_str());
        } else {
            LogInfo("  FG_M row[%d]: %s G=%.4f", row, source.objectName.c_str(), source.strength);
        }
    }
}

void FlexGravityMod::ScanRArray(const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(kArrayR);
    if (!array) {
        Warn("Array not found: FG_R");
        return;
    }

    m_Scan.rArray.present = true;
    m_Scan.rArray.rows = array->GetRowCount();
    m_Scan.rArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayR);

    if (m_Scan.rArray.columns < 2) {
        Warn("FG_R needs at least two object columns (region + at least one source)");
        return;
    }

    for (int row = 0; row < m_Scan.rArray.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, 0);
        if (!cellObject) {
            LogInfo("  FG_R row[%d]: empty region cell", row);
            continue;
        }

        CK3dEntity *regionEntity = CK3dEntity::Cast(cellObject);
        if (!regionEntity) {
            Warn("FG_R row " + std::to_string(row) + " first cell is not a 3D entity: " +
                 ObjectText(cellObject));
            continue;
        }

        RRowInfo rRow;
        rRow.regionObjectId = regionEntity->GetID();
        rRow.regionName = SafeName(regionEntity);

        if (groupMembers.find(rRow.regionObjectId) == groupMembers.end()) {
            Warn("FG_R region object is not in the FG_R group: " + rRow.regionName);
        }

        BuildRegionCache(regionEntity, rRow.region, rRow.regionName.c_str());

        std::unordered_set<CK_ID> seenSources;
        for (int column = 1; column < m_Scan.rArray.columns; ++column) {
            CKObject *cell = GetCellObject(array, row, column);
            if (!cell) {
                continue;
            }

            CK3dEntity *sourceEntity = CK3dEntity::Cast(cell);
            if (!sourceEntity) {
                Warn("FG_R row " + std::to_string(row) + " column " + std::to_string(column) +
                     " is not a 3D entity: " + ObjectText(cell));
                continue;
            }

            if (!seenSources.insert(sourceEntity->GetID()).second) {
                continue;
            }
            if (!FindSource(sourceEntity->GetID())) {
                Warn("FG_R lists an object that has no FG_M/FG_L/FG_S row: " + SafeName(sourceEntity));
                continue;
            }

            rRow.sourceIds.push_back(sourceEntity->GetID());
            rRow.sourceNames.push_back(SafeName(sourceEntity));
        }

        if (rRow.sourceIds.empty()) {
            Warn("FG_R row " + std::to_string(row) + " has no valid source objects");
        }

        std::string list;
        for (const std::string &name : rRow.sourceNames) {
            if (!list.empty()) {
                list += ", ";
            }
            list += name;
        }
        m_Scan.rRows.push_back(rRow);
        LogInfo("  FG_R row[%d]: region=%s -> [%s]", row, rRow.regionName.c_str(), list.c_str());
    }
}

void FlexGravityMod::ScanRGArray() {
    CKDataArray *array = FindArray(kArrayRG);
    if (!array) {
        LogInfo("Array FG_RG not present; no global source list");
        return;
    }

    m_Scan.rgArray.present = true;
    m_Scan.rgArray.rows = array->GetRowCount();
    m_Scan.rgArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayRG);

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < m_Scan.rgArray.rows; ++row) {
        for (int column = 0; column < m_Scan.rgArray.columns; ++column) {
            CKObject *cellObject = GetCellObject(array, row, column);
            if (!cellObject) {
                continue;
            }

            CK3dEntity *entity = CK3dEntity::Cast(cellObject);
            if (!entity) {
                Warn("FG_RG row " + std::to_string(row) + " column " + std::to_string(column) +
                     " is not a 3D entity: " + ObjectText(cellObject));
                continue;
            }

            if (!seen.insert(entity->GetID()).second) {
                continue;
            }
            if (!FindSource(entity->GetID())) {
                Warn("FG_RG lists an object that has no FG_M/FG_L/FG_S row: " + SafeName(entity));
                continue;
            }

            m_Scan.globalSourceIds.push_back(entity->GetID());
            m_Scan.globalSourceNames.push_back(SafeName(entity));
            LogInfo("  FG_RG row[%d] col[%d]: %s", row, column, SafeName(entity).c_str());
        }
    }
}

const SourceInfo *FlexGravityMod::FindSource(CK_ID objectId) const {
    for (const SourceInfo &source : m_Scan.sources) {
        if (source.objectId == objectId) {
            return &source;
        }
    }
    return nullptr;
}

void FlexGravityMod::ValidateMembership(const std::unordered_map<CK_ID, std::string> &uGroupMembers,
                                        const std::unordered_map<CK_ID, std::string> &mGroupMembers,
                                        const std::unordered_map<CK_ID, std::string> &lGroupMembers,
                                        const std::unordered_map<CK_ID, std::string> &sGroupMembers,
                                        const std::unordered_map<CK_ID, std::string> &rGroupMembers) {
    std::unordered_set<CK_ID> uConfigured;
    for (const UFieldInfo &field : m_Scan.uFields) {
        uConfigured.insert(field.objectId);
    }
    for (const auto &entry : uGroupMembers) {
        if (uConfigured.find(entry.first) == uConfigured.end()) {
            Warn("FG_U group member has no FG_U array row: " + entry.second);
        }
    }

    std::unordered_set<CK_ID> mConfigured;
    std::unordered_set<CK_ID> lConfigured;
    std::unordered_set<CK_ID> sConfigured;
    for (const SourceInfo &source : m_Scan.sources) {
        if (source.kind == SourceInfo::KindMonopole) {
            mConfigured.insert(source.objectId);
        } else if (source.kind == SourceInfo::KindLine) {
            lConfigured.insert(source.objectId);
        } else {
            sConfigured.insert(source.objectId);
        }
    }

    for (const auto &entry : mGroupMembers) {
        if (mConfigured.find(entry.first) == mConfigured.end()) {
            Warn("FG_M group member has no FG_M array row: " + entry.second);
        }
    }
    for (const auto &entry : lGroupMembers) {
        if (lConfigured.find(entry.first) == lConfigured.end()) {
            Warn("FG_L group member has no FG_L array row: " + entry.second);
        }
    }
    for (const auto &entry : sGroupMembers) {
        if (sConfigured.find(entry.first) == sConfigured.end()) {
            Warn("FG_S group member has no FG_S array row: " + entry.second);
        }
    }

    std::unordered_set<CK_ID> rConfigured;
    for (const RRowInfo &row : m_Scan.rRows) {
        rConfigured.insert(row.regionObjectId);
    }
    for (const auto &entry : rGroupMembers) {
        if (rConfigured.find(entry.first) == rConfigured.end()) {
            Warn("FG_R group member has no FG_R array row: " + entry.second);
        }
    }
}

void FlexGravityMod::Scan(bool verbose) {
    m_Scan = ScanInfo{};
    m_Scan.ran = true;
    m_PendingLineLogged = false;
    m_PendingSheetLogged = false;

    CKContext *context = Context();
    if (!context) {
        Warn("No CKContext available; cannot scan");
        return;
    }

    // A normal Ballance level has no FlexGravity data at all.  Keep the log
    // quiet in that case instead of producing one warning per missing object.
    const bool anyFlexGravityData =
            FindGroup(kGroupU) || FindGroup(kGroupM) || FindGroup(kGroupL) ||
            FindGroup(kGroupS) || FindGroup(kGroupR) ||
            FindArray(kArrayU) || FindArray(kArrayM) || FindArray(kArrayL) ||
            FindArray(kArrayS) || FindArray(kArrayR) || FindArray(kArrayRG);
    if (!anyFlexGravityData) {
        LogInfo("No FlexGravity data in this level");
        return;
    }

    std::unordered_map<CK_ID, std::string> uGroupMembers;
    std::unordered_map<CK_ID, std::string> mGroupMembers;
    std::unordered_map<CK_ID, std::string> lGroupMembers;
    std::unordered_map<CK_ID, std::string> sGroupMembers;
    std::unordered_map<CK_ID, std::string> rGroupMembers;

    ScanGroup(kGroupU, m_Scan.uGroup, uGroupMembers);
    ScanGroup(kGroupM, m_Scan.mGroup, mGroupMembers);
    ScanGroup(kGroupL, m_Scan.lGroup, lGroupMembers);
    ScanGroup(kGroupS, m_Scan.sGroup, sGroupMembers);
    ScanGroup(kGroupR, m_Scan.rGroup, rGroupMembers);

    ScanUArray(uGroupMembers);
    ScanSourceArray(SourceInfo::KindMonopole, kArrayM, "G", m_Scan.mArray, mGroupMembers);
    ScanSourceArray(SourceInfo::KindLine, kArrayL, "Lambda", m_Scan.lArray, lGroupMembers);
    ScanSourceArray(SourceInfo::KindSheet, kArrayS, "Sigma", m_Scan.sArray, sGroupMembers);
    ScanRArray(rGroupMembers);
    ScanRGArray();

    ValidateMembership(uGroupMembers, mGroupMembers, lGroupMembers, sGroupMembers, rGroupMembers);

    LogInfo("Scan summary: groups(U=%d M=%d L=%d S=%d R=%d) "
            "arrays(U=%d M=%d L=%d S=%d R=%d RG=%d) "
            "fields(U=%zu M=%zu L=%zu S=%zu RRows=%zu Global=%zu) warnings=%zu",
            m_Scan.uGroup.count, m_Scan.mGroup.count, m_Scan.lGroup.count,
            m_Scan.sGroup.count, m_Scan.rGroup.count,
            m_Scan.uArray.rows, m_Scan.mArray.rows, m_Scan.lArray.rows,
            m_Scan.sArray.rows, m_Scan.rArray.rows, m_Scan.rgArray.rows,
            m_Scan.uFields.size(),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                             [](const SourceInfo &s) { return s.kind == SourceInfo::KindMonopole; })),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                             [](const SourceInfo &s) { return s.kind == SourceInfo::KindLine; })),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                             [](const SourceInfo &s) { return s.kind == SourceInfo::KindSheet; })),
            m_Scan.rRows.size(), m_Scan.globalSourceIds.size(), m_Scan.warnings.size());

    if (verbose) {
        LogInfo("Scan complete for level; use 'flexgravity list' for parsed fields.");
    }
}

// ---------------------------------------------------------------------------
// Object sampling / region tests
// ---------------------------------------------------------------------------

CK3dEntity *FlexGravityMod::ResolveEntity(CK_ID objectId) {
    CKContext *context = Context();
    if (!context || objectId == 0) {
        return nullptr;
    }
    return CK3dEntity::Cast(context->GetObject(objectId));
}

std::vector<VxVector> FlexGravityMod::BuildObjectSamples(CK3dEntity *entity) {
    std::vector<VxVector> samples;
    if (!entity) {
        return samples;
    }

    const VxMatrix &world = entity->GetWorldMatrix();
    VxBbox box = entity->GetBoundingBox(TRUE);
    if (!box.IsValid()) {
        samples.push_back(world * VxVector(0.0f, 0.0f, 0.0f));
        return samples;
    }

    const VxVector center((box.Min.x + box.Max.x) * 0.5f,
                          (box.Min.y + box.Max.y) * 0.5f,
                          (box.Min.z + box.Max.z) * 0.5f);
    samples.reserve(15);
    auto addLocal = [&](const VxVector &point) {
        samples.push_back(world * point);
    };

    addLocal(center);
    for (int index = 0; index < 8; ++index) {
        addLocal(VxVector((index & 1) ? box.Max.x : box.Min.x,
                          (index & 2) ? box.Max.y : box.Min.y,
                          (index & 4) ? box.Max.z : box.Min.z));
    }
    addLocal(VxVector(center.x, center.y, box.Min.z));
    addLocal(VxVector(center.x, center.y, box.Max.z));
    addLocal(VxVector(center.x, box.Min.y, center.z));
    addLocal(VxVector(center.x, box.Max.y, center.z));
    addLocal(VxVector(box.Min.x, center.y, center.z));
    addLocal(VxVector(box.Max.x, center.y, center.z));
    return samples;
}

VxVector FlexGravityMod::GetObjectCenter(CK3dEntity *entity) {
    const std::vector<VxVector> samples = BuildObjectSamples(entity);
    if (samples.empty()) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    return samples[0];
}

bool FlexGravityMod::PointInRegionLocal(const RegionInfo &region, const VxVector &localPoint) {
    if (!BoundsContains(region.localBounds, localPoint)) {
        return false;
    }

    if (!region.meshValid) {
        return region.bboxValid;
    }

    // A slanted ray avoids most axis-aligned edge hits on box-shaped regions.
    const VxVector direction(0.37139067f, 0.61898445f, 0.69179575f);
    int crossings = 0;
    for (size_t index = 0; index + 2 < region.localIndices.size(); index += 3) {
        const VxVector &v0 = region.localVertices[static_cast<size_t>(region.localIndices[index])];
        const VxVector &v1 = region.localVertices[static_cast<size_t>(region.localIndices[index + 1])];
        const VxVector &v2 = region.localVertices[static_cast<size_t>(region.localIndices[index + 2])];
        if (RayTriangle(localPoint, direction, v0, v1, v2)) {
            ++crossings;
        }
    }
    return (crossings & 1) != 0;
}

bool FlexGravityMod::ObjectTouchesRegion(const RegionInfo &region, CK_ID regionObjectId,
                                         const std::vector<VxVector> &objectSamples) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }

    CK3dEntity *regionEntity = ResolveEntity(regionObjectId);
    if (!regionEntity) {
        return false;
    }

    const VxMatrix &inverse = regionEntity->GetInverseWorldMatrix();
    for (const VxVector &sample : objectSamples) {
        const VxVector localPoint = inverse * sample;
        if (PointInRegionLocal(region, localPoint)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Field evaluation
// ---------------------------------------------------------------------------

bool FlexGravityMod::IsSourceActive(CK_ID objectId, const std::vector<char> &rowTouched) const {
    for (CK_ID globalId : m_Scan.globalSourceIds) {
        if (globalId == objectId) {
            return true;
        }
    }

    const size_t count = (std::min)(m_Scan.rRows.size(), rowTouched.size());
    for (size_t index = 0; index < count; ++index) {
        if (!rowTouched[index]) {
            continue;
        }
        for (CK_ID sourceId : m_Scan.rRows[index].sourceIds) {
            if (sourceId == objectId) {
                return true;
            }
        }
    }
    return false;
}

VxVector FlexGravityMod::ComputeSegmentAcceleration(const VxVector &a, const VxVector &b,
                                                    const VxVector &center, float strength) {
    // Exact field of a finite uniform line segment, normalised so that an
    // infinite line produces strength / d:
    //   g = (lambda/2) * [ -(K1/d) * p_hat + K2 * u ]
    const VxVector direction = Subtract(b, a);
    const float length = std::sqrt(LengthSquared(direction));
    if (length <= kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    const VxVector u = Scale(direction, 1.0f / length);

    const VxVector w = Subtract(center, a);
    const float s = Dot(w, u);
    const VxVector p = Subtract(w, Scale(u, s));
    const float distance = std::sqrt(LengthSquared(p));

    const float d1 = std::sqrt(LengthSquared(w));
    const float d2 = std::sqrt(LengthSquared(Subtract(center, b)));
    if (d1 <= kTinyDistance || d2 <= kTinyDistance) {
        // Standing exactly on an endpoint: the field is undefined.
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float k1 = s / d1 - (s - length) / d2;
    const float k2 = 1.0f / d1 - 1.0f / d2;

    VxVector result = Scale(u, k2);
    if (distance > kTinyDistance) {
        const VxVector perpendicular = Scale(p, 1.0f / distance);
        result = Add(result, Scale(perpendicular, -k1 / distance));
    }
    return Scale(result, 0.5f * strength);
}

float FlexGravityMod::DistanceToPolyline(const std::vector<VxVector> &points, bool closed,
                                         const VxVector &center) {
    if (points.size() < 2) {
        return 0.0f;
    }
    const size_t count = points.size();
    const size_t segments = closed ? count : count - 1;
    float best = 1.0e30f;
    for (size_t index = 0; index < segments; ++index) {
        const VxVector &a = points[index];
        const VxVector &b = points[(index + 1) % count];
        const VxVector direction = Subtract(b, a);
        const float lengthSquared = LengthSquared(direction);
        float t = 0.0f;
        if (lengthSquared > kTinyDistance * kTinyDistance) {
            t = Dot(Subtract(center, a), direction) / lengthSquared;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
        }
        const VxVector closest = Add(a, Scale(direction, t));
        const float distance = std::sqrt(LengthSquared(Subtract(center, closest)));
        best = (std::min)(best, distance);
    }
    return best;
}

VxVector FlexGravityMod::ComputeSheetAcceleration(const SourceInfo &source, const VxVector &center) {
    CK3dEntity *entity = ResolveEntity(source.objectId);
    if (!entity) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const VxMatrix &world = entity->GetWorldMatrix();
    const VxVector origin = world * source.localCenter;

    VxVector axisA = Subtract(world * Add(source.localCenter, source.localAxisA), origin);
    VxVector axisB = Subtract(world * Add(source.localCenter, source.localAxisB), origin);
    const float scaleA = std::sqrt(LengthSquared(axisA));
    const float scaleB = std::sqrt(LengthSquared(axisB));
    if (scaleA <= kTinyDistance || scaleB <= kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    axisA = Scale(axisA, 1.0f / scaleA);
    axisB = Scale(axisB, 1.0f / scaleB);

    VxVector normal = Cross(axisA, axisB);
    if (!Normalize(normal, normal)) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float a = source.halfA * scaleA;
    const float b = source.halfB * scaleB;

    const VxVector relative = Subtract(center, origin);
    const float x = Dot(relative, axisA);
    const float y = Dot(relative, axisB);
    const float z = Dot(relative, normal);

    const float x1 = -a - x;
    const float x2 = a - x;
    const float y1 = -b - y;
    const float y2 = b - y;
    const float absZ = std::fabs(z);
    const float z2 = z * z;

    auto distanceToCorner = [z2](float px, float py) {
        return std::sqrt(px * px + py * py + z2);
    };

    // Solid angle of the rectangle seen from the object centre.
    const float omega =
            std::atan2(x1 * y1, absZ * distanceToCorner(x1, y1)) -
            std::atan2(x1 * y2, absZ * distanceToCorner(x1, y2)) -
            std::atan2(x2 * y1, absZ * distanceToCorner(x2, y1)) +
            std::atan2(x2 * y2, absZ * distanceToCorner(x2, y2));

    const float d1 = std::sqrt(x1 * x1 + z2);
    const float d2 = std::sqrt(x2 * x2 + z2);
    const float e1 = std::sqrt(y1 * y1 + z2);
    const float e2 = std::sqrt(y2 * y2 + z2);

    float inPlaneX = 0.0f;
    float inPlaneY = 0.0f;
    if (d1 > kTinyDistance && d2 > kTinyDistance &&
        e1 > kTinyDistance && e2 > kTinyDistance) {
        inPlaneX = std::asinh(y2 / d1) - std::asinh(y1 / d1) -
                   std::asinh(y2 / d2) + std::asinh(y1 / d2);
        inPlaneY = std::asinh(x2 / e1) - std::asinh(x1 / e1) -
                   std::asinh(x2 / e2) + std::asinh(x1 / e2);
    }

    const float coupling = source.strength / (2.0f * 3.14159265358979323846f);
    VxVector result = Add(Scale(axisA, coupling * inPlaneX),
                          Scale(axisB, coupling * inPlaneY));

    if (absZ > kTinyDistance) {
        const float towardSheet = z > 0.0f ? -1.0f : 1.0f;
        result = Add(result, Scale(normal, towardSheet * coupling * omega));
    }
    return result;
}

VxVector FlexGravityMod::ComputeSourceAcceleration(const SourceInfo &source, const VxVector &center) {
    if (source.kind == SourceInfo::KindLine) {
        CK3dEntity *lineEntity = ResolveEntity(source.objectId);
        if (!lineEntity || source.localPoints.size() < 2) {
            return VxVector(0.0f, 0.0f, 0.0f);
        }

        const VxMatrix &world = lineEntity->GetWorldMatrix();
        std::vector<VxVector> points;
        points.reserve(source.localPoints.size());
        for (const VxVector &local : source.localPoints) {
            points.push_back(world * local);
        }

        VxVector total(0.0f, 0.0f, 0.0f);
        const size_t count = points.size();
        const size_t segments = source.closed ? count : count - 1;
        for (size_t index = 0; index < segments; ++index) {
            total = Add(total, ComputeSegmentAcceleration(points[index],
                                                          points[(index + 1) % count],
                                                          center, source.strength));
        }
        return total;
    }

    if (source.kind == SourceInfo::KindSheet) {
        return ComputeSheetAcceleration(source, center);
    }

    CK3dEntity *entity = ResolveEntity(source.objectId);
    if (!entity) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const VxVector position = entity->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const VxVector r = Subtract(position, center);
    const float distanceSquared = Dot(r, r);
    if (distanceSquared < kTinyDistance * kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float inverse = 1.0f / (distanceSquared * std::sqrt(distanceSquared));
    return Scale(r, source.strength * inverse);
}

void FlexGravityMod::UpdateFields() {
    if (m_ManualForceActive) {
        ClearAllFieldForces();
        return;
    }
    if (!m_Scan.ran || (m_Scan.uFields.empty() && m_Scan.sources.empty())) {
        ClearAllFieldForces();
        return;
    }

    std::vector<char> rowTouched(m_Scan.rRows.size(), 0);
    std::unordered_set<CK_ID> touched;
    int affected = 0;

    for (auto &entry : m_Bodies) {
        BodyInfo &body = entry.second;
        if (body.fixed || !body.physicalized || body.mass <= 0.0f) {
            continue;
        }

        CK3dEntity *entity = ResolveEntity(body.objectId);
        if (!entity) {
            continue;
        }

        const std::vector<VxVector> samples = BuildObjectSamples(entity);
        VxVector acceleration(0.0f, 0.0f, 0.0f);

        for (const UFieldInfo &field : m_Scan.uFields) {
            if (ObjectTouchesRegion(field.region, field.objectId, samples)) {
                acceleration = Add(acceleration, field.gravity);
            }
        }

        if (!m_Scan.sources.empty()) {
            for (size_t index = 0; index < m_Scan.rRows.size(); ++index) {
                const RRowInfo &rRow = m_Scan.rRows[index];
                rowTouched[index] = ObjectTouchesRegion(rRow.region, rRow.regionObjectId, samples) ? 1 : 0;
            }

            const VxVector center = samples.empty() ? VxVector(0.0f, 0.0f, 0.0f) : samples[0];
            for (const SourceInfo &source : m_Scan.sources) {
                if (!IsSourceActive(source.objectId, rowTouched)) {
                    continue;
                }
                acceleration = Add(acceleration, ComputeSourceAcceleration(source, center));
            }
        }

        if (LengthSquared(acceleration) <= 1.0e-8f) {
            auto channel = m_ForceChannels.find(body.objectId);
            if (channel != m_ForceChannels.end()) {
                ClearChannelForce(channel->second);
            }
            continue;
        }

        ApplyChannelForce(entity, body, acceleration);
        touched.insert(body.objectId);
        ++affected;
    }

    for (auto &entry : m_ForceChannels) {
        if (touched.find(entry.first) == touched.end()) {
            ClearChannelForce(entry.second);
        }
    }

    if (affected != m_LastAffectedCount) {
        m_LastAffectedCount = affected;
        LogInfo("Fields: %zu U + %zu source(s), affected dynamic objects: %d",
                m_Scan.uFields.size(), m_Scan.sources.size(), affected);
    }
}

// ---------------------------------------------------------------------------
// Per-object force channels
// ---------------------------------------------------------------------------

CKBehavior *FlexGravityMod::GetOrCreateForceBehavior(ForceChannel &channel) {
    if (channel.behavior) {
        return channel.behavior;
    }
    if (m_ForceBackendFailed || !m_BML) {
        return nullptr;
    }

    CKBehavior *ownerScript = m_BML->GetScriptByName("Level_Init");
    if (!ownerScript) {
        m_ForceBackendFailed = true;
        LogWarn("Cannot create force behavior: Level_Init script not found");
        return nullptr;
    }

    CKBehavior *behavior = ExecuteBB::CreateSetPhysicsForce(ownerScript);
    if (!behavior) {
        m_ForceBackendFailed = true;
        LogWarn("Cannot create SetPhysicsForce behavior");
        return nullptr;
    }

    channel.behavior = behavior;
    if (!m_OwnerScript) {
        m_OwnerScript = ownerScript;
    }
    LogInfo("Created force channel for object id=%lu",
            static_cast<unsigned long>(channel.objectId));
    return behavior;
}

void FlexGravityMod::ClearChannelForce(ForceChannel &channel) {
    // Never execute a behavior while the engine is dismantling the level.
    if (channel.behavior && channel.active && !m_TearingDown) {
        channel.behavior->ActivateInput(1);
        channel.behavior->Execute(0);
    }
    channel.active = false;
    channel.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    channel.lastMass = 0.0f;
}

void FlexGravityMod::DropAllFieldForces() {
    // Forgets the state without touching the engine (used during teardown).
    for (auto &entry : m_ForceChannels) {
        entry.second.active = false;
        entry.second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        entry.second.lastMass = 0.0f;
    }
    m_LastAffectedCount = -1;
}

void FlexGravityMod::ApplyChannelForce(CK3dEntity *entity, const BodyInfo &body,
                                       const VxVector &acceleration) {
    const float lengthSquared = LengthSquared(acceleration);
    if (lengthSquared <= 1.0e-8f || !entity || body.mass <= 0.0f) {
        return;
    }

    auto it = m_ForceChannels.find(body.objectId);
    if (it == m_ForceChannels.end()) {
        ForceChannel channel;
        channel.objectId = body.objectId;
        it = m_ForceChannels.emplace(body.objectId, channel).first;
    }
    ForceChannel &channel = it->second;

    // A re-physicalized object (death respawn, ball type switch, ...) is a new
    // rigid body: the old behavior still points at the destroyed one, so the
    // first "set" after that would silently do nothing.  Recreate it here,
    // from the frame callback (never during script execution).
    if (channel.needsRebuild) {
        channel.needsRebuild = false;
        if (channel.behavior) {
            CKBehavior *owner = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
            if (owner && owner == m_OwnerScript) {
                ScriptHelper::DeleteBB(owner, channel.behavior);
            }
            channel.behavior = nullptr;
            LogInfo("Rebuilt force channel for object id=%lu after re-physicalize",
                    static_cast<unsigned long>(channel.objectId));
        }
        channel.active = false;
        channel.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel.lastMass = 0.0f;
    }

    const VxVector delta = Subtract(channel.lastAcceleration, acceleration);
    if (channel.active && channel.lastMass == body.mass && LengthSquared(delta) <= 1.0e-8f) {
        return;
    }

    CKBehavior *behavior = GetOrCreateForceBehavior(channel);
    if (!behavior) {
        return;
    }

    if (channel.active) {
        behavior->ActivateInput(1);
        behavior->Execute(0);
        channel.active = false;
    }

    CKParameterIn *targetParameter = behavior->GetTargetParameter();
    CKParameterIn *positionParameter = behavior->GetInputParameter(0);
    CKParameterIn *positionRefParameter = behavior->GetInputParameter(1);
    CKParameterIn *directionParameter = behavior->GetInputParameter(2);
    CKParameterIn *directionRefParameter = behavior->GetInputParameter(3);
    CKParameterIn *forceParameter = behavior->GetInputParameter(4);
    if (!targetParameter || !positionParameter || !positionRefParameter ||
        !directionParameter || !directionRefParameter || !forceParameter ||
        !targetParameter->GetDirectSource() || !positionParameter->GetDirectSource() ||
        !positionRefParameter->GetDirectSource() || !directionParameter->GetDirectSource() ||
        !directionRefParameter->GetDirectSource() || !forceParameter->GetDirectSource()) {
        if (!m_ForceBackendFailed) {
            m_ForceBackendFailed = true;
            LogWarn("SetPhysicsForce behavior has no direct parameter sources");
        }
        return;
    }

    ScriptHelper::SetParamObject(targetParameter->GetDirectSource(), entity);
    ScriptHelper::SetParamValue(positionParameter->GetDirectSource(), VxVector(0.0f, 0.0f, 0.0f));
    ScriptHelper::SetParamObject(positionRefParameter->GetDirectSource(), entity);
    ScriptHelper::SetParamValue(directionParameter->GetDirectSource(), acceleration);
    ScriptHelper::SetParamObject(directionRefParameter->GetDirectSource(), nullptr);

    const float magnitude = std::sqrt(lengthSquared);
    const float rawValue = body.mass * magnitude / kForceScale;
    ScriptHelper::SetParamValue(forceParameter->GetDirectSource(), rawValue);

    ExecuteBB::PhysicsWakeUp(entity);
    behavior->ActivateInput(0);
    behavior->Execute(0);

    channel.active = true;
    channel.lastAcceleration = acceleration;
    channel.lastMass = body.mass;

    if (m_DebugMode) {
        LogInfo("Field force: %s accel=(%.4f, %.4f, %.4f) raw=%.4f",
                body.objectName.c_str(), acceleration.x, acceleration.y, acceleration.z, rawValue);
    }
}

void FlexGravityMod::ClearAllFieldForces() {
    for (auto &entry : m_ForceChannels) {
        ClearChannelForce(entry.second);
    }
    m_LastAffectedCount = 0;
}

void FlexGravityMod::DestroyForceChannels() {
    CKBehavior *ownerScript = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    const bool canDelete = !m_TearingDown && ownerScript && ownerScript == m_OwnerScript;
    for (auto &entry : m_ForceChannels) {
        if (m_TearingDown) {
            entry.second.active = false;
            entry.second.lastMass = 0.0f;
        } else {
            ClearChannelForce(entry.second);
        }
        if (canDelete && entry.second.behavior) {
            ScriptHelper::DeleteBB(ownerScript, entry.second.behavior);
        }
    }
    m_ForceChannels.clear();
    m_OwnerScript = nullptr;
    m_LastAffectedCount = -1;
    m_ForceBackendFailed = false;
}

void FlexGravityMod::RebuildForceChannels() {
    // Called from the frame callback (never during script execution) right
    // after a level reset.  A SetPhysicsForce behavior keeps a handle to the
    // rigid body it was last applied to; after a reset that body is gone, so
    // the first "set" would silently do nothing.  Recreating the behaviors
    // restores the exact state of a freshly loaded level.
    CKBehavior *levelInit = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    int deleted = 0;
    if (levelInit && levelInit == m_OwnerScript) {
        for (auto &entry : m_ForceChannels) {
            if (entry.second.behavior) {
                ScriptHelper::DeleteBB(levelInit, entry.second.behavior);
                entry.second.behavior = nullptr;
                ++deleted;
            }
        }
    }
    m_ForceChannels.clear();
    m_LastAffectedCount = -1;
    m_ForceBackendFailed = false;
    LogInfo("Rebuilt force channels after level reset (deleted %d)", deleted);
}

// ---------------------------------------------------------------------------
// Physics probe
// ---------------------------------------------------------------------------

CK3dEntity *FlexGravityMod::FindActiveBall() {
    if (!m_BML) {
        return nullptr;
    }
    CKDataArray *currentLevel = m_BML->GetArrayByName("CurrentLevel");
    if (!currentLevel) {
        return nullptr;
    }
    const int column = FindColumn(currentLevel, "ActiveBall");
    if (column < 0) {
        return nullptr;
    }
    return CK3dEntity::Cast(currentLevel->GetElementObject(0, column));
}

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

void FlexGravityMod::ClearAppliedForce() {
    if (!m_ForceApplied) {
        return;
    }

    CKContext *context = Context();
    CKObject *object = context ? context->GetObject(m_ForceObjectId) : nullptr;
    CK3dEntity *entity = CK3dEntity::Cast(object);
    if (entity && !m_TearingDown) {
        ExecuteBB::UnsetPhysicsForce(entity);
        LogInfo("Cleared physics force from %s", SafeName(entity).c_str());
    }

    m_ForceApplied = false;
    m_ForceObjectId = 0;
}

void FlexGravityMod::ApplyForceToBall(CK3dEntity *ball, const VxVector &acceleration, float mass) {
    ClearAppliedForce();

    if (!ball || mass <= 0.0f) {
        return;
    }

    const float lengthSquared = LengthSquared(acceleration);
    if (lengthSquared <= 1.0e-8f) {
        return;
    }

    const float magnitude = std::sqrt(lengthSquared);
    const float rawValue = mass * magnitude / kForceScale;
    const VxVector force = Scale(acceleration, mass);

    ExecuteBB::PhysicsWakeUp(ball);
    ExecuteBB::SetPhysicsForce(ball,
                               VxVector(0.0f, 0.0f, 0.0f), ball,
                               acceleration, nullptr,
                               rawValue);

    m_ForceApplied = true;
    m_ForceObjectId = ball->GetID();

    LogInfo("Applied force to %s: mass=%.4f accel=(%.4f, %.4f, %.4f) force=(%.4f, %.4f, %.4f) raw=%.4f",
            SafeName(ball).c_str(), mass,
            acceleration.x, acceleration.y, acceleration.z,
            force.x, force.y, force.z, rawValue);
}

void FlexGravityMod::ApplyCurrentManualForce() {
    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        SendMessage("No active ball found.");
        return;
    }

    bool found = false;
    const float mass = GetBodyMass(ball, &found);
    if (!found && !m_MissingMassWarned) {
        m_MissingMassWarned = true;
        LogWarn("Active ball has no physicalize mass record; using fallback mass %.4f", mass);
    }
    ApplyForceToBall(ball, m_ManualAcceleration, mass);
}

void FlexGravityMod::UpdateManualForce() {
    if (!m_ManualForceActive) {
        return;
    }

    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        ClearAppliedForce();
        return;
    }

    if (m_ForceApplied && m_ForceObjectId == ball->GetID()) {
        return;
    }

    ApplyCurrentManualForce();
}

// ---------------------------------------------------------------------------
// Status / list
// ---------------------------------------------------------------------------

void FlexGravityMod::PrintBall(IBML *bml) {
    (void) bml;
    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        SendMessage("No active ball found.");
        return;
    }

    auto it = m_Bodies.find(ball->GetID());
    const bool known = it != m_Bodies.end();
    const float mass = known ? it->second.mass : 0.0f;
    const bool physicalized = known && it->second.physicalized;
    const bool fixed = known && it->second.fixed;

    const std::string message =
            "ball=" + SafeName(ball) +
            " id=" + IdText(ball) +
            " mass=" + std::to_string(mass) +
            " physicalized=" + BoolText(physicalized) +
            " fixed=" + BoolText(fixed);
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

void FlexGravityMod::PrintUStatus(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'flexgravity reload'.");
        return;
    }

    CK3dEntity *ball = FindActiveBall();
    std::vector<VxVector> ballSamples;
    if (ball) {
        ballSamples = BuildObjectSamples(ball);
    }

    int touches = 0;
    for (const UFieldInfo &field : m_Scan.uFields) {
        const bool ballTouches = ball && ObjectTouchesRegion(field.region, field.objectId, ballSamples);
        if (ballTouches) {
            ++touches;
        }
        LogInfo("U[%s] g=(%.4f, %.4f, %.4f) mesh=%s bbox=%s ballTouches=%s",
                field.objectName.c_str(), field.gravity.x, field.gravity.y, field.gravity.z,
                BoolText(field.region.meshValid).c_str(), BoolText(field.region.bboxValid).c_str(),
                BoolText(ballTouches).c_str());
    }

    const std::string message =
            "U fields=" + std::to_string(m_Scan.uFields.size()) +
            " ballTouches=" + std::to_string(touches) +
            " channels=" + std::to_string(m_ForceChannels.size()) +
            " lastAffected=" + std::to_string(m_LastAffectedCount);
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

void FlexGravityMod::PrintFieldStatus(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'flexgravity reload'.");
        return;
    }

    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        SendMessage("No active ball found.");
        return;
    }

    const std::vector<VxVector> ballSamples = BuildObjectSamples(ball);
    const VxVector center = ballSamples.empty() ? VxVector(0.0f, 0.0f, 0.0f) : ballSamples[0];

    std::vector<char> rowTouched(m_Scan.rRows.size(), 0);
    for (size_t index = 0; index < m_Scan.rRows.size(); ++index) {
        const RRowInfo &rRow = m_Scan.rRows[index];
        rowTouched[index] = ObjectTouchesRegion(rRow.region, rRow.regionObjectId, ballSamples) ? 1 : 0;
    }

    VxVector total(0.0f, 0.0f, 0.0f);
    LogInfo("Field status for %s center=%s", SafeName(ball).c_str(), FormatVec(center).c_str());

    for (const UFieldInfo &field : m_Scan.uFields) {
        const bool ballTouches = ObjectTouchesRegion(field.region, field.objectId, ballSamples);
        if (ballTouches) {
            total = Add(total, field.gravity);
        }
        LogInfo("  U[%s] touch=%s g=(%.4f, %.4f, %.4f)", field.objectName.c_str(),
                BoolText(ballTouches).c_str(), field.gravity.x, field.gravity.y, field.gravity.z);
    }

    for (const RRowInfo &rRow : m_Scan.rRows) {
        LogInfo("  R[%s] ballInside=%s sources=%zu", rRow.regionName.c_str(),
                BoolText(ObjectTouchesRegion(rRow.region, rRow.regionObjectId, ballSamples)).c_str(),
                rRow.sourceIds.size());
    }

    for (const SourceInfo &source : m_Scan.sources) {
        const bool active = IsSourceActive(source.objectId, rowTouched);
        VxVector contribution(0.0f, 0.0f, 0.0f);
        std::string detail;
        if (source.kind == SourceInfo::KindMonopole) {
            CK3dEntity *entity = ResolveEntity(source.objectId);
            if (entity) {
                const VxVector position = entity->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
                const VxVector r = Subtract(position, center);
                detail = " d=" + std::to_string(std::sqrt(LengthSquared(r)));
            }
            contribution = ComputeSourceAcceleration(source, center);
        } else if (source.kind == SourceInfo::KindLine) {
            CK3dEntity *entity = ResolveEntity(source.objectId);
            if (entity && source.localPoints.size() >= 2) {
                const VxMatrix &world = entity->GetWorldMatrix();
                std::vector<VxVector> points;
                points.reserve(source.localPoints.size());
                for (const VxVector &local : source.localPoints) {
                    points.push_back(world * local);
                }
                detail = " d=" + std::to_string(DistanceToPolyline(points, source.closed, center));
            }
            contribution = ComputeSourceAcceleration(source, center);
        } else {
            CK3dEntity *entity = ResolveEntity(source.objectId);
            if (entity) {
                const VxMatrix &world = entity->GetWorldMatrix();
                const VxVector origin = world * source.localCenter;
                VxVector axisA = Subtract(world * Add(source.localCenter, source.localAxisA), origin);
                VxVector axisB = Subtract(world * Add(source.localCenter, source.localAxisB), origin);
                Normalize(axisA, axisA);
                Normalize(axisB, axisB);
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(Cross(axisA, axisB), normal);
                detail = " z=" + std::to_string(
                        std::fabs(Dot(Subtract(center, origin), normal)));
            }
            contribution = ComputeSourceAcceleration(source, center);
        }
        if (active) {
            total = Add(total, contribution);
        }
        LogInfo("  %s[%s] strength=%.4f active=%s%s g=(%.4f, %.4f, %.4f)",
                SourceKindName(source.kind), source.objectName.c_str(), source.strength,
                BoolText(active).c_str(), detail.c_str(),
                contribution.x, contribution.y, contribution.z);
    }

    const std::string message =
            "sources=" + std::to_string(m_Scan.sources.size()) +
            " R rows=" + std::to_string(m_Scan.rRows.size()) +
            " global=" + std::to_string(m_Scan.globalSourceIds.size()) +
            " total=" + FormatVec(total) +
            " channels=" + std::to_string(m_ForceChannels.size());
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

void FlexGravityMod::PrintStatus(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'flexgravity reload'.");
        return;
    }

    const size_t mCount = static_cast<size_t>(std::count_if(
            m_Scan.sources.begin(), m_Scan.sources.end(),
            [](const SourceInfo &s) { return s.kind == SourceInfo::KindMonopole; }));
    const size_t lCount = static_cast<size_t>(std::count_if(
            m_Scan.sources.begin(), m_Scan.sources.end(),
            [](const SourceInfo &s) { return s.kind == SourceInfo::KindLine; }));
    const size_t sCount = static_cast<size_t>(std::count_if(
            m_Scan.sources.begin(), m_Scan.sources.end(),
            [](const SourceInfo &s) { return s.kind == SourceInfo::KindSheet; }));

    const std::string summary =
            "status: U(group=" + BoolText(m_Scan.uGroup.present) +
            ", fields=" + std::to_string(m_Scan.uFields.size()) +
            ") M(group=" + BoolText(m_Scan.mGroup.present) +
            ", sources=" + std::to_string(mCount) +
            ") L(group=" + BoolText(m_Scan.lGroup.present) +
            ", sources=" + std::to_string(lCount) +
            ") S(group=" + BoolText(m_Scan.sGroup.present) +
            ", sources=" + std::to_string(sCount) +
            ") R(group=" + BoolText(m_Scan.rGroup.present) +
            ", rows=" + std::to_string(m_Scan.rRows.size()) +
            ") RG(global=" + std::to_string(m_Scan.globalSourceIds.size()) +
            ") warnings=" + std::to_string(m_Scan.warnings.size());

    SendMessage(summary);
    LogInfo("%s", summary.c_str());

    for (const std::string &warning : m_Scan.warnings) {
        LogWarn("scan warning: %s", warning.c_str());
    }
}

void FlexGravityMod::PrintList(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'flexgravity reload'.");
        return;
    }

    LogInfo("--- FlexGravity parsed data ---");
    LogInfo("U fields: %zu", m_Scan.uFields.size());
    for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
        const UFieldInfo &field = m_Scan.uFields[index];
        LogInfo("  U[%zu] %s (id=%lu) g=(%.4f, %.4f, %.4f)",
                index, field.objectName.c_str(),
                static_cast<unsigned long>(field.objectId),
                field.gravity.x, field.gravity.y, field.gravity.z);
    }

    LogInfo("Sources: %zu", m_Scan.sources.size());
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        const SourceInfo &source = m_Scan.sources[index];
        if (source.kind == SourceInfo::KindLine) {
            LogInfo("  L[%zu] %s (id=%lu) Lambda=%.4f curve=%d points=%d closed=%s",
                    index, source.objectName.c_str(),
                    static_cast<unsigned long>(source.objectId), source.strength,
                    source.usedCurve ? 1 : 0, source.controlPointCount,
                    BoolText(source.closed).c_str());
            for (size_t point = 0; point < source.localPoints.size(); ++point) {
                const VxVector &world = source.localPoints[point];
                LogInfo("      p[%zu] local=(%.4f, %.4f, %.4f)", point, world.x, world.y, world.z);
            }
        } else if (source.kind == SourceInfo::KindSheet) {
            LogInfo("  S[%zu] %s (id=%lu) Sigma=%.4f center=(%.4f, %.4f, %.4f) size=(%.4f x %.4f)",
                    index, source.objectName.c_str(),
                    static_cast<unsigned long>(source.objectId), source.strength,
                    source.localCenter.x, source.localCenter.y, source.localCenter.z,
                    source.halfA * 2.0f, source.halfB * 2.0f);
        } else {
            LogInfo("  M[%zu] %s (id=%lu) G=%.4f",
                    index, source.objectName.c_str(),
                    static_cast<unsigned long>(source.objectId), source.strength);
        }
    }

    LogInfo("R rows: %zu", m_Scan.rRows.size());
    for (size_t index = 0; index < m_Scan.rRows.size(); ++index) {
        const RRowInfo &row = m_Scan.rRows[index];
        std::string names;
        for (const std::string &name : row.sourceNames) {
            if (!names.empty()) {
                names += ", ";
            }
            names += name;
        }
        LogInfo("  R[%zu] %s -> [%s]", index, row.regionName.c_str(), names.c_str());
    }

    LogInfo("Global sources (FG_RG): %zu", m_Scan.globalSourceIds.size());
    for (size_t index = 0; index < m_Scan.globalSourceNames.size(); ++index) {
        LogInfo("  RG[%zu] %s", index, m_Scan.globalSourceNames[index].c_str());
    }

    SendMessage("Parsed data written to ModLoader.log.");
}

void FlexGravityMod::Reload(IBML *bml) {
    (void) bml;
    Scan(true);
    SendMessage("Scan reloaded; see ModLoader.log.");
}

void FlexGravityMod::SetManualForce(IBML *bml, const std::vector<std::string> &args) {
    (void) bml;
    if (args.size() < 5) {
        SendMessage("Usage: flexgravity force <x> <y> <z>   (acceleration, e.g. 0 20 0)");
        return;
    }

    float values[3] = {};
    for (int index = 0; index < 3; ++index) {
        const char *text = args[static_cast<size_t>(index + 2)].c_str();
        char *end = nullptr;
        const float value = std::strtof(text, &end);
        if (end == text || (end && *end != '\0')) {
            SendMessage("Invalid float: " + args[static_cast<size_t>(index + 2)]);
            return;
        }
        values[index] = value;
    }

    m_ManualAcceleration = VxVector(values[0], values[1], values[2]);
    if (LengthSquared(m_ManualAcceleration) <= 1.0e-8f) {
        ClearManualForce(bml);
        return;
    }

    m_ManualForceActive = true;
    m_MissingMassWarned = false;
    ClearAllFieldForces();
    ApplyCurrentManualForce();

    const std::string message =
            "manual acceleration set to (" + std::to_string(values[0]) + ", " +
            std::to_string(values[1]) + ", " + std::to_string(values[2]) + ")";
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

void FlexGravityMod::ClearManualForce(IBML *bml) {
    (void) bml;
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    SendMessage("manual force cleared.");
}

// ---------------------------------------------------------------------------
// IMod lifecycle
// ---------------------------------------------------------------------------

void FlexGravityMod::OnLoad() {
    LogInfo("FlexGravity 0.1.0 loading (BML+ %s)", BML_VERSION);

    IConfig *config = GetConfig();
    if (config) {
        config->SetCategoryComment("Debug", "FlexGravity diagnostics.");
        IProperty *debug = config->GetProperty("Debug", "DebugMode");
        if (debug) {
            debug->SetDefaultBoolean(false);
            debug->SetComment("Write detailed scan/physics diagnostics to ModLoader.log.");
            m_DebugMode = debug->GetBoolean();
        }
    }

    // The command is always registered; the debug subcommands check DebugMode
    // themselves, so turning it on/off takes effect immediately.
    m_BML->RegisterCommand(new FlexGravityCommand(this));
    m_CommandRegistered = true;

    m_BML->SendIngameMessage("FlexGravity 0.1.0 loaded.");
    LogInfo("FlexGravity 0.1.0 loaded; DebugMode=%s, console: fg debug <command>",
            m_DebugMode ? "on" : "off");
}

void FlexGravityMod::RegisterDebugCommand() {
    if (m_CommandRegistered || !m_BML) {
        return;
    }
    m_BML->RegisterCommand(new FlexGravityCommand(this));
    m_CommandRegistered = true;
}

void FlexGravityMod::OnUnload() {
    LogInfo("FlexGravity unloading");
    m_TearingDown = true;
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    DestroyForceChannels();
    m_Bodies.clear();
    m_Scan = ScanInfo{};
}

void FlexGravityMod::OnStartLevel() {
    m_TearingDown = false;

    CKBehavior *levelInit = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    if (!m_ForceChannels.empty()) {
        if (levelInit && levelInit == m_OwnerScript) {
            // Same Level_Init object: this was a level reset, and the force
            // behaviors survived with a handle to the previous rigid body.
            // They are rebuilt on the next frame, outside script execution.
            m_PendingChannelRebuild = true;
        } else {
            // The level was reloaded (not just reset): the old script is gone
            // and every behavior we created with it is dangling.
            LogInfo("Level init changed; dropping %zu stale force channel(s)",
                    m_ForceChannels.size());
            m_ForceChannels.clear();
        }
    }
    m_OwnerScript = levelInit;

    LogInfo("OnStartLevel: scanning FlexGravity groups/arrays");
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    m_ForceApplied = false;
    m_ForceObjectId = 0;
    m_MissingMassWarned = false;
    m_ForceBackendFailed = false;
    m_LastAffectedCount = -1;

    Scan(m_DebugMode);

    CK3dEntity *ball = FindActiveBall();
    if (ball) {
        bool found = false;
        const float mass = GetBodyMass(ball, &found);
        LogInfo("Active ball at level start: %s id=%s mass=%.4f known=%s",
                SafeName(ball).c_str(), IdText(ball).c_str(), mass, BoolText(found).c_str());
    } else {
        LogWarn("No active ball found at level start");
    }
}

void FlexGravityMod::OnPreLoadLevel() {
    m_TearingDown = true;
    LogInfo("OnPreLoadLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    DestroyForceChannels();
    m_Bodies.clear();
    m_Scan = ScanInfo{};
}

void FlexGravityMod::OnPreExitLevel() {
    m_TearingDown = true;
    LogInfo("OnPreExitLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    DestroyForceChannels();
    m_Bodies.clear();
    m_Scan = ScanInfo{};
}

void FlexGravityMod::OnProcess() {
    if (m_TearingDown) {
        return;
    }
    if (m_PendingChannelRebuild) {
        m_PendingChannelRebuild = false;
        RebuildForceChannels();
    }
    UpdateFields();
    UpdateManualForce();
}

void FlexGravityMod::OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float friction, float elasticity, float mass,
                                   const char *collGroup, CKBOOL startFrozen, CKBOOL enableColl,
                                   CKBOOL calcMassCenter, float linearDamp, float rotDamp, const char *collSurface,
                                   VxVector massCenter, int convexCnt, CKMesh **convexMesh, int ballCnt,
                                   VxVector *ballCenter, float *ballRadius, int concaveCnt,
                                   CKMesh **concaveMesh) {
    (void) friction;
    (void) elasticity;
    (void) collGroup;
    (void) startFrozen;
    (void) enableColl;
    (void) calcMassCenter;
    (void) linearDamp;
    (void) rotDamp;
    (void) collSurface;
    (void) massCenter;
    (void) convexCnt;
    (void) convexMesh;
    (void) ballCnt;
    (void) ballCenter;
    (void) ballRadius;
    (void) concaveCnt;
    (void) concaveMesh;

    if (!target) {
        return;
    }

    BodyInfo body;
    body.objectId = target->GetID();
    body.objectName = SafeName(target);
    body.mass = mass;
    body.fixed = fixed != FALSE;
    body.physicalized = true;
    m_Bodies[body.objectId] = body;

    if (!body.fixed) {
        LogInfo("Physicalize: %s id=%s mass=%.4f fixed=false",
                body.objectName.c_str(), IdText(target).c_str(), body.mass);
    }

    if (m_ForceApplied && m_ForceObjectId == body.objectId) {
        // The old rigid body is being replaced; forget the old handle and let
        // OnProcess reapply the probe force to the new body.
        m_ForceApplied = false;
        m_ForceObjectId = 0;
    }

    auto channel = m_ForceChannels.find(body.objectId);
    if (channel != m_ForceChannels.end()) {
        // The rigid body was replaced (respawn, ball switch, ...).  The force
        // behavior still holds the previous body, so it has to be rebuilt
        // before it is used again.
        channel->second.active = false;
        channel->second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel->second.lastMass = 0.0f;
        if (channel->second.behavior) {
            channel->second.needsRebuild = true;
        }
    }
}

void FlexGravityMod::OnUnphysicalize(CK3dEntity *target) {
    if (!target) {
        return;
    }

    const CK_ID objectId = target->GetID();
    auto it = m_Bodies.find(objectId);
    if (it != m_Bodies.end()) {
        it->second.physicalized = false;
    }

    if (m_ForceApplied && m_ForceObjectId == objectId) {
        if (!m_TearingDown) {
            ExecuteBB::UnsetPhysicsForce(target);
        }
        m_ForceApplied = false;
        m_ForceObjectId = 0;
    }

    auto channel = m_ForceChannels.find(objectId);
    if (channel != m_ForceChannels.end()) {
        // The rigid body is already being destroyed; calling into the behavior
        // here (UnsetPhysicsForce) hangs the engine.  Just forget the state.
        channel->second.active = false;
        channel->second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel->second.lastMass = 0.0f;
    }
}

void FlexGravityMod::OnDead() {
    LogInfo("OnDead: dropping field forces");
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    ClearAllFieldForces();
}

void FlexGravityMod::OnPreResetLevel() {
    m_TearingDown = true;
    LogInfo("OnPreResetLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    m_ManualForceActive = false;
    m_ManualAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    ClearAppliedForce();
    // The engine is about to destroy every rigid body: forget the forces
    // without executing a single behavior.
    DropAllFieldForces();
    m_Bodies.clear();
    m_ForceApplied = false;
    m_ForceObjectId = 0;
}

void FlexGravityMod::OnModifyConfig(const char *category, const char *key, IProperty *prop) {
    if (!category || !key || !prop) {
        return;
    }
    if (_stricmp(category, "Debug") == 0 && _stricmp(key, "DebugMode") == 0) {
        m_DebugMode = prop->GetBoolean();
        LogInfo("Config: Debug.DebugMode=%s", m_DebugMode ? "true" : "false");
    }
}

// ---------------------------------------------------------------------------
// Command
// ---------------------------------------------------------------------------

void FlexGravityCommand::Execute(IBML *bml, const std::vector<std::string> &args) {
    if (!m_Mod) {
        return;
    }

    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };

    // fg
    if (args.size() < 2) {
        bml->SendIngameMessage("FlexGravity: usage: fg debug <command>");
        return;
    }

    // fg <group>
    if (lower(args[1]) != "debug") {
        const std::string message = "FlexGravity: unknown group '" + args[1] + "'. Try: fg debug";
        bml->SendIngameMessage(message.c_str());
        return;
    }

    if (!m_Mod->IsDebugMode()) {
        bml->SendIngameMessage(
                "FlexGravity: debug commands are disabled (set Debug/DebugMode=1 in FlexGravity.cfg).");
        return;
    }

    // fg debug
    if (args.size() < 3) {
        bml->SendIngameMessage(
                "FlexGravity debug: status | list | ball | ustatus | fstatus | force x y z | clear | reload | help");
        return;
    }

    // fg debug <command> [args...]  ->  re-index so handlers see the old layout
    std::vector<std::string> subArgs;
    subArgs.reserve(args.size() - 1);
    subArgs.push_back(args[0]);
    for (size_t index = 2; index < args.size(); ++index) {
        subArgs.push_back(args[index]);
    }

    const std::string sub = lower(subArgs[1]);
    if (sub == "status") {
        m_Mod->PrintStatus(bml);
    } else if (sub == "list") {
        m_Mod->PrintList(bml);
    } else if (sub == "ball") {
        m_Mod->PrintBall(bml);
    } else if (sub == "ustatus") {
        m_Mod->PrintUStatus(bml);
    } else if (sub == "fstatus") {
        m_Mod->PrintFieldStatus(bml);
    } else if (sub == "force") {
        m_Mod->SetManualForce(bml, subArgs);
    } else if (sub == "clear") {
        m_Mod->ClearManualForce(bml);
    } else if (sub == "reload") {
        m_Mod->Reload(bml);
    } else if (sub == "help" || sub == "?") {
        bml->SendIngameMessage(
                "FlexGravity debug: status | list | ball | ustatus | fstatus | force x y z | clear | reload | help");
    } else {
        bml->SendIngameMessage("Unknown FlexGravity debug command. Try: fg debug help");
    }
}

const std::vector<std::string> FlexGravityCommand::GetTabCompletion(
        IBML *bml, const std::vector<std::string> &args) {
    (void) bml;

    constexpr const char *kDebugCommands[] = {
            "status", "list", "ball", "ustatus", "fstatus", "force", "clear", "reload", "help"};

    auto completion = [&]() {
        std::vector<std::string> items;
        for (const char *item : kDebugCommands) {
            items.emplace_back(item);
        }
        return items;
    };

    if (args.size() <= 1) {
        return {"debug"};
    }
    if (args.size() == 2) {
        const std::string second = args[1];
        std::string lowered = second;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.empty()) {
            return {"debug"};
        }
        if (std::string("debug").compare(0, lowered.size(), lowered) == 0) {
            if (lowered == "debug" && m_Mod && m_Mod->IsDebugMode()) {
                return completion();
            }
            return {"debug"};
        }
        return {};
    }
    if (args.size() == 3) {
        if (m_Mod && m_Mod->IsDebugMode() && args[1] == "debug") {
            return completion();
        }
        return {};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

MOD_EXPORT IMod *BMLEntry(IBML *bml) {
    return new FlexGravityMod(bml);
}

MOD_EXPORT void BMLExit(IMod *mod) {
    delete mod;
}
