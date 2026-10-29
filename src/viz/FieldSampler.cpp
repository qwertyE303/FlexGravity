#include "mod/FlexGravityMod.h"

#include "core/Potential.h"

// Total acceleration at an arbitrary world point, for the debug overlay.
//
// This is the physics rule of UpdateFields with the body replaced by a
// zero-mass probe: vanilla gravity, plus every FG_UField whose region contains
// the point, plus every source active through FG_Global / FG_Local.  CFields
// are ordinary sources now, so they are included exactly where the physics
// path includes them.  The physics path never calls this, so a probe can never
// change what a body feels.
VxVector FlexGravityMod::SampleFieldAtPoint(const VxVector &worldPoint) {
    VxVector acceleration = core::kVanillaGravity;
    if (!m_Scan.ran) {
        return acceleration;
    }

    if (m_UFieldValid.size() < m_Scan.uFields.size() || m_RegionValid.size() < m_Scan.regions.size() ||
        m_SourceValid.size() < m_Scan.sources.size()) {
        // Nothing has evaluated the fields yet this level: the caches are built
        // by RefreshFieldTransforms, which only runs inside UpdateFields.
        RefreshFieldTransforms();
    }

    for (size_t index = 0; index < m_Scan.uFields.size() && index < m_UFieldValid.size(); ++index) {
        if (!m_UFieldValid[index]) {
            continue;
        }
        if (PointInsideRegion(m_Scan.uFields[index].region, m_UFieldInverse[index], worldPoint)) {
            acceleration = Add(acceleration, m_Scan.uFields[index].gravity);
        }
    }

    if (!m_Scan.localRows.empty() || !m_Scan.globalSources.empty() || !m_Scan.cFields.empty()) {
        acceleration = Add(acceleration,
                           EvaluateActiveFieldsAtPoint(worldPoint, m_VizRows, m_VizSources,
                                                       m_VizCFields));
    }
    return acceleration;
}

// Scalar potential whose negative gradient is the acceleration above.  Only the
// equipotential overlay reads it, and it deliberately uses the very same
// activation rule, so a surface of constant Phi always crosses the field lines
// at a right angle.
//
// CFields are NOT part of this: an arbitrary expression vector field has no
// potential in general, and inventing one (a path integral) would draw surfaces
// that disagree with the physics.  The arrow and the field lines do include
// them, because they only need the acceleration.
float FlexGravityMod::PotentialAtPoint(const VxVector &worldPoint) {
    // A uniform acceleration g has potential -g . p; the constant is irrelevant
    // because only differences between lattice samples matter.
    float potential = -core::Dot(core::kVanillaGravity, worldPoint);
    if (!m_Scan.ran) {
        return potential;
    }

    if (m_UFieldValid.size() < m_Scan.uFields.size() || m_RegionValid.size() < m_Scan.regions.size()) {
        RefreshFieldTransforms();
    }

    for (size_t index = 0; index < m_Scan.uFields.size() && index < m_UFieldValid.size(); ++index) {
        if (!m_UFieldValid[index]) {
            continue;
        }
        if (PointInsideRegion(m_Scan.uFields[index].region, m_UFieldInverse[index], worldPoint)) {
            potential -= core::Dot(m_Scan.uFields[index].gravity, worldPoint);
        }
    }

    if (!m_Scan.localRows.empty() || !m_Scan.globalSources.empty()) {
        potential += EvaluateSourcePotentialsAtPoint(worldPoint, m_VizRows, m_VizSources,
                                                     m_VizCFields);
    }
    return potential;
}

// Potential counterpart of EvaluateActiveFieldsAtPoint: identical activation
// mask, identical dispatch, summed as scalars instead of accelerations.
// CFields are absent here on purpose (see PotentialAtPoint).
float FlexGravityMod::EvaluateSourcePotentialsAtPoint(const VxVector &point,
                                                      std::vector<char> &rowTouched,
                                                      std::vector<char> &sourceMask,
                                                      std::vector<char> &cFieldMask) {
    (void) cFieldMask;
    float potential = 0.0f;
    if (m_Scan.localRows.empty() && m_Scan.globalSources.empty()) {
        return potential;
    }
    if (m_SourceValid.size() < m_Scan.sources.size() || m_RegionValid.size() < m_Scan.regions.size()) {
        return potential;
    }

    if (rowTouched.size() != m_Scan.localRows.size()) {
        rowTouched.assign(m_Scan.localRows.size(), 0);
    }
    if (sourceMask.size() != m_Scan.sources.size()) {
        sourceMask.assign(m_Scan.sources.size(), 0);
    }

    const bool needFrame = !m_Scan.cRegions.empty();
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
    for (const SourceRef &ref : m_Scan.globalSources) {
        if (ref.kind == SourceRef::Builtin && ref.index >= 0 &&
            ref.index < static_cast<int>(sourceMask.size())) {
            sourceMask[static_cast<size_t>(ref.index)] = 1;
        }
    }
    for (size_t row = 0; row < rowTouched.size(); ++row) {
        if (!rowTouched[row]) {
            continue;
        }
        for (const SourceRef &ref : m_Scan.localRows[row].sources) {
            if (ref.kind == SourceRef::Builtin && ref.index >= 0 &&
                ref.index < static_cast<int>(sourceMask.size())) {
                sourceMask[static_cast<size_t>(ref.index)] = 1;
            }
        }
    }

    for (size_t index = 0; index < m_Scan.sources.size(); ++index) {
        if (!sourceMask[index] || !m_SourceValid[index]) {
            continue;
        }

        const SourceInfo &source = m_Scan.sources[index];
        if (source.kind == SourceInfo::KindLine) {
            potential += core::ComputePolylinePotential(m_SourceLines[index], source.closed, point,
                                                        source.strength);
        } else if (source.kind == SourceInfo::KindSheet && source.SheetPatchCount() > 0 &&
                   m_SourceSheets[index].offsets.size() ==
                           static_cast<size_t>(source.SheetPatchCount()) + 1) {
            const SheetWorldCache &cache = m_SourceSheets[index];
            potential += core::ComputeSheetPatchesPotential(
                    cache.vertices.data(), cache.offsets.data(), cache.normals.data(),
                    source.SheetPatchCount(), point, source.strength);
        } else {
            potential += core::ComputeSourcePotential(source, m_SourceWorld[index], point);
        }
    }
    return potential;
}
