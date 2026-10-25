#pragma once

#include <VxVector.h>

namespace fg::core {

// IPion's fixed PSI step is 1/66 s.  SetPhysicsForce's raw value is the
// per-step impulse, so raw = mass * acceleration / 66.
inline constexpr float kForceScale = 66.0f;

// The physics simulation advances at this fixed rate, so evaluating the
// fields faster than this cannot change the result of a physics step.
inline constexpr float kPhysicsStepSeconds = 1.0f / 66.0f;

// Only guards divisions; it is not a clamp and not a gameplay parameter.
inline constexpr float kTinyDistance = 1.0e-6f;

inline constexpr int kMaxCurvePoints = 4096;

// A body is judged by one point: its origin, i.e. the translation of its world
// matrix.  That is also the point SetPhysicsForce pulls on, so "inside the
// region" and "receives the force" can never disagree.
inline constexpr int kBodySampleCount = 1;

// Squared acceleration below which a body is considered force-free.
inline constexpr float kNoAccelerationSquared = 1.0e-8f;

// A slanted ray avoids most axis-aligned edge hits on box-shaped regions.
inline constexpr float kRegionRayDirectionX = 0.37139067f;
inline constexpr float kRegionRayDirectionY = 0.61898445f;
inline constexpr float kRegionRayDirectionZ = 0.69179575f;

// Ballance's world is Y-up and the engine pulls every dynamic object with
// (0, -20, 0).  The Virtools 2.1 SDK shipped with the mod SDK does not expose
// CKPhysicsManager, so this constant is the only way to know it.  Fields are
// additive on top of it, which is also what the debug overlay displays.
inline const VxVector kVanillaGravity(0.0f, -20.0f, 0.0f);

// Debug overlay: the gravity arrow keeps a fixed world-space length so its
// direction stays readable at any field magnitude.  This is a drawing constant,
// not a field parameter.  The length is measured from the ball centre, and the
// part inside the ball (radius 2 m) is not drawn.
inline constexpr float kGravityArrowLength = 7.0f;

// Arrow proportions, in world units (metres).  Every one of them is multiplied
// by the local pixels-per-metre factor when drawing, so the arrow shrinks with
// distance exactly like a solid object would.
inline constexpr float kGravityArrowShaftBase = 0.100f;   // shaft radius at the tail
inline constexpr float kGravityArrowShaftTip = 0.065f;    // shaft radius where the head starts
inline constexpr float kGravityArrowHeadLength = 0.850f;  // head length
inline constexpr float kGravityArrowHeadRadius = 0.300f;  // head half width

// Wood and stone balls are 4 m across (radius 2) and that radius is baked into
// the level script; the engine reports it through OnPhysicalize.  This is only
// the fallback for a ball that was never physicalized.
inline constexpr float kDefaultBallRadius = 2.0f;

// --- Field line overlay -----------------------------------------------------
// Density is "lines per 100 world units at the vanilla gravity magnitude", the
// same scale the settings panel documents: spacing = (100 / density) *
// sqrt(20 / |g|).  At density 100 and |g| = 20 there is one line per metre.
inline constexpr float kFieldLineDensityScale = 100.0f;
inline constexpr float kFieldLineReferenceGravity = 20.0f;

// Traced with RK4 on the normalised field.  The step starts at kFieldLineStep
// and adapts to the curvature: it grows while the direction turns less than
// ~0.5 degrees per step (a uniform field becomes a handful of long segments)
// and halves again above ~5 degrees, so the drawn polyline keeps its accuracy
// without paying a fine step in flat regions.
inline constexpr float kFieldLineStep = 0.5f;
inline constexpr float kFieldLineMaxStep = 8.0f;
inline constexpr float kFieldLineMinTurnCosine = 0.99996192f; // cos(0.5 deg)
inline constexpr float kFieldLineMaxTurnCosine = 0.99619470f; // cos(5 deg)
inline constexpr int kFieldLineMaxSteps = 400;
inline constexpr float kFieldLineMinMagnitude = 1.0e-4f;

// A traced line ends slightly outside the drawn sphere, so the visible part is
// never cut off by the tracer itself.
inline constexpr float kFieldLineTraceMargin = 1.15f;

// The seed lattice is re-anchored to the ball when it moved more than this
// fraction of the spacing, or when the local gravity direction turned more than
// about 5 degrees.  Between re-anchors the cached lines are reused as they are.
inline constexpr float kFieldLineReanchorFraction = 0.30f;
inline constexpr float kFieldLineReanchorCosine = 0.996f;

// A cached line serves a seed if the two are closer than this fraction of the
// spacing.  That is what makes movement incremental instead of a full rebuild.
inline constexpr float kFieldLineMatchFraction = 0.50f;

// Lines that no longer reach the view sphere are dropped.
inline constexpr float kFieldLinePruneSeconds = 0.25f;

// Tracing happens on the render thread, so it is spread over frames: each frame
// traces at most this many milliseconds worth of new lines.  The first fill of a
// large view sphere therefore takes a few frames instead of one long stall.
inline constexpr float kFieldLineFrameBudgetSeconds = 0.004f;

// Drawing: decimate by screen distance (dense near the camera, coarse far away)
// and stop once this many points are queued.  One point costs four ImGui
// vertices (an anti-aliased thick stroke writes four vertices per point), and
// the surfaces are drawn into the same list, so the two budgets together stay
// far below the 65535 vertices a 16-bit index buffer can address.
inline constexpr float kFieldLineScreenStep = 3.0f;
inline constexpr int kFieldLinePointBudget = 6000;
inline constexpr int kFieldLineVerticesPerPoint = 4;
inline constexpr int kFieldLineVertexBudget = kFieldLinePointBudget * kFieldLineVerticesPerPoint;
// A single polyline never exceeds this many points; a longer run is emitted as
// several polylines so one line cannot crowd out everything else.
inline constexpr int kFieldLinePathLimit = 512;

// Safety valve, not a user-facing setting: with a very high density and a very
// large view distance the seed lattice alone would be tens of thousands of
// lines.  Past this many seeds the spacing is widened until it fits, which
// lowers the density instead of stalling the game.
inline constexpr int kFieldLineSeedLimit = 6000;

// --- Equipotential overlay ---------------------------------------------------
// Surfaces of constant potential are extracted from a regular lattice that
// spans the view sphere.  Sixteen cells across the diameter is 12.5 m at the
// default 100 m view distance: coarse enough that one fill costs about five
// thousand field evaluations, fine enough that the shape of the surfaces is
// readable.
inline constexpr int kEquipotentialCells = 16;
inline constexpr float kEquipotentialSampleBudgetSeconds = 0.004f;

// Only the levels within this many spacing steps of the ball's own potential are
// drawn.  Seven translucent sheets are already a wash over the whole screen, and
// a potential that grows with height produces one sheet every metre; the band
// keeps the spacing the author asked for while bounding what is on screen.
inline constexpr int kEquipotentialBandHalf = 3;

// Drawing budget.  A band of levels still produces thousands of triangles; past
// this many the ones farthest from the camera are dropped (the nearest are kept,
// then painted far to near) so the overlay stays a diagnostic instead of a
// second renderer.  Three ImGui vertices per triangle: the filled surfaces are
// drawn with the anti-aliased fill disabled.
inline constexpr int kEquipotentialTriangleBudget = 6000;
inline constexpr int kEquipotentialVerticesPerTriangle = 3;
inline constexpr int kEquipotentialVertexBudget =
        kEquipotentialTriangleBudget * kEquipotentialVerticesPerTriangle;

// Alpha of one sheet.  At 46/255 seven overlapping sheets were 98% opaque; this
// keeps the stack a tint (about 30%) while a single sheet is still visible.
inline constexpr int kEquipotentialSurfaceAlpha = 12;

// The triangles are painted far to near, so their order only has to follow the
// camera when it actually moved.
inline constexpr float kEquipotentialResortDistance = 2.0f;

// Extraction still stops here: the band is picked at draw time out of whatever
// the lattice produced.
inline constexpr int kEquipotentialMaxTriangles = 12000;
inline constexpr int kEquipotentialMaxLevels = 64;

} // namespace fg::core
