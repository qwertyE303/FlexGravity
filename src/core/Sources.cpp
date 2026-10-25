#include "core/Sources.h"

#include <cmath>

#include "core/Constants.h"
#include "core/Math.h"

namespace fg::core {

VxVector ComputeSegmentAcceleration(const VxVector &a, const VxVector &b,
                                    const VxVector &center, float strength) {
    // Exact field of a finite uniform line segment, normalised so that an
    // infinite line produces strength / d:
    //   g = (lambda/2) * [ -(K1/d) * p_hat + K2 * u ]
    const VxVector direction = Subtract(b, a);
    const float length = std::sqrt(LengthSquared(direction));
    if (length <= kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    const VxVector u = Scale(direction, 1.0f / length);

    const VxVector w = Subtract(center, a);
    const float s = Dot(w, u);
    const VxVector p = Subtract(w, Scale(u, s));
    const float distance = std::sqrt(LengthSquared(p));

    const float d1 = std::sqrt(LengthSquared(w));
    const float d2 = std::sqrt(LengthSquared(Subtract(center, b)));
    if (d1 <= kTinyDistance || d2 <= kTinyDistance) {
        // Standing exactly on an endpoint: the field is undefined.
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float k1 = s / d1 - (s - length) / d2;
    const float k2 = 1.0f / d1 - 1.0f / d2;

    VxVector result = Scale(u, k2);
    if (distance > kTinyDistance) {
        const VxVector perpendicular = Scale(p, 1.0f / distance);
        result = Add(result, Scale(perpendicular, -k1 / distance));
    }
    return Scale(result, 0.5f * strength);
}

VxVector ComputeMonopoleAcceleration(const VxVector &position, float strength,
                                     const VxVector &center) {
    const VxVector r = Subtract(position, center);
    const float distanceSquared = Dot(r, r);
    if (distanceSquared < kTinyDistance * kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float inverse = 1.0f / (distanceSquared * std::sqrt(distanceSquared));
    return Scale(r, strength * inverse);
}

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Signed solid angle of the triangle (a, b, c) seen from p (Van Oosterom &
// Strackee): one atan2 and three square roots, against four atan2 and four
// square roots per corner for the corner-sum form.
float SolidAngle(const VxVector &a, const VxVector &b, const VxVector &c,
                 const VxVector &p) {
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

VxVector ComputePolygonAcceleration(const VxVector *vertices, int count,
                                    const VxVector &normal, const VxVector &center,
                                    float strength) {
    if (!vertices || count < 3) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    const float coupling = strength / kTwoPi;
    VxVector result(0.0f, 0.0f, 0.0f);

    // In-plane part: the boundary integral
    //   E_t = -coupling * closed_integral( n_out / |r - r'| ds )
    // Every edge contributes one asinh difference, which is exactly what the
    // per-axis terms of the old rectangle formula were the four edges of.
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
        const VxVector w = Subtract(center, a);
        const float s = Dot(w, u);
        const VxVector perpendicular = Subtract(w, Scale(u, s));
        const float distance = std::sqrt(LengthSquared(perpendicular));
        if (distance <= kTinyDistance) {
            // Standing on the edge line: the log term diverges.
            continue;
        }

        const float integral = std::asinh((length - s) / distance) + std::asinh(s / distance);
        result = Subtract(result, Scale(outward, coupling * integral));
    }

    // Normal part: the solid angle.  A fan of triangles covers any convex
    // polygon, and the signed angles add up to the polygon's own solid angle.
    float omega = 0.0f;
    for (int index = 1; index + 1 < count; ++index) {
        omega += SolidAngle(vertices[0], vertices[index], vertices[index + 1], center);
    }
    omega = std::fabs(omega);

    const float height = Dot(Subtract(center, vertices[0]), normal);
    if (std::fabs(height) > kTinyDistance) {
        const float towardSheet = height > 0.0f ? -1.0f : 1.0f;
        result = Add(result, Scale(normal, towardSheet * coupling * omega));
    }

    return result;
}

VxVector ComputeSheetPatchesAcceleration(const VxVector *vertices, const int *offsets,
                                         const VxVector *normals, int patchCount,
                                         const VxVector &center, float strength) {
    if (!vertices || !offsets || !normals || patchCount <= 0) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    VxVector total(0.0f, 0.0f, 0.0f);
    for (int patch = 0; patch < patchCount; ++patch) {
        const int begin = offsets[patch];
        const int count = offsets[patch + 1] - begin;
        if (count < 3) {
            continue;
        }
        total = Add(total, ComputePolygonAcceleration(vertices + begin, count,
                                                      normals[patch], center, strength));
    }
    return total;
}

VxVector ComputeSheetAcceleration(const SourceInfo &source, const VxMatrix &world,
                                  const VxVector &center) {
    const VxVector origin = world * source.localCenter;

    VxVector axisA = Subtract(world * Add(source.localCenter, source.localAxisA), origin);
    VxVector axisB = Subtract(world * Add(source.localCenter, source.localAxisB), origin);
    const float scaleA = std::sqrt(LengthSquared(axisA));
    const float scaleB = std::sqrt(LengthSquared(axisB));
    if (scaleA <= kTinyDistance || scaleB <= kTinyDistance) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    axisA = Scale(axisA, 1.0f / scaleA);
    axisB = Scale(axisB, 1.0f / scaleB);

    VxVector normal = Cross(axisA, axisB);
    if (!Normalize(normal, normal)) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    // The rectangle is a parallelogram: half-extents along the two in-plane
    // axes, wound counter-clockwise around the normal.
    const VxVector cornerA = Scale(axisA, source.halfA * scaleA);
    const VxVector cornerB = Scale(axisB, source.halfB * scaleB);
    const VxVector quad[4] = {
            Subtract(Subtract(origin, cornerA), cornerB),
            Subtract(Add(origin, cornerA), cornerB),
            Add(Add(origin, cornerA), cornerB),
            Add(Subtract(origin, cornerA), cornerB)};

    return ComputePolygonAcceleration(quad, 4, normal, center, source.strength);
}

VxVector ComputePolylineAcceleration(const std::vector<VxVector> &points, bool closed,
                                     const VxVector &center, float strength) {
    if (points.size() < 2) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }

    VxVector total(0.0f, 0.0f, 0.0f);
    const size_t count = points.size();
    const size_t segments = closed ? count : count - 1;
    for (size_t index = 0; index < segments; ++index) {
        total = Add(total, ComputeSegmentAcceleration(points[index],
                                                      points[(index + 1) % count],
                                                      center, strength));
    }
    return total;
}

VxVector ComputeSourceAcceleration(const SourceInfo &source, const VxMatrix &world,
                                   const VxVector &center) {
    if (source.kind == SourceInfo::KindLine) {
        if (source.localPoints.size() < 2) {
            return VxVector(0.0f, 0.0f, 0.0f);
        }

        std::vector<VxVector> points;
        points.reserve(source.localPoints.size());
        for (const VxVector &local : source.localPoints) {
            points.push_back(world * local);
        }
        return ComputePolylineAcceleration(points, source.closed, center, source.strength);
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
            return ComputeSheetPatchesAcceleration(vertices.data(), source.sheetOffsets.data(),
                                                   normals.data(), patchCount, center,
                                                   source.strength);
        }
        return ComputeSheetAcceleration(source, world, center);
    }

    return ComputeMonopoleAcceleration(world * VxVector(0.0f, 0.0f, 0.0f),
                                       source.strength, center);
}

float DistanceToPolyline(const std::vector<VxVector> &points, bool closed,
                         const VxVector &center) {
    if (points.size() < 2) {
        return 0.0f;
    }
    const size_t count = points.size();
    const size_t segments = closed ? count : count - 1;
    float best = 1.0e30f;
    for (size_t index = 0; index < segments; ++index) {
        const VxVector &a = points[index];
        const VxVector &b = points[(index + 1) % count];
        const VxVector direction = Subtract(b, a);
        const float lengthSquared = LengthSquared(direction);
        float t = 0.0f;
        if (lengthSquared > kTinyDistance * kTinyDistance) {
            t = Dot(Subtract(center, a), direction) / lengthSquared;
            t = (std::max)(0.0f, (std::min)(1.0f, t));
        }
        const VxVector closest = Add(a, Scale(direction, t));
        const float distance = std::sqrt(LengthSquared(Subtract(center, closest)));
        best = (std::min)(best, distance);
    }
    return best;
}

} // namespace fg::core
