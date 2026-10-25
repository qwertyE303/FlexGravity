#include "core/Potential.h"

#include <cmath>

#include "core/Constants.h"
#include "core/Math.h"

namespace fg::core {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// The potential of a line segment is a pair of asinh terms:
//   Phi = -(lambda / 2) * [ asinh((L - s) / d) + asinh(s / d) ]
// which differentiates to exactly ComputeSegmentAcceleration.  The arguments are
// clamped away from zero so a point on the axis yields a large finite value
// instead of an infinity.
float SegmentIntegral(float length, float s, float distance) {
    const float safe = (std::max)(distance, kTinyDistance);
    return std::asinh((length - s) / safe) + std::asinh(s / safe);
}

// Same Van Oosterom & Strackee form as the acceleration uses, so the height term
// of the sheet potential is derived from exactly the solid angle the field does.
float SolidAngle(const VxVector &a, const VxVector &b, const VxVector &c, const VxVector &p) {
    const VxVector va = Subtract(a, p);
    const VxVector vb = Subtract(b, p);
    const VxVector vc = Subtract(c, p);
    const float la = std::sqrt(LengthSquared(va));
    const float lb = std::sqrt(LengthSquared(vb));
    const float lc = std::sqrt(LengthSquared(vc));
    const float numerator = Dot(va, Cross(vb, vc));
    const float denominator = la * lb * lc +
                              Dot(va, vb) * lc + Dot(vb, vc) * la + Dot(vc, va) * lb;
    return 2.0f * std::atan2(numerator, denominator);
}

} // namespace

float ComputeSegmentPotential(const VxVector &a, const VxVector &b, const VxVector &center,
                              float strength) {
    const VxVector direction = Subtract(b, a);
    const float length = std::sqrt(LengthSquared(direction));
    if (length <= kTinyDistance) {
        return 0.0f;
    }
    const VxVector u = Scale(direction, 1.0f / length);
    const VxVector w = Subtract(center, a);
    const float s = Dot(w, u);
    const VxVector p = Subtract(w, Scale(u, s));
    const float distance = std::sqrt(LengthSquared(p));
    return -0.5f * strength * SegmentIntegral(length, s, distance);
}

float ComputeMonopolePotential(const VxVector &position, float strength, const VxVector &center) {
    const VxVector r = Subtract(position, center);
    const float distanceSquared = Dot(r, r);
    if (distanceSquared < kTinyDistance * kTinyDistance) {
        return 0.0f;
    }
    return strength / std::sqrt(distanceSquared);
}

float ComputePolygonPotential(const VxVector *vertices, int count, const VxVector &normal,
                              const VxVector &center, float strength) {
    if (!vertices || count < 3) {
        return 0.0f;
    }

    // The surface integral of 1/r over a planar polygon reduces to a boundary
    // integral plus the height term of the solid angle:
    //   I = 2 * sum_edges (rho / 2) * ( asinh((s + L) / D) - asinh(s / D) )
    //       - |h| * |Omega|
    // with rho the signed in-plane distance from the point to the edge line, s
    // the projection of the edge start onto it and D = sqrt(rho^2 + h^2) --
    // i.e. the edge sum below is half of the boundary integral, and the second
    // term is h^2 * integral(dS / R^3) written as the solid angle.
    const float height = Dot(Subtract(center, vertices[0]), normal);
    const float heightSquared = height * height;

    float edgeSum = 0.0f;
    for (int index = 0; index < count; ++index) {
        const VxVector &a = vertices[index];
        const VxVector &b = vertices[(index + 1) % count];
        const VxVector edge = Subtract(b, a);
        const float length = std::sqrt(LengthSquared(edge));
        if (length <= kTinyDistance) {
            continue;
        }
        const VxVector u = Scale(edge, 1.0f / length);
        const VxVector outward = Cross(u, normal);
        const VxVector w = Subtract(a, center);
        const float s = Dot(w, u);
        const float rho = Dot(w, outward);
        const float diagonal = std::sqrt(rho * rho + heightSquared);
        if (diagonal <= kTinyDistance) {
            continue;
        }
        edgeSum += 0.5f * rho *
                   (std::asinh((s + length) / diagonal) - std::asinh(s / diagonal));
    }

    float omega = 0.0f;
    for (int index = 1; index + 1 < count; ++index) {
        omega += SolidAngle(vertices[0], vertices[index], vertices[index + 1], center);
    }

    const float integral = 2.0f * edgeSum - std::fabs(height) * std::fabs(omega);
    return -(strength / kTwoPi) * integral;
}

float ComputeSheetPatchesPotential(const VxVector *vertices, const int *offsets,
                                   const VxVector *normals, int patchCount, const VxVector &center,
                                   float strength) {
    if (!vertices || !offsets || !normals || patchCount <= 0) {
        return 0.0f;
    }

    float total = 0.0f;
    for (int patch = 0; patch < patchCount; ++patch) {
        const int begin = offsets[patch];
        const int count = offsets[patch + 1] - begin;
        if (count < 3) {
            continue;
        }
        total += ComputePolygonPotential(vertices + begin, count, normals[patch], center, strength);
    }
    return total;
}

float ComputePolylinePotential(const std::vector<VxVector> &points, bool closed,
                               const VxVector &center, float strength) {
    if (points.size() < 2) {
        return 0.0f;
    }

    float total = 0.0f;
    const size_t count = points.size();
    const size_t segments = closed ? count : count - 1;
    for (size_t index = 0; index < segments; ++index) {
        total += ComputeSegmentPotential(points[index], points[(index + 1) % count], center, strength);
    }
    return total;
}

float ComputeSourcePotential(const SourceInfo &source, const VxMatrix &world,
                             const VxVector &center) {
    if (source.kind == SourceInfo::KindLine) {
        if (source.localPoints.size() < 2) {
            return 0.0f;
        }
        std::vector<VxVector> points;
        points.reserve(source.localPoints.size());
        for (const VxVector &local : source.localPoints) {
            points.push_back(world * local);
        }
        return ComputePolylinePotential(points, source.closed, center, source.strength);
    }

    if (source.kind == SourceInfo::KindSheet) {
        const int patchCount = source.SheetPatchCount();
        if (patchCount > 0) {
            std::vector<VxVector> vertices;
            vertices.reserve(source.sheetVertices.size());
            for (const VxVector &local : source.sheetVertices) {
                vertices.push_back(world * local);
            }

            std::vector<VxVector> normals(static_cast<size_t>(patchCount));
            for (int patch = 0; patch < patchCount; ++patch) {
                const int begin = source.sheetOffsets[patch];
                const int count = source.sheetOffsets[patch + 1] - begin;
                VxVector normal(0.0f, 0.0f, 0.0f);
                if (count >= 3) {
                    Normalize(Cross(Subtract(vertices[begin + 1], vertices[begin]),
                                    Subtract(vertices[begin + 2], vertices[begin])),
                              normal);
                }
                normals[static_cast<size_t>(patch)] = normal;
            }
            return ComputeSheetPatchesPotential(vertices.data(), source.sheetOffsets.data(),
                                                normals.data(), patchCount, center, source.strength);
        }

        // Fallback rectangle, built exactly like ComputeSheetAcceleration does.
        const VxVector origin = world * source.localCenter;
        VxVector axisA = Subtract(world * Add(source.localCenter, source.localAxisA), origin);
        VxVector axisB = Subtract(world * Add(source.localCenter, source.localAxisB), origin);
        const float scaleA = std::sqrt(LengthSquared(axisA));
        const float scaleB = std::sqrt(LengthSquared(axisB));
        if (scaleA <= kTinyDistance || scaleB <= kTinyDistance) {
            return 0.0f;
        }
        axisA = Scale(axisA, 1.0f / scaleA);
        axisB = Scale(axisB, 1.0f / scaleB);
        VxVector normal = Cross(axisA, axisB);
        if (!Normalize(normal, normal)) {
            return 0.0f;
        }
        const VxVector cornerA = Scale(axisA, source.halfA * scaleA);
        const VxVector cornerB = Scale(axisB, source.halfB * scaleB);
        const VxVector quad[4] = {
                Subtract(Subtract(origin, cornerA), cornerB),
                Subtract(Add(origin, cornerA), cornerB),
                Add(Add(origin, cornerA), cornerB),
                Add(Subtract(origin, cornerA), cornerB)};
        return ComputePolygonPotential(quad, 4, normal, center, source.strength);
    }

    return ComputeMonopolePotential(world * VxVector(0.0f, 0.0f, 0.0f), source.strength, center);
}

} // namespace fg::core
