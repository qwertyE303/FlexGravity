#include "mod/FlexGravityMod.h"

#include <cstdint>

namespace {

struct EdgeRef {
    int triangle = -1;
    int slot = -1;
};

std::uint64_t EdgeKey(int a, int b) {
    const std::uint32_t low = static_cast<std::uint32_t>((std::min)(a, b));
    const std::uint32_t high = static_cast<std::uint32_t>((std::max)(a, b));
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

// Two coplanar triangles that share an edge merge into one quad when every
// turn around the loop goes the same way around the normal.
bool IsConvexLoop(const VxVector &a, const VxVector &b, const VxVector &c,
                  const VxVector &d, const VxVector &normal) {
    const VxVector loop[4] = {a, b, c, d};
    for (int index = 0; index < 4; ++index) {
        const VxVector first = Subtract(loop[(index + 1) % 4], loop[index]);
        const VxVector second = Subtract(loop[(index + 2) % 4], loop[(index + 1) % 4]);
        if (Dot(Cross(first, second), normal) <= 0.0f) {
            return false;
        }
    }
    return true;
}

VxVector TriangleNormal(const VxVector &a, const VxVector &b, const VxVector &c) {
    VxVector normal(0.0f, 0.0f, 0.0f);
    Normalize(Cross(Subtract(b, a), Subtract(c, a)), normal);
    return normal;
}

// Turns the triangles into convex planar patches.  Adjacent coplanar triangles
// are merged pairwise (a quad mesh arrives triangulated, so this puts the
// rectangle back); a curved surface keeps one patch per triangle.
void BuildSheetPatchesFromTriangles(const std::vector<VxVector> &vertices,
                                    const std::vector<int> &indices,
                                    SourceInfo &source) {
    const int triangleCount = static_cast<int>(indices.size() / 3);
    source.sheetTriangleCount = triangleCount;
    source.sheetVertices.clear();
    source.sheetOffsets.assign(1, 0);
    source.sheetNormals.clear();

    std::unordered_map<std::uint64_t, std::vector<EdgeRef>> edges;
    edges.reserve(static_cast<size_t>(triangleCount) * 3);
    for (int triangle = 0; triangle < triangleCount; ++triangle) {
        for (int slot = 0; slot < 3; ++slot) {
            const int a = indices[static_cast<size_t>(triangle) * 3 + slot];
            const int b = indices[static_cast<size_t>(triangle) * 3 + (slot + 1) % 3];
            edges[EdgeKey(a, b)].push_back(EdgeRef{triangle, slot});
        }
    }

    std::vector<char> consumed(static_cast<size_t>(triangleCount), 0);
    for (int triangle = 0; triangle < triangleCount; ++triangle) {
        if (consumed[static_cast<size_t>(triangle)]) {
            continue;
        }
        consumed[static_cast<size_t>(triangle)] = 1;

        const int base = triangle * 3;
        const VxVector &v0 = vertices[static_cast<size_t>(indices[base])];
        const VxVector &v1 = vertices[static_cast<size_t>(indices[base + 1])];
        const VxVector &v2 = vertices[static_cast<size_t>(indices[base + 2])];
        const VxVector normal = TriangleNormal(v0, v1, v2);

        VxVector patch[4] = {v0, v1, v2, VxVector(0.0f, 0.0f, 0.0f)};
        int patchSize = 3;

        // Pick the neighbour that yields the tightest quad: on a quad mesh that
        // is the triangle across the cell diagonal, i.e. the rectangle itself.
        int bestOther = -1;
        float bestPerimeter = 1.0e30f;
        VxVector bestPatch[4];
        for (int slot = 0; slot < 3; ++slot) {
            const int a = indices[base + slot];
            const int b = indices[base + (slot + 1) % 3];
            const auto it = edges.find(EdgeKey(a, b));
            if (it == edges.end() || it->second.size() != 2) {
                continue;   // boundary or non-manifold edge
            }

            for (const EdgeRef &ref : it->second) {
                if (ref.triangle == triangle || consumed[static_cast<size_t>(ref.triangle)]) {
                    continue;
                }
                const int other = ref.triangle;
                const int otherBase = other * 3;
                // The neighbour has to traverse the shared edge the other way;
                // the same direction means the pair folds over the edge.
                if (indices[otherBase + ref.slot] != b ||
                    indices[otherBase + (ref.slot + 1) % 3] != a) {
                    continue;
                }

                const VxVector &o0 = vertices[static_cast<size_t>(indices[otherBase])];
                const VxVector &o1 = vertices[static_cast<size_t>(indices[otherBase + 1])];
                const VxVector &o2 = vertices[static_cast<size_t>(indices[otherBase + 2])];
                const VxVector otherNormal = TriangleNormal(o0, o1, o2);
                if (Dot(normal, otherNormal) < 0.999f) {
                    continue;   // curved: keep the two triangles apart
                }
                if (std::fabs(Dot(Subtract(o0, v0), normal)) > 1.0e-3f) {
                    continue;   // parallel but not the same plane
                }

                const VxVector &thirdA = vertices[static_cast<size_t>(indices[base + (slot + 2) % 3])];
                const VxVector &thirdB =
                        vertices[static_cast<size_t>(indices[otherBase + (ref.slot + 2) % 3])];
                const VxVector &edgeA = vertices[static_cast<size_t>(a)];
                const VxVector &edgeB = vertices[static_cast<size_t>(b)];
                // Loop around the union: third(A) -> a -> third(B) -> b.
                if (!IsConvexLoop(thirdA, edgeA, thirdB, edgeB, normal)) {
                    continue;
                }

                const float perimeter =
                        std::sqrt(LengthSquared(Subtract(edgeA, thirdA))) +
                        std::sqrt(LengthSquared(Subtract(thirdB, edgeA))) +
                        std::sqrt(LengthSquared(Subtract(edgeB, thirdB))) +
                        std::sqrt(LengthSquared(Subtract(thirdA, edgeB)));
                if (perimeter < bestPerimeter) {
                    bestPerimeter = perimeter;
                    bestOther = other;
                    bestPatch[0] = thirdA;
                    bestPatch[1] = edgeA;
                    bestPatch[2] = thirdB;
                    bestPatch[3] = edgeB;
                }
            }
        }

        if (bestOther >= 0) {
            consumed[static_cast<size_t>(bestOther)] = 1;
            for (int index = 0; index < 4; ++index) {
                patch[index] = bestPatch[index];
            }
            patchSize = 4;
            ++source.sheetMergedCount;
        }

        for (int index = 0; index < patchSize; ++index) {
            source.sheetVertices.push_back(patch[index]);
        }
        source.sheetOffsets.push_back(static_cast<int>(source.sheetVertices.size()));
        source.sheetNormals.push_back(normal);
    }
}

} // namespace

void FlexGravityMod::BuildRegionCache(CK3dEntity *entity, RegionVolume &region, const char *label) {
    region.localVertices.clear();
    region.localIndices.clear();
    region.meshValid = false;
    region.bboxValid = false;
    region.localBounds = VxBbox{};

    if (!entity) {
        Warn(std::string("Region object is missing: ") + (label ? label : "<null>"));
        return;
    }

    VxBbox bbox = entity->GetBoundingBox(TRUE);
    CKMesh *mesh = entity->GetCurrentMesh();
    if (mesh) {
        const int vertexCount = mesh->GetVertexCount();
        const int faceCount = mesh->GetFaceCount();
        if (vertexCount > 0 && faceCount > 0) {
            region.localVertices.reserve(static_cast<size_t>(vertexCount));
            for (int index = 0; index < vertexCount; ++index) {
                VxVector vertex(0.0f, 0.0f, 0.0f);
                mesh->GetVertexPosition(index, &vertex);
                region.localVertices.push_back(vertex);
            }

            region.localIndices.reserve(static_cast<size_t>(faceCount) * 3);
            for (int face = 0; face < faceCount; ++face) {
                int v0 = 0, v1 = 0, v2 = 0;
                mesh->GetFaceVertexIndex(face, v0, v1, v2);
                if (v0 >= 0 && v1 >= 0 && v2 >= 0 &&
                    v0 < vertexCount && v1 < vertexCount && v2 < vertexCount) {
                    region.localIndices.push_back(v0);
                    region.localIndices.push_back(v1);
                    region.localIndices.push_back(v2);
                }
            }

            region.meshValid = !region.localIndices.empty();
            if (bbox.IsValid()) {
                region.localBounds = bbox;
            } else {
                VxVector minimum(1.0e6f, 1.0e6f, 1.0e6f);
                VxVector maximum(-1.0e6f, -1.0e6f, -1.0e6f);
                for (const VxVector &vertex : region.localVertices) {
                    minimum.x = (std::min)(minimum.x, vertex.x);
                    minimum.y = (std::min)(minimum.y, vertex.y);
                    minimum.z = (std::min)(minimum.z, vertex.z);
                    maximum.x = (std::max)(maximum.x, vertex.x);
                    maximum.y = (std::max)(maximum.y, vertex.y);
                    maximum.z = (std::max)(maximum.z, vertex.z);
                }
                region.localBounds.SetCorners(minimum, maximum);
            }
            region.bboxValid = region.localBounds.IsValid();
            BuildRegionBvh(region);

            LogInfo("Region %s: mesh=%d vertices=%d faces=%d bboxValid=%s",
                    label ? label : "<null>", region.meshValid ? 1 : 0,
                    vertexCount, faceCount, BoolText(region.bboxValid).c_str());
            return;
        }
    }

    if (bbox.IsValid()) {
        region.localBounds = bbox;
        region.bboxValid = true;
        LogWarn("Region has no usable mesh; using local bounding box: %s",
                label ? label : "<null>");
    } else {
        Warn(std::string("Region has no usable mesh or bounding box: ") + (label ? label : "<null>"));
    }
}

bool FlexGravityMod::BuildLineGeometry(SourceInfo &source, CK3dEntity *entity) {
    source.localPoints.clear();
    source.controlPointCount = 0;
    source.usedCurve = false;
    source.closed = false;
    source.geometryValid = false;

    if (!entity) {
        return false;
    }

    CKCurve *curve = SafeCurveCast(entity);

    if (curve) {
        const int count = SafeCurvePointCount(curve);
        if (count >= 2 && count <= kMaxCurvePoints) {
            std::vector<VxVector> worldPoints;
            worldPoints.reserve(static_cast<size_t>(count));
            bool ok = true;
            for (int index = 0; index < count; ++index) {
                CKCurvePoint *point = SafeCurveControlPoint(curve, index);
                if (!point) {
                    ok = false;
                    break;
                }
                VxVector world(0.0f, 0.0f, 0.0f);
                if (!SafeWorldPosition(point, world)) {
                    ok = false;
                    break;
                }
                worldPoints.push_back(world);
            }

            if (ok) {
                const VxMatrix &inverse = entity->GetInverseWorldMatrix();
                for (const VxVector &world : worldPoints) {
                    source.localPoints.push_back(inverse * world);
                }
                source.controlPointCount = count;
                source.usedCurve = true;
                source.closed = SafeCurveIsOpen(curve) == 0;
                source.geometryValid = source.localPoints.size() >= 2;
            } else {
                Warn("FG_L curve control point query failed: " + SafeName(entity));
            }
        } else if (count >= 0) {
            Warn("FG_L curve has an unusable control point count: " + SafeName(entity) +
                 " count=" + std::to_string(count));
        } else {
            Warn("FG_L curve probe raised an exception; falling back to bounding box: " +
                 SafeName(entity));
        }
    }

    if (!source.geometryValid) {
        // Fallback: the longest local bounding box axis becomes the segment.
        VxBbox box = entity->GetBoundingBox(TRUE);
        if (!box.IsValid()) {
            Warn("FG_L has no usable curve or bounding box: " + SafeName(entity));
            return false;
        }

        const VxVector size = Subtract(box.Max, box.Min);
        int axis = 0;
        if (GetComponent(size, 1) > GetComponent(size, axis)) {
            axis = 1;
        }
        if (GetComponent(size, 2) > GetComponent(size, axis)) {
            axis = 2;
        }

        VxVector center = Scale(Add(box.Min, box.Max), 0.5f);
        VxVector a = center;
        VxVector b = center;
        SetComponent(a, axis, GetComponent(box.Min, axis));
        SetComponent(b, axis, GetComponent(box.Max, axis));

        source.localPoints.clear();
        source.localPoints.push_back(a);
        source.localPoints.push_back(b);
        source.geometryValid = true;
        LogWarn("FG_L is not a curve; using the longest bounding box axis: %s", SafeName(entity).c_str());
    }

    return source.geometryValid;
}

bool FlexGravityMod::BuildSheetGeometry(SourceInfo &source, CK3dEntity *entity) {
    source.sheetVertices.clear();
    source.sheetOffsets.clear();
    source.sheetNormals.clear();
    source.sheetTriangleCount = 0;
    source.sheetMergedCount = 0;
    source.geometryValid = false;

    if (!entity) {
        return false;
    }

    // The mesh is the sheet: every triangle is a piece of the surface, and a
    // curved surface finally produces a real curved field instead of the flat
    // rectangle the bounding box used to stand in for.
    CKMesh *mesh = entity->GetCurrentMesh();
    if (mesh) {
        const int vertexCount = mesh->GetVertexCount();
        const int faceCount = mesh->GetFaceCount();
        if (vertexCount >= 3 && faceCount > 0) {
            std::vector<VxVector> vertices(static_cast<size_t>(vertexCount));
            for (int index = 0; index < vertexCount; ++index) {
                VxVector vertex(0.0f, 0.0f, 0.0f);
                mesh->GetVertexPosition(index, &vertex);
                vertices[static_cast<size_t>(index)] = vertex;
            }

            std::vector<int> indices;
            indices.reserve(static_cast<size_t>(faceCount) * 3);
            for (int face = 0; face < faceCount; ++face) {
                int v0 = 0, v1 = 0, v2 = 0;
                mesh->GetFaceVertexIndex(face, v0, v1, v2);
                if (v0 < 0 || v1 < 0 || v2 < 0 ||
                    v0 >= vertexCount || v1 >= vertexCount || v2 >= vertexCount) {
                    continue;
                }
                const float areaSquared = LengthSquared(
                        Cross(Subtract(vertices[static_cast<size_t>(v1)],
                                       vertices[static_cast<size_t>(v0)]),
                              Subtract(vertices[static_cast<size_t>(v2)],
                                       vertices[static_cast<size_t>(v0)])));
                if (areaSquared <= 1.0e-10f) {
                    continue;   // degenerate sliver
                }
                indices.push_back(v0);
                indices.push_back(v1);
                indices.push_back(v2);
            }

            if (!indices.empty()) {
                BuildSheetPatchesFromTriangles(vertices, indices, source);
                source.geometryValid = source.SheetPatchCount() > 0;
                if (source.geometryValid) {
                    LogInfo("Sheet %s: vertices=%d faces=%d triangles=%d patches=%d merged=%d",
                            SafeName(entity).c_str(), vertexCount, faceCount,
                            source.sheetTriangleCount, source.SheetPatchCount(),
                            source.sheetMergedCount);
                    if (source.SheetPatchCount() > 400) {
                        LogWarn("Sheet %s uses %d patches; every dynamic object pays for all of them",
                                SafeName(entity).c_str(), source.SheetPatchCount());
                    }
                    return true;
                }
            }
            Warn("FG_S mesh has no usable triangles: " + SafeName(entity));
        }
    }

    // Fallback: no mesh, treat the local bounding box as one rectangle.
    VxBbox box = entity->GetBoundingBox(TRUE);
    if (!box.IsValid()) {
        Warn("FG_S has no usable mesh or bounding box: " + SafeName(entity));
        return false;
    }

    const VxVector size = Subtract(box.Max, box.Min);

    int normalAxis = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (GetComponent(size, axis) < GetComponent(size, normalAxis)) {
            normalAxis = axis;
        }
    }
    const int axisA = (normalAxis + 1) % 3;
    const int axisB = (normalAxis + 2) % 3;

    const float extentA = GetComponent(size, axisA);
    const float extentB = GetComponent(size, axisB);
    const float thickness = GetComponent(size, normalAxis);

    if (extentA <= 1.0e-4f || extentB <= 1.0e-4f) {
        Warn("FG_S bounding box is degenerate: " + SafeName(entity));
        return false;
    }
    if (thickness > 0.5f * (std::max)(extentA, extentB)) {
        Warn("FG_S object does not look flat: " + SafeName(entity) + " size=" + FormatVec(size));
    }

    source.localCenter = Scale(Add(box.Min, box.Max), 0.5f);
    source.localAxisA = AxisVector(axisA);
    source.localAxisB = AxisVector(axisB);
    source.localNormal = AxisVector(normalAxis);
    source.halfA = extentA * 0.5f;
    source.halfB = extentB * 0.5f;
    source.geometryValid = true;

    LogWarn("FG_S has no usable mesh; using its bounding box rectangle: %s",
            SafeName(entity).c_str());
    return true;
}
bool FlexGravityMod::ObjectTouchesRegion(const RegionVolume &region, CK_ID regionObjectId,
                                         const BodySamples &objectSamples) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }

    CK3dEntity *regionEntity = ResolveEntity(regionObjectId);
    if (!regionEntity) {
        return false;
    }

    return core::TouchesRegion(region, regionEntity->GetInverseWorldMatrix(), objectSamples);
}

bool FlexGravityMod::ObjectTouchesRegion(const RegionVolume &region, const VxMatrix &inverseWorld,
                                         const BodySamples &objectSamples) {
    if (!region.meshValid && !region.bboxValid) {
        return false;
    }
    return core::TouchesRegion(region, inverseWorld, objectSamples);
}

