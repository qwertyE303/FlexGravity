#include "core/MarchingTetrahedra.h"

#include <cmath>

namespace fg::core {
namespace {

// Kuhn split: six tetrahedra that all share the corner 0 -> corner 7 diagonal.
// Every cell uses the same split, so two neighbouring cells interpolate along
// identical edges on their shared face and the resulting surface is watertight.
constexpr int kTetCorners[6][4] = {
        {0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};

// Corner index -> (x, y, z) offset inside the cell.
constexpr int kCornerX[8] = {0, 1, 0, 1, 0, 1, 0, 1};
constexpr int kCornerY[8] = {0, 0, 1, 1, 0, 0, 1, 1};
constexpr int kCornerZ[8] = {0, 0, 0, 0, 1, 1, 1, 1};

// Point where the segment a-b crosses the iso value.
VxVector Interpolate(const VxVector &a, const VxVector &b, float valueA, float valueB, float iso) {
    const float denominator = valueB - valueA;
    float t = 0.5f;
    if (std::fabs(denominator) > 1.0e-20f) {
        t = (iso - valueA) / denominator;
    }
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return VxVector(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

// Emits the cross-section of one tetrahedron.  `corner` holds the four corner
// indices of the tetrahedron, `position`/`value` the eight corners of the cell.
void EmitTetrahedron(const int *corner, const VxVector *position, const float *value, float iso,
                     std::vector<VxVector> &triangles) {
    int inside[4];
    int outside[4];
    int insideCount = 0;
    int outsideCount = 0;
    for (int slot = 0; slot < 4; ++slot) {
        const int c = corner[slot];
        if (value[c] > iso) {
            inside[insideCount++] = c;
        } else {
            outside[outsideCount++] = c;
        }
    }
    if (insideCount == 0 || insideCount == 4) {
        return;
    }

    if (insideCount == 1 || insideCount == 3) {
        // One vertex on its own side: a single triangle through the three
        // edges that leave it.
        const int single = insideCount == 1 ? inside[0] : outside[0];
        const int *others = insideCount == 1 ? outside : inside;
        const VxVector a = Interpolate(position[single], position[others[0]], value[single],
                                      value[others[0]], iso);
        const VxVector b = Interpolate(position[single], position[others[1]], value[single],
                                      value[others[1]], iso);
        const VxVector c = Interpolate(position[single], position[others[2]], value[single],
                                      value[others[2]], iso);
        triangles.push_back(a);
        triangles.push_back(b);
        triangles.push_back(c);
        return;
    }

    // Two and two: the cross-section is a quad, split into a triangle fan
    // around its first vertex.
    const VxVector a =
            Interpolate(position[inside[0]], position[outside[0]], value[inside[0]],
                        value[outside[0]], iso);
    const VxVector b =
            Interpolate(position[inside[0]], position[outside[1]], value[inside[0]],
                        value[outside[1]], iso);
    const VxVector c =
            Interpolate(position[inside[1]], position[outside[1]], value[inside[1]],
                        value[outside[1]], iso);
    const VxVector d =
            Interpolate(position[inside[1]], position[outside[0]], value[inside[1]],
                        value[outside[0]], iso);
    triangles.push_back(a);
    triangles.push_back(b);
    triangles.push_back(c);
    triangles.push_back(a);
    triangles.push_back(c);
    triangles.push_back(d);
}

} // namespace

void ExtractIsosurface(const float *values, int nx, int ny, int nz, const VxVector &origin,
                       float cellSize, float isoValue, std::vector<VxVector> &triangles) {
    ExtractIsosurfaces(values, nx, ny, nz, origin, cellSize, &isoValue, 1, triangles);
}

void ExtractIsosurfaces(const float *values, int nx, int ny, int nz, const VxVector &origin,
                        float cellSize, const float *isoValues, int isoCount,
                        std::vector<VxVector> &triangles) {
    if (!values || !isoValues || isoCount <= 0 || nx < 2 || ny < 2 || nz < 2) {
        return;
    }

    const int strideY = nx;
    const int strideZ = nx * ny;

    for (int z = 0; z + 1 < nz; ++z) {
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                const int base = x + strideY * y + strideZ * z;

                // The eight corners are fetched and placed once per cell, not
                // once per level: this is the bulk of the multi-level saving.
                VxVector position[8];
                float value[8];
                float cellLow = 0.0f;
                float cellHigh = 0.0f;
                for (int corner = 0; corner < 8; ++corner) {
                    const int index = base + kCornerX[corner] + strideY * kCornerY[corner] +
                                      strideZ * kCornerZ[corner];
                    value[corner] = values[index];
                    position[corner] =
                            VxVector(origin.x + static_cast<float>(x + kCornerX[corner]) * cellSize,
                                     origin.y + static_cast<float>(y + kCornerY[corner]) * cellSize,
                                     origin.z + static_cast<float>(z + kCornerZ[corner]) * cellSize);
                    if (corner == 0) {
                        cellLow = value[0];
                        cellHigh = value[0];
                    } else if (value[corner] < cellLow) {
                        cellLow = value[corner];
                    } else if (value[corner] > cellHigh) {
                        cellHigh = value[corner];
                    }
                }

                // No level passes through this cell at all: skip the six
                // tetrahedra.  With a band of levels this discards most of the
                // lattice, which is what makes the bounded band cheap.
                bool cellCrossed = false;
                for (int level = 0; level < isoCount; ++level) {
                    const float iso = isoValues[level];
                    if (iso >= cellLow && iso < cellHigh) {
                        cellCrossed = true;
                        break;
                    }
                }
                if (!cellCrossed) {
                    continue;
                }

                for (int tet = 0; tet < 6; ++tet) {
                    const int *corner = kTetCorners[tet];
                    float tetLow = value[corner[0]];
                    float tetHigh = tetLow;
                    for (int slot = 1; slot < 4; ++slot) {
                        const float v = value[corner[slot]];
                        if (v < tetLow) {
                            tetLow = v;
                        } else if (v > tetHigh) {
                            tetHigh = v;
                        }
                    }
                    if (!(tetHigh > tetLow)) {
                        continue;
                    }

                    for (int level = 0; level < isoCount; ++level) {
                        const float iso = isoValues[level];
                        // Same predicate as the single-level version: a corner
                        // counts as inside when value > iso, so a crossing
                        // exists exactly for iso in [tetLow, tetHigh).
                        if (!(iso >= tetLow) || !(iso < tetHigh)) {
                            continue;
                        }
                        EmitTetrahedron(corner, position, value, iso, triangles);
                    }
                }
            }
        }
    }
}

} // namespace fg::core
