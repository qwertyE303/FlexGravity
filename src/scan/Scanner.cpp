#include "mod/FlexGravityMod.h"

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
            Warn("FG_U object is not in the FG_U group: " + field.objectName + " (ignored)");
            ++m_Scan.ignoredEntries;
            continue;
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
            LogInfo("  FG_L row[%d]: %s Lambda=%.4f curve=%d points=%d closed=%s A=%s B=%s",
                    row, source.objectName.c_str(), source.strength,
                    source.usedCurve ? 1 : 0, source.controlPointCount,
                    BoolText(source.closed).c_str(), first.c_str(), last.c_str());
        } else if (kind == SourceInfo::KindSheet) {
            const VxMatrix &world = entity->GetWorldMatrix();
            if (source.SheetPatchCount() > 0) {
                // World centroid, local bounding box and the first patch's
                // world normal: enough to recognise the object in the log.
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
                const VxVector center = Scale(sum, 1.0f /
                                              static_cast<float>(source.sheetVertices.size()));
                const int begin = source.sheetOffsets[0];
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(Cross(Subtract(world * source.sheetVertices[begin + 1],
                                         world * source.sheetVertices[begin]),
                                Subtract(world * source.sheetVertices[begin + 2],
                                         world * source.sheetVertices[begin])),
                          normal);
                const VxVector size = Subtract(maximum, minimum);
                LogInfo("  FG_S row[%d]: %s Sigma=%.4f patches=%d tris=%d merged=%d "
                        "center=%s size=(%.4f x %.4f x %.4f) normal=%s",
                        row, source.objectName.c_str(), source.strength,
                        source.SheetPatchCount(), source.sheetTriangleCount,
                        source.sheetMergedCount, FormatVec(center).c_str(),
                        size.x, size.y, size.z, FormatVec(normal).c_str());
            } else {
                const VxVector center = world * source.localCenter;
                VxVector axisA(0.0f, 0.0f, 0.0f);
                VxVector axisB(0.0f, 0.0f, 0.0f);
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(world * source.localAxisA, axisA);
                Normalize(world * source.localAxisB, axisB);
                Normalize(Cross(axisA, axisB), normal);
                LogInfo("  FG_S row[%d]: %s Sigma=%.4f rect center=%s size=(%.4f x %.4f) normal=%s",
                        row, source.objectName.c_str(), source.strength,
                        FormatVec(center).c_str(), source.halfA * 2.0f, source.halfB * 2.0f,
                        FormatVec(normal).c_str());
            }
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
            // The region is the scope: without it the row scopes nothing, so
            // the whole row (region + sources) is inert.
            Warn("FG_R region object is not in the FG_R group: " + rRow.regionName + " (row ignored)");
            ++m_Scan.ignoredEntries;
            continue;
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

// FG_Custom is a pure data array: no group, no object column, and the Domain is
// the only thing that decides whether a row applies.  Every row is compiled here
// (once per level), never per frame.
void FlexGravityMod::ScanCustomArray() {
    CKDataArray *array = FindArray(kArrayCustom);
    if (!array) {
        LogInfo("Array FG_Custom not present");
        return;
    }

    m_Scan.customArray.present = true;
    m_Scan.customArray.rows = array->GetRowCount();
    m_Scan.customArray.columns = array->GetColumnCount();
    LogArrayColumns(array, kArrayCustom);

    static const char *const kComponentColumns[6] = {"X", "Y", "Z", "R", "Theta", "Phi"};

    const int nameColumn = FindColumn(array, "Name");
    const int modeColumn = FindColumn(array, "Mode");
    const int domainColumn = FindColumn(array, "Domain");
    int componentColumn[6] = {-1, -1, -1, -1, -1, -1};
    for (int index = 0; index < 6; ++index) {
        componentColumn[index] = FindColumn(array, kComponentColumns[index]);
    }

    if (nameColumn < 0) {
        Warn("FG_Custom has no column named Name");
    }
    if (modeColumn < 0) {
        LogInfo("FG_Custom has no Mode column; every row is rectangular (legacy layout)");
    }
    if (domainColumn < 0) {
        Warn("FG_Custom has no column named Domain");
    }
    for (int index = 0; index < 6; ++index) {
        if (componentColumn[index] < 0) {
            Warn(std::string("FG_Custom has no column named ") + kComponentColumns[index]);
        }
    }

    for (int row = 0; row < m_Scan.customArray.rows; ++row) {
        CustomFieldInfo field;
        bool anyText = false;

        if (nameColumn >= 0 && GetCellString(array, row, nameColumn, field.name)) {
            anyText = anyText || !field.name.empty();
        }
        for (int index = 0; index < 6; ++index) {
            if (componentColumn[index] >= 0 &&
                GetCellString(array, row, componentColumn[index], field.text[index])) {
                anyText = anyText || !field.text[index].empty();
            }
        }
        if (domainColumn >= 0 && GetCellString(array, row, domainColumn, field.domainText)) {
            anyText = anyText || !field.domainText.empty();
        }

        if (!anyText) {
            // The editor's table can end with empty rows; they are not an error.
            LogInfo("  FG_Custom row[%d]: empty row, skipped", row);
            continue;
        }

        int mode = 0;
        const bool modeRead = modeColumn >= 0 && GetCellInt(array, row, modeColumn, mode);
        if (modeRead && mode != 0 && mode != 1) {
            Warn("FG_Custom row " + std::to_string(row) + " has Mode=" + std::to_string(mode) +
                 "; using rectangular coordinates");
            mode = 0;
        }
        field.mode = mode == 1 ? 1 : 0;

        core::expr::CompileCustomField(field);

        const int group = field.mode;
        if (!field.valid) {
            Warn("FG_Custom row " + std::to_string(row) + " \"" + field.name +
                 "\" rejected: " + field.error);
        } else {
            for (int component = 0; component < 3; ++component) {
                const int column = group * 3 + component;
                if (field.text[column].empty()) {
                    Warn("FG_Custom row " + std::to_string(row) + " \"" + field.name + "\": " +
                         kComponentColumns[column] + " is empty, treated as 0");
                }
            }
            if (!field.hasDomain) {
                Warn("FG_Custom row " + std::to_string(row) + " \"" + field.name +
                     "\" has no Domain, so it applies everywhere");
            }
        }

        // The lengths prove the dynamic string read works, which is the one
        // engine-side assumption of this feature.
        std::string detail = "  FG_Custom row[" + std::to_string(row) + "]: \"" + field.name +
                             "\" mode=" + std::to_string(field.mode);
        for (int component = 0; component < 3; ++component) {
            const int column = group * 3 + component;
            detail += " ";
            detail += kComponentColumns[column];
            detail += "(len=" + std::to_string(field.text[column].size()) + ")=\"" +
                      field.text[column] + "\"";
        }
        detail += " domain(len=" + std::to_string(field.domainText.size()) + ")=\"" +
                  field.domainText + "\"";
        if (!field.valid) {
            detail += " [rejected: " + field.error + "]";
        }
        LogInfo("%s", detail.c_str());

        m_Scan.customFields.push_back(field);
    }

    LogInfo("FG_Custom: %zu row(s) parsed", m_Scan.customFields.size());
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
    const bool anyFlexGravityData =
            FindGroup(kGroupU) || FindGroup(kGroupM) || FindGroup(kGroupL) ||
            FindGroup(kGroupS) || FindGroup(kGroupR) ||
            FindArray(kArrayU) || FindArray(kArrayM) || FindArray(kArrayL) ||
            FindArray(kArrayS) || FindArray(kArrayR) || FindArray(kArrayRG) ||
            FindArray(kArrayCustom);
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
    ScanCustomArray();

    ValidateMembership(uGroupMembers, mGroupMembers, lGroupMembers, sGroupMembers, rGroupMembers);

    LogInfo("Scan summary: groups(U=%d M=%d L=%d S=%d R=%d) "
            "arrays(U=%d M=%d L=%d S=%d R=%d RG=%d) "
            "fields(U=%zu M=%zu L=%zu S=%zu RRows=%zu Global=%zu Custom=%zu) warnings=%zu",
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
            m_Scan.rRows.size(), m_Scan.globalSourceIds.size(), m_Scan.customFields.size(),
            m_Scan.warnings.size());
}

