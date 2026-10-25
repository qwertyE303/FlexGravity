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
    const size_t regionCount = m_Scan.uFields.size() + m_Scan.rRows.size();
    m_RegionInverse.resize(regionCount);
    m_RegionValid.assign(regionCount, 0);

    size_t cursor = 0;
    for (const UFieldInfo &field : m_Scan.uFields) {
        if (CK3dEntity *entity = ResolveEntity(field.objectId)) {
            m_RegionInverse[cursor] = entity->GetInverseWorldMatrix();
            m_RegionValid[cursor] = 1;
        }
        ++cursor;
    }
    for (const RRowInfo &row : m_Scan.rRows) {
        if (CK3dEntity *entity = ResolveEntity(row.regionObjectId)) {
            m_RegionInverse[cursor] = entity->GetInverseWorldMatrix();
            m_RegionValid[cursor] = 1;
        }
        ++cursor;
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

// id -> index into m_Scan.sources, so the per-body activation mask is O(1)
// per lookup instead of a scan over all rows.
void FlexGravityMod::BuildSourceIndex() {
    m_SourceIndex.clear();
    m_SourceIndex.reserve(m_Scan.sources.size());
    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        m_SourceIndex[m_Scan.sources[index].objectId] = index;
    }

    m_ActiveSources.assign(m_Scan.sources.size(), 0);
    m_SourceWorld.assign(m_Scan.sources.size(), VxMatrix{});
    m_SourceValid.assign(m_Scan.sources.size(), 0);
    m_SourceLines.clear();
    m_SourceLines.resize(m_Scan.sources.size());
    m_SourceSheets.clear();
    m_SourceSheets.resize(m_Scan.sources.size());
    m_RegionInverse.assign(m_Scan.uFields.size() + m_Scan.rRows.size(), VxMatrix{});
    m_RegionValid.assign(m_Scan.uFields.size() + m_Scan.rRows.size(), 0);
    m_LastFieldUpdate = -1.0f;
}

VxVector FlexGravityMod::ComputeSourceAcceleration(const SourceInfo &source, const VxVector &center) {
    CK3dEntity *entity = ResolveEntity(source.objectId);
    if (!entity) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    return core::ComputeSourceAcceleration(source, entity->GetWorldMatrix(), center);
}

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

// One point in the region's local space: the same test every body makes with
// its own origin, exposed so the debug overlay can probe arbitrary points.
bool FlexGravityMod::PointInsideRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                                       const VxVector &worldPoint) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }
    return core::ContainsLocalPoint(region, inverseWorld * worldPoint);
}

// FG_Custom rows, evaluated at one world point.  The spherical frame is built
// once per point and shared by every row, and nothing here touches the object
// caches, so a level that only has custom fields is live from the first frame.
VxVector FlexGravityMod::EvaluateCustomFieldsAtPoint(const VxVector &point) {
    if (m_Scan.customFields.empty()) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const PointFrame frame = core::expr::MakePointFrame(point);
    if (m_CustomWarned.size() != m_Scan.customFields.size() * 2) {
        m_CustomWarned.assign(m_Scan.customFields.size() * 2, 0);
    }

    VxVector total(0.0f, 0.0f, 0.0f);
    for (size_t index = 0; index < m_Scan.customFields.size(); ++index) {
        const CustomFieldInfo &field = m_Scan.customFields[index];
        if (!field.valid) {
            continue;
        }

        bool nonFinite = false;
        const bool active = core::expr::EvaluateCustomDomain(field, frame, &nonFinite);
        if (nonFinite && !m_CustomWarned[index * 2]) {
            m_CustomWarned[index * 2] = 1;
            LogWarn("FG_Custom row %zu \"%s\": the Domain evaluated to NaN/Inf, so the row is "
                    "inactive at that point",
                    index, field.name.c_str());
        }
        if (!active) {
            continue;
        }

        const VxVector contribution = core::expr::EvaluateCustomComponents(field, frame, &nonFinite);
        if (nonFinite && !m_CustomWarned[index * 2 + 1]) {
            m_CustomWarned[index * 2 + 1] = 1;
            LogWarn("FG_Custom row %zu \"%s\": a component evaluated to NaN/Inf and was treated as 0",
                    index, field.name.c_str());
        }
        total = Add(total, contribution);
    }
    return total;
}

// The activation rule, in one place: an object source contributes when it is
// listed in FG_RG, or when it is listed in a FG_R row whose region contains the
// point; a FG_Custom row contributes when its Domain holds.  The custom rows are
// summed first so no cache problem can ever drop them.
VxVector FlexGravityMod::EvaluateActiveFieldsAtPoint(const VxVector &point,
                                                     std::vector<char> &rowTouched,
                                                     std::vector<char> &sourceMask) {
    VxVector acceleration = EvaluateCustomFieldsAtPoint(point);
    if (m_Scan.sources.empty()) {
        return acceleration;
    }

    const size_t regionCount = m_Scan.uFields.size() + m_Scan.rRows.size();
    if (m_RegionValid.size() < regionCount || m_RegionInverse.size() < regionCount ||
        m_SourceValid.size() < m_Scan.sources.size() || m_SourceLines.size() < m_Scan.sources.size() ||
        m_SourceSheets.size() < m_Scan.sources.size() || m_SourceWorld.size() < m_Scan.sources.size()) {
        return acceleration;
    }
    if (rowTouched.size() != m_Scan.rRows.size()) {
        rowTouched.assign(m_Scan.rRows.size(), 0);
    }
    if (sourceMask.size() != m_Scan.sources.size()) {
        sourceMask.assign(m_Scan.sources.size(), 0);
    }

    for (size_t index = 0; index < m_Scan.rRows.size(); ++index) {
        const size_t regionIndex = m_Scan.uFields.size() + index;
        rowTouched[index] =
                (m_RegionValid[regionIndex] &&
                 PointInsideRegion(m_Scan.rRows[index].region, m_RegionInverse[regionIndex], point))
                ? 1
                : 0;
    }

    std::fill(sourceMask.begin(), sourceMask.end(), 0);
    for (CK_ID sourceId : m_Scan.globalSourceIds) {
        auto it = m_SourceIndex.find(sourceId);
        if (it != m_SourceIndex.end()) {
            sourceMask[it->second] = 1;
        }
    }
    for (size_t row = 0; row < rowTouched.size(); ++row) {
        if (!rowTouched[row]) {
            continue;
        }
        for (CK_ID sourceId : m_Scan.rRows[row].sourceIds) {
            auto it = m_SourceIndex.find(sourceId);
            if (it != m_SourceIndex.end()) {
                sourceMask[it->second] = 1;
            }
        }
    }

    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        if (!sourceMask[index] || !m_SourceValid[index]) {
            continue;
        }

        const SourceInfo &source = m_Scan.sources[index];
        if (source.kind == SourceInfo::KindLine) {
            // The world polyline was built once in RefreshFieldTransforms.
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
    return acceleration;
}

void FlexGravityMod::UpdateFields() {
    if (!m_Scan.ran ||
        (m_Scan.uFields.empty() && m_Scan.sources.empty() && m_Scan.customFields.empty())) {
        ClearAllFieldForces();
        return;
    }

    RefreshFieldTransforms();

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

        const BodySamples samples = BuildBodySamples(entity->GetWorldMatrix());
        VxVector acceleration(0.0f, 0.0f, 0.0f);

        for (size_t index = 0; index < m_Scan.uFields.size(); ++index) {
            if (!m_RegionValid[index]) {
                continue;
            }
            if (ObjectTouchesRegion(m_Scan.uFields[index].region, m_RegionInverse[index], samples)) {
                acceleration = Add(acceleration, m_Scan.uFields[index].gravity);
            }
        }

        // One activation test per body, at the body's origin (the same point the
        // force is applied to).  FG_Custom rows are evaluated in there too.
        if (!m_Scan.sources.empty() || !m_Scan.customFields.empty()) {
            acceleration = Add(
                    acceleration,
                    EvaluateActiveFieldsAtPoint(samples.Center(), rowTouched, m_ActiveSources));
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
        LogInfo("Fields: %zu U + %zu source(s) + %zu custom row(s), affected dynamic objects: %d",
                m_Scan.uFields.size(), m_Scan.sources.size(), m_Scan.customFields.size(), affected);
    }
}
