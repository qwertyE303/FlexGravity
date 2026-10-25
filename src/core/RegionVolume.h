#pragma once

#include <vector>

#include <VxMatrix.h>
#include <VxVector.h>

#include "core/BodySamples.h"

namespace fg::core {

// Bounding-volume hierarchy over the region mesh.  It only prunes triangles
// the ray cannot hit; the parity test itself is unchanged, so the answer is
// identical to a brute-force scan.
struct BvhNode {
    VxBbox bounds;
    int left = -1;
    int right = -1;
    int start = 0;
    int count = 0; // > 0 means a leaf holding triangles [start, start + count)
};

// A FG_U / FG_R region: the inside of the object's own closed mesh, expressed
// in the object's local space.  When no usable mesh exists the local bounding
// box is used instead.
struct RegionVolume {
    VxBbox localBounds;
    std::vector<VxVector> localVertices;
    std::vector<int> localIndices;
    bool meshValid = false;
    bool bboxValid = false;

    std::vector<BvhNode> bvhNodes;
    std::vector<int> bvhTriangles;
};

// Builds the acceleration structure; call once after the mesh is filled.
// Without it ContainsLocalPoint falls back to the brute-force scan.
void BuildRegionBvh(RegionVolume &region);

// `localPoint` must already be expressed in the region object's local space.
bool ContainsLocalPoint(const RegionVolume &region, const VxVector &localPoint);

// Any sample inside the region means the whole body is affected.
bool TouchesRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                   const BodySamples &samples);

} // namespace fg::core
