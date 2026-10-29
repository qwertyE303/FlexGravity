#pragma once

#include <string>
#include <vector>

#include <VxVector.h>

#include "core/expr/FunctionField.h"
#include "core/expr/Parser.h"
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

// A member of the FG_Region group.  The group is the only source of built-in
// regions in v2.1; there is no FG_Region array.
struct RegionMember {
    ObjectId objectId = 0;
    std::string objectName;
    RegionVolume volume;
};

// One FG_CField row.  Matching is by object reference (`objectId`), never by
// name; `displayName` only exists for logs.
struct CFieldRow {
    ObjectId objectId = 0;
    std::string objectName;  // raw bytes from the object table (ANSI/GBK)
    std::string displayName; // objectName without the 0x01 "__" prefix
    expr::CompiledCField compiled;
};

// One FG_CRegion row: an expression-defined region.
struct CRegionRow {
    ObjectId objectId = 0;
    std::string objectName;
    std::string displayName;
    std::string domainText;
    expr::Domain domain;
    bool valid = false;
    std::string error;
};

// A reference to a source from FG_Local / FG_Global.  `Builtin` indexes
// ScanInfo::sources (M/L/S), `Custom` indexes ScanInfo::cFields.
struct SourceRef {
    enum Kind { Builtin = 0, Custom = 1 };
    Kind kind = Builtin;
    int index = -1;
};

// One FG_Local row: a region (built-in or CRegion) plus the sources that only
// apply inside it.
struct LocalRow {
    enum RegionKind { BuiltinRegion = 0, CustomRegion = 1 };
    RegionKind regionKind = BuiltinRegion;
    int regionIndex = -1;  // index into ScanInfo::regions
    int cRegionIndex = -1; // index into ScanInfo::cRegions
    std::vector<SourceRef> sources;
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
    GroupScan regionGroup;
    GroupScan cFieldGroup;
    GroupScan cRegionGroup;

    ArrayScan uArray;
    ArrayScan mArray;
    ArrayScan lArray;
    ArrayScan sArray;
    ArrayScan cFieldArray;
    ArrayScan cRegionArray;
    ArrayScan localArray;
    ArrayScan globalArray;

    std::vector<UFieldInfo> uFields;
    std::vector<SourceInfo> sources;
    std::vector<RegionMember> regions;
    std::vector<CRegionRow> cRegions;
    // Rejected CField rows stay in the list so the debug reports can show why
    // they were rejected; the runtime skips them.
    std::vector<CFieldRow> cFields;
    std::vector<LocalRow> localRows;
    std::vector<SourceRef> globalSources;

    std::vector<std::string> warnings;

    // Rows dropped because the object is not a member of the matching group,
    // because the placeholder prefix is missing, or because it is not a 3D
    // entity.  Membership is mandatory in v2.1.
    int ignoredEntries = 0;
};

// "M" / "L" / "S", used by the field status report.
const char *SourceKindName(SourceInfo::Kind kind);

} // namespace fg::core
