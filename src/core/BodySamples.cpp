#include "core/BodySamples.h"

namespace fg::core {

BodySamples BuildBodySamples(const VxMatrix &world) {
    BodySamples samples;
    samples.points[samples.count++] = world * VxVector(0.0f, 0.0f, 0.0f);
    return samples;
}

} // namespace fg::core
