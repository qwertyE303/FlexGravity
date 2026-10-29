// FlexGravity - BML+ 0.3.13 native mod for map-authored gravity fields.
//
// Implementation layout (one responsibility per file):
//   mod/FlexGravityMod.cpp   identity, configuration, entry points
//   mod/Lifecycle.cpp        level and body lifecycle callbacks
//   vt/EngineBindings.*      text helpers and SEH-guarded curve probes
//   vt/EngineAccess.cpp      logging, object lookup, DataArray access
//   vt/Geometry.cpp          region/curve/sheet extraction and region tests
//   vt/ForceBackend.cpp      per-object SetPhysicsForce channels
//   scan/Scanner.cpp         group/array parsing and validation
//   runtime/FieldRuntime.cpp per-frame field evaluation
//   runtime/ManualProbe.cpp  fg debug force probe
//   diagnostics/Reports.cpp  console reports
//   diagnostics/Command.cpp  console command
//   viz/FieldSampler.cpp     field value at an arbitrary point (debug overlay)
//   viz/Overlay.cpp          ImGui debug overlay (arrow, field lines, ...)
//   viz/FieldLines.cpp       cached field line tracing (incremental, world space)
#pragma once

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

// The overlay draws through ImGui's public draw-list API; only the pointer type
// is needed here, the header itself stays out of every other translation unit.
struct ImDrawList;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/BodySamples.h"
#include "core/Constants.h"
#include "core/FieldData.h"
#include "core/Math.h"
#include "core/RegionVolume.h"
#include "core/Sources.h"
#include "core/TextFormat.h"
#include "core/Types.h"
#include "version.h"
#include "vt/EngineBindings.h"

namespace core = fg::core;

// Internal aliases so the implementation files can keep using the core names
// unqualified.
using core::Add;
using core::AxisVector;
using core::BoolText;
using core::BuildBodySamples;
using core::Cross;
using core::Dot;
using core::FormatVec;
using core::GetComponent;
using core::kForceScale;
using core::kMaxCurvePoints;
using core::kNoAccelerationSquared;
using core::kPhysicsStepSeconds;
using core::LengthSquared;
using core::Normalize;
using core::Scale;
using core::SetComponent;
using core::SourceKindName;
using core::Subtract;

using core::ArrayScan;
using core::BodySamples;
using core::CFieldRow;
using core::CRegionRow;
using core::GroupScan;
using core::LocalRow;
using core::RegionMember;
using core::RegionVolume;
using core::ScanInfo;
using core::SourceInfo;
using core::SourceRef;
using core::UFieldInfo;

using fg::core::expr::CompileError;
using fg::core::expr::PointFrame;

using fg::vt::ArrayTypeName;
using fg::vt::ClassText;
using fg::vt::DisplayName;
using fg::vt::DisplayObjectName;
using fg::vt::HasPlaceholderPrefix;
using fg::vt::IdText;
using fg::vt::ObjectText;
using fg::vt::SafeCurveCast;
using fg::vt::SafeCurveControlPoint;
using fg::vt::SafeCurveIsOpen;
using fg::vt::SafeCurvePointCount;
using fg::vt::SafeName;
using fg::vt::SafeWorldPosition;

constexpr const char *kGroupU = "FG_UField";
constexpr const char *kGroupM = "FG_MField";
constexpr const char *kGroupL = "FG_LField";
constexpr const char *kGroupS = "FG_SField";
constexpr const char *kGroupRegion = "FG_Region";
constexpr const char *kGroupCField = "FG_CField";
constexpr const char *kGroupCRegion = "FG_CRegion";

constexpr const char *kArrayU = "FG_UField";
constexpr const char *kArrayM = "FG_MField";
constexpr const char *kArrayL = "FG_LField";
constexpr const char *kArrayS = "FG_SField";
constexpr const char *kArrayCField = "FG_CField";
constexpr const char *kArrayCRegion = "FG_CRegion";
// FG_Local / FG_Global are the only pure-array pairing tables: no group.
constexpr const char *kArrayLocal = "FG_Local";
constexpr const char *kArrayGlobal = "FG_Global";

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

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

// One sheet source copied into world space, rebuilt once per field evaluation
// so the body loop never touches the object's transform.
struct SheetWorldCache {
    std::vector<VxVector> vertices;
    std::vector<VxVector> normals;
    std::vector<int> offsets;
};

class FlexGravityMod;

class FlexGravityCommand final : public ICommand {
public:
    explicit FlexGravityCommand(FlexGravityMod *mod) : m_Mod(mod) {}

    std::string GetName() override { return "fg"; }
    // No alias: the console is opened with '/', the command itself is "fg".
    std::string GetAlias() override { return std::string(); }
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
    const char *GetVersion() override { return FLEXGRAVITY_VERSION; }
    const char *GetName() override { return "FlexGravity"; }
    const char *GetAuthor() override { return "FlexGravity"; }
    const char *GetDescription() override {
        return "Map-authored gravity fields: uniform, monopole, line, sheet and custom "
               "expression fields.";
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

    // Single implementation of "the level is going away": every teardown
    // callback funnels through here so the engine-call rules live in one place.
    // `destroyChannels` = false keeps the SetPhysicsForce behaviors alive for
    // the post-reset rebuild; `clearScan` = false keeps the parsed data across
    // a reset (OnStartLevel rescans anyway).
    void TearDownLevel(bool destroyChannels, bool clearScan);

    void Scan();
    void PrintStatus(IBML *bml);
    void PrintList(IBML *bml);
    void Reload(IBML *bml);
    void PrintFieldStatus(IBML *bml);
    void PrintVizStatus(IBML *bml);
    // FG_Custom diagnostics.  `hasPoint` false means "evaluate at the ball".
    void PrintCustomStatus(IBML *bml, bool hasPoint, const VxVector &point);
    void PrintExpressionProbe(IBML *bml, bool hasPoint, const VxVector &point,
                              const std::string &text);
    void PrintDomainProbe(IBML *bml, bool hasPoint, const VxVector &point, const std::string &text);

    // Total acceleration a zero-mass probe would feel at `worldPoint`: vanilla
    // gravity plus every active field, using the same activation rules as
    // UpdateFields.  Used by the debug overlay only; never by the physics path.
    VxVector SampleFieldAtPoint(const VxVector &worldPoint);

    // Potential of that same field at `worldPoint`.  Its negative gradient is
    // SampleFieldAtPoint, which is what the equipotential overlay relies on.
    float PotentialAtPoint(const VxVector &worldPoint);

    // `fg debug freemode`: on/off for the player's ball only.  Persists across
    // levels and respawns until the process restarts.
    // freemode: cancel every gravity source for the active ball only, by
    // applying the exact opposite of the engine's fixed gravity as a force.
    // Route C: no physics-RT ABI is touched; the engine gravity stays -20.
    void SetFreeMode(IBML *bml, bool enable);
    bool FreeModeEnabled() const { return m_FreeMode; }

    bool IsDebugMode() const { return m_DebugMode; }

    // Detailed logging needs both switches: DebugMode is the master switch for
    // everything diagnostic, EnableLog is the (opt-in) log writer.  Warnings,
    // errors and the one-line level summary never depend on either.
    bool DebugLogging() const { return m_DebugMode && m_EnableLog; }

    // The single gate every log write goes through: with EnableLog off nothing
    // is written at all, except while a console command is running (that is an
    // explicit request, and its output would otherwise be unreachable).
    bool LogEnabled() const { return m_DebugMode && (m_EnableLog || m_CommandScope); }

    // Set by the console command for the duration of its dispatch.
    void BeginCommandScope() { m_CommandScope = true; }
    void EndCommandScope() { m_CommandScope = false; }

private:
    CKContext *Context() const { return m_BML ? m_BML->GetCKContext() : nullptr; }

    void LogInfo(const char *fmt, ...);
    void LogWarn(const char *fmt, ...);
    // Writes even when DebugMode/EnableLog are off.  Used only for one-shot
    // "the feature cannot work at all" diagnostics.
    void LogAlways(const char *fmt, ...);
    void Warn(const std::string &message);
    void SendMessage(const std::string &message);

    CKGroup *FindGroup(const char *name);
    CKDataArray *FindArray(const char *name);
    CKObject *GetCellObject(CKDataArray *array, int row, int column);
    float GetCellFloat(CKDataArray *array, int row, int column, float fallback);
    bool GetCellInt(CKDataArray *array, int row, int column, int &value);
    // Dynamic-size string read: GetElementStringValue(row, col, nullptr) returns
    // the length including the terminating NUL, so no fixed buffer is needed.
    bool GetCellString(CKDataArray *array, int row, int column, std::string &out);
    int FindColumn(CKDataArray *array, const char *name);
    void LogArrayColumns(CKDataArray *array, const char *label);

    void ScanGroup(const char *name, GroupScan &result,
                   std::unordered_map<CK_ID, std::string> &members);
    void ScanUArray(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanSourceArray(SourceInfo::Kind kind, const char *arrayName, const char *strengthColumn,
                         ArrayScan &arrayScan, const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanRegionGroup(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanCFieldArray(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanCRegionArray(const std::unordered_map<CK_ID, std::string> &groupMembers);
    void ScanLocalArray(const std::unordered_map<CK_ID, int> &regionIndex,
                        const std::unordered_map<CK_ID, int> &cRegionIndex,
                        const std::unordered_map<CK_ID, int> &sourceIndex,
                        const std::unordered_map<CK_ID, int> &cFieldIndex,
                        const std::unordered_set<CK_ID> &uObjects);
    void ScanGlobalArray(const std::unordered_map<CK_ID, int> &sourceIndex,
                         const std::unordered_map<CK_ID, int> &cFieldIndex,
                         const std::unordered_set<CK_ID> &uObjects);
    void ValidateMembership(const std::unordered_map<CK_ID, std::string> &uGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &mGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &lGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &sGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &cFieldGroupMembers,
                            const std::unordered_map<CK_ID, std::string> &cRegionGroupMembers);

    CK3dEntity *FindActiveBall();
    CK3dEntity *ResolveEntity(CK_ID objectId);
    float GetBodyMass(CK3dEntity *entity, bool *found);
    // Flushes the counters that replaced per-object log lines, once per frame.
    void FlushLogCounters();

    void BuildRegionCache(CK3dEntity *entity, RegionVolume &region, const char *label);
    bool BuildLineGeometry(SourceInfo &source, CK3dEntity *entity);
    bool BuildSheetGeometry(SourceInfo &source, CK3dEntity *entity);
    bool ObjectTouchesRegion(const RegionVolume &region, CK_ID regionObjectId,
                             const BodySamples &objectSamples);
    bool ObjectTouchesRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                             const BodySamples &objectSamples);
    // Linear-scan variant used by the reports only, not by the per-frame path.
    VxVector ComputeSourceAcceleration(const SourceInfo &source, const VxVector &center);
    void UpdateFields();

    // Shared by UpdateFields and SampleFieldAtPoint: the acceleration that every
    // active field produces at one point.  A source contributes when it is
    // listed in FG_Global, or in an FG_Local row whose region contains the
    // point.  `rowTouched`, `sourceMask` and `cFieldMask` are caller-owned
    // scratch buffers, already sized to localRows / sources / cFields.
    VxVector EvaluateActiveFieldsAtPoint(const VxVector &point, std::vector<char> &rowTouched,
                                         std::vector<char> &sourceMask,
                                         std::vector<char> &cFieldMask);
    // Same rule for the scalar potential, for the equipotential overlay.
    // CFields are deliberately excluded: an arbitrary expression field has no
    // potential in general.
    float EvaluateSourcePotentialsAtPoint(const VxVector &point, std::vector<char> &rowTouched,
                                          std::vector<char> &sourceMask,
                                          std::vector<char> &cFieldMask);
    bool PointInsideRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                           const VxVector &worldPoint);

    // Debug overlay (viz/Overlay.cpp).  BML 0.3.13 ends the ImGui frame before
    // OnRender, so the overlay is drawn from OnProcess, inside the frame.
    void DrawOverlayFrame();
    void DrawOverlay();
    void DrawGravityArrow(ImDrawList *draw, CKRenderContext *renderContext);
    float BallRadiusFor(CK3dEntity *ball);
    bool ProjectToScreen(CKRenderContext *renderContext, const VxVector &localPoint,
                         CK3dEntity *reference, VxVector &screenPoint);
    VxVector CameraPosition(CKRenderContext *renderContext) const;
    // World matrix of the viewpoint, used as the "did the view change" key for
    // the overlay caches and for nothing else.
    VxMatrix CameraWorldMatrix(CKRenderContext *renderContext) const;
    // Projects a whole array in one engine call (CKRenderContext::TransformVertices)
    // and keeps the per-vertex frustum flags.  The overlays draw tens of
    // thousands of points per frame; one call per array instead of one per point
    // is most of the difference between 60 fps and 25 fps.  `localPoints` are in
    // `reference`'s space, exactly like ProjectToScreen.
    void ProjectBatch(CKRenderContext *renderContext, CK3dEntity *reference,
                      const VxVector *localPoints, size_t count, std::vector<VxVector4> &screen,
                      std::vector<unsigned int> &clip);

    // Field line overlay (viz/FieldLines.cpp).
    //
    // The field itself never moves, so a traced line stays valid for the whole
    // level: lines are cached in world space and only added or dropped as the
    // ball travels.  That is what keeps the per-frame cost independent of the
    // number of lines after the first fill.
    struct TracedFieldLine {
        VxVector seed = VxVector(0.0f, 0.0f, 0.0f);
        std::vector<VxVector> points;
        bool empty = false;
    };
    static VxVector EvaluateFieldThunk(void *context, const VxVector &point);
    void UpdateFieldLines();
    void ReanchorFieldLines(const VxVector &origin, const VxVector &normal, const VxVector &camera,
                            float spacing);
    void TracePendingFieldLines();
    void PruneFieldLines(const VxVector &camera);
    void DropFieldLines();
    void DrawFieldLines(ImDrawList *draw, CKRenderContext *renderContext, CK3dEntity *ball);
    // Flattens the cached polylines into one array (nearest lines first), drops
    // the points that cannot be on screen and projects the whole batch once.
    void RebuildFieldLineDrawCache(CKRenderContext *renderContext, CK3dEntity *ball,
                                   const VxVector &camera);

    // A contiguous run of visible points inside the flattened array.  A line
    // that leaves the view sphere (or the forward half space) and comes back is
    // two runs, so the polyline is never drawn across the invisible part.
    struct LineRun {
        int begin = 0;
        int count = 0;
    };

    // Equipotential overlay (viz/Equipotentials.cpp).
    //
    // Surfaces of constant potential are extracted from a regular lattice that
    // spans the view sphere.  The lattice is snapped to its own grid, so rolling
    // inside one cell reuses every sample; crossing a cell boundary only samples
    // the slab that is actually new.  Only the levels within a few steps of the
    // ball's potential are extracted and drawn.
    struct EquipotentialCache {
        VxVector origin = VxVector(0.0f, 0.0f, 0.0f);
        float cellSize = 0.0f;
        int cells = 0;
        float levelStep = 0.0f;
        std::vector<float> values;
        // 1 for a sample that has been evaluated: a shifted lattice keeps the
        // samples it can, so the fill is not all-or-nothing any more.
        std::vector<char> validSample;
        size_t filled = 0;
        size_t cursor = 0;
        bool extracted = false;
        int bandFirst = 0;
        int bandLast = -1;
        // World-space triangles, three vertices each, sorted far to near by
        // `order` so the translucent surfaces blend in the right order.
        std::vector<VxVector> triangles;
        std::vector<float> depths;
        std::vector<unsigned int> order;
        int levels = 0;
        float lastBuildMs = 0.0f;
    };
    void UpdateEquipotentials(CK3dEntity *ball);
    void ExtractEquipotentials();
    void DropEquipotentials();
    void DrawEquipotentials(ImDrawList *draw, CKRenderContext *renderContext, CK3dEntity *ball);
    // Re-selects (nearest first), flattens and batch-projects the triangles that
    // will be drawn this frame.
    void RebuildEquipotentialDrawCache(CKRenderContext *renderContext, CK3dEntity *ball);
    // Recomputes the far-to-near order for the current camera.
    void SortEquipotentialTriangles(const VxVector &camera);

    // Field evaluation is capped at the fixed physics rate: evaluating more
    // often than the simulation steps cannot change any physics result.
    bool ShouldUpdateFields();
    void RefreshFieldTransforms();
    void BuildSourceIndex();
    void ApplyChannelForce(CK3dEntity *entity, const BodyInfo &body, const VxVector &acceleration);
    void ClearChannelForce(ForceChannel &channel);
    void ClearAllFieldForces();
    void DropAllFieldForces();
    void DestroyForceChannels();
    void RebuildForceChannels();
    CKBehavior *GetOrCreateForceBehavior(ForceChannel &channel);

    ScanInfo m_Scan;
    bool m_DebugMode = false;
    bool m_EnableLog = false;

    // One warn-once flag per FG_CField row (component NaN).  The hot path never
    // grows the log without this.
    std::vector<char> m_CustomWarned;

    // freemode state.  It survives level changes and respawns for the whole
    // process; only a new game start resets it.
    bool m_FreeMode = false;

    std::unordered_map<CK_ID, BodyInfo> m_Bodies;

    // Collision sphere radius reported by the level script for every object
    // that was physicalized with a ball collider.  The overlay uses it to stop
    // the gravity arrow at the ball surface instead of drawing it through the
    // ball; it is also the exact number the game uses, so no guessing.
    std::unordered_map<CK_ID, float> m_BallRadii;

    // Counters that replaced per-object log lines.  They are flushed once per
    // frame, so a 150-body level produces one summary line instead of 150.
    int m_PendingPhysicalizeCount = 0;
    int m_PendingFixedCount = 0;
    int m_PendingChannelsCreated = 0;
    int m_PendingChannelsRebuilt = 0;
    bool m_CommandScope = false;

    std::unordered_map<CK_ID, ForceChannel> m_ForceChannels;
    int m_LastAffectedCount = -1;
    bool m_ForceBackendFailed = false;

    // Per-evaluation caches: refreshed once per UpdateFields call, never
    // per body, and never during script execution.
    std::vector<VxMatrix> m_UFieldInverse;
    std::vector<char> m_UFieldValid;
    std::vector<VxMatrix> m_RegionInverse;
    std::vector<char> m_RegionValid;
    std::vector<VxMatrix> m_SourceWorld;
    std::vector<char> m_SourceValid;
    // World-space source geometry, so the polyline and the patch vertices are
    // transformed once per evaluation instead of once per body.
    std::vector<std::vector<VxVector>> m_SourceLines;
    std::vector<SheetWorldCache> m_SourceSheets;
    std::unordered_map<CK_ID, size_t> m_SourceIndex;
    std::unordered_map<CK_ID, size_t> m_CFieldIndex;
    std::vector<char> m_ActiveSources;
    std::vector<char> m_ActiveCFields;

    // Debug overlay.  Every switch is off by default; with everything off the
    // overlay costs one comparison per frame.
    bool m_EnableFieldLines = false;
    bool m_EnableEquipotentials = false;
    bool m_ShowGVector = false;
    float m_FieldLineDensity = 10.0f;
    float m_EquipotentialDensity = 10.0f;
    float m_DebugViewDistance = 100.0f;

    // Scratch reused by SampleFieldAtPoint so sampling a point allocates nothing.
    std::vector<char> m_VizRows;
    std::vector<char> m_VizSources;
    std::vector<char> m_VizCFields;

    // Overlay diagnostics, filled by the last rendered frame and printed on
    // demand by `fg debug viz` (nothing is logged unless asked for).
    int m_OverlayFrames = 0;
    int m_OverlayDrawn = 0;
    int m_OverlayNoBall = 0;
    int m_OverlayNoProjection = 0;
    int m_OverlayHidden = 0;
    // Gate counters for the "switch is on but nothing appears" case: the
    // overlay can be skipped before DrawOverlay for several distinct reasons.
    int m_OverlayPaused = 0;
    int m_OverlayNoContext = 0;
    int m_OverlayGateFrames = 0;
    bool m_OverlayGateWarned = false;
    bool m_OverlayDrawWarned = false;
    int m_OverlayProcessFrames = 0;
    int m_OverlaySpanCount = 0;
    float m_OverlayVisibleMetres = 0.0f;
    bool m_OverlayHadFrame = false;
    VxVector m_OverlayBall = VxVector(0.0f, 0.0f, 0.0f);
    VxVector m_OverlayGravity = VxVector(0.0f, 0.0f, 0.0f);
    VxVector m_OverlayBase = VxVector(0.0f, 0.0f, 0.0f);
    VxVector m_OverlayTip = VxVector(0.0f, 0.0f, 0.0f);
    float m_OverlayBallRadius = 0.0f;
    float m_OverlayPixelsPerMetre = 0.0f;
    float m_OverlayDisplayWidth = 0.0f;
    float m_OverlayDisplayHeight = 0.0f;
    int m_OverlayContextWidth = 0;
    int m_OverlayContextHeight = 0;

    // Field line cache.  Lines live in world space and survive frame to frame;
    // `m_PendingFieldLines` points at the entries that still have to be traced
    // (the unique_ptr keeps those pointers stable while lines are dropped).
    std::vector<std::unique_ptr<TracedFieldLine>> m_FieldLines;
    std::vector<TracedFieldLine *> m_PendingFieldLines;
    // Draw order, nearest first; rebuilt every frame and reused to keep the
    // rendering path allocation free.
    std::vector<std::pair<float, size_t>> m_FieldLineOrder;
    size_t m_PendingCursor = 0;
    VxVector m_FieldLineOrigin = VxVector(0.0f, 0.0f, 0.0f);
    VxVector m_FieldLineNormal = VxVector(0.0f, 1.0f, 0.0f);
    VxVector m_FieldLineAxisU = VxVector(1.0f, 0.0f, 0.0f);
    VxVector m_FieldLineAxisV = VxVector(0.0f, 0.0f, 1.0f);
    VxVector m_FieldLinePruneCamera = VxVector(0.0f, 0.0f, 0.0f);
    bool m_FieldLinePruneValid = false;
    float m_FieldLineSpacing = 0.0f;
    bool m_FieldLinesAnchored = false;
    int m_FieldLineTraced = 0;
    int m_FieldLineDropped = 0;
    int m_FieldLinePending = 0;
    int m_FieldLineDrawn = 0;
    int m_FieldLineVertices = 0;
    int m_FieldLinePoints = 0;
    float m_FieldLineLastTraceMs = 0.0f;

    // Draw cache of the field lines: one flattened, batch-projected array plus
    // the runs that are actually visible.  It is rebuilt only when the traced
    // geometry or the view changed, so a still camera costs one draw-list pass
    // and no projection at all.
    std::vector<VxVector> m_LineLocal;
    std::vector<VxVector4> m_LineScreen;
    std::vector<unsigned int> m_LineClip;
    std::vector<LineRun> m_LineRuns;
    VxMatrix m_LineCacheView{};
    VxVector m_LineCacheBall = VxVector(0.0f, 0.0f, 0.0f);
    float m_LineCacheDisplayWidth = 0.0f;
    float m_LineCacheDisplayHeight = 0.0f;
    int m_LineCacheContextWidth = 0;
    int m_LineCacheContextHeight = 0;
    bool m_LineCacheValid = false;
    int m_FieldLineVersion = 0;
    int m_LineCacheVersion = -1;
    int m_FieldLineProjected = 0;

    // Equipotential cache: one lattice spanning the view sphere, refilled only
    // when the camera crosses a cell boundary.
    EquipotentialCache m_Equipotentials;
    int m_EquipotentialSamples = 0;
    int m_EquipotentialTriangles = 0;
    int m_EquipotentialLevels = 0;

    // Draw cache of the equipotentials: the selected triangles, batch-projected.
    std::vector<unsigned int> m_EqDraw;
    std::vector<VxVector> m_EqLocal;
    std::vector<VxVector4> m_EqScreen;
    std::vector<unsigned int> m_EqClip;
    VxMatrix m_EqCacheView{};
    VxVector m_EqCacheBall = VxVector(0.0f, 0.0f, 0.0f);
    float m_EqCacheDisplayWidth = 0.0f;
    float m_EqCacheDisplayHeight = 0.0f;
    int m_EqCacheContextWidth = 0;
    int m_EqCacheContextHeight = 0;
    bool m_EqCacheValid = false;
    int m_EqVersion = 0;
    int m_EqCacheVersion = -1;
    int m_EqProjected = 0;
    VxVector m_EqSortCamera = VxVector(0.0f, 0.0f, 0.0f);
    bool m_EqSortValid = false;

    // Real vertex count of the last overlay frame, for `fg debug viz`: this is
    // the number that has to stay under the 16-bit index limit.  The projection
    // delta is the periodic batch-vs-single-point self-check.
    int m_OverlayVertices = 0;
    float m_OverlayProjectionDelta = 0.0f;
    bool m_OverlayVtxOffsetSupported = false;

    float m_LastFieldUpdate = -1.0f;

    // While the engine is tearing the level down (reset / exit / reload) the
    // mod must not touch physics at all: executing a SetPhysicsForce behavior
    // or unsetting a force mid-teardown hangs the game's main thread.
    bool m_TearingDown = false;
    CKBehavior *m_OwnerScript = nullptr;
    bool m_PendingChannelRebuild = false;
};

