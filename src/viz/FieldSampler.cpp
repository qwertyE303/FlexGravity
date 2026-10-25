#include "mod/FlexGravityMod.h"

#include "core/Potential.h"

// Total acceleration at an arbitrary world point, for the debug overlay.
//
// This is the physics rule of UpdateFields with the body replaced by a
// zero-mass probe: vanilla gravity, plus every FG_U whose region contains the
// point, plus every field source that is active at the point (FG_RG, or an
// FG_R row whose region contains the point).  The physics path never calls
// this, so a probe can never change what a body feels.
VxVector FlexGravityMod::SampleFieldAtPoint(const VxVector &worldPoint) {
    VxVector acceleration = core::kVanillaGravity;
    if (!m_Scan.ran) {
        return acceleration;
    }

    const size_t regionCount = m_Scan.uFields.size() + m_Scan.rRows.size();
    if (m_RegionValid.size() < regionCount || m_RegionInverse.size() < regionCount) {
        // Nothing has evaluated the fields yet this level: the caches are built
        // by RefreshFieldTransforms, which only runs inside UpdateFields.
        RefreshFieldTransforms();
    }

    for (size_t index = 0; index < m_Scan.uFields.size() && index < m_RegionValid.size(); ++index) {
        if (!m_RegionValid[index]) {
            continue;
        }
        if (PointInsideRegion(m_Scan.uFields[index].region, m_RegionInverse[index], worldPoint)) {
            acceleration = Add(acceleration, m_Scan.uFields[index].gravity);
        }
    }

    if (!m_Scan.sources.empty() || !m_Scan.customFields.empty()) {
        acceleration =
                Add(acceleration, EvaluateActiveFieldsAtPoint(worldPoint, m_VizRows, m_VizSources));
    }
    return acceleration;
}

// Scalar potential whose negative gradient is the acceleration above.  Only the
// equipotential overlay reads it, and it deliberately uses the very same
// activation rule, so a surface of constant Phi always crosses the field lines
// at a right angle.
//
// FG_Custom rows are NOT part of this: an arbitrary expression vector field has
// no potential in general, and inventing one (a path integral) would draw
// surfaces that disagree with the physics.  The arrow and the field lines do
// include them, because they only need the acceleration.
float FlexGravityMod::PotentialAtPoint(const VxVector &worldPoint) {
    // A uniform acceleration g has potential -g . p; the constant is irrelevant
    // because only differences between lattice samples matter.
    float potential = -core::Dot(core::kVanillaGravity, worldPoint);
    if (!m_Scan.ran) {
        return potential;
    }

    const size_t regionCount = m_Scan.uFields.size() + m_Scan.rRows.size();
    if (m_RegionValid.size() < regionCount || m_RegionInverse.size() < regionCount) {
        RefreshFieldTransforms();
    }

    for (size_t index = 0; index < m_Scan.uFields.size() && index < m_RegionValid.size(); ++index) {
        if (!m_RegionValid[index]) {
            continue;
        }
        if (PointInsideRegion(m_Scan.uFields[index].region, m_RegionInverse[index], worldPoint)) {
            potential -= core::Dot(m_Scan.uFields[index].gravity, worldPoint);
        }
    }

    if (!m_Scan.sources.empty()) {
        potential += EvaluateSourcePotentialsAtPoint(worldPoint, m_VizRows, m_VizSources);
    }
    return potential;
}

// Potential counterpart of EvaluateActiveFieldsAtPoint: identical activation mask,
// identical dispatch, summed as scalars instead of accelerations.  FG_Custom rows
// are absent here on purpose (see PotentialAtPoint).
float FlexGravityMod::EvaluateSourcePotentialsAtPoint(const VxVector &point,
                                                      std::vector<char> &rowTouched,
                                                      std::vector<char> &sourceMask) {
    float potential = 0.0f;
    if (m_Scan.sources.empty()) {
        return potential;
    }

    const size_t regionCount = m_Scan.uFields.size() + m_Scan.rRows.size();
    if (m_RegionValid.size() < regionCount || m_RegionInverse.size() < regionCount ||
        m_SourceValid.size() < m_Scan.sources.size() || m_SourceLines.size() < m_Scan.sources.size() ||
        m_SourceSheets.size() < m_Scan.sources.size() || m_SourceWorld.size() < m_Scan.sources.size()) {
        return potential;
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
