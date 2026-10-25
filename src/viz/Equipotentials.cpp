#include "mod/FlexGravityMod.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "core/Constants.h"
#include "core/MarchingTetrahedra.h"

// Equipotential overlay: surfaces of constant potential, extracted from a
// regular lattice that spans the view sphere.
//
// The potential is static, so the lattice only depends on where the camera is:
// it is snapped to its own world grid, which means rolling around inside one
// cell reuses every sample.  Crossing a cell boundary only samples the slab that
// is actually new (the shifted lattice keeps the rest), and the fill is spread
// over frames with a small budget; extraction and drawing then only run for what
// the band of levels around the ball actually covers.
//
// Only levels close to the ball's own potential are drawn.  A potential that
// grows with height has one surface every metre, and a camera sitting inside two
// hundred of them sees a single flat wash of colour; the band keeps the spacing
// the author asked for and bounds both the overdraw and the vertex count.

namespace {

using Clock = std::chrono::steady_clock;

// Iso values form a fixed global grid, k * levelStep.  At the reference gravity
// (20 m/s^2) and density 100 two neighbouring surfaces are one metre apart,
// which is exactly the spacing rule the field lines use; because the grid is
// absolute, surfaces do not move when the ball travels.
float LevelStepFor(float density) {
    const float safe = density > 1.0e-3f ? density : 1.0e-3f;
    return fg::core::kFieldLineReferenceGravity * (fg::core::kFieldLineDensityScale / safe);
}

// Half-transparent yellow: the surfaces are context, not the thing being read.
// The alpha is per sheet; the band above keeps at most seven of them stacked.
constexpr ImU32 kSurfaceColor = IM_COL32(250, 210, 90, fg::core::kEquipotentialSurfaceAlpha);

} // namespace

void FlexGravityMod::DropEquipotentials() {
    m_Equipotentials = EquipotentialCache();
    m_EquipotentialSamples = 0;
    m_EquipotentialTriangles = 0;
    m_EquipotentialLevels = 0;
    m_EqDraw.clear();
    m_EqLocal.clear();
    m_EqScreen.clear();
    m_EqClip.clear();
    m_EqProjected = 0;
    m_EqCacheValid = false;
    m_EqSortValid = false;
    ++m_EqVersion;
}

void FlexGravityMod::UpdateEquipotentials(CK3dEntity *ball) {
    if (!m_EnableEquipotentials || m_TearingDown || !m_BML || !ball) {
        DropEquipotentials();
        return;
    }
    CKRenderContext *renderContext = m_BML->GetRenderContext();
    if (!renderContext) {
        return;
    }

    const VxVector camera = CameraPosition(renderContext);
    const float extent = m_DebugViewDistance > 1.0f ? m_DebugViewDistance : 1.0f;
    const int cells = core::kEquipotentialCells;
    const float cellSize = 2.0f * extent / static_cast<float>(cells);

    // Snap the lattice corner to the world grid: as long as the camera stays
    // within the same cell the lattice (and every sample) is unchanged.
    const VxVector origin(std::floor((camera.x - extent) / cellSize) * cellSize,
                          std::floor((camera.y - extent) / cellSize) * cellSize,
                          std::floor((camera.z - extent) / cellSize) * cellSize);

    EquipotentialCache &cache = m_Equipotentials;
    const int points = cells + 1;
    const size_t total = static_cast<size_t>(points) * points * points;

    const auto resetLattice = [&](const VxVector &newOrigin) {
        cache.origin = newOrigin;
        cache.cellSize = cellSize;
        cache.cells = cells;
        cache.values.assign(total, 0.0f);
        cache.validSample.assign(total, 0);
        cache.filled = 0;
        cache.cursor = 0;
        cache.extracted = false;
        cache.bandFirst = 0;
        cache.bandLast = -1;
        cache.triangles.clear();
        cache.depths.clear();
        cache.order.clear();
        cache.levels = 0;
    };

    if (cache.values.size() != total || cache.cells != cells || cache.cellSize != cellSize ||
        cache.validSample.size() != total) {
        resetLattice(origin);
    } else {
        // The snapped origin only ever moves in whole cells, so the previous
        // samples can be reused: shift them in place (through a scratch copy)
        // and let the fill evaluate just the new slab.
        //
        // `shift` is how many cells the old origin sits above the new one, so
        // the sample that now lives at index p is the one that used to live at
        // p - shift (both are the same world position).
        const int shiftX = static_cast<int>(std::lround((cache.origin.x - origin.x) / cellSize));
        const int shiftY = static_cast<int>(std::lround((cache.origin.y - origin.y) / cellSize));
        const int shiftZ = static_cast<int>(std::lround((cache.origin.z - origin.z) / cellSize));
        if (std::abs(shiftX) > 1 || std::abs(shiftY) > 1 || std::abs(shiftZ) > 1) {
            resetLattice(origin);
        } else if (shiftX != 0 || shiftY != 0 || shiftZ != 0) {
            std::vector<float> shifted(total, 0.0f);
            std::vector<char> shiftedValid(total, 0);
            size_t valid = 0;
            for (int z = 0; z < points; ++z) {
                const int sourceZ = z - shiftZ;
                for (int y = 0; y < points; ++y) {
                    const int sourceY = y - shiftY;
                    for (int x = 0; x < points; ++x) {
                        const int sourceX = x - shiftX;
                        const size_t destination =
                                static_cast<size_t>(x) + static_cast<size_t>(points) *
                                        (static_cast<size_t>(y) + static_cast<size_t>(points) * z);
                        if (sourceX < 0 || sourceX >= points || sourceY < 0 || sourceY >= points ||
                            sourceZ < 0 || sourceZ >= points) {
                            continue;
                        }
                        const size_t source = static_cast<size_t>(sourceX) +
                                              static_cast<size_t>(points) *
                                                      (static_cast<size_t>(sourceY) +
                                                       static_cast<size_t>(points) * sourceZ);
                        shifted[destination] = cache.values[source];
                        shiftedValid[destination] = cache.validSample[source];
                        valid += shiftedValid[destination] ? 1 : 0;
                    }
                }
            }
            cache.values.swap(shifted);
            cache.validSample.swap(shiftedValid);
            cache.origin = origin;
            cache.filled = valid;
            cache.cursor = 0;
            // The samples are still there, but they describe a different
            // lattice: the triangles have to be rebuilt (cheap, and it happens
            // in this same call because every sample is already valid).
            cache.extracted = false;
        }
    }

    if (cache.filled < total) {
        const auto start = Clock::now();
        size_t sampled = 0;
        while (cache.cursor < total) {
            const size_t index = cache.cursor++;
            if (cache.validSample[index]) {
                continue;
            }
            const int x = static_cast<int>(index % static_cast<size_t>(points));
            const int y =
                    static_cast<int>((index / static_cast<size_t>(points)) % static_cast<size_t>(points));
            const int z = static_cast<int>(index / (static_cast<size_t>(points) * points));
            cache.values[index] = PotentialAtPoint(
                    VxVector(origin.x + static_cast<float>(x) * cellSize,
                             origin.y + static_cast<float>(y) * cellSize,
                             origin.z + static_cast<float>(z) * cellSize));
            cache.validSample[index] = 1;
            ++cache.filled;
            if ((++sampled & 0x3F) == 0) {
                const float elapsed = std::chrono::duration<float>(Clock::now() - start).count();
                if (elapsed >= core::kEquipotentialSampleBudgetSeconds) {
                    break;
                }
            }
        }
        m_EquipotentialSamples = static_cast<int>(cache.filled);
    }

    // Band of levels around the ball, in whole level steps so that walking does
    // not change which surfaces exist until a step boundary is crossed.
    const float step = LevelStepFor(m_EquipotentialDensity);
    const VxVector ballCentre = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const float ballPotential = PotentialAtPoint(ballCentre);
    const int ballLevel =
            step > 0.0f ? static_cast<int>(std::floor(ballPotential / step)) : 0;
    const int bandFirst = ballLevel - core::kEquipotentialBandHalf;
    const int bandLast = ballLevel + core::kEquipotentialBandHalf;

    if (cache.filled >= total &&
        (!cache.extracted || cache.bandFirst != bandFirst || cache.bandLast != bandLast ||
         cache.levelStep != step)) {
        cache.bandFirst = bandFirst;
        cache.bandLast = bandLast;
        ExtractEquipotentials();
    }
}

void FlexGravityMod::ExtractEquipotentials() {
    EquipotentialCache &cache = m_Equipotentials;
    const auto start = Clock::now();
    const int points = cache.cells + 1;
    if (points < 2 || cache.values.size() != static_cast<size_t>(points) * points * points) {
        return;
    }

    cache.triangles.clear();
    cache.depths.clear();
    cache.order.clear();
    cache.levels = 0;

    float lowest = cache.values[0];
    float highest = cache.values[0];
    for (float value : cache.values) {
        lowest = (std::min)(lowest, value);
        highest = (std::max)(highest, value);
    }

    const float step = LevelStepFor(m_EquipotentialDensity);
    if (!(step > 0.0f) || !(highest > lowest)) {
        cache.extracted = true;
        cache.levelStep = step;
        m_EqCacheValid = false;
        ++m_EqVersion;
        return;
    }
    cache.levelStep = step;

    // Only the band around the ball, clamped to the levels the lattice actually
    // contains.  The absolute grid is kept, so the spacing between two surfaces
    // is exactly what the density says.
    const int lowestLevel = static_cast<int>(std::ceil(lowest / step));
    const int highestLevel = static_cast<int>(std::floor(highest / step));
    int first = (std::max)(cache.bandFirst, lowestLevel);
    int last = (std::min)(cache.bandLast, highestLevel);
    if (last - first + 1 > core::kEquipotentialMaxLevels) {
        last = first + core::kEquipotentialMaxLevels - 1;
    }

    std::vector<float> levels;
    for (int level = first; level <= last; ++level) {
        const float iso = static_cast<float>(level) * step;
        if (!(iso > lowest) || !(iso < highest)) {
            continue;
        }
        levels.push_back(iso);
    }

    if (!levels.empty()) {
        core::ExtractIsosurfaces(cache.values.data(), points, points, points, cache.origin,
                                 cache.cellSize, levels.data(), static_cast<int>(levels.size()),
                                 cache.triangles);
        cache.levels = static_cast<int>(levels.size());
    }
    // Defensive cap: the band is bounded, but a tiny cell size could still ask
    // for more triangles than the draw list should ever see.
    if (cache.triangles.size() / 3 > static_cast<size_t>(core::kEquipotentialMaxTriangles)) {
        cache.triangles.resize(static_cast<size_t>(core::kEquipotentialMaxTriangles) * 3);
    }

    const VxVector camera = CameraPosition(m_BML ? m_BML->GetRenderContext() : nullptr);
    SortEquipotentialTriangles(camera);

    cache.extracted = true;
    m_EquipotentialTriangles = static_cast<int>(cache.triangles.size() / 3);
    m_EquipotentialLevels = cache.levels;
    cache.lastBuildMs = std::chrono::duration<float>(Clock::now() - start).count() * 1000.0f;
    m_EqCacheValid = false;
    ++m_EqVersion;
}

// Painter's algorithm: translucent surfaces have no depth test in a 2D draw
// list, so they are blended from the farthest triangle to the nearest.
void FlexGravityMod::SortEquipotentialTriangles(const VxVector &camera) {
    EquipotentialCache &cache = m_Equipotentials;
    const size_t triangleCount = cache.triangles.size() / 3;
    cache.depths.resize(triangleCount);
    cache.order.resize(triangleCount);
    for (size_t index = 0; index < triangleCount; ++index) {
        const VxVector &a = cache.triangles[index * 3];
        const VxVector &b = cache.triangles[index * 3 + 1];
        const VxVector &c = cache.triangles[index * 3 + 2];
        const VxVector centre((a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f,
                              (a.z + b.z + c.z) / 3.0f);
        cache.depths[index] = LengthSquared(Subtract(centre, camera));
        cache.order[index] = static_cast<unsigned int>(index);
    }
    std::sort(cache.order.begin(), cache.order.end(),
              [&cache](unsigned int left, unsigned int right) {
                  return cache.depths[left] > cache.depths[right];
              });
    m_EqSortCamera = camera;
    m_EqSortValid = true;
}

// Selects the triangles to draw (the nearest ones, then reversed into painter
// order), flattens them into the ball's space and projects the whole array in
// one engine call.
void FlexGravityMod::RebuildEquipotentialDrawCache(CKRenderContext *renderContext,
                                                    CK3dEntity *ball) {
    m_EqDraw.clear();
    m_EqLocal.clear();
    m_EqScreen.clear();
    m_EqClip.clear();
    m_EqProjected = 0;
    m_EqCacheValid = false;

    const EquipotentialCache &cache = m_Equipotentials;
    if (cache.triangles.empty()) {
        return;
    }

    const size_t limit = static_cast<size_t>(core::kEquipotentialTriangleBudget);
    for (size_t index = cache.order.size(); index-- > 0 && m_EqDraw.size() < limit;) {
        m_EqDraw.push_back(cache.order[index]);
    }
    std::reverse(m_EqDraw.begin(), m_EqDraw.end());

    const VxMatrix inverseBall = ball->GetInverseWorldMatrix();
    m_EqLocal.resize(m_EqDraw.size() * 3);
    for (size_t index = 0; index < m_EqDraw.size(); ++index) {
        const size_t base = static_cast<size_t>(m_EqDraw[index]) * 3;
        for (size_t corner = 0; corner < 3; ++corner) {
            m_EqLocal[index * 3 + corner] = inverseBall * cache.triangles[base + corner];
        }
    }

    ProjectBatch(renderContext, ball, m_EqLocal.data(), m_EqLocal.size(), m_EqScreen, m_EqClip);
    if (m_EqScreen.size() != m_EqLocal.size() || m_EqClip.size() != m_EqLocal.size()) {
        m_EqDraw.clear();
        m_EqLocal.clear();
        m_EqScreen.clear();
        m_EqClip.clear();
        return;
    }
    m_EqProjected = static_cast<int>(m_EqScreen.size());
    m_EqCacheValid = true;
}

void FlexGravityMod::DrawEquipotentials(ImDrawList *draw, CKRenderContext *renderContext,
                                        CK3dEntity *ball) {
    m_EquipotentialTriangles = 0;
    const EquipotentialCache &cache = m_Equipotentials;
    // The triangles of the previous lattice are kept while a new one is being
    // filled, so crossing a cell boundary does not blank the overlay for a frame.
    if (!draw || !renderContext || !ball || cache.triangles.empty()) {
        return;
    }

    const ImGuiIO &io = ImGui::GetIO();
    if (!(io.DisplaySize.x > 0.0f) || !(io.DisplaySize.y > 0.0f)) {
        return;
    }
    const int contextWidth = renderContext->GetWidth();
    const int contextHeight = renderContext->GetHeight();
    if (contextWidth <= 0 || contextHeight <= 0) {
        return;
    }

    const VxVector camera = CameraPosition(renderContext);
    const VxVector ballCentre = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const VxMatrix view = CameraWorldMatrix(renderContext);

    const bool fresh = m_EqCacheValid && m_EqVersion == m_EqCacheVersion &&
                       std::memcmp(&view, &m_EqCacheView, sizeof(VxMatrix)) == 0 &&
                       m_EqCacheBall.x == ballCentre.x && m_EqCacheBall.y == ballCentre.y &&
                       m_EqCacheBall.z == ballCentre.z &&
                       m_EqCacheDisplayWidth == io.DisplaySize.x &&
                       m_EqCacheDisplayHeight == io.DisplaySize.y &&
                       m_EqCacheContextWidth == contextWidth &&
                       m_EqCacheContextHeight == contextHeight;
    if (!fresh) {
        // The blend order only has to follow a camera that actually moved, and
        // re-sorting thousands of triangles every frame is not free.
        if (!m_EqSortValid ||
            LengthSquared(Subtract(camera, m_EqSortCamera)) >
                    core::kEquipotentialResortDistance * core::kEquipotentialResortDistance) {
            SortEquipotentialTriangles(camera);
        }
        RebuildEquipotentialDrawCache(renderContext, ball);
        m_EqCacheView = view;
        m_EqCacheBall = ballCentre;
        m_EqCacheDisplayWidth = io.DisplaySize.x;
        m_EqCacheDisplayHeight = io.DisplaySize.y;
        m_EqCacheContextWidth = contextWidth;
        m_EqCacheContextHeight = contextHeight;
        m_EqCacheVersion = m_EqVersion;
    }
    if (m_EqScreen.empty() || m_EqClip.size() != m_EqScreen.size()) {
        return;
    }

    const float scaleX = io.DisplaySize.x / static_cast<float>(contextWidth);
    const float scaleY = io.DisplaySize.y / static_cast<float>(contextHeight);
    const float margin = 32.0f;

    // A 12/255 fill needs no anti-aliased fringe: the fringe would be invisible
    // and it costs twice the vertices and seven times the indices.
    const ImDrawListFlags savedFlags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;

    int drawn = 0;
    int vertices = 0;
    for (size_t index = 0; index < m_EqDraw.size(); ++index) {
        const size_t base = index * 3;
        // All three corners outside the same frustum plane: the triangle cannot
        // appear on screen.
        if ((m_EqClip[base] & m_EqClip[base + 1] & m_EqClip[base + 2]) & VXCLIP_ALL) {
            continue;
        }
        const ImVec2 a(m_EqScreen[base].x * scaleX, m_EqScreen[base].y * scaleY);
        const ImVec2 b(m_EqScreen[base + 1].x * scaleX, m_EqScreen[base + 1].y * scaleY);
        const ImVec2 c(m_EqScreen[base + 2].x * scaleX, m_EqScreen[base + 2].y * scaleY);

        // Whole triangle off-screen: skip it before touching the draw list.
        if ((a.x < -margin && b.x < -margin && c.x < -margin) ||
            (a.x > io.DisplaySize.x + margin && b.x > io.DisplaySize.x + margin &&
             c.x > io.DisplaySize.x + margin) ||
            (a.y < -margin && b.y < -margin && c.y < -margin) ||
            (a.y > io.DisplaySize.y + margin && b.y > io.DisplaySize.y + margin &&
             c.y > io.DisplaySize.y + margin)) {
            continue;
        }

        draw->AddTriangleFilled(a, b, c, kSurfaceColor);
        ++drawn;
        vertices += core::kEquipotentialVerticesPerTriangle;
        if (vertices >= core::kEquipotentialVertexBudget) {
            break;
        }
    }

    draw->Flags = savedFlags;
    m_EquipotentialTriangles = drawn;
}
