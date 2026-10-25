#pragma once

#include <VxVector.h>

#include <vector>

namespace fg::core {

// Extracts the iso-surface of a scalar field sampled on a regular lattice.
//
// `values` holds (nx * ny * nz) samples, index = x + nx * (y + ny * z), taken at
// world positions origin + (x, y, z) * cellSize.  The surface is emitted as
// triangles (three consecutive vertices each) appended to `triangles`; every
// vertex is linearly interpolated along a lattice edge, so the surface is the
// piecewise-linear interpolation of the field, exactly like marching cubes but
// with a tetrahedral split (no case table, no ambiguous faces).
//
// The same lattice split is used for every cell, so neighbouring cells always
// agree on the shared diagonal and the surface has no cracks.
void ExtractIsosurface(const float *values, int nx, int ny, int nz, const VxVector &origin,
                       float cellSize, float isoValue, std::vector<VxVector> &triangles);

// Multi-level variant: emits the surface of every value in `isoValues` in one
// pass over the lattice.  A cell's eight corner positions are computed once for
// all of its levels and a tetrahedron is only classified for the levels that
// actually cross it, so this costs a fraction of calling ExtractIsosurface once
// per level.  The triangles are identical; only their order differs, because
// they are emitted cell by cell instead of level by level.
void ExtractIsosurfaces(const float *values, int nx, int ny, int nz, const VxVector &origin,
                        float cellSize, const float *isoValues, int isoCount,
                        std::vector<VxVector> &triangles);

} // namespace fg::core
