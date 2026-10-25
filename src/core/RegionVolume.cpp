#include "core/RegionVolume.h"

#include <algorithm>
#include <cmath>

#include "core/Constants.h"
#include "core/Math.h"

namespace fg::core {
namespace {

// Small enough to keep the tree tight, large enough that a leaf scan is cheap.
constexpr int kBvhLeafSize = 6;

// Node boxes are inflated so a grazing triangle can never be pruned away by
// floating-point error; the parity count must match a brute-force scan.
constexpr float kBvhBoundsEpsilon = 1.0e-3f;

VxBbox TriangleBounds(const RegionVolume &region, int triangle) {
    const size_t base = static_cast<size_t>(triangle) * 3;
    const VxVector &a = region.localVertices[static_cast<size_t>(region.localIndices[base])];
    const VxVector &b = region.localVertices[static_cast<size_t>(region.localIndices[base + 1])];
    const VxVector &c = region.localVertices[static_cast<size_t>(region.localIndices[base + 2])];

    VxBbox box;
    box.Min = VxVector((std::min)(a.x, (std::min)(b.x, c.x)),
                       (std::min)(a.y, (std::min)(b.y, c.y)),
                       (std::min)(a.z, (std::min)(b.z, c.z)));
    box.Max = VxVector((std::max)(a.x, (std::max)(b.x, c.x)),
                       (std::max)(a.y, (std::max)(b.y, c.y)),
                       (std::max)(a.z, (std::max)(b.z, c.z)));
    return box;
}

VxVector TriangleCentroid(const RegionVolume &region, int triangle) {
    const size_t base = static_cast<size_t>(triangle) * 3;
    const VxVector &a = region.localVertices[static_cast<size_t>(region.localIndices[base])];
    const VxVector &b = region.localVertices[static_cast<size_t>(region.localIndices[base + 1])];
    const VxVector &c = region.localVertices[static_cast<size_t>(region.localIndices[base + 2])];
    return VxVector((a.x + b.x + c.x) / 3.0f,
                    (a.y + b.y + c.y) / 3.0f,
                    (a.z + b.z + c.z) / 3.0f);
}

void Inflate(VxBbox &box) {
    box.Min.x -= kBvhBoundsEpsilon;
    box.Min.y -= kBvhBoundsEpsilon;
    box.Min.z -= kBvhBoundsEpsilon;
    box.Max.x += kBvhBoundsEpsilon;
    box.Max.y += kBvhBoundsEpsilon;
    box.Max.z += kBvhBoundsEpsilon;
}

int BuildNode(RegionVolume &region, int begin, int count, std::vector<BvhNode> &nodes) {
    const int index = static_cast<int>(nodes.size());
    nodes.emplace_back();

    VxBbox bounds;
    for (int i = 0; i < count; ++i) {
        bounds.Merge(TriangleBounds(region, region.bvhTriangles[static_cast<size_t>(begin + i)]));
    }
    Inflate(bounds);
    nodes[static_cast<size_t>(index)].bounds = bounds;

    if (count <= kBvhLeafSize) {
        nodes[static_cast<size_t>(index)].start = begin;
        nodes[static_cast<size_t>(index)].count = count;
        return index;
    }

    const VxVector extent = Subtract(bounds.Max, bounds.Min);
    int axis = 0;
    if (extent.y > extent.x) {
        axis = 1;
    }
    if (extent.z > GetComponent(extent, axis)) {
        axis = 2;
    }

    const int mid = count / 2;
    std::nth_element(region.bvhTriangles.begin() + begin,
                     region.bvhTriangles.begin() + begin + mid,
                     region.bvhTriangles.begin() + begin + count,
                     [&region, axis](int lhs, int rhs) {
                         return GetComponent(TriangleCentroid(region, lhs), axis) <
                                GetComponent(TriangleCentroid(region, rhs), axis);
                     });

    const int left = BuildNode(region, begin, mid, nodes);
    const int right = BuildNode(region, begin + mid, count - mid, nodes);
    nodes[static_cast<size_t>(index)].left = left;
    nodes[static_cast<size_t>(index)].right = right;
    nodes[static_cast<size_t>(index)].count = 0;
    return index;
}

bool RayHitsBox(const VxBbox &box, const VxVector &origin, const VxVector &inverseDirection) {
    float tMin = -1.0e30f;
    float tMax = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        const float direction = GetComponent(inverseDirection, axis);
        const float entry = (GetComponent(box.Min, axis) - GetComponent(origin, axis)) * direction;
        const float exit = (GetComponent(box.Max, axis) - GetComponent(origin, axis)) * direction;
        tMin = (std::max)(tMin, (std::min)(entry, exit));
        tMax = (std::min)(tMax, (std::max)(entry, exit));
    }
    return tMax >= tMin && tMax >= 0.0f;
}

int CountCrossingsBruteForce(const RegionVolume &region, const VxVector &localPoint,
                             const VxVector &direction) {
    int crossings = 0;
    for (size_t index = 0; index + 2 < region.localIndices.size(); index += 3) {
        const VxVector &v0 = region.localVertices[static_cast<size_t>(region.localIndices[index])];
        const VxVector &v1 = region.localVertices[static_cast<size_t>(region.localIndices[index + 1])];
        const VxVector &v2 = region.localVertices[static_cast<size_t>(region.localIndices[index + 2])];
        if (RayTriangle(localPoint, direction, v0, v1, v2)) {
            ++crossings;
        }
    }
    return crossings;
}

} // namespace

void BuildRegionBvh(RegionVolume &region) {
    region.bvhNodes.clear();
    region.bvhTriangles.clear();
    if (!region.meshValid || region.localIndices.size() < 3) {
        return;
    }

    const size_t triangles = region.localIndices.size() / 3;
    region.bvhTriangles.resize(triangles);
    for (size_t i = 0; i < triangles; ++i) {
        region.bvhTriangles[i] = static_cast<int>(i);
    }

    region.bvhNodes.reserve(triangles * 2);
    BuildNode(region, 0, static_cast<int>(triangles), region.bvhNodes);
}

bool ContainsLocalPoint(const RegionVolume &region, const VxVector &localPoint) {
    if (!BoundsContains(region.localBounds, localPoint)) {
        return false;
    }

    if (!region.meshValid) {
        return region.bboxValid;
    }

    // A slanted ray avoids most axis-aligned edge hits on box-shaped regions.
    const VxVector direction(kRegionRayDirectionX, kRegionRayDirectionY, kRegionRayDirectionZ);

    if (region.bvhNodes.empty()) {
        return (CountCrossingsBruteForce(region, localPoint, direction) & 1) != 0;
    }

    const VxVector inverseDirection(1.0f / direction.x,
                                    1.0f / direction.y,
                                    1.0f / direction.z);

    int crossings = 0;
    int stack[64];
    int depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const BvhNode &node = region.bvhNodes[static_cast<size_t>(stack[--depth])];
        if (!RayHitsBox(node.bounds, localPoint, inverseDirection)) {
            continue;
        }

        if (node.count > 0) {
            for (int i = 0; i < node.count; ++i) {
                const int triangle = region.bvhTriangles[static_cast<size_t>(node.start + i)];
                const size_t base = static_cast<size_t>(triangle) * 3;
                const VxVector &v0 = region.localVertices[static_cast<size_t>(region.localIndices[base])];
                const VxVector &v1 = region.localVertices[static_cast<size_t>(region.localIndices[base + 1])];
                const VxVector &v2 = region.localVertices[static_cast<size_t>(region.localIndices[base + 2])];
                if (RayTriangle(localPoint, direction, v0, v1, v2)) {
                    ++crossings;
                }
            }
        } else {
            stack[depth++] = node.left;
            stack[depth++] = node.right;
        }
    }
    return (crossings & 1) != 0;
}

bool TouchesRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                   const BodySamples &samples) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }

    for (int index = 0; index < samples.count; ++index) {
        const VxVector localPoint = inverseWorld * samples.points[index];
        if (ContainsLocalPoint(region, localPoint)) {
            return true;
        }
    }
    return false;
}

} // namespace fg::core
