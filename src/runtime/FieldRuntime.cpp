#include "mod/FlexGravityMod.h"

// Evaluation runs at most once per physics step.  The simulation advances at
// a fixed 1/66 s, so a force refresh between two steps cannot be observed;
// doing it every rendered frame at 240+ fps only burns CPU.
bool FlexGravityMod::ShouldUpdateFields() {
    CKTimeManager *timeManager = m_BML ? m_BML->GetTimeManager() : nullptr;
    if (!timeManager) {
        return true;
    }

    const float now = timeManager->GetTime();
    // `now < m_LastFieldUpdate` means the game clock restarted (level load);
    // treat it as "due" instead of waiting for the clock to catch up.
    if (m_LastFieldUpdate >= 0.0f && now >= m_LastFieldUpdate &&
        now - m_LastFieldUpdate < kPhysicsStepSeconds) {
        return false;
    }
    m_LastFieldUpdate = now;
    return true;
}

// Resolves every region and source object once per evaluation instead of once
// per body.  Object transforms cannot change inside our own frame callback.
void FlexGravityMod::RefreshFieldTransforms() {
    m_UFieldInverse.resize(m_Scan.uFields.size());
    m_UFieldValid.assign(m_Scan.uFields.size(), 0);
    for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
        if (CK3dEntity *entity = ResolveEntity(m_Scan.uFields[index].objectId)) {
            m_UFieldInverse[index] = entity->GetInverseWorldMatrix();
            m_UFieldValid[index] = 1;
        }
    }

    m_RegionInverse.resize(m_Scan.regions.size());
    m_RegionValid.assign(m_Scan.regions.size(), 0);
    for (size_t index = 0; index < m_Scan.regions.size(); ++index) {
        if (CK3dEntity *entity = ResolveEntity(m_Scan.regions[index].objectId)) {
            m_RegionInverse[index] = entity->GetInverseWorldMatrix();
            m_RegionValid[index] = 1;
        }
    }

    m_SourceWorld.resize(m_Scan.sources.size());
    m_SourceValid.assign(m_Scan.sources.size(), 0);
    m_SourceLines.resize(m_Scan.sources.size());
    m_SourceSheets.resize(m_Scan.sources.size());
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        const SourceInfo &source = m_Scan.sources[index];
        CK3dEntity *entity = ResolveEntity(source.objectId);
        if (!entity) {
            continue;
        }

        m_SourceWorld[index] = entity->GetWorldMatrix();
        m_SourceValid[index] = 1;
        const VxMatrix &world = m_SourceWorld[index];

        // Line: the whole polyline is transformed once here, not once per body.
        if (source.kind == SourceInfo::KindLine) {
            std::vector<VxVector> &points = m_SourceLines[index];
            points.resize(source.localPoints.size());
            for (size_t point = 0; point < points.size(); ++point) {
                points[point] = world * source.localPoints[point];
            }
            continue;
        }

        // Sheet: every patch vertex lands in world space once here.
        if (source.kind == SourceInfo::KindSheet && source.SheetPatchCount() > 0) {
            SheetWorldCache &cache = m_SourceSheets[index];
            const int patchCount = source.SheetPatchCount();
            cache.vertices.resize(source.sheetVertices.size());
            for (size_t vertex = 0; vertex < cache.vertices.size(); ++vertex) {
                cache.vertices[vertex] = world * source.sheetVertices[vertex];
            }
            cache.offsets = source.sheetOffsets;
            cache.normals.resize(static_cast<size_t>(patchCount));
            for (int patch = 0; patch < patchCount; ++patch) {
                const size_t begin = static_cast<size_t>(source.sheetOffsets[patch]);
                // Derived from the transformed vertices, so a rotated or
                // non-uniformly scaled source still yields a true plane normal.
                VxVector normal(0.0f, 0.0f, 0.0f);
                Normalize(Cross(Subtract(cache.vertices[begin + 1], cache.vertices[begin]),
                                Subtract(cache.vertices[begin + 2], cache.vertices[begin])),
                          normal);
                cache.normals[static_cast<size_t>(patch)] = normal;
            }
        }
    }
}

// id -> index into the scanned source / CField lists, so the per-body mask is
// O(1) per lookup instead of a scan over all rows.
void FlexGravityMod::BuildSourceIndex() {
    m_SourceIndex.clear();
    m_SourceIndex.reserve(m_Scan.sources.size());
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        m_SourceIndex[m_Scan.sources[index].objectId] = index;
    }

    m_CFieldIndex.clear();
    m_CFieldIndex.reserve(m_Scan.cFields.size());
    for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
        m_CFieldIndex[m_Scan.cFields[index].objectId] = index;
    }

    m_ActiveSources.assign(m_Scan.sources.size(), 0);
    m_ActiveCFields.assign(m_Scan.cFields.size(), 0);
    m_SourceWorld.assign(m_Scan.sources.size(), VxMatrix{});
    m_SourceValid.assign(m_Scan.sources.size(), 0);
    m_SourceLines.clear();
    m_SourceLines.resize(m_Scan.sources.size());
    m_SourceSheets.clear();
    m_SourceSheets.resize(m_Scan.sources.size());
    m_UFieldInverse.assign(m_Scan.uFields.size(), VxMatrix{});
    m_UFieldValid.assign(m_Scan.uFields.size(), 0);
    m_RegionInverse.assign(m_Scan.regions.size(), VxMatrix{});
    m_RegionValid.assign(m_Scan.regions.size(), 0);
    m_LastFieldUpdate = -1.0f;
}

VxVector FlexGravityMod::ComputeSourceAcceleration(const SourceInfo &source, const VxVector &center) {
    CK3dEntity *entity = ResolveEntity(source.objectId);
    if (!entity) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    return core::ComputeSourceAcceleration(source, entity->GetWorldMatrix(), center);
}

// One point in the region's local space: the same test every body makes with
// its own origin, exposed so the debug overlay can probe arbitrary points.
bool FlexGravityMod::PointInsideRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                                       const VxVector &worldPoint) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }
    return core::ContainsLocalPoint(region, inverseWorld * worldPoint);
}

// The activation rule, in one place: a source contributes when it is listed in
// FG_Global, or in an FG_Local row whose region contains the point.  A CField
// is just another source in those tables.
VxVector FlexGravityMod::EvaluateActiveFieldsAtPoint(const VxVector &point,
                                                     std::vector<char> &rowTouched,
                                                     std::vector<char> &sourceMask,
                                                     std::vector<char> &cFieldMask) {
    VxVector acceleration(0.0f, 0.0f, 0.0f);
    if (m_Scan.localRows.empty() && m_Scan.globalSources.empty()) {
        return acceleration;
    }
    if (m_SourceValid.size() < m_Scan.sources.size() || m_RegionValid.size() < m_Scan.regions.size()) {
        return acceleration;
    }

    if (rowTouched.size() != m_Scan.localRows.size()) {
        rowTouched.assign(m_Scan.localRows.size(), 0);
    }
    if (sourceMask.size() != m_Scan.sources.size()) {
        sourceMask.assign(m_Scan.sources.size(), 0);
    }
    if (cFieldMask.size() != m_Scan.cFields.size()) {
        cFieldMask.assign(m_Scan.cFields.size(), 0);
    }

    const bool needFrame = !m_Scan.cRegions.empty() || !m_Scan.cFields.empty();
    PointFrame frame{};
    bool frameReady = false;
    const auto ensureFrame = [&]() {
        if (!frameReady) {
            frame = core::expr::MakePointFrame(point);
            frameReady = true;
        }
    };

    for (size_t index = 0; index < m_Scan.localRows.size(); ++index) {
        const LocalRow &row = m_Scan.localRows[index];
        bool touched = false;
        if (row.regionKind == LocalRow::BuiltinRegion) {
            if (row.regionIndex >= 0 &&
                row.regionIndex < static_cast<int>(m_RegionValid.size()) &&
                m_RegionValid[static_cast<size_t>(row.regionIndex)]) {
                const size_t region = static_cast<size_t>(row.regionIndex);
                touched = PointInsideRegion(m_Scan.regions[region].volume, m_RegionInverse[region],
                                            point);
            }
        } else if (row.cRegionIndex >= 0 &&
                   row.cRegionIndex < static_cast<int>(m_Scan.cRegions.size())) {
            const CRegionRow &region = m_Scan.cRegions[static_cast<size_t>(row.cRegionIndex)];
            if (region.valid) {
                ensureFrame();
                bool nonFinite = false;
                touched = core::expr::EvaluateDomain(region.domain, frame.variables, &nonFinite);
                if (nonFinite) {
                    touched = false;
                }
            }
        }
        rowTouched[index] = touched ? 1 : 0;
    }

    std::fill(sourceMask.begin(), sourceMask.end(), 0);
    std::fill(cFieldMask.begin(), cFieldMask.end(), 0);

    const auto activate = [&](const SourceRef &ref) {
        if (ref.kind == SourceRef::Builtin) {
            if (ref.index >= 0 && ref.index < static_cast<int>(sourceMask.size())) {
                sourceMask[static_cast<size_t>(ref.index)] = 1;
            }
        } else if (ref.index >= 0 && ref.index < static_cast<int>(cFieldMask.size())) {
            cFieldMask[static_cast<size_t>(ref.index)] = 1;
        }
    };

    for (const SourceRef &ref : m_Scan.globalSources) {
        activate(ref);
    }
    for (size_t row = 0; row < rowTouched.size(); ++row) {
        if (!rowTouched[row]) {
            continue;
        }
        for (const SourceRef &ref : m_Scan.localRows[row].sources) {
            activate(ref);
        }
    }

    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        if (!sourceMask[index] || !m_SourceValid[index]) {
            continue;
        }

        const SourceInfo &source = m_Scan.sources[index];
        if (source.kind == SourceInfo::KindLine) {
            acceleration = Add(acceleration,
                               core::ComputePolylineAcceleration(m_SourceLines[index], source.closed,
                                                                 point, source.strength));
        } else if (source.kind == SourceInfo::KindSheet && source.SheetPatchCount() > 0 &&
                   m_SourceSheets[index].offsets.size() ==
                           static_cast<size_t>(source.SheetPatchCount()) + 1) {
            const SheetWorldCache &cache = m_SourceSheets[index];
            acceleration = Add(acceleration,
                               core::ComputeSheetPatchesAcceleration(
                                       cache.vertices.data(), cache.offsets.data(), cache.normals.data(),
                                       source.SheetPatchCount(), point, source.strength));
        } else {
            acceleration =
                    Add(acceleration,
                        core::ComputeSourceAcceleration(source, m_SourceWorld[index], point));
        }
    }

    if (!m_Scan.cFields.empty()) {
        if (m_CustomWarned.size() != m_Scan.cFields.size()) {
            m_CustomWarned.assign(m_Scan.cFields.size(), 0);
        }
        for (size_t index = 0; index < m_Scan.cFields.size(); ++index) {
            if (!cFieldMask[index]) {
                continue;
            }
            const CFieldRow &field = m_Scan.cFields[index];
            if (!field.compiled.valid) {
                continue;
            }
            ensureFrame();
            bool nonFinite = false;
            acceleration = Add(acceleration,
                               core::expr::EvaluateCField(field.compiled, frame, &nonFinite));
            if (nonFinite && !m_CustomWarned[index]) {
                m_CustomWarned[index] = 1;
                LogWarn("FG_CField \"%s\": a component evaluated to NaN/Inf and was treated as 0",
                        field.displayName.c_str());
            }
        }
    }

    return acceleration;
}

void FlexGravityMod::UpdateFields() {
    if (!m_Scan.ran) {
        ClearAllFieldForces();
        return;
    }

    const bool hasFields = !m_Scan.uFields.empty() || !m_Scan.sources.empty() ||
                           !m_Scan.cFields.empty() || !m_Scan.localRows.empty() ||
                           !m_Scan.globalSources.empty();
    if (!hasFields && !m_FreeMode) {
        ClearAllFieldForces();
        return;
    }

    RefreshFieldTransforms();

    CK_ID freeModeBallId = 0;
    if (m_FreeMode) {
        if (CK3dEntity *ball = FindActiveBall()) {
            freeModeBallId = ball->GetID();
        }
    }

    std::vector<char> rowTouched(m_Scan.localRows.size(), 0);
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

        if (m_FreeMode && body.objectId == freeModeBallId) {
            // Route C: the engine's gravity is a fixed (0,-20,0); cancel it with
            // the exact opposite acceleration on the ball's own force channel.
            // Field sources are skipped for the ball while freemode is on.
            const VxVector acceleration = Scale(core::kVanillaGravity, -1.0f);
            ApplyChannelForce(entity, body, acceleration);
            touched.insert(body.objectId);
            ++affected;
            continue;
        }

        if (!hasFields) {
            continue;
        }

        const BodySamples samples = BuildBodySamples(entity->GetWorldMatrix());
        VxVector acceleration(0.0f, 0.0f, 0.0f);

        for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
            if (!m_UFieldValid[index]) {
                continue;
            }
            if (ObjectTouchesRegion(m_Scan.uFields[index].region, m_UFieldInverse[index], samples)) {
                acceleration = Add(acceleration, m_Scan.uFields[index].gravity);
            }
        }

        if (!m_Scan.localRows.empty() || !m_Scan.globalSources.empty() || !m_Scan.cFields.empty()) {
            acceleration = Add(acceleration,
                               EvaluateActiveFieldsAtPoint(samples.Center(), rowTouched,
                                                           m_ActiveSources, m_ActiveCFields));
        }

        if (LengthSquared(acceleration) <= kNoAccelerationSquared) {
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
        LogInfo("Fields: %zu U + %zu source(s) + %zu CField(s), %zu local row(s), %zu global "
                "source(s), affected dynamic objects: %d",
                m_Scan.uFields.size(), m_Scan.sources.size(), m_Scan.cFields.size(),
                m_Scan.localRows.size(), m_Scan.globalSources.size(), affected);
    }
}

void FlexGravityMod::SetFreeMode(IBML *bml, bool enable) {
    (void) bml;
    if (enable == m_FreeMode) {
        SendMessage(enable ? "freemode is already on." : "freemode is already off.");
        return;
    }

    m_FreeMode = enable;
    if (enable) {
        // Route C: the engine pulls every dynamic body with a fixed (0,-20,0).
        // Cancelling it on the ball's own channel is exact for vanilla gravity
        // and needs no physics-RT ABI, which the retail DLL does not expose.
        SendMessage("freemode on: the active ball's gravity is cancelled.");
        LogInfo("freemode enabled (route C: counter-force against (0, -20, 0))");
    } else {
        SendMessage("freemode off.");
        LogInfo("freemode disabled");
    }
}
