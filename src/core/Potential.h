#pragma once

#include <vector>

#include <VxVector.h>

#include "core/FieldData.h"

namespace fg::core {

// Potential of every source kind, defined so that the acceleration the physics
// path applies is exactly -grad(Phi).  The debug overlay uses it for the
// equipotential surfaces; the tests verify the gradient against the
// accelerations above, which is the only thing that keeps the two in sync.
float ComputeSegmentPotential(const VxVector &a, const VxVector &b, const VxVector &center,
                              float strength);

float ComputeMonopolePotential(const VxVector &position, float strength, const VxVector &center);

float ComputePolygonPotential(const VxVector *vertices, int count, const VxVector &normal,
                              const VxVector &center, float strength);

float ComputeSheetPatchesPotential(const VxVector *vertices, const int *offsets,
                                   const VxVector *normals, int patchCount, const VxVector &center,
                                   float strength);

float ComputePolylinePotential(const std::vector<VxVector> &points, bool closed,
                               const VxVector &center, float strength);

float ComputeSourcePotential(const SourceInfo &source, const VxMatrix &world,
                             const VxVector &center);

} // namespace fg::core
