#include "mod/FlexGravityMod.h"

#include <unordered_map>
#include <unordered_set>

// v2.1 data layout.  Group membership is mandatory for every field and region:
// a row whose object is not in the matching group is invalid, not merely a
// warning.  Only FG_Local / FG_Global are pure arrays with no group.
//
//   FG_UField / FG_MField / FG_LField / FG_SField : group + array (Object + value)
//   FG_Region                                    : group only
//   FG_CField                                    : group + array (Name object, Mode, 6 strings)
//   FG_CRegion                                   : group + array (Name object, Domain)
//   FG_Local                                     : array (Region, Source1..N)
//   FG_Global                                    : array (Source1..N)

namespace {

struct Lookup {
    std::unordered_map<CK_ID, int> byObject;
};

std::string JoinNames(const std::vector<std::string> &names) {
    std::string joined;
    for (const std::string &name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

} // namespace

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
            Warn(std::string("Group ") + name + " has a null member at index " +
                 std::to_string(index));
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(beObject);
        if (!entity) {
            Warn(std::string("Group ") + name + " member is not a 3D entity: " +
                 ObjectText(beObject));
            continue;
        }

        members[entity->GetID()] = SafeName(entity);
        // DisplayObjectName strips the \x01__ placeholder prefix and converts
        // ANSI/GBK to UTF-8, so the log never receives a raw control byte.
        LogInfo("  [%d] %s (id=%lu, %s)", index, DisplayObjectName(entity).c_str(),
                static_cast<unsigned long>(entity->GetID()), ClassText(entity).c_str());
    }
}

void FlexGravityMod::ScanUArray(const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(kArrayU);
    if (!array) {
        Warn("Array not found: FG_UField");
        return;
    }

    m_Scan.uArray.present = true;
    m_Scan.uArray.rows = array->GetRowCount();
    m_Scan.uArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayU);

    int objectColumn = FindColumn(array, "Object");
    if (objectColumn < 0) {
        objectColumn = 0;
        Warn("FG_UField has no column named Object; falling back to column 0");
    }

    int xColumn = FindColumn(array, "x");
    int yColumn = FindColumn(array, "y");
    int zColumn = FindColumn(array, "z");
    if (xColumn < 0 || yColumn < 0 || zColumn < 0) {
        Warn("FG_UField is missing one or more x/y/z columns");
        if (xColumn < 0) xColumn = 1;
        if (yColumn < 0) yColumn = 2;
        if (zColumn < 0) zColumn = 3;
    }

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < m_Scan.uArray.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, objectColumn);
        if (!cellObject) {
            LogInfo("  FG_UField row[%d]: empty object cell", row);
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(cellObject);
        if (!entity) {
            Warn("FG_UField row " + std::to_string(row) + " object is not a 3D entity: " +
                 ObjectText(cellObject));
            ++m_Scan.ignoredEntries;
            continue;
        }

        UFieldInfo field;
        field.objectId = entity->GetID();
        field.objectName = SafeName(entity);
        field.gravity = VxVector(GetCellFloat(array, row, xColumn, 0.0f),
                                 GetCellFloat(array, row, yColumn, 0.0f),
                                 GetCellFloat(array, row, zColumn, 0.0f));

        if (!seen.insert(field.objectId).second) {
            Warn("FG_UField lists object more than once: " + field.objectName);
        }
        if (groupMembers.find(field.objectId) == groupMembers.end()) {
            Warn("FG_UField object is not in the FG_UField group: " + field.objectName +
                 " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }

        BuildRegionCache(entity, field.region, field.objectName.c_str());
        m_Scan.uFields.push_back(field);
        LogInfo("  FG_UField row[%d]: %s g=(%.4f, %.4f, %.4f)", row, field.objectName.c_str(),
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
            ++m_Scan.ignoredEntries;
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
                 " group: " + source.objectName + " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
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
            LogInfo("  FG_LField row[%d]: %s Lambda=%.4f curve=%d points=%d closed=%s A=%s B=%s",
                    row, source.objectName.c_str(), source.strength,
                    source.usedCurve ? 1 : 0, source.controlPointCount,
                    BoolText(source.closed).c_str(), first.c_str(), last.c_str());
        } else if (kind == SourceInfo::KindSheet) {
            const VxMatrix &world = entity->GetWorldMatrix();
            if (source.SheetPatchCount() > 0) {
                VxVector sum(0.0f, 0.0f, 0.0f);
                VxVector minimum(1.0e6f, 1.0e6f, 1.0e6f);
                VxVector maximum(-1.0e6f, -1.0e6f, -1.0e6f);
                for (const VxVector &local : source.sheetVertices) {
                    sum = Add(sum, world * local);
                    minimum.x = (std::min)(minimum.x, local.x);
                    minimum.y = (std::min)(minimum.y, local.y);
                    minimum.z = (std::min)(minimum.z, local.z);
                    maximum.x = (std::max)(maximum.x, local.x);
                    maximum.y = (std::max)(maximum.y, local.y);
                    maximum.z = (std::max)(maximum.z, local.z);
                }
                const VxVector center =
                        Scale(sum, 1.0f / static_cast<float>(source.sheetVertices.size()));
                const int begin = source.sheetOffsets[0];
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(Cross(Subtract(world * source.sheetVertices[begin + 1],
                                         world * source.sheetVertices[begin]),
                                Subtract(world * source.sheetVertices[begin + 2],
                                         world * source.sheetVertices[begin])),
                          normal);
                const VxVector size = Subtract(maximum, minimum);
                LogInfo("  FG_SField row[%d]: %s Sigma=%.4f patches=%d tris=%d merged=%d "
                        "center=%s size=(%.4f x %.4f x %.4f) normal=%s",
                        row, source.objectName.c_str(), source.strength,
                        source.SheetPatchCount(), source.sheetTriangleCount,
                        source.sheetMergedCount, FormatVec(center).c_str(), size.x, size.y, size.z,
                        FormatVec(normal).c_str());
            } else {
                const VxVector center = world * source.localCenter;
                VxVector axisA(0.0f, 0.0f, 0.0f);
                VxVector axisB(0.0f, 0.0f, 0.0f);
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(world * source.localAxisA, axisA);
                Normalize(world * source.localAxisB, axisB);
                Normalize(Cross(axisA, axisB), normal);
                LogInfo("  FG_SField row[%d]: %s Sigma=%.4f rect center=%s size=(%.4f x %.4f) "
                        "normal=%s",
                        row, source.objectName.c_str(), source.strength,
                        FormatVec(center).c_str(), source.halfA * 2.0f, source.halfB * 2.0f,
                        FormatVec(normal).c_str());
            }
        } else {
            LogInfo("  FG_MField row[%d]: %s G=%.4f", row, source.objectName.c_str(),
                    source.strength);
        }
    }
}

// FG_Region has no array: the group members themselves are the regions.
void FlexGravityMod::ScanRegionGroup(const std::unordered_map<CK_ID, std::string> &members) {
    for (const auto &entry : members) {
        CK3dEntity *entity = ResolveEntity(entry.first);
        if (!entity) {
            Warn("FG_Region member disappeared while scanning: " + entry.second);
            continue;
        }
        RegionMember region;
        region.objectId = entry.first;
        region.objectName = entry.second;
        BuildRegionCache(entity, region.volume, entry.second.c_str());
        m_Scan.regions.push_back(region);
        LogInfo("  FG_Region member: %s", entry.second.c_str());
    }
}

void FlexGravityMod::ScanCFieldArray(const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(kArrayCField);
    if (!array) {
        Warn("Array not found: FG_CField");
        return;
    }

    m_Scan.cFieldArray.present = true;
    m_Scan.cFieldArray.rows = array->GetRowCount();
    m_Scan.cFieldArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayCField);

    static const char *const kComponentColumns[6] = {"X", "Y", "Z", "R", "Theta", "Phi"};

    const int nameColumn = FindColumn(array, "Name");
    const int modeColumn = FindColumn(array, "Mode");
    if (nameColumn < 0) {
        Warn("FG_CField has no column named Name");
        return;
    }
    if (array->GetColumnType(nameColumn) != CKARRAYTYPE_OBJECT) {
        Warn("FG_CField column Name is not an Object column; every row is ignored");
        return;
    }
    if (m_Scan.cFieldArray.columns != 8) {
        Warn("FG_CField has " + std::to_string(m_Scan.cFieldArray.columns) +
             " columns; expected 8 (Name, Mode, X, Y, Z, R, Theta, Phi)");
    }

    int componentColumn[6] = {-1, -1, -1, -1, -1, -1};
    for (int index = 0; index < 6; ++index) {
        componentColumn[index] = FindColumn(array, kComponentColumns[index]);
        if (componentColumn[index] < 0) {
            Warn(std::string("FG_CField has no column named ") + kComponentColumns[index]);
        }
    }

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < m_Scan.cFieldArray.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, nameColumn);
        if (!cellObject) {
            LogInfo("  FG_CField row[%d]: empty Name cell", row);
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(cellObject);
        if (!entity) {
            Warn("FG_CField row " + std::to_string(row) + " Name is not a 3D entity: " +
                 ObjectText(cellObject));
            ++m_Scan.ignoredEntries;
            continue;
        }

        const std::string rawName = SafeName(entity);
        if (!HasPlaceholderPrefix(rawName)) {
            Warn("FG_CField row " + std::to_string(row) +
                 " object name lacks the \\x01__ prefix: " + DisplayName(rawName) + " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }
        if (groupMembers.find(entity->GetID()) == groupMembers.end()) {
            Warn("FG_CField object is not in the FG_CField group: " + DisplayName(rawName) +
                 " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }
        if (!seen.insert(entity->GetID()).second) {
            Warn("FG_CField lists object more than once: " + DisplayName(rawName) +
                 " (duplicate row ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }

        int mode = 0;
        if (modeColumn >= 0 && GetCellInt(array, row, modeColumn, mode)) {
            if (mode != 0 && mode != 1) {
                Warn("FG_CField row " + std::to_string(row) + " has Mode=" +
                     std::to_string(mode) + "; using rectangular coordinates");
                mode = 0;
            }
        } else {
            mode = 0;
        }

        std::string text[6];
        for (int index = 0; index < 6; ++index) {
            if (componentColumn[index] >= 0) {
                GetCellString(array, row, componentColumn[index], text[index]);
            }
        }

        CFieldRow field;
        field.objectId = entity->GetID();
        field.objectName = rawName;
        field.displayName = DisplayName(rawName);
        CompileCField(text, mode, field.compiled);
        field.compiled.name = field.displayName;

        const int group = field.compiled.mode;
        if (!field.compiled.error.empty()) {
            Warn("FG_CField row " + std::to_string(row) + " \"" + field.displayName +
                 "\": " + field.compiled.error + " (component treated as 0)");
        }
        for (int component = 0; component < 3; ++component) {
            if (field.compiled.emptyComponent[component]) {
                Warn("FG_CField row " + std::to_string(row) + " \"" + field.displayName + "\": " +
                     kComponentColumns[group * 3 + component] + " is empty, treated as 0");
            }
        }

        std::string detail = "  FG_CField row[" + std::to_string(row) + "]: \"" +
                             field.displayName + "\" mode=" + std::to_string(field.compiled.mode);
        for (int component = 0; component < 3; ++component) {
            const int column = group * 3 + component;
            detail += " ";
            detail += kComponentColumns[column];
            detail += "(len=" + std::to_string(field.compiled.text[column].size()) + ")=\"" +
                      field.compiled.text[column] + "\"";
        }
        LogInfo("%s", detail.c_str());

        m_Scan.cFields.push_back(field);
    }

    LogInfo("FG_CField: %zu row(s) parsed", m_Scan.cFields.size());
}

void FlexGravityMod::ScanCRegionArray(const std::unordered_map<CK_ID, std::string> &groupMembers) {
    CKDataArray *array = FindArray(kArrayCRegion);
    if (!array) {
        Warn("Array not found: FG_CRegion");
        return;
    }

    m_Scan.cRegionArray.present = true;
    m_Scan.cRegionArray.rows = array->GetRowCount();
    m_Scan.cRegionArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayCRegion);

    const int nameColumn = FindColumn(array, "Name");
    const int domainColumn = FindColumn(array, "Domain");
    if (nameColumn < 0) {
        Warn("FG_CRegion has no column named Name");
        return;
    }
    if (array->GetColumnType(nameColumn) != CKARRAYTYPE_OBJECT) {
        Warn("FG_CRegion column Name is not an Object column; every row is ignored");
        return;
    }
    if (domainColumn < 0) {
        Warn("FG_CRegion has no column named Domain; every row applies everywhere");
    }
    if (m_Scan.cRegionArray.columns != 2) {
        Warn("FG_CRegion has " + std::to_string(m_Scan.cRegionArray.columns) +
             " columns; expected 2 (Name, Domain)");
    }

    std::unordered_set<CK_ID> seen;
    for (int row = 0; row < m_Scan.cRegionArray.rows; ++row) {
        CKObject *cellObject = GetCellObject(array, row, nameColumn);
        if (!cellObject) {
            LogInfo("  FG_CRegion row[%d]: empty Name cell", row);
            continue;
        }

        CK3dEntity *entity = CK3dEntity::Cast(cellObject);
        if (!entity) {
            Warn("FG_CRegion row " + std::to_string(row) + " Name is not a 3D entity: " +
                 ObjectText(cellObject));
            ++m_Scan.ignoredEntries;
            continue;
        }

        const std::string rawName = SafeName(entity);
        if (!HasPlaceholderPrefix(rawName)) {
            Warn("FG_CRegion row " + std::to_string(row) +
                 " object name lacks the \\x01__ prefix: " + DisplayName(rawName) + " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }
        if (groupMembers.find(entity->GetID()) == groupMembers.end()) {
            Warn("FG_CRegion object is not in the FG_CRegion group: " + DisplayName(rawName) +
                 " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }
        if (!seen.insert(entity->GetID()).second) {
            Warn("FG_CRegion lists object more than once: " + DisplayName(rawName) +
                 " (duplicate row ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }

        CRegionRow region;
        region.objectId = entity->GetID();
        region.objectName = rawName;
        region.displayName = DisplayName(rawName);
        if (domainColumn >= 0) {
            GetCellString(array, row, domainColumn, region.domainText);
        }

        region.valid = true;
        if (region.domainText.empty()) {
            // Defensive: the editor requires a non-empty Domain.  An empty one
            // means "no restriction" and is reported, not fatal.
            Warn("FG_CRegion \"" + region.displayName +
                 "\" has an empty Domain, so it applies everywhere");
        } else {
            CompileError error;
            if (!CompileDomain(region.domainText, region.domain, error)) {
                region.valid = false;
                region.error = error.message + " (byte " + std::to_string(error.position) + ")";
                Warn("FG_CRegion \"" + region.displayName + "\" rejected: " + region.error);
            }
        }

        LogInfo("  FG_CRegion row[%d]: \"%s\" domain(len=%zu)=\"%s\"%s", row,
                region.displayName.c_str(), region.domainText.size(), region.domainText.c_str(),
                region.valid ? "" : " [rejected]");
        m_Scan.cRegions.push_back(region);
    }

    LogInfo("FG_CRegion: %zu row(s) parsed", m_Scan.cRegions.size());
}

void FlexGravityMod::ScanLocalArray(
        const std::unordered_map<CK_ID, int> &regionIndex,
        const std::unordered_map<CK_ID, int> &cRegionIndex,
        const std::unordered_map<CK_ID, int> &sourceIndex,
        const std::unordered_map<CK_ID, int> &cFieldIndex,
        const std::unordered_set<CK_ID> &uObjects) {
    CKDataArray *array = FindArray(kArrayLocal);
    if (!array) {
        Warn("Array not found: FG_Local");
        return;
    }

    m_Scan.localArray.present = true;
    m_Scan.localArray.rows = array->GetRowCount();
    m_Scan.localArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayLocal);

    if (m_Scan.localArray.columns < 2) {
        Warn("FG_Local needs a Region column and at least one Source column");
        return;
    }

    for (int row = 0; row < m_Scan.localArray.rows; ++row) {
        CKObject *regionCell = GetCellObject(array, row, 0);
        if (!regionCell) {
            Warn("FG_Local row " + std::to_string(row) + " has an empty Region cell (row ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }

        CK3dEntity *regionEntity = CK3dEntity::Cast(regionCell);
        if (!regionEntity) {
            Warn("FG_Local row " + std::to_string(row) + " Region is not a 3D entity: " +
                 ObjectText(regionCell));
            ++m_Scan.ignoredEntries;
            continue;
        }

        LocalRow local;
        auto builtin = regionIndex.find(regionEntity->GetID());
        auto custom = cRegionIndex.find(regionEntity->GetID());
        if (builtin != regionIndex.end()) {
            local.regionKind = LocalRow::BuiltinRegion;
            local.regionIndex = builtin->second;
        } else if (custom != cRegionIndex.end()) {
            local.regionKind = LocalRow::CustomRegion;
            local.cRegionIndex = custom->second;
            if (!m_Scan.cRegions[static_cast<size_t>(local.cRegionIndex)].valid) {
                Warn("FG_Local row " + std::to_string(row) +
                     " references a rejected FG_CRegion; row ignored");
                ++m_Scan.ignoredEntries;
                continue;
            }
        } else {
            Warn("FG_Local row " + std::to_string(row) +
                 " Region is neither an FG_Region member nor a valid FG_CRegion: " +
                 SafeName(regionEntity) + " (row ignored)");
            ++m_Scan.ignoredEntries;
            continue;
        }

        std::unordered_set<CK_ID> seen;
        std::vector<std::string> names;
        for (int column = 1; column < m_Scan.localArray.columns; ++column) {
            CKObject *cell = GetCellObject(array, row, column);
            if (!cell) {
                continue;
            }
            CK3dEntity *sourceEntity = CK3dEntity::Cast(cell);
            if (!sourceEntity) {
                Warn("FG_Local row " + std::to_string(row) + " column " + std::to_string(column) +
                     " is not a 3D entity: " + ObjectText(cell));
                continue;
            }

            const CK_ID id = sourceEntity->GetID();
            if (!seen.insert(id).second) {
                continue;
            }

            auto builtinSource = sourceIndex.find(id);
            auto customSource = cFieldIndex.find(id);
            auto forbiddenU = uObjects.find(id);
            if (builtinSource != sourceIndex.end()) {
                SourceRef ref;
                ref.kind = SourceRef::Builtin;
                ref.index = builtinSource->second;
                local.sources.push_back(ref);
                names.push_back(SafeName(sourceEntity));
            } else if (customSource != cFieldIndex.end()) {
                const CFieldRow &field = m_Scan.cFields[static_cast<size_t>(customSource->second)];
                if (!field.compiled.valid) {
                    Warn("FG_Local references a rejected FG_CField: " + field.displayName);
                    continue;
                }
                SourceRef ref;
                ref.kind = SourceRef::Custom;
                ref.index = customSource->second;
                local.sources.push_back(ref);
                names.push_back(field.displayName);
            } else if (forbiddenU != uObjects.end()) {
                Warn("FG_Local uses a uniform field (FG_UField) as a source, which is not allowed: " +
                     SafeName(sourceEntity));
                continue;
            } else {
                Warn("FG_Local row " + std::to_string(row) + " column " +
                     std::to_string(column) + " is not a known source: " +
                     SafeName(sourceEntity));
                continue;
            }
        }

        const std::string regionName = local.regionKind == LocalRow::BuiltinRegion
                                               ? SafeName(regionEntity)
                                               : m_Scan.cRegions[static_cast<size_t>(local.cRegionIndex)]
                                                         .displayName;
        LogInfo("  FG_Local row[%d]: region=%s -> [%s]", row, regionName.c_str(),
                JoinNames(names).c_str());
        m_Scan.localRows.push_back(std::move(local));
    }
}

void FlexGravityMod::ScanGlobalArray(const std::unordered_map<CK_ID, int> &sourceIndex,
                                     const std::unordered_map<CK_ID, int> &cFieldIndex,
                                     const std::unordered_set<CK_ID> &uObjects) {
    CKDataArray *array = FindArray(kArrayGlobal);
    if (!array) {
        Warn("Array not found: FG_Global");
        return;
    }

    m_Scan.globalArray.present = true;
    m_Scan.globalArray.rows = array->GetRowCount();
    m_Scan.globalArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayGlobal);

    std::unordered_set<CK_ID> seen;
    std::vector<std::string> names;
    for (int row = 0; row < m_Scan.globalArray.rows; ++row) {
        for (int column = 0; column < m_Scan.globalArray.columns; ++column) {
            CKObject *cell = GetCellObject(array, row, column);
            if (!cell) {
                continue;
            }
            CK3dEntity *sourceEntity = CK3dEntity::Cast(cell);
            if (!sourceEntity) {
                Warn("FG_Global row " + std::to_string(row) + " column " +
                     std::to_string(column) + " is not a 3D entity: " + ObjectText(cell));
                continue;
            }

            const CK_ID id = sourceEntity->GetID();
            if (!seen.insert(id).second) {
                continue;
            }

            auto builtinSource = sourceIndex.find(id);
            auto customSource = cFieldIndex.find(id);
            if (builtinSource != sourceIndex.end()) {
                SourceRef ref;
                ref.kind = SourceRef::Builtin;
                ref.index = builtinSource->second;
                m_Scan.globalSources.push_back(ref);
                names.push_back(SafeName(sourceEntity));
            } else if (customSource != cFieldIndex.end()) {
                const CFieldRow &field = m_Scan.cFields[static_cast<size_t>(customSource->second)];
                if (!field.compiled.valid) {
                    Warn("FG_Global references a rejected FG_CField: " + field.displayName);
                    continue;
                }
                SourceRef ref;
                ref.kind = SourceRef::Custom;
                ref.index = customSource->second;
                m_Scan.globalSources.push_back(ref);
                names.push_back(field.displayName);
            } else if (uObjects.find(id) != uObjects.end()) {
                Warn("FG_Global uses a uniform field (FG_UField) as a source, which is not allowed: " +
                     SafeName(sourceEntity));
            } else {
                Warn("FG_Global row " + std::to_string(row) + " column " +
                     std::to_string(column) + " is not a known source: " +
                     SafeName(sourceEntity));
            }
        }
    }

    if (!names.empty()) {
        LogInfo("FG_Global: [%s]", JoinNames(names).c_str());
    }
}

void FlexGravityMod::ValidateMembership(
        const std::unordered_map<CK_ID, std::string> &uGroupMembers,
        const std::unordered_map<CK_ID, std::string> &mGroupMembers,
        const std::unordered_map<CK_ID, std::string> &lGroupMembers,
        const std::unordered_map<CK_ID, std::string> &sGroupMembers,
        const std::unordered_map<CK_ID, std::string> &cFieldGroupMembers,
        const std::unordered_map<CK_ID, std::string> &cRegionGroupMembers) {
    std::unordered_set<CK_ID> uConfigured;
    for (const UFieldInfo &field : m_Scan.uFields) {
        uConfigured.insert(field.objectId);
    }
    for (const auto &entry : uGroupMembers) {
        if (uConfigured.find(entry.first) == uConfigured.end()) {
            Warn("FG_UField group member has no FG_UField array row: " + entry.second);
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
            Warn("FG_MField group member has no FG_MField array row: " + entry.second);
        }
    }
    for (const auto &entry : lGroupMembers) {
        if (lConfigured.find(entry.first) == lConfigured.end()) {
            Warn("FG_LField group member has no FG_LField array row: " + entry.second);
        }
    }
    for (const auto &entry : sGroupMembers) {
        if (sConfigured.find(entry.first) == sConfigured.end()) {
            Warn("FG_SField group member has no FG_SField array row: " + entry.second);
        }
    }

    std::unordered_set<CK_ID> cFieldConfigured;
    for (const CFieldRow &field : m_Scan.cFields) {
        cFieldConfigured.insert(field.objectId);
    }
    for (const auto &entry : cFieldGroupMembers) {
        if (cFieldConfigured.find(entry.first) == cFieldConfigured.end()) {
            Warn("FG_CField group member has no FG_CField array row: " +
                 DisplayName(entry.second));
        }
    }

    std::unordered_set<CK_ID> cRegionConfigured;
    for (const CRegionRow &region : m_Scan.cRegions) {
        cRegionConfigured.insert(region.objectId);
    }
    for (const auto &entry : cRegionGroupMembers) {
        if (cRegionConfigured.find(entry.first) == cRegionConfigured.end()) {
            Warn("FG_CRegion group member has no FG_CRegion array row: " +
                 DisplayName(entry.second));
        }
    }
}

void FlexGravityMod::Scan() {
    m_Scan = ScanInfo{};
    m_Scan.ran = true;
    m_CustomWarned.clear();

    CKContext *context = Context();
    if (!context) {
        Warn("No CKContext available; cannot scan");
        return;
    }

    // A normal Ballance level has no FlexGravity data at all.  Keep the log
    // quiet in that case instead of producing one warning per missing object.
    // Old FG_* names are deliberately not checked: v2.1 does not read them.
    const bool anyFlexGravityData =
            FindGroup(kGroupU) || FindGroup(kGroupM) || FindGroup(kGroupL) ||
            FindGroup(kGroupS) || FindGroup(kGroupRegion) || FindGroup(kGroupCField) ||
            FindGroup(kGroupCRegion) ||
            FindArray(kArrayU) || FindArray(kArrayM) || FindArray(kArrayL) ||
            FindArray(kArrayS) || FindArray(kArrayCField) || FindArray(kArrayCRegion) ||
            FindArray(kArrayLocal) || FindArray(kArrayGlobal);
    if (!anyFlexGravityData) {
        LogInfo("No FlexGravity data in this level");
        return;
    }

    std::unordered_map<CK_ID, std::string> uGroupMembers;
    std::unordered_map<CK_ID, std::string> mGroupMembers;
    std::unordered_map<CK_ID, std::string> lGroupMembers;
    std::unordered_map<CK_ID, std::string> sGroupMembers;
    std::unordered_map<CK_ID, std::string> regionGroupMembers;
    std::unordered_map<CK_ID, std::string> cFieldGroupMembers;
    std::unordered_map<CK_ID, std::string> cRegionGroupMembers;

    ScanGroup(kGroupU, m_Scan.uGroup, uGroupMembers);
    ScanGroup(kGroupM, m_Scan.mGroup, mGroupMembers);
    ScanGroup(kGroupL, m_Scan.lGroup, lGroupMembers);
    ScanGroup(kGroupS, m_Scan.sGroup, sGroupMembers);
    ScanGroup(kGroupRegion, m_Scan.regionGroup, regionGroupMembers);
    ScanGroup(kGroupCField, m_Scan.cFieldGroup, cFieldGroupMembers);
    ScanGroup(kGroupCRegion, m_Scan.cRegionGroup, cRegionGroupMembers);

    ScanUArray(uGroupMembers);
    ScanSourceArray(SourceInfo::KindMonopole, kArrayM, "G", m_Scan.mArray, mGroupMembers);
    ScanSourceArray(SourceInfo::KindLine, kArrayL, "Lambda", m_Scan.lArray, lGroupMembers);
    ScanSourceArray(SourceInfo::KindSheet, kArrayS, "Sigma", m_Scan.sArray, sGroupMembers);
    ScanRegionGroup(regionGroupMembers);
    ScanCFieldArray(cFieldGroupMembers);
    ScanCRegionArray(cRegionGroupMembers);

    // Object -> row indices, used by FG_Local / FG_Global and by the runtime.
    std::unordered_map<CK_ID, int> sourceIndex;
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        sourceIndex[m_Scan.sources[index].objectId] = static_cast<int>(index);
    }
    std::unordered_map<CK_ID, int> regionIndex;
    for (size_t index = 0; index < m_Scan.regions.size(); ++index) {
        regionIndex[m_Scan.regions[index].objectId] = static_cast<int>(index);
    }
    std::unordered_map<CK_ID, int> cFieldIndex;
    for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
        cFieldIndex[m_Scan.cFields[index].objectId] = static_cast<int>(index);
    }
    std::unordered_map<CK_ID, int> cRegionIndex;
    for (size_t index = 0; index < m_Scan.cRegions.size(); ++index) {
        cRegionIndex[m_Scan.cRegions[index].objectId] = static_cast<int>(index);
    }
    std::unordered_set<CK_ID> uObjects;
    for (const UFieldInfo &field : m_Scan.uFields) {
        uObjects.insert(field.objectId);
    }

    ScanLocalArray(regionIndex, cRegionIndex, sourceIndex, cFieldIndex, uObjects);
    ScanGlobalArray(sourceIndex, cFieldIndex, uObjects);

    ValidateMembership(uGroupMembers, mGroupMembers, lGroupMembers, sGroupMembers,
                       cFieldGroupMembers, cRegionGroupMembers);

    LogInfo("Scan summary: groups(UField=%d MField=%d LField=%d SField=%d Region=%d "
            "CField=%d CRegion=%d) arrays(UField=%d MField=%d LField=%d SField=%d "
            "CField=%d CRegion=%d Local=%d Global=%d) "
            "fields(U=%zu M=%zu L=%zu S=%zu Regions=%zu Local=%zu Global=%zu "
            "CField=%zu CRegion=%zu) ignored=%d warnings=%zu",
            m_Scan.uGroup.count, m_Scan.mGroup.count, m_Scan.lGroup.count, m_Scan.sGroup.count,
            m_Scan.regionGroup.count, m_Scan.cFieldGroup.count, m_Scan.cRegionGroup.count,
            m_Scan.uArray.rows, m_Scan.mArray.rows, m_Scan.lArray.rows, m_Scan.sArray.rows,
            m_Scan.cFieldArray.rows, m_Scan.cRegionArray.rows, m_Scan.localArray.rows,
            m_Scan.globalArray.rows, m_Scan.uFields.size(),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                              [](const SourceInfo &s) {
                                                  return s.kind == SourceInfo::KindMonopole;
                                              })),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                              [](const SourceInfo &s) {
                                                  return s.kind == SourceInfo::KindLine;
                                              })),
            static_cast<size_t>(std::count_if(m_Scan.sources.begin(), m_Scan.sources.end(),
                                              [](const SourceInfo &s) {
                                                  return s.kind == SourceInfo::KindSheet;
                                              })),
            m_Scan.regions.size(), m_Scan.localRows.size(), m_Scan.globalSources.size(),
            m_Scan.cFields.size(), m_Scan.cRegions.size(), m_Scan.ignoredEntries,
            m_Scan.warnings.size());
}
