#pragma once

#include <cmath>

#include <VxMatrix.h>
#include <VxVector.h>

namespace fg::core {

// VxVector already provides most operators, but the field code is written in
// terms of these helpers so the maths can be unit tested without the engine.
inline float Dot(const VxVector &a, const VxVector &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline VxVector Cross(const VxVector &a, const VxVector &b) {
    return VxVector(a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x);
}

inline VxVector Subtract(const VxVector &a, const VxVector &b) {
    return VxVector(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline VxVector Add(const VxVector &a, const VxVector &b) {
    return VxVector(a.x + b.x, a.y + b.y, a.z + b.z);
}

inline VxVector Scale(const VxVector &a, float factor) {
    return VxVector(a.x * factor, a.y * factor, a.z * factor);
}

inline float LengthSquared(const VxVector &a) {
    return a.x * a.x + a.y * a.y + a.z * a.z;
}

inline bool Normalize(const VxVector &value, VxVector &out) {
    const float lengthSquared = LengthSquared(value);
    if (lengthSquared <= 1.0e-12f) {
        out = VxVector(0.0f, 0.0f, 0.0f);
        return false;
    }
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    out = VxVector(value.x * inverse, value.y * inverse, value.z * inverse);
    return true;
}

inline float GetComponent(const VxVector &value, int axis) {
    switch (axis) {
        case 0: return value.x;
        case 1: return value.y;
        default: return value.z;
    }
}

inline void SetComponent(VxVector &value, int axis, float component) {
    switch (axis) {
        case 0: value.x = component; break;
        case 1: value.y = component; break;
        default: value.z = component; break;
    }
}

inline VxVector AxisVector(int axis) {
    switch (axis) {
        case 0: return VxVector(1.0f, 0.0f, 0.0f);
        case 1: return VxVector(0.0f, 1.0f, 0.0f);
        default: return VxVector(0.0f, 0.0f, 1.0f);
    }
}

inline bool BoundsContains(const VxBbox &box, const VxVector &point) {
    if (!box.IsValid()) {
        return false;
    }
    const float epsilon = 1.0e-4f;
    return point.x >= box.Min.x - epsilon && point.x <= box.Max.x + epsilon &&
           point.y >= box.Min.y - epsilon && point.y <= box.Max.y + epsilon &&
           point.z >= box.Min.z - epsilon && point.z <= box.Max.z + epsilon;
}

// Moller-Trumbore, double sided: the ray may hit the triangle from either face.
inline bool RayTriangle(const VxVector &origin, const VxVector &direction,
                        const VxVector &v0, const VxVector &v1, const VxVector &v2) {
    const VxVector edge1 = Subtract(v1, v0);
    const VxVector edge2 = Subtract(v2, v0);
    const VxVector p = Cross(direction, edge2);
    const float determinant = Dot(edge1, p);
    if (std::fabs(determinant) < 1.0e-7f) {
        return false;
    }

    const float inverse = 1.0f / determinant;
    const VxVector t = Subtract(origin, v0);
    const float u = Dot(t, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }

    const VxVector q = Cross(t, edge1);
    const float v = Dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }

    const float distance = Dot(edge2, q) * inverse;
    return distance > 1.0e-5f;
}

} // namespace fg::core
