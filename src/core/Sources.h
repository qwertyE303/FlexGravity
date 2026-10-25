#pragma once

#include <vector>

#include <VxMatrix.h>
#include <VxVector.h>

#include "core/FieldData.h"

namespace fg::core {

// Exact field of a finite uniform line segment, normalised so that an infinite
// line produces strength / d:
//   g = (lambda/2) * [ -(K1/d) * p_hat + K2 * u ]
VxVector ComputeSegmentAcceleration(const VxVector &a, const VxVector &b,
                                    const VxVector &center, float strength);

// g = strength * r / |r|^3, with r pointing from the body towards the source,
// so a positive strength attracts.  At |r| = 1 the magnitude is `strength`.
VxVector ComputeMonopoleAcceleration(const VxVector &position, float strength,
                                     const VxVector &center);

// Exact field of a convex planar polygon (double sided), normalised so that an
// infinite sheet produces `strength`.  `vertices` are world space and ordered
// around `normal`; the polygon must be planar and convex.  This is the single
// primitive behind every sheet: a triangle, a merged quad, or the rectangle the
// old formula evaluated, which it reproduces to float precision.
VxVector ComputePolygonAcceleration(const VxVector *vertices, int count,
                                    const VxVector &normal, const VxVector &center,
                                    float strength);

// Sum over every patch of a sheet source.  `offsets` holds patchCount + 1
// vertex indices, `normals` one unit normal per patch; all world space.
VxVector ComputeSheetPatchesAcceleration(const VxVector *vertices, const int *offsets,
                                         const VxVector *normals, int patchCount,
                                         const VxVector &center, float strength);

// Exact field of a finite rectangle, normalised like ComputePolygonAcceleration.
// `world` is the sheet object's world matrix.  Used when a sheet object has no
// usable mesh and falls back to its bounding box.
VxVector ComputeSheetAcceleration(const SourceInfo &source, const VxMatrix &world,
                                  const VxVector &center);

// Sum of the exact segment fields over a world-space polyline.  This is the
// per-body entry point for line sources: the polyline is transformed once per
// evaluation, not once per body.
VxVector ComputePolylineAcceleration(const std::vector<VxVector> &points, bool closed,
                                     const VxVector &center, float strength);

// Field of one declared source evaluated at `center`, in world space.
// `world` is the source object's world matrix.  Convenience path used by tests
// and reports; the per-frame path calls the specialised entry points above.
VxVector ComputeSourceAcceleration(const SourceInfo &source, const VxMatrix &world,
                                   const VxVector &center);

// Distance from `center` to a polyline; used by the field status report only.
float DistanceToPolyline(const std::vector<VxVector> &points, bool closed,
                         const VxVector &center);

} // namespace fg::core
