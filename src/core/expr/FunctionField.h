#pragma once

// One compiled FG_Custom row: three component programs plus a Domain, in world
// space.  Engine-free, so the whole row can be compiled and evaluated in tests.

#include <string>

#include <VxVector.h>

#include "core/expr/Parser.h"

namespace fg::core::expr {

// Everything a row needs to know about the evaluated point.  The six variables
// and the spherical frame are computed once per point and shared by every row,
// because the transcendental part is per point, not per row.
struct PointFrame {
    double variables[kVariableCount];
    double radial[3];    // e_r
    double thetaAxis[3]; // e_theta: +x towards +z around +y
    double phiAxis[3];   // e_phi
};

// y is up and the world is left handed (as in Virtools), so:
//   r     = |p|
//   theta = atan2(z, x) wrapped into [0, 2pi)   (from +x towards +z)
//   phi   = asin(y / r)                         in [-pi/2, pi/2]
PointFrame MakePointFrame(const VxVector &worldPoint);

struct CustomFieldInfo {
    std::string name;    // raw bytes from the DataArray (ANSI/GBK); logs only
    int mode = 0;        // 0 = rectangular, 1 = spherical
    std::string text[6]; // X, Y, Z, R, Theta, Phi exactly as authored
    std::string domainText;

    Program components[3]; // the three components of the active group
    Domain domain;
    bool valid = false;
    bool hasDomain = false;
    std::string error; // first compile problem, already formatted for the log
};

// Compiles the active group and the Domain.  An empty component compiles to the
// constant 0 (the caller warns about it); any real syntax error clears `valid`.
void CompileCustomField(CustomFieldInfo &field);

bool EvaluateCustomDomain(const CustomFieldInfo &field, const PointFrame &frame, bool *nonFinite);

// Acceleration of one row at the point.  A component that evaluates to NaN/Inf
// is treated as 0 and reported through `nonFinite` (the specification's rule);
// the spherical basis is applied after that, so a NaN can never leak into the
// force.
VxVector EvaluateCustomComponents(const CustomFieldInfo &field, const PointFrame &frame,
                                  bool *nonFinite);

} // namespace fg::core::expr
