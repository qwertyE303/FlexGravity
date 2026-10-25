#pragma once

#include <VxMatrix.h>
#include <VxVector.h>

#include "core/Constants.h"

namespace fg::core {

// A body is judged by a single point: its origin, i.e. the translation of its
// world matrix.  For the ball that is the centre of the sphere; for a
// mechanism it is the pivot the map author placed, not the centre of its
// bounding box.  No bounding box or mesh query is involved any more, so the
// per-frame cost is one matrix multiply.
struct BodySamples {
    VxVector points[kBodySampleCount];
    int count = 0;

    // The point the field is evaluated at; identical to the point the region
    // test uses.
    const VxVector &Center() const { return points[0]; }
};

// `world` is the body's world matrix.
BodySamples BuildBodySamples(const VxMatrix &world);

} // namespace fg::core
