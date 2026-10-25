#include "core/expr/FunctionField.h"

#include <cmath>

namespace fg::core::expr {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// The variables each coordinate system's components may use.  Domain always
// sees all six.
constexpr unsigned kGroupMask[2] = {kMaskRectangular, kMaskSpherical};

const char *const kGroupNames[2][3] = {
        {"X", "Y", "Z"},
        {"R", "Theta", "Phi"},
};

double Clamp(double value, double low, double high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

Instruction ConstantZero() {
    Instruction instruction;
    instruction.op = OpCode::PushNumber;
    instruction.number = 0.0;
    return instruction;
}

} // namespace

PointFrame MakePointFrame(const VxVector &worldPoint) {
    PointFrame frame{};

    const double x = static_cast<double>(worldPoint.x);
    const double y = static_cast<double>(worldPoint.y);
    const double z = static_cast<double>(worldPoint.z);

    frame.variables[kVarX] = x;
    frame.variables[kVarY] = y;
    frame.variables[kVarZ] = z;

    const double radius = std::sqrt(x * x + y * y + z * z);
    double theta = 0.0;
    double phi = 0.0;

    // At the origin the angles are undefined; the specification keeps the row
    // evaluable, so the fallback frame is +x / +z / +y.
    if (radius > 1.0e-9) {
        theta = std::atan2(z, x);
        if (theta < 0.0) {
            theta += kTwoPi;
        }
        phi = std::asin(Clamp(y / radius, -1.0, 1.0));
    }

    frame.variables[kVarR] = radius;
    frame.variables[kVarTheta] = theta;
    frame.variables[kVarPhi] = phi;

    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    const double cosTheta = std::cos(theta);
    const double sinTheta = std::sin(theta);

    frame.radial[0] = cosPhi * cosTheta;
    frame.radial[1] = sinPhi;
    frame.radial[2] = cosPhi * sinTheta;

    frame.thetaAxis[0] = -sinTheta;
    frame.thetaAxis[1] = 0.0;
    frame.thetaAxis[2] = cosTheta;

    frame.phiAxis[0] = -sinPhi * cosTheta;
    frame.phiAxis[1] = cosPhi;
    frame.phiAxis[2] = -sinPhi * sinTheta;

    return frame;
}

void CompileCustomField(CustomFieldInfo &field) {
    field.valid = false;
    field.error.clear();
    field.domain = Domain{};
    field.hasDomain = false;

    const int group = field.mode == 1 ? 1 : 0;
    const unsigned mask = kGroupMask[group];

    for (int component = 0; component < 3; ++component) {
        const int column = group * 3 + component;
        field.components[component] = Program{};

        if (field.text[column].empty()) {
            // Defensive: an empty cell is 0, not a reason to reject the row.
            field.components[component].code.push_back(ConstantZero());
            field.components[component].maxDepth = 1;
            continue;
        }

        CompileError error;
        if (!CompileExpression(field.text[column], mask, field.components[component], error)) {
            field.error = std::string(kGroupNames[group][component]) + ": " + error.message +
                          " (byte " + std::to_string(error.position) + ")";
            return;
        }
    }

    field.hasDomain = !field.domainText.empty();
    if (field.hasDomain) {
        CompileError error;
        if (!CompileDomain(field.domainText, field.domain, error)) {
            field.error = "Domain: " + error.message + " (byte " + std::to_string(error.position) + ")";
            return;
        }
    }

    field.valid = true;
}

bool EvaluateCustomDomain(const CustomFieldInfo &field, const PointFrame &frame, bool *nonFinite) {
    if (nonFinite) {
        *nonFinite = false;
    }
    if (!field.valid) {
        return false;
    }
    if (!field.hasDomain) {
        return true;
    }
    return EvaluateDomain(field.domain, frame.variables, nonFinite);
}

VxVector EvaluateCustomComponents(const CustomFieldInfo &field, const PointFrame &frame,
                                  bool *nonFinite) {
    if (nonFinite) {
        *nonFinite = false;
    }
    if (!field.valid) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    double values[3] = {0.0, 0.0, 0.0};
    for (int component = 0; component < 3; ++component) {
        const double value = EvaluateProgram(field.components[component], frame.variables);
        if (!IsFinite(value)) {
            values[component] = 0.0;
            if (nonFinite) {
                *nonFinite = true;
            }
        } else {
            values[component] = value;
        }
    }

    // Rectangular rows are literal world acceleration components.
    if (field.mode != 1) {
        return VxVector(static_cast<float>(values[0]), static_cast<float>(values[1]),
                        static_cast<float>(values[2]));
    }

    double world[3] = {0.0, 0.0, 0.0};
    for (int axis = 0; axis < 3; ++axis) {
        world[axis] = values[0] * frame.radial[axis] + values[1] * frame.thetaAxis[axis] +
                      values[2] * frame.phiAxis[axis];
    }
    if (!IsFinite(world[0]) || !IsFinite(world[1]) || !IsFinite(world[2])) {
        if (nonFinite) {
            *nonFinite = true;
        }
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    return VxVector(static_cast<float>(world[0]), static_cast<float>(world[1]),
                    static_cast<float>(world[2]));
}

} // namespace fg::core::expr
