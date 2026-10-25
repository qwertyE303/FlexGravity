#include "mod/FlexGravityMod.h"

#include <chrono>

#include <imgui.h>

#include "core/FieldLines.h"

// Field line cache.
//
// The total field of a level is static: U regions, M/L/S sources and the vanilla
// gravity never move, so a traced field line stays valid for the whole level.
// Lines are therefore cached in world space and only added or dropped as the
// ball travels:
//   * the seed lattice is re-anchored to the ball when it moved a fraction of a
//     spacing or the local gravity direction turned noticeably;
//   * a seed that has a cached line within half a spacing reuses it, so normal
//     movement costs a few distance tests, not a single field evaluation;
//   * seeds without a cached line are traced, but only within a per-frame time
//     budget, so the first fill is spread over a handful of frames;
//   * lines that no longer reach the view sphere are dropped.

namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

VxVector FlexGravityMod::EvaluateFieldThunk(void *context, const VxVector &point) {
    return static_cast<FlexGravityMod *>(context)->SampleFieldAtPoint(point);
}

void FlexGravityMod::DropFieldLines() {
    m_FieldLines.clear();
    m_PendingFieldLines.clear();
    m_PendingCursor = 0;
    m_FieldLinesAnchored = false;
    m_FieldLinePruneValid = false;
    m_FieldLineSpacing = 0.0f;
    m_FieldLinePending = 0;
    m_FieldLineDrawn = 0;
    m_FieldLineVertices = 0;
    m_FieldLinePoints = 0;
    m_FieldLineProjected = 0;
    m_LineLocal.clear();
    m_LineScreen.clear();
    m_LineClip.clear();
    m_LineRuns.clear();
    m_LineCacheValid = false;
    ++m_FieldLineVersion;
}

// Rebuilds the flattened, batch-projected draw list.  Runs only when the traced
// geometry or the view changed: the projection is the expensive part, so a still
// camera reuses last frame's pixels and only re-emits the draw commands.
void FlexGravityMod::RebuildFieldLineDrawCache(CKRenderContext *renderContext, CK3dEntity *ball,
                                               const VxVector &camera) {
    m_LineLocal.clear();
    m_LineRuns.clear();
    m_LineScreen.clear();
    m_LineClip.clear();
    m_FieldLineProjected = 0;
    m_LineCacheValid = false;

    if (m_FieldLines.empty()) {
        return;
    }

    const VxVector ballCentre = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const VxVector forward = Subtract(ballCentre, camera);
    const bool haveForward = LengthSquared(forward) > 0.0f;
    const float radiusSquared = m_DebugViewDistance * m_DebugViewDistance;
    const VxMatrix inverseBall = ball->GetInverseWorldMatrix();

    // Nearest lines first, so the point budget spends itself around the ball.
    m_FieldLineOrder.clear();
    m_FieldLineOrder.reserve(m_FieldLines.size());
    for (size_t index = 0; index < m_FieldLines.size(); ++index) {
        m_FieldLineOrder.emplace_back(LengthSquared(Subtract(m_FieldLines[index]->seed, camera)),
                                      index);
    }
    std::sort(m_FieldLineOrder.begin(), m_FieldLineOrder.end(),
              [](const std::pair<float, size_t> &left, const std::pair<float, size_t> &right) {
                  return left.first < right.first;
              });

    const int pointBudget = core::kFieldLinePointBudget;
    int runBegin = 0;
    int runCount = 0;
    const auto closeRun = [&]() {
        if (runCount >= 2) {
            LineRun run;
            run.begin = runBegin;
            run.count = runCount;
            m_LineRuns.push_back(run);
        }
        runCount = 0;
    };

    bool budgetSpent = false;
    for (const auto &entry : m_FieldLineOrder) {
        if (budgetSpent) {
            break;
        }
        const auto &line = m_FieldLines[entry.second];
        if (line->points.size() < 2) {
            continue;
        }
        for (const VxVector &point : line->points) {
            if (LengthSquared(Subtract(point, camera)) > radiusSquared) {
                closeRun();
                continue;
            }
            if (haveForward && core::Dot(Subtract(point, camera), forward) <= 0.0f) {
                closeRun();
                continue;
            }
            if (static_cast<int>(m_LineLocal.size()) >= pointBudget) {
                closeRun();
                budgetSpent = true;
                break;
            }
            if (runCount == 0) {
                runBegin = static_cast<int>(m_LineLocal.size());
            }
            // The engine projects through the ball's matrix, so world points have
            // to be handed over in the ball's space (the inverse cancels out
            // inside the engine).
            m_LineLocal.push_back(inverseBall * point);
            ++runCount;
        }
        closeRun();
    }

    if (m_LineLocal.empty()) {
        return;
    }

    ProjectBatch(renderContext, ball, m_LineLocal.data(), m_LineLocal.size(), m_LineScreen, m_LineClip);
    if (m_LineScreen.size() != m_LineLocal.size()) {
        m_LineScreen.clear();
        m_LineClip.clear();
        m_LineRuns.clear();
        return;
    }
    m_FieldLineProjected = static_cast<int>(m_LineScreen.size());
    m_LineCacheValid = true;
}

void FlexGravityMod::UpdateFieldLines() {
    if (!m_EnableFieldLines || !m_DebugMode) {
        if (!m_FieldLines.empty()) {
            DropFieldLines();
        }
        return;
    }

    CKRenderContext *renderContext = m_BML ? m_BML->GetRenderContext() : nullptr;
    CK3dEntity *ball = FindActiveBall();
    if (!renderContext || !ball) {
        if (!m_FieldLines.empty()) {
            DropFieldLines();
        }
        return;
    }

    const VxVector centre = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const VxVector gravity = SampleFieldAtPoint(centre);
    const float magnitude = std::sqrt(LengthSquared(gravity));
    VxVector normal(0.0f, 0.0f, 0.0f);
    if (!(magnitude > core::kFieldLineMinMagnitude) || !Normalize(gravity, normal) ||
        !(m_FieldLineDensity > 1.0e-3f) || !(m_DebugViewDistance > 1.0e-3f)) {
        if (!m_FieldLines.empty()) {
            DropFieldLines();
        }
        return;
    }

    // Spacing in world units: one line per metre at density 100 and |g| = 20.
    const float spacing = (core::kFieldLineDensityScale / m_FieldLineDensity) *
                          std::sqrt(core::kFieldLineReferenceGravity / magnitude);
    if (!(spacing > 1.0e-3f)) {
        return;
    }

    // A density edited in the config must take effect immediately: the cached
    // lines were traced on the old lattice, so they are dropped and rebuilt.
    if (m_FieldLinesAnchored && m_FieldLineSpacing > 0.0f &&
        std::fabs(m_FieldLineSpacing - spacing) > 0.01f * m_FieldLineSpacing) {
        DropFieldLines();
    }

    const VxVector camera = CameraPosition(renderContext);
    const float moved = std::sqrt(LengthSquared(Subtract(centre, m_FieldLineOrigin)));
    const bool turned = core::Dot(normal, m_FieldLineNormal) < core::kFieldLineReanchorCosine;
    if (!m_FieldLinesAnchored || turned || moved > core::kFieldLineReanchorFraction * spacing) {
        ReanchorFieldLines(centre, normal, camera, spacing);
    }

    TracePendingFieldLines();

    // Dropping is cheap but not free, so it happens only when the camera moved
    // a noticeable fraction of a cell.
    if (!m_FieldLinePruneValid ||
        LengthSquared(Subtract(camera, m_FieldLinePruneCamera)) >
                (0.20f * spacing) * (0.20f * spacing)) {
        PruneFieldLines(camera);
    }
}

void FlexGravityMod::ReanchorFieldLines(const VxVector &origin, const VxVector &normal,
                                        const VxVector &camera, float spacing) {
    // Orthonormal basis of the seed plane (the plane through the ball centre
    // perpendicular to the local gravity).
    VxVector up(0.0f, 1.0f, 0.0f);
    if (std::fabs(core::Dot(normal, up)) > 0.95f) {
        up = VxVector(1.0f, 0.0f, 0.0f);
    }
    VxVector axisU(1.0f, 0.0f, 0.0f);
    VxVector axisV(0.0f, 0.0f, 1.0f);
    if (!core::Normalize(core::Cross(normal, up), axisU) ||
        !core::Normalize(core::Cross(normal, axisU), axisV)) {
        return;
    }

    // Seeds on the lattice that land inside the view sphere.  A seed slightly
    // outside is still useful (its line may cross the sphere), so the lattice
    // reaches a little past the sphere and only the very ends are unused.
    const float spawnRadius = m_DebugViewDistance * 1.05f;
    const float spawnRadiusSquared = spawnRadius * spawnRadius;

    // A dense setting and a large view distance would ask for tens of thousands
    // of lines.  Widen the spacing until the lattice fits the safety limit;
    // only a pathological setting ever reaches this, and the analytic bound
    // keeps the lattice loops below from growing out of hand.
    const float minimumSpacing =
            std::sqrt(3.14159265f * spawnRadius * spawnRadius / static_cast<float>(core::kFieldLineSeedLimit));
    if (spacing < minimumSpacing) {
        spacing = minimumSpacing;
    }
    const int half = static_cast<int>(std::ceil(m_DebugViewDistance / spacing)) + 1;

    m_FieldLineOrigin = origin;
    m_FieldLineNormal = normal;
    m_FieldLineAxisU = axisU;
    m_FieldLineAxisV = axisV;
    m_FieldLineSpacing = spacing;
    m_FieldLinesAnchored = true;

    const float matchRadius = spacing * core::kFieldLineMatchFraction;
    const float matchRadiusSquared = matchRadius * matchRadius;

    for (int along = -half; along <= half; ++along) {
        for (int across = -half; across <= half; ++across) {
            const VxVector seed = Add(origin, Add(Scale(axisU, static_cast<float>(along) * spacing),
                                                 Scale(axisV, static_cast<float>(across) * spacing)));
            if (LengthSquared(Subtract(seed, camera)) > spawnRadiusSquared) {
                continue;
            }

            bool served = false;
            for (const auto &line : m_FieldLines) {
                if (LengthSquared(Subtract(line->seed, seed)) <= matchRadiusSquared) {
                    served = true;
                    break;
                }
            }
            if (served) {
                continue;
            }

            auto line = std::make_unique<TracedFieldLine>();
            line->seed = seed;
            m_PendingFieldLines.push_back(line.get());
            m_FieldLines.push_back(std::move(line));
        }
    }

    m_FieldLinePending = static_cast<int>(m_PendingFieldLines.size() - m_PendingCursor);
}

void FlexGravityMod::TracePendingFieldLines() {
    if (m_PendingCursor >= m_PendingFieldLines.size()) {
        m_PendingFieldLines.clear();
        m_PendingCursor = 0;
        m_FieldLinePending = 0;
        return;
    }

    CKRenderContext *renderContext = m_BML ? m_BML->GetRenderContext() : nullptr;
    if (!renderContext) {
        return;
    }
    const VxVector camera = CameraPosition(renderContext);

    core::FieldLineOptions options;
    options.step = core::kFieldLineStep;
    options.maxSteps = core::kFieldLineMaxSteps;
    options.minMagnitude = core::kFieldLineMinMagnitude;
    options.boundsCenter = camera;
    options.boundsRadius = m_DebugViewDistance * core::kFieldLineTraceMargin;

    const auto start = Clock::now();
    bool geometryChanged = false;
    while (m_PendingCursor < m_PendingFieldLines.size()) {
        TracedFieldLine *line = m_PendingFieldLines[m_PendingCursor++];
        if (!line) {
            continue;
        }
        if (!core::TraceFieldLine(line->seed, &FlexGravityMod::EvaluateFieldThunk, this, options,
                                  line->points)) {
            // Null field at the seed: nothing to draw, and it must not be
            // offered to the next re-anchor as a cache hit.
            line->empty = true;
        } else if (!line->points.empty()) {
            geometryChanged = true;
        }
        ++m_FieldLineTraced;
        if (MillisecondsSince(start) >= core::kFieldLineFrameBudgetSeconds * 1000.0f) {
            break;
        }
    }
    if (geometryChanged) {
        ++m_FieldLineVersion;
    }

    m_FieldLinePending = static_cast<int>(m_PendingFieldLines.size() - m_PendingCursor);
    m_FieldLineLastTraceMs = static_cast<float>(MillisecondsSince(start));
    if (m_PendingCursor >= m_PendingFieldLines.size()) {
        m_PendingFieldLines.clear();
        m_PendingCursor = 0;
        m_FieldLinePending = 0;
    }
}

void FlexGravityMod::PruneFieldLines(const VxVector &camera) {
    m_FieldLinePruneCamera = camera;
    m_FieldLinePruneValid = true;
    const float radius = m_DebugViewDistance * 1.05f;
    const float radiusSquared = radius * radius;

    size_t kept = 0;
    for (size_t index = 0; index < m_FieldLines.size(); ++index) {
        TracedFieldLine *line = m_FieldLines[index].get();
        bool reachable = false;
        for (const VxVector &point : line->points) {
            if (LengthSquared(Subtract(point, camera)) <= radiusSquared) {
                reachable = true;
                break;
            }
        }
        if (!reachable && !line->empty) {
            ++m_FieldLineDropped;
        }
        if (reachable) {
            m_FieldLines[kept++] = std::move(m_FieldLines[index]);
        }
    }
    if (kept != m_FieldLines.size()) {
        ++m_FieldLineVersion;
    }
    m_FieldLines.resize(kept);

    // Dropping renumbers the vector, so the pending pointers have to be rebuilt
    // from the lines that still have no points.
    if (m_PendingCursor < m_PendingFieldLines.size()) {
        m_PendingFieldLines.clear();
        for (const auto &line : m_FieldLines) {
            if (line->points.empty() && !line->empty) {
                m_PendingFieldLines.push_back(line.get());
            }
        }
        m_PendingCursor = 0;
        m_FieldLinePending = static_cast<int>(m_PendingFieldLines.size());
    }

    if (m_FieldLines.capacity() > 8192) {
        m_FieldLines.shrink_to_fit();
    }
}

void FlexGravityMod::DrawFieldLines(ImDrawList *draw, CKRenderContext *renderContext,
                                    CK3dEntity *ball) {
    m_FieldLineDrawn = 0;
    m_FieldLineVertices = 0;
    m_FieldLinePoints = 0;
    m_FieldLineProjected = 0;

    if (!draw || !ball || !renderContext || m_FieldLines.empty()) {
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

    // Reuse the projection of the previous frame while neither the traced
    // geometry nor the view changed.  Sampling and projecting are the expensive
    // part; emitting the draw commands is not.
    const bool fresh = m_LineCacheValid && m_FieldLineVersion == m_LineCacheVersion &&
                       std::memcmp(&view, &m_LineCacheView, sizeof(VxMatrix)) == 0 &&
                       m_LineCacheBall.x == ballCentre.x && m_LineCacheBall.y == ballCentre.y &&
                       m_LineCacheBall.z == ballCentre.z &&
                       m_LineCacheDisplayWidth == io.DisplaySize.x &&
                       m_LineCacheDisplayHeight == io.DisplaySize.y &&
                       m_LineCacheContextWidth == contextWidth &&
                       m_LineCacheContextHeight == contextHeight;
    if (!fresh) {
        RebuildFieldLineDrawCache(renderContext, ball, camera);
        m_LineCacheView = view;
        m_LineCacheBall = ballCentre;
        m_LineCacheDisplayWidth = io.DisplaySize.x;
        m_LineCacheDisplayHeight = io.DisplaySize.y;
        m_LineCacheContextWidth = contextWidth;
        m_LineCacheContextHeight = contextHeight;
        m_LineCacheVersion = m_FieldLineVersion;
    }
    if (m_LineScreen.empty() || m_LineClip.size() != m_LineScreen.size()) {
        return;
    }

    const float scaleX = io.DisplaySize.x / static_cast<float>(contextWidth);
    const float scaleY = io.DisplaySize.y / static_cast<float>(contextHeight);
    const ImU32 color = IM_COL32(235, 70, 70, 255);
    const float stepSquared = core::kFieldLineScreenStep * core::kFieldLineScreenStep;
    const float margin = 8.0f;

    // Reused across runs and frames: AddPolyline only reads the array.
    static std::vector<ImVec2> path;

    int vertices = 0;
    const auto flush = [&]() {
        if (path.size() >= 2) {
            draw->AddPolyline(path.data(), static_cast<int>(path.size()), color, 1.5f,
                              ImDrawFlags_None);
            vertices += core::kFieldLineVerticesPerPoint * static_cast<int>(path.size());
            ++m_FieldLineDrawn;
        }
        path.clear();
    };

    for (const LineRun &run : m_LineRuns) {
        if (vertices >= core::kFieldLineVertexBudget) {
            break;
        }
        path.clear();
        const int end = run.begin + run.count;
        for (int index = run.begin; index < end; ++index) {
            const size_t slot = static_cast<size_t>(index);
            // Fully outside the near or far plane: this point's projection is
            // meaningless, so the run has to be cut here.
            if (m_LineClip[slot] & (VXCLIP_FRONT | VXCLIP_BACK)) {
                flush();
                continue;
            }
            const ImVec2 current(m_LineScreen[slot].x * scaleX, m_LineScreen[slot].y * scaleY);
            if (!(current.x == current.x) || !(current.y == current.y)) {
                flush();
                continue;
            }
            if (current.x < -margin || current.y < -margin || current.x > io.DisplaySize.x + margin ||
                current.y > io.DisplaySize.y + margin) {
                flush();
                continue;
            }
            if (!path.empty()) {
                const float dx = current.x - path.back().x;
                const float dy = current.y - path.back().y;
                if (dx * dx + dy * dy < stepSquared) {
                    continue;
                }
            }
            path.push_back(current);
            ++m_FieldLinePoints;
            if (static_cast<int>(path.size()) >= core::kFieldLinePathLimit) {
                flush();
                // Keep the join: this polyline continues from the same point.
                path.push_back(current);
            }
        }
        flush();
        m_FieldLineVertices = vertices;
    }
    m_FieldLineVertices = vertices;
}
