#include "mod/FlexGravityMod.h"

namespace {

const char *RelOpText(core::expr::RelOp op) {
    switch (op) {
    case core::expr::RelOp::Equal:
        return "=";
    case core::expr::RelOp::NotEqual:
        return "!=";
    case core::expr::RelOp::Less:
        return "<";
    case core::expr::RelOp::LessEqual:
        return "<=";
    case core::expr::RelOp::Greater:
        return ">";
    case core::expr::RelOp::GreaterEqual:
        return ">=";
    }
    return "?";
}

bool RefsContain(const std::vector<SourceRef> &refs, SourceRef::Kind kind, int index) {
    for (const SourceRef &ref : refs) {
        if (ref.kind == kind && ref.index == index) {
            return true;
        }
    }
    return false;
}

const char *RegionKindText(const LocalRow &row) {
    return row.regionKind == LocalRow::BuiltinRegion ? "region" : "cregion";
}

} // namespace

void FlexGravityMod::PrintFieldStatus(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'fg debug reload'.");
        return;
    }

    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        SendMessage("No active ball found.");
        return;
    }

    const BodySamples ballSamples = BuildBodySamples(ball->GetWorldMatrix());
    const VxVector center = ballSamples.Center();

    LogInfo("Field status for %s center=%s", SafeName(ball).c_str(), FormatVec(center).c_str());

    VxVector total(0.0f, 0.0f, 0.0f);
    for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
        const bool touches = ObjectTouchesRegion(m_Scan.uFields[index].region,
                                                 m_Scan.uFields[index].objectId, ballSamples);
        if (touches) {
            total = Add(total, m_Scan.uFields[index].gravity);
        }
        LogInfo("  U[%s] touch=%s g=(%.4f, %.4f, %.4f)", m_Scan.uFields[index].objectName.c_str(),
                BoolText(touches).c_str(), m_Scan.uFields[index].gravity.x,
                m_Scan.uFields[index].gravity.y, m_Scan.uFields[index].gravity.z);
    }

    std::vector<char> rowTouched(m_Scan.localRows.size(), 0);
    std::vector<char> sourceMask(m_Scan.sources.size(), 0);
    std::vector<char> cFieldMask(m_Scan.cFields.size(), 0);
    const VxVector active = EvaluateActiveFieldsAtPoint(center, rowTouched, sourceMask, cFieldMask);
    total = Add(total, active);

    for (size_t index = 0; index < m_Scan.localRows.size(); ++index) {
        const LocalRow &row = m_Scan.localRows[index];
        std::string regionName = "<unknown>";
        if (row.regionKind == LocalRow::BuiltinRegion && row.regionIndex >= 0 &&
            row.regionIndex < static_cast<int>(m_Scan.regions.size())) {
            regionName = m_Scan.regions[static_cast<size_t>(row.regionIndex)].objectName;
        } else if (row.cRegionIndex >= 0 &&
                   row.cRegionIndex < static_cast<int>(m_Scan.cRegions.size())) {
            regionName = m_Scan.cRegions[static_cast<size_t>(row.cRegionIndex)].displayName;
        }
        LogInfo("  L[%s] kind=%s ballInside=%s sources=%zu", regionName.c_str(),
                RegionKindText(row), BoolText(rowTouched[index] != 0).c_str(),
                row.sources.size());
    }

    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        const SourceInfo &source = m_Scan.sources[index];
        const bool activeSource = index < sourceMask.size() && sourceMask[index] != 0;
        const bool global = RefsContain(m_Scan.globalSources, SourceRef::Builtin,
                                        static_cast<int>(index));
        bool local = false;
        for (size_t row = 0; row < rowTouched.size(); ++row) {
            if (rowTouched[row] && RefsContain(m_Scan.localRows[row].sources, SourceRef::Builtin,
                                               static_cast<int>(index))) {
                local = true;
                break;
            }
        }
        const char *scope = global && local ? "local+global" : (global ? "global" : (local ? "local" : "-"));

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
                detail = " d=" + std::to_string(core::DistanceToPolyline(points, source.closed, center));
            }
            contribution = ComputeSourceAcceleration(source, center);
        } else {
            CK3dEntity *entity = ResolveEntity(source.objectId);
            if (entity) {
                const VxMatrix &world = entity->GetWorldMatrix();
                if (source.SheetPatchCount() > 0) {
                    float nearest = 1.0e30f;
                    for (int patch = 0; patch < source.SheetPatchCount(); ++patch) {
                        const int begin = source.sheetOffsets[patch];
                        const VxVector &p0 = world * source.sheetVertices[begin];
                        const VxVector &p1 = world * source.sheetVertices[begin + 1];
                        const VxVector &p2 = world * source.sheetVertices[begin + 2];
                        VxVector normal(0.0f, 0.0f, 0.0f);
                        Normalize(Cross(Subtract(p1, p0), Subtract(p2, p0)), normal);
                        nearest = (std::min)(nearest, std::fabs(Dot(Subtract(center, p0), normal)));
                    }
                    detail = " patches=" + std::to_string(source.SheetPatchCount()) +
                             " nearestPlane=" + std::to_string(nearest);
                } else {
                    const VxVector origin = world * source.localCenter;
                    VxVector axisA = Subtract(world * Add(source.localCenter, source.localAxisA), origin);
                    VxVector axisB = Subtract(world * Add(source.localCenter, source.localAxisB), origin);
                    Normalize(axisA, axisA);
                    Normalize(axisB, axisB);
                    VxVector normal(0.0f, 0.0f, 0.0f);
                    Normalize(Cross(axisA, axisB), normal);
                    detail = " z=" + std::to_string(std::fabs(Dot(Subtract(center, origin), normal)));
                }
            }
            contribution = ComputeSourceAcceleration(source, center);
        }
        LogInfo("  %s[%s] strength=%.4f active=%s scope=%s%s g=(%.4f, %.4f, %.4f)",
                SourceKindName(source.kind), source.objectName.c_str(), source.strength,
                BoolText(activeSource).c_str(), scope, detail.c_str(), contribution.x,
                contribution.y, contribution.z);
    }

    if (!m_Scan.cFields.empty()) {
        const PointFrame frame = core::expr::MakePointFrame(center);
        for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
            const CFieldRow &field = m_Scan.cFields[index];
            const bool activeField = index < cFieldMask.size() && cFieldMask[index] != 0;
            const bool global =
                    RefsContain(m_Scan.globalSources, SourceRef::Custom, static_cast<int>(index));
            bool local = false;
            for (size_t row = 0; row < rowTouched.size(); ++row) {
                if (rowTouched[row] &&
                    RefsContain(m_Scan.localRows[row].sources, SourceRef::Custom,
                                static_cast<int>(index))) {
                    local = true;
                    break;
                }
            }
            const char *scope =
                    global && local ? "local+global" : (global ? "global" : (local ? "local" : "-"));
            VxVector contribution(0.0f, 0.0f, 0.0f);
            if (activeField && field.compiled.valid) {
                bool nonFinite = false;
                contribution = core::expr::EvaluateCField(field.compiled, frame, &nonFinite);
            }
            LogInfo("  C[%s] mode=%d active=%s scope=%s g=(%.4f, %.4f, %.4f)",
                    field.displayName.c_str(), field.compiled.mode, BoolText(activeField).c_str(),
                    scope, contribution.x, contribution.y, contribution.z);
        }
    }

    const std::string message =
            "U=" + std::to_string(m_Scan.uFields.size()) +
            " M=" + std::to_string(static_cast<size_t>(std::count_if(
                              m_Scan.sources.begin(), m_Scan.sources.end(),
                              [](const SourceInfo &s) { return s.kind == SourceInfo::KindMonopole; }))) +
            " L=" + std::to_string(static_cast<size_t>(std::count_if(
                              m_Scan.sources.begin(), m_Scan.sources.end(),
                              [](const SourceInfo &s) { return s.kind == SourceInfo::KindLine; }))) +
            " S=" + std::to_string(static_cast<size_t>(std::count_if(
                              m_Scan.sources.begin(), m_Scan.sources.end(),
                              [](const SourceInfo &s) { return s.kind == SourceInfo::KindSheet; }))) +
            " regions=" + std::to_string(m_Scan.regions.size()) +
            " localRows=" + std::to_string(m_Scan.localRows.size()) +
            " global=" + std::to_string(m_Scan.globalSources.size()) +
            " cfields=" + std::to_string(m_Scan.cFields.size()) +
            " cregions=" + std::to_string(m_Scan.cRegions.size()) +
            " total=" + FormatVec(total) +
            " channels=" + std::to_string(m_ForceChannels.size()) +
            " affected=" + std::to_string(m_LastAffectedCount) +
            " freemode=" + std::string(m_FreeMode ? "on" : "off");
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

void FlexGravityMod::PrintStatus(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'fg debug reload'.");
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

    const size_t validCField = static_cast<size_t>(std::count_if(
            m_Scan.cFields.begin(), m_Scan.cFields.end(),
            [](const CFieldRow &field) { return field.compiled.valid; }));
    const size_t rejectedCField = m_Scan.cFields.size() - validCField;
    const size_t validCRegion = static_cast<size_t>(std::count_if(
            m_Scan.cRegions.begin(), m_Scan.cRegions.end(),
            [](const CRegionRow &region) { return region.valid; }));

    const std::string summary =
            "status: UField(group=" + BoolText(m_Scan.uGroup.present) +
            ", fields=" + std::to_string(m_Scan.uFields.size()) +
            ") MField(group=" + BoolText(m_Scan.mGroup.present) +
            ", sources=" + std::to_string(mCount) +
            ") LField(group=" + BoolText(m_Scan.lGroup.present) +
            ", sources=" + std::to_string(lCount) +
            ") SField(group=" + BoolText(m_Scan.sGroup.present) +
            ", sources=" + std::to_string(sCount) +
            ") Region(group=" + BoolText(m_Scan.regionGroup.present) +
            ", members=" + std::to_string(m_Scan.regions.size()) +
            ") CField(group=" + BoolText(m_Scan.cFieldGroup.present) +
            ", rows=" + std::to_string(m_Scan.cFields.size()) +
            ", valid=" + std::to_string(validCField) +
            ") CRegion(group=" + BoolText(m_Scan.cRegionGroup.present) +
            ", rows=" + std::to_string(m_Scan.cRegions.size()) +
            ", valid=" + std::to_string(validCRegion) +
            ") Local(rows=" + std::to_string(m_Scan.localRows.size()) +
            ") Global(sources=" + std::to_string(m_Scan.globalSources.size()) +
            ") ignored=" + std::to_string(m_Scan.ignoredEntries) +
            " warnings=" + std::to_string(m_Scan.warnings.size()) +
            " cfieldRejected=" + std::to_string(rejectedCField) +
            " freemode=" + std::string(m_FreeMode ? "on" : "off");

    SendMessage(summary);
    LogInfo("%s", summary.c_str());

    for (const std::string &warning : m_Scan.warnings) {
        LogWarn("scan warning: %s", warning.c_str());
    }
}

void FlexGravityMod::PrintList(IBML *bml) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'fg debug reload'.");
        return;
    }

    LogInfo("--- FlexGravity parsed data (v2.1) ---");
    LogInfo("U fields: %zu", m_Scan.uFields.size());
    for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
        const UFieldInfo &field = m_Scan.uFields[index];
        LogInfo("  U[%zu] %s (id=%lu) g=(%.4f, %.4f, %.4f)", index, field.objectName.c_str(),
                static_cast<unsigned long>(field.objectId), field.gravity.x, field.gravity.y,
                field.gravity.z);
    }

    LogInfo("Sources: %zu", m_Scan.sources.size());
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        const SourceInfo &source = m_Scan.sources[index];
        if (source.kind == SourceInfo::KindLine) {
            LogInfo("  L[%zu] %s (id=%lu) Lambda=%.4f curve=%d points=%d closed=%s", index,
                    source.objectName.c_str(), static_cast<unsigned long>(source.objectId),
                    source.strength, source.usedCurve ? 1 : 0, source.controlPointCount,
                    BoolText(source.closed).c_str());
        } else if (source.kind == SourceInfo::KindSheet) {
            LogInfo("  S[%zu] %s (id=%lu) Sigma=%.4f patches=%d triangles=%d merged=%d", index,
                    source.objectName.c_str(), static_cast<unsigned long>(source.objectId),
                    source.strength, source.SheetPatchCount(), source.sheetTriangleCount,
                    source.sheetMergedCount);
        } else {
            LogInfo("  M[%zu] %s (id=%lu) G=%.4f", index, source.objectName.c_str(),
                    static_cast<unsigned long>(source.objectId), source.strength);
        }
    }

    LogInfo("Regions (FG_Region members): %zu", m_Scan.regions.size());
    for (size_t index = 0; index < m_Scan.regions.size(); ++index) {
        LogInfo("  R[%zu] %s (id=%lu) mesh=%s triangles=%zu", index,
                m_Scan.regions[index].objectName.c_str(),
                static_cast<unsigned long>(m_Scan.regions[index].objectId),
                BoolText(m_Scan.regions[index].volume.meshValid).c_str(),
                m_Scan.regions[index].volume.localIndices.size() / 3);
    }

    LogInfo("CField rows: %zu", m_Scan.cFields.size());
    static const char *const kCustomColumns[2][3] = {{"X", "Y", "Z"}, {"R", "Theta", "Phi"}};
    for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
        const CFieldRow &field = m_Scan.cFields[index];
        const int group = field.compiled.mode == 1 ? 1 : 0;
        LogInfo("  C[%zu] \"%s\" mode=%d valid=%s", index, field.displayName.c_str(),
                field.compiled.mode, BoolText(field.compiled.valid).c_str());
        for (int component = 0; component < 3; ++component) {
            const int column = group * 3 + component;
            LogInfo("      %s(len=%zu)=\"%s\"", kCustomColumns[group][component],
                    field.compiled.text[column].size(), field.compiled.text[column].c_str());
        }
        if (!field.compiled.error.empty()) {
            LogInfo("      problem: %s", field.compiled.error.c_str());
        }
    }

    LogInfo("CRegion rows: %zu", m_Scan.cRegions.size());
    for (size_t index = 0; index < m_Scan.cRegions.size(); ++index) {
        const CRegionRow &region = m_Scan.cRegions[index];
        LogInfo("  CR[%zu] \"%s\" valid=%s domain=\"%s\"%s", index, region.displayName.c_str(),
                BoolText(region.valid).c_str(), region.domainText.c_str(),
                region.valid ? "" : (" [" + region.error + "]").c_str());
    }

    LogInfo("Local rows: %zu", m_Scan.localRows.size());
    for (size_t index = 0; index < m_Scan.localRows.size(); ++index) {
        const LocalRow &row = m_Scan.localRows[index];
        std::string regionName = "<unknown>";
        if (row.regionKind == LocalRow::BuiltinRegion && row.regionIndex >= 0 &&
            row.regionIndex < static_cast<int>(m_Scan.regions.size())) {
            regionName = m_Scan.regions[static_cast<size_t>(row.regionIndex)].objectName;
        } else if (row.cRegionIndex >= 0 &&
                   row.cRegionIndex < static_cast<int>(m_Scan.cRegions.size())) {
            regionName = m_Scan.cRegions[static_cast<size_t>(row.cRegionIndex)].displayName;
        }
        std::string names;
        for (const SourceRef &ref : row.sources) {
            if (!names.empty()) {
                names += ", ";
            }
            if (ref.kind == SourceRef::Builtin) {
                names += m_Scan.sources[static_cast<size_t>(ref.index)].objectName;
            } else {
                names += "[C] ";
                names += m_Scan.cFields[static_cast<size_t>(ref.index)].displayName;
            }
        }
        LogInfo("  L[%zu] %s (%s) -> [%s]", index, regionName.c_str(), RegionKindText(row),
                names.c_str());
    }

    LogInfo("Global sources (FG_Global): %zu", m_Scan.globalSources.size());
    for (size_t index = 0; index < m_Scan.globalSources.size(); ++index) {
        const SourceRef &ref = m_Scan.globalSources[index];
        if (ref.kind == SourceRef::Builtin) {
            LogInfo("  G[%zu] %s", index, m_Scan.sources[static_cast<size_t>(ref.index)].objectName.c_str());
        } else {
            LogInfo("  G[%zu] [C] %s", index,
                    m_Scan.cFields[static_cast<size_t>(ref.index)].displayName.c_str());
        }
    }

    SendMessage("Parsed data written to ModLoader.log.");
}

void FlexGravityMod::Reload(IBML *bml) {
    (void) bml;
    Scan();
    BuildSourceIndex();
    SendMessage("Scan reloaded; see ModLoader.log.");
}

// CField / CRegion view at one point.  This is the "why is this row not doing
// anything here" report: it shows the Local/Global scope and the contribution.
void FlexGravityMod::PrintCustomStatus(IBML *bml, bool hasPoint, const VxVector &point) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'fg debug reload'.");
        return;
    }
    if (m_Scan.cFields.empty() && m_Scan.cRegions.empty()) {
        SendMessage("No FG_CField / FG_CRegion rows in this level.");
        return;
    }

    VxVector where = point;
    std::string label = "at the given point";
    if (!hasPoint) {
        CK3dEntity *ball = FindActiveBall();
        if (!ball) {
            SendMessage("No active ball found; use 'fg debug custom at <x> <y> <z>'.");
            return;
        }
        where = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
        label = "at the ball";
    }

    const PointFrame frame = core::expr::MakePointFrame(where);
    LogInfo("FG_CField/FG_CRegion %s: p=%s r=%.4f theta=%.4f phi=%.4f", label.c_str(),
            FormatVec(where).c_str(), frame.variables[core::expr::kVarR],
            frame.variables[core::expr::kVarTheta], frame.variables[core::expr::kVarPhi]);

    std::vector<char> rowTouched(m_Scan.localRows.size(), 0);
    std::vector<char> sourceMask(m_Scan.sources.size(), 0);
    std::vector<char> cFieldMask(m_Scan.cFields.size(), 0);
    const VxVector total = EvaluateActiveFieldsAtPoint(where, rowTouched, sourceMask, cFieldMask);

    int activeFields = 0;
    for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
        const CFieldRow &field = m_Scan.cFields[index];
        const bool active = index < cFieldMask.size() && cFieldMask[index] != 0;
        if (active) {
            ++activeFields;
        }
        const bool global =
                RefsContain(m_Scan.globalSources, SourceRef::Custom, static_cast<int>(index));
        bool local = false;
        for (size_t row = 0; row < rowTouched.size(); ++row) {
            if (rowTouched[row] &&
                RefsContain(m_Scan.localRows[row].sources, SourceRef::Custom, static_cast<int>(index))) {
                local = true;
                break;
            }
        }
        VxVector contribution(0.0f, 0.0f, 0.0f);
        if (active && field.compiled.valid) {
            bool nonFinite = false;
            contribution = core::expr::EvaluateCField(field.compiled, frame, &nonFinite);
        }
        LogInfo("  C[%zu] \"%s\" mode=%d scope=%s active=%s g=(%.4f, %.4f, %.4f)", index,
                field.displayName.c_str(), field.compiled.mode,
                global && local ? "local+global" : (global ? "global" : (local ? "local" : "-")),
                BoolText(active).c_str(), contribution.x, contribution.y, contribution.z);
    }

    int activeRegions = 0;
    for (size_t index = 0; index < m_Scan.cRegions.size(); ++index) {
        const CRegionRow &region = m_Scan.cRegions[index];
        bool inside = false;
        bool nonFinite = false;
        if (region.valid) {
            inside = core::expr::EvaluateDomain(region.domain, frame.variables, &nonFinite);
        }
        if (inside) {
            ++activeRegions;
        }
        LogInfo("  CR[%zu] \"%s\" valid=%s inside=%s%s", index, region.displayName.c_str(),
                BoolText(region.valid).c_str(), BoolText(inside).c_str(),
                nonFinite ? " (NaN/Inf)" : "");
    }

    const std::string message = "custom: cfields=" + std::to_string(m_Scan.cFields.size()) +
                                " active=" + std::to_string(activeFields) +
                                " cregions=" + std::to_string(m_Scan.cRegions.size()) +
                                " inside=" + std::to_string(activeRegions) +
                                " total=" + FormatVec(total) + " " + label;
    SendMessage(message);
    LogInfo("%s", message.c_str());
}

// Compile-and-evaluate one expression at a point.  This is how a formula can be
// checked in game without exporting a map: the parser is the same one the
// scanner uses, so "the editor accepted it" and "the mod accepts it" can be
// compared directly.
void FlexGravityMod::PrintExpressionProbe(IBML *bml, bool hasPoint, const VxVector &point,
                                          const std::string &text) {
    (void) bml;
    VxVector where = point;
    if (!hasPoint) {
        CK3dEntity *ball = FindActiveBall();
        if (!ball) {
            SendMessage("No active ball found; use 'fg debug expr at <x> <y> <z> <expression>'.");
            return;
        }
        where = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    }
    if (text.empty()) {
        SendMessage("Usage: fg debug expr [at x y z] <expression>");
        return;
    }

    core::expr::Program program;
    core::expr::CompileError error;
    if (!core::expr::CompileExpression(text, core::expr::kMaskAll, program, error)) {
        const std::string message = "expr: " + error.message + " (byte " +
                                    std::to_string(error.position) + ")";
        SendMessage(message);
        LogInfo("expr \"%s\": %s", text.c_str(), message.c_str());
        return;
    }

    const PointFrame frame = core::expr::MakePointFrame(where);
    const double value = core::expr::EvaluateProgram(program, frame.variables);
    char formatted[64] = {};
    std::snprintf(formatted, sizeof(formatted), "%.6f", value);

    const std::string message = std::string("expr = ") + formatted + "  (x=" +
                                std::to_string(where.x) + " y=" + std::to_string(where.y) +
                                " z=" + std::to_string(where.z) + ")";
    SendMessage(message);
    LogInfo("expr \"%s\" at %s: value=%s r=%.6f theta=%.6f phi=%.6f", text.c_str(),
            FormatVec(where).c_str(), formatted, frame.variables[core::expr::kVarR],
            frame.variables[core::expr::kVarTheta], frame.variables[core::expr::kVarPhi]);
}

// Domain probe: every relation is printed with both sides, so a tolerance
// question ("why is this row not active here") can be answered from the log.
void FlexGravityMod::PrintDomainProbe(IBML *bml, bool hasPoint, const VxVector &point,
                                      const std::string &text) {
    (void) bml;
    VxVector where = point;
    if (!hasPoint) {
        CK3dEntity *ball = FindActiveBall();
        if (!ball) {
            SendMessage("No active ball found; use 'fg debug domain at <x> <y> <z> <expression>'.");
            return;
        }
        where = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    }
    if (text.empty()) {
        SendMessage("Usage: fg debug domain [at x y z] <domain expression>");
        return;
    }

    core::expr::Domain domain;
    core::expr::CompileError error;
    if (!core::expr::CompileDomain(text, domain, error)) {
        const std::string message = "domain: " + error.message + " (byte " +
                                    std::to_string(error.position) + ")";
        SendMessage(message);
        LogInfo("domain \"%s\": %s", text.c_str(), message.c_str());
        return;
    }

    const PointFrame frame = core::expr::MakePointFrame(where);
    for (size_t index = 0; index < domain.relations.size(); ++index) {
        const core::expr::Relation &relation = domain.relations[index];
        const double left = core::expr::EvaluateProgram(relation.left, frame.variables);
        const double right = core::expr::EvaluateProgram(relation.right, frame.variables);
        LogInfo("  relation[%zu] %.6f %s %.6f", index, left, RelOpText(relation.op), right);
    }

    bool nonFinite = false;
    const bool holds = core::expr::EvaluateDomain(domain, frame.variables, &nonFinite);
    const std::string message = std::string("domain = ") + (holds ? "true" : "false") + "  (" +
                                std::to_string(domain.relations.size()) + " relation(s)" +
                                (nonFinite ? ", NaN/Inf seen" : "") + ") at " + FormatVec(where);
    SendMessage(message);
    LogInfo("%s", message.c_str());
}
