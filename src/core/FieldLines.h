#pragma once

#include <vector>

#include <VxVector.h>

#include "core/Constants.h"

namespace fg::core {

// Evaluates the total gravity (metres per second squared) at a world point.
// The overlay passes a static thunk plus a pointer to the mod instance, so this
// file stays free of engine types.
using FieldEvaluator = VxVector (*)(void *context, const VxVector &point);

struct FieldLineOptions {
    float step = kFieldLineStep;
    int maxSteps = kFieldLineMaxSteps;
    float minMagnitude = kFieldLineMinMagnitude;
    VxVector boundsCenter = VxVector(0.0f, 0.0f, 0.0f);
    float boundsRadius = 0.0f; // <= 0 disables the bound
};

// Traces the field line through `seed` in both directions (along +g and -g)
// with RK4 on the normalised field, so `step` is an arc length in world units.
// `points` receives the polyline starting at the backward end and ending at the
// forward end; the seed is one of the points.  Returns false when the field at
// the seed is (numerically) zero, in which case there is no line to draw.
bool TraceFieldLine(const VxVector &seed, FieldEvaluator evaluate, void *context,
                    const FieldLineOptions &options, std::vector<VxVector> &points);

} // namespace fg::core
