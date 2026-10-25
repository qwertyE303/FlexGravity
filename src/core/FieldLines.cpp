#include "core/FieldLines.h"

#include <cmath>

#include "core/Math.h"

namespace fg::core {
namespace {

bool InsideBounds(const VxVector &point, const FieldLineOptions &options) {
    if (!(options.boundsRadius > 0.0f)) {
        return true;
    }
    return LengthSquared(Subtract(point, options.boundsCenter)) <=
           options.boundsRadius * options.boundsRadius;
}

// Unit tangent of the field line at `point`.  False means the field is a null
// field there and the line has to stop.
bool SampleDirection(FieldEvaluator evaluate, void *context, const VxVector &point,
                     const FieldLineOptions &options, VxVector &direction) {
    const VxVector field = evaluate(context, point);
    if (LengthSquared(field) <= options.minMagnitude * options.minMagnitude) {
        return false;
    }
    return Normalize(field, direction);
}

enum class StepResult {
    Ok,
    Null,       // the field vanished along the step: the line ends there
    OutOfBounds // the step left the traced sphere: clipped, then the line ends
};

// One RK4 step of the arc-length parameterisation.  `k1` is the already
// normalised field at `point` (the caller always has it: it is the direction
// returned by the previous step), so a step costs four field evaluations instead
// of five.  `nextDirection` is the normalised field at the new point, which the
// caller uses as the curvature estimate and as the next `k1`.
StepResult TakeStep(const VxVector &point, const VxVector &k1, float step, FieldEvaluator evaluate,
                    void *context, const FieldLineOptions &options, VxVector &next,
                    VxVector &nextDirection) {
    VxVector k2(0.0f, 0.0f, 0.0f);
    VxVector k3(0.0f, 0.0f, 0.0f);
    VxVector k4(0.0f, 0.0f, 0.0f);

    const VxVector mid2 = Add(point, Scale(k1, step * 0.5f));
    if (!InsideBounds(mid2, options)) {
        next = mid2;
        return StepResult::OutOfBounds;
    }
    if (!SampleDirection(evaluate, context, mid2, options, k2)) {
        return StepResult::Null;
    }
    const VxVector mid3 = Add(point, Scale(k2, step * 0.5f));
    if (!InsideBounds(mid3, options)) {
        next = mid3;
        return StepResult::OutOfBounds;
    }
    if (!SampleDirection(evaluate, context, mid3, options, k3)) {
        return StepResult::Null;
    }
    const VxVector end4 = Add(point, Scale(k3, step));
    if (!InsideBounds(end4, options)) {
        next = end4;
        return StepResult::OutOfBounds;
    }
    if (!SampleDirection(evaluate, context, end4, options, k4)) {
        return StepResult::Null;
    }

    const VxVector change =
            Scale(Add(Add(k1, Scale(k2, 2.0f)), Add(Scale(k3, 2.0f), k4)), step / 6.0f);
    next = Add(point, change);
    if (!InsideBounds(next, options)) {
        return StepResult::OutOfBounds;
    }
    if (!SampleDirection(evaluate, context, next, options, nextDirection)) {
        return StepResult::Null;
    }
    return StepResult::Ok;
}

// Walks from `seed` in one direction and appends every point it keeps to `out`.
// The first point is not appended; the seed is already in the polyline.
// `seedDirection` is the normalised field at the seed, sampled once by the
// caller and shared by both directions.
void TraceDirection(const VxVector &seed, const VxVector &seedDirection, float sign,
                    FieldEvaluator evaluate, void *context, const FieldLineOptions &options,
                    std::vector<VxVector> &out) {
    const float minimumStep = options.step * 0.125f;
    float step = options.step * sign;
    VxVector point = seed;
    VxVector direction = seedDirection;

    for (int index = 0; index < options.maxSteps; ++index) {
        bool advanced = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            VxVector next(0.0f, 0.0f, 0.0f);
            VxVector nextDirection(0.0f, 0.0f, 0.0f);
            const StepResult result =
                    TakeStep(point, direction, step, evaluate, context, options, next, nextDirection);

            if (result == StepResult::Null) {
                return;
            }
            if (result == StepResult::OutOfBounds) {
                // Stop exactly on the sphere instead of a step short of it, so
                // the drawn line reaches the edge of the debug view.
                const VxVector change = Subtract(next, point);
                float inside = 0.0f;
                float outside = 1.0f;
                for (int bisect = 0; bisect < 12; ++bisect) {
                    const float middle = 0.5f * (inside + outside);
                    if (InsideBounds(Add(point, Scale(change, middle)), options)) {
                        inside = middle;
                    } else {
                        outside = middle;
                    }
                }
                if (inside > 1.0e-4f) {
                    out.push_back(Add(point, Scale(change, inside)));
                }
                return;
            }

            // Curvature control: too much turn means the step was too long.
            const float cosine = Dot(direction, nextDirection);
            if (cosine < kFieldLineMaxTurnCosine && std::fabs(step) > minimumStep) {
                step *= 0.5f;
                continue;
            }

            out.push_back(next);
            point = next;
            direction = nextDirection;
            if (cosine > kFieldLineMinTurnCosine) {
                const float grown = std::fabs(step) * 1.7f;
                step = (step < 0.0f ? -1.0f : 1.0f) * (std::min)(grown, kFieldLineMaxStep);
            } else if (std::fabs(step) > options.step) {
                const float shrunk = std::fabs(step) / 1.3f;
                step = (step < 0.0f ? -1.0f : 1.0f) * (std::max)(shrunk, options.step);
            }
            advanced = true;
            break;
        }
        if (!advanced) {
            return;
        }
    }
}

} // namespace

bool TraceFieldLine(const VxVector &seed, FieldEvaluator evaluate, void *context,
                    const FieldLineOptions &options, std::vector<VxVector> &points) {
    points.clear();

    VxVector direction(0.0f, 0.0f, 0.0f);
    if (!SampleDirection(evaluate, context, seed, options, direction)) {
        return false;
    }

    // Buffers live across calls: tracing is single threaded and a long level
    // start would otherwise churn the allocator once per line.
    static std::vector<VxVector> forward;
    static std::vector<VxVector> backward;
    forward.clear();
    backward.clear();
    forward.reserve(static_cast<size_t>(options.maxSteps));
    backward.reserve(static_cast<size_t>(options.maxSteps));

    TraceDirection(seed, direction, 1.0f, evaluate, context, options, forward);
    TraceDirection(seed, direction, -1.0f, evaluate, context, options, backward);

    points.reserve(1 + forward.size() + backward.size());
    for (auto it = backward.rbegin(); it != backward.rend(); ++it) {
        points.push_back(*it);
    }
    points.push_back(seed);
    points.insert(points.end(), forward.begin(), forward.end());
    return true;
}

} // namespace fg::core
