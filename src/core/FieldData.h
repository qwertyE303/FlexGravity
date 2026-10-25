#pragma once

#include <string>
#include <vector>

#include <VxVector.h>

#include "core/expr/FunctionField.h"
#include "core/RegionVolume.h"
#include "core/Types.h"

namespace fg::core {

// ---------------------------------------------------------------------------
// Everything the scanner produces.  These are plain data: no engine pointers,
// no behaviour, so the parser and the field maths can be tested off-engine.
// ---------------------------------------------------------------------------

struct UFieldInfo {
    ObjectId objectId = 0;
    std::string objectName;
    VxVector gravity = VxVector(0.0f, 0.0f, 0.0f);
    RegionVolume region;
};

struct SourceInfo {
    enum Kind {
        KindMonopole = 0,
        KindLine = 1,
        KindSheet = 2
    };

    ObjectId objectId = 0;
    std::string objectName;
    Kind kind = KindMonopole;
    float strength = 0.0f;

    // Line: polyline in the object's local space (curve control points).
    std::vector<VxVector> localPoints;
    bool closed = false;
    int controlPointCount = 0;
    bool usedCurve = false;

    // Sheet, mesh path: the object's mesh tessellated into convex planar
    // patches, in the object's local space.  Patch i owns the vertices
    // [sheetOffsets[i], sheetOffsets[i + 1]); sheetNormals[i] is its unit
    // normal.  A flat rectangle keeps its two triangles merged back into one
    // quad, so a mesh sheet evaluates like the single-rectangle formula.
    std::vector<VxVector> sheetVertices;
    std::vector<int> sheetOffsets;
    std::vector<VxVector> sheetNormals;
    int sheetTriangleCount = 0;
    int sheetMergedCount = 0;

    int SheetPatchCount() const {
        return sheetOffsets.empty() ? 0 : static_cast<int>(sheetOffsets.size()) - 1;
    }

    // Sheet, fallback path: a world-space parallelogram built from the local
    // bounding box.  Only used when the object has no usable mesh.
    VxVector localCenter = VxVector(0.0f, 0.0f, 0.0f);
    VxVector localAxisA = VxVector(1.0f, 0.0f, 0.0f);
    VxVector localAxisB = VxVector(0.0f, 1.0f, 0.0f);
    VxVector localNormal = VxVector(0.0f, 0.0f, 1.0f);
    float halfA = 0.0f;
    float halfB = 0.0f;

    bool geometryValid = false;
};

struct RRowInfo {
    ObjectId regionObjectId = 0;
    std::string regionName;
    RegionVolume region;
    std::vector<ObjectId> sourceIds;
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
    ArrayScan customArray;

    std::vector<UFieldInfo> uFields;
    std::vector<SourceInfo> sources;
    std::vector<RRowInfo> rRows;
    // FG_Custom rows.  Rejected rows stay in the list so the debug reports can
    // show why they were rejected; the runtime skips them.
    std::vector<expr::CustomFieldInfo> customFields;
    std::vector<ObjectId> globalSourceIds;
    std::vector<std::string> globalSourceNames;

    std::vector<std::string> warnings;

    // Array rows dropped because the object is not a member of the matching
    // group.  Group membership is mandatory: an array entry alone does nothing.
    int ignoredEntries = 0;
};

// "M" / "L" / "S", used by the field status report.
const char *SourceKindName(SourceInfo::Kind kind);

} // namespace fg::core
