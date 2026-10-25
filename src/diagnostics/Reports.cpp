#include "mod/FlexGravityMod.h"

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
                detail = " d=" + std::to_string(core::DistanceToPolyline(points, source.closed, center));
            }
            contribution = ComputeSourceAcceleration(source, center);
        } else {
            CK3dEntity *entity = ResolveEntity(source.objectId);
            if (entity) {
                const VxMatrix &world = entity->GetWorldMatrix();
                if (source.SheetPatchCount() > 0) {
                    // Distance to the nearest patch plane: the sheet's own
                    // "am I near the surface" number, now over every patch.
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
                    detail = " z=" + std::to_string(
                            std::fabs(Dot(Subtract(center, origin), normal)));
                }
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

    const PointFrame frame = core::expr::MakePointFrame(center);
    for (size_t index = 0; index < m_Scan.customFields.size(); ++index) {
        const CustomFieldInfo &field = m_Scan.customFields[index];
        if (!field.valid) {
            LogInfo("  C[%zu] \"%s\" rejected: %s", index, field.name.c_str(), field.error.c_str());
            continue;
        }
        bool nonFinite = false;
        const bool active = core::expr::EvaluateCustomDomain(field, frame, &nonFinite);
        VxVector contribution(0.0f, 0.0f, 0.0f);
        if (active) {
            contribution = core::expr::EvaluateCustomComponents(field, frame, &nonFinite);
            total = Add(total, contribution);
        }
        LogInfo("  C[%zu] \"%s\" mode=%d domain=\"%s\" active=%s g=(%.4f, %.4f, %.4f)", index,
                field.name.c_str(), field.mode, field.domainText.c_str(), BoolText(active).c_str(),
                contribution.x, contribution.y, contribution.z);
    }

    const std::string message =
            "sources=" + std::to_string(m_Scan.sources.size()) +
            " R rows=" + std::to_string(m_Scan.rRows.size()) +
            " global=" + std::to_string(m_Scan.globalSourceIds.size()) +
            " custom=" + std::to_string(m_Scan.customFields.size()) +
            " total=" + FormatVec(total) +
            " channels=" + std::to_string(m_ForceChannels.size()) +
            " affected=" + std::to_string(m_LastAffectedCount);
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

    const size_t validCustom = static_cast<size_t>(std::count_if(
            m_Scan.customFields.begin(), m_Scan.customFields.end(),
            [](const CustomFieldInfo &field) { return field.valid; }));
    const size_t rejectedCustom = m_Scan.customFields.size() - validCustom;

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
            ") Custom(rows=" + std::to_string(m_Scan.customFields.size()) +
            ", valid=" + std::to_string(validCustom) +
            ", rejected=" + std::to_string(rejectedCustom) +
            ") ignored=" + std::to_string(m_Scan.ignoredEntries) +
            " warnings=" + std::to_string(m_Scan.warnings.size());

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
            LogInfo("  S[%zu] %s (id=%lu) Sigma=%.4f patches=%d triangles=%d merged=%d",
                    index, source.objectName.c_str(),
                    static_cast<unsigned long>(source.objectId), source.strength,
                    source.SheetPatchCount(), source.sheetTriangleCount, source.sheetMergedCount);
            if (source.SheetPatchCount() == 0) {
                LogInfo("      rectangle fallback center=(%.4f, %.4f, %.4f) size=(%.4f x %.4f)",
                        source.localCenter.x, source.localCenter.y, source.localCenter.z,
                        source.halfA * 2.0f, source.halfB * 2.0f);
            }
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

    static const char *const kCustomColumns[2][3] = {{"X", "Y", "Z"}, {"R", "Theta", "Phi"}};
    LogInfo("Custom rows (FG_Custom): %zu", m_Scan.customFields.size());
    for (size_t index = 0; index < m_Scan.customFields.size(); ++index) {
        const CustomFieldInfo &field = m_Scan.customFields[index];
        const int group = field.mode == 1 ? 1 : 0;
        LogInfo("  C[%zu] \"%s\" mode=%d valid=%s", index, field.name.c_str(), field.mode,
                BoolText(field.valid).c_str());
        for (int component = 0; component < 3; ++component) {
            const int column = group * 3 + component;
            LogInfo("      %s(len=%zu)=\"%s\"", kCustomColumns[group][component],
                    field.text[column].size(), field.text[column].c_str());
        }
        for (int column = 0; column < 6; ++column) {
            if (column / 3 != group && !field.text[column].empty()) {
                LogInfo("      ignored %s(len=%zu)", kCustomColumns[column / 3][column % 3],
                        field.text[column].size());
            }
        }
        LogInfo("      domain(len=%zu)=\"%s\"", field.domainText.size(), field.domainText.c_str());
        if (!field.valid) {
            LogInfo("      rejected: %s", field.error.c_str());
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

// ---------------------------------------------------------------------------
// FG_Custom diagnostics (fg debug custom / expr / domain)
// ---------------------------------------------------------------------------

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

} // namespace

// Per-row Domain result and contribution at one point: the author's view of what
// the custom fields are actually doing.
void FlexGravityMod::PrintCustomStatus(IBML *bml, bool hasPoint, const VxVector &point) {
    (void) bml;
    if (!m_Scan.ran) {
        SendMessage("Not scanned yet. Enter a level or run 'fg debug reload'.");
        return;
    }
    if (m_Scan.customFields.empty()) {
        SendMessage("No FG_Custom rows in this level.");
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
    LogInfo("FG_Custom %s: p=%s r=%.4f theta=%.4f phi=%.4f", label.c_str(),
            FormatVec(where).c_str(), frame.variables[core::expr::kVarR],
            frame.variables[core::expr::kVarTheta], frame.variables[core::expr::kVarPhi]);

    VxVector total(0.0f, 0.0f, 0.0f);
    int activeCount = 0;
    for (size_t index = 0; index < m_Scan.customFields.size(); ++index) {
        const CustomFieldInfo &field = m_Scan.customFields[index];
        if (!field.valid) {
            LogInfo("  C[%zu] \"%s\" rejected: %s", index, field.name.c_str(), field.error.c_str());
            continue;
        }

        bool nonFinite = false;
        const bool active = core::expr::EvaluateCustomDomain(field, frame, &nonFinite);
        VxVector contribution(0.0f, 0.0f, 0.0f);
        if (active) {
            contribution = core::expr::EvaluateCustomComponents(field, frame, &nonFinite);
            total = Add(total, contribution);
            ++activeCount;
        }
        LogInfo("  C[%zu] \"%s\" mode=%d domain=\"%s\" active=%s%s g=(%.4f, %.4f, %.4f)", index,
                field.name.c_str(), field.mode, field.domainText.c_str(), BoolText(active).c_str(),
                nonFinite ? " (NaN/Inf)" : "", contribution.x, contribution.y, contribution.z);
    }

    const std::string message = "custom rows=" + std::to_string(m_Scan.customFields.size()) +
                                " active=" + std::to_string(activeCount) +
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
            SendMessage("No active ball found; use 'fg debug domain at <x> <y> <z> <relations>'.");
            return;
        }
        where = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    }
    if (text.empty()) {
        SendMessage("Usage: fg debug domain [at x y z] <relations>");
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

