#include "mod/FlexGravityMod.h"

#include <BML/Bui.h>

#include <imgui.h>

// Debug overlay, drawn from OnRender: that is the only place where BML+ lets a
// mod touch ImGui, and BML+ guarantees a frame is active there.  Everything is
// diagnostic-only; with the master switch off OnRender is one comparison.
//
// BML+ owns the ImGui context, so every ImGui call below happens inside a
// Bui::ImGuiContextScope.  Without it the current context in this DLL is
// whatever was left over, and anything drawn lands in a draw list nobody
// submits - the overlay then silently shows nothing.

namespace {

constexpr ImU32 kArrowColor = IM_COL32(80, 160, 255, 255);
constexpr ImU32 kArrowEdge = IM_COL32(8, 18, 36, 225);
constexpr ImU32 kStatusColor = IM_COL32(120, 190, 255, 255);
constexpr ImU32 kShadowColor = IM_COL32(0, 0, 0, 190);

// Outline thickness added around every polygon, in pixels.
constexpr float kOutline = 1.3f;

// Occlusion sampling: the arrow is split into this many slices and every slice
// boundary is tested against the ball, then the hidden/visible transitions are
// bisected.  Thirty-two slices over 5 m is 16 cm, and bisection takes the
// boundary to well under a millimetre.
constexpr int kOcclusionSlices = 32;
constexpr int kBisectionSteps = 12;

struct VisibleSpan {
    float from = 0.0f;
    float to = 0.0f;
};

void AddShadowedText(ImDrawList *draw, const ImVec2 &position, const char *text) {
    draw->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f), kShadowColor, text);
    draw->AddText(position, kStatusColor, text);
}

} // namespace

void FlexGravityMod::OnRender(CK_RENDER_FLAGS flags) {
    (void) flags;
    // DebugMode is the master switch: with it off nothing is drawn, whatever
    // the individual overlay switches say.
    if (!m_DebugMode || m_TearingDown || !m_BML) {
        return;
    }
    if (!m_ShowGVector && !m_EnableFieldLines && !m_EnableEquipotentials) {
        return;
    }
    if (!m_BML->IsIngame()) {
        return;
    }
    // While the game is paused its own menu is on screen.  BML submits the whole
    // ImGui draw data in OnPostSpriteRender, i.e. after the game's sprites, so
    // nothing drawn here can ever go *below* that menu; hiding the overlay while
    // paused is what keeps it from covering the menu instead.
    if (m_BML->IsPaused()) {
        return;
    }
    if (!Bui::GetImGuiContext()) {
        return;
    }
    Bui::ImGuiContextScope contextScope;
    DrawOverlay();
}

void FlexGravityMod::DrawOverlay() {
    ++m_OverlayFrames;
    m_OverlayHadFrame = true;

    ImGuiIO &io = ImGui::GetIO();
    m_OverlayDisplayWidth = io.DisplaySize.x;
    m_OverlayDisplayHeight = io.DisplaySize.y;
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        return;
    }

    CKRenderContext *renderContext = m_BML->GetRenderContext();
    if (!renderContext) {
        return;
    }
    m_OverlayContextWidth = renderContext->GetWidth();
    m_OverlayContextHeight = renderContext->GetHeight();
    if (m_OverlayContextWidth <= 0 || m_OverlayContextHeight <= 0) {
        return;
    }

    // The foreground list is the one BML definitely submits for mod drawing.
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    const int verticesBefore = draw->VtxBuffer.Size;
    if (m_EnableEquipotentials) {
        CK3dEntity *ball = FindActiveBall();
        if (ball) {
            // Surfaces first: they are the context the lines and the arrow are
            // read against.
            UpdateEquipotentials(ball);
            DrawEquipotentials(draw, renderContext, ball);
        }
    }
    if (m_EnableFieldLines) {
        CK3dEntity *ball = FindActiveBall();
        if (ball) {
            // Field lines go first: they are the background, the arrow is the
            // thing that must stay readable on top of them.
            UpdateFieldLines();
            DrawFieldLines(draw, renderContext, ball);
        }
    }
    // The arrow has exactly one switch.  It used to be drawn whenever the field
    // lines were on, "as the reference the lines are read against", which made
    // turning ShowGVector off look broken while the lines were enabled.
    if (m_ShowGVector) {
        DrawGravityArrow(draw, renderContext);
    }
    // Exactly what the two overlays added to the draw list.  Everything else in
    // this function is the arrow, which is a handful of quads.
    m_OverlayVertices = draw->VtxBuffer.Size - verticesBefore;

    // Periodic self-check: the batch projection must land on the same pixels as
    // the single-point call, otherwise the overlays would drift against the game
    // view.  Sampled every sixty frames so it costs nothing measurable.
    m_OverlayVtxOffsetSupported =
            (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0;
    if (m_OverlayFrames % 60 == 1) {
        CK3dEntity *ball = FindActiveBall();
        if (ball) {
            const VxVector local[3] = {VxVector(0.0f, 0.0f, 0.0f), VxVector(3.0f, 0.0f, 0.0f),
                                       VxVector(0.0f, -4.0f, 2.0f)};
            std::vector<VxVector4> batch;
            std::vector<unsigned int> clip;
            ProjectBatch(renderContext, ball, local, 3, batch, clip);
            float worst = 0.0f;
            for (size_t index = 0; index < 3 && index < batch.size(); ++index) {
                VxVector single;
                if (!ProjectToScreen(renderContext, local[index], ball, single)) {
                    continue;
                }
                worst = (std::max)(worst, (std::max)(std::fabs(single.x - batch[index].x),
                                                    std::fabs(single.y - batch[index].y)));
            }
            m_OverlayProjectionDelta = worst;
        }
    }
}

// World position of the camera.  The viewpoint entity is the only reliable
// source: the render context's world matrix is not the camera.
VxVector FlexGravityMod::CameraPosition(CKRenderContext *renderContext) const {
    if (!renderContext) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    CK3dEntity *viewpoint = renderContext->GetViewpoint();
    if (!viewpoint) {
        return VxVector(0.0f, 0.0f, 0.0f);
    }
    return viewpoint->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
}

// The viewpoint's world matrix is the camera transform, and comparing it is how
// the overlay caches decide whether last frame's projected pixels are still
// valid.  Cheap, exact, and the only thing it is used for.
VxMatrix FlexGravityMod::CameraWorldMatrix(CKRenderContext *renderContext) const {
    if (!renderContext) {
        return VxMatrix{};
    }
    CK3dEntity *viewpoint = renderContext->GetViewpoint();
    if (!viewpoint) {
        return VxMatrix{};
    }
    return viewpoint->GetWorldMatrix();
}

// Radius of the ball in world units: the collision sphere the level script
// hands to Physicalize, which for the 4 m balls is exactly the visual radius.
// The bounding box is only a fallback, then the known default.
float FlexGravityMod::BallRadiusFor(CK3dEntity *ball) {
    if (!ball) {
        return core::kDefaultBallRadius;
    }
    const auto cached = m_BallRadii.find(ball->GetID());
    if (cached != m_BallRadii.end() && cached->second > 0.0f) {
        return cached->second;
    }
    const VxBbox &box = ball->GetBoundingBox();
    if (box.IsValid()) {
        const VxVector half = box.GetHalfSize();
        const float smallest = (std::min)((std::min)(half.x, half.y), half.z);
        if (smallest > 0.05f) {
            return smallest;
        }
    }
    return core::kDefaultBallRadius;
}

// Blue arrow from the ball's centre along the total gravity.  It starts at the
// ball surface and every part of it that the ball hides from the camera is not
// drawn, so looking straight down leaves (almost) nothing visible.  Every
// dimension is a world size multiplied by the local pixels-per-metre factor, so
// the arrow shrinks with distance exactly like a solid object would.
void FlexGravityMod::DrawGravityArrow(ImDrawList *draw, CKRenderContext *renderContext) {
    CK3dEntity *ball = FindActiveBall();
    if (!ball) {
        ++m_OverlayNoBall;
        DrawOverlayStatus(draw, renderContext);
        return;
    }

    const VxVector centre = ball->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
    const VxVector acceleration = SampleFieldAtPoint(centre);
    m_OverlayBall = centre;
    m_OverlayGravity = acceleration;

    VxVector direction(0.0f, 0.0f, 0.0f);
    if (!Normalize(acceleration, direction)) {
        ++m_OverlayNoProjection;
        DrawOverlayStatus(draw, renderContext);
        return;
    }
    const float magnitude = std::sqrt(LengthSquared(acceleration));

    const float radius = BallRadiusFor(ball);
    m_OverlayBallRadius = radius;
    const float tailT = radius;
    const float tipT = core::kGravityArrowLength;
    const float visibleLength = tipT - tailT;
    if (visibleLength <= 0.1f) {
        ++m_OverlayNoProjection;
        return;
    }

    // Is the ball between the camera and the point at distance t from the ball
    // centre?  That is the only honest occlusion an overlay without a depth
    // buffer can do, and it is exactly a ray/sphere test.
    VxVector cameraPosition(0.0f, 0.0f, 0.0f);
    bool occlusionValid = false;
    if (CK3dEntity *viewpoint = renderContext->GetViewpoint()) {
        cameraPosition = viewpoint->GetWorldMatrix() * VxVector(0.0f, 0.0f, 0.0f);
        const float cameraDistance =
                std::sqrt(LengthSquared(Subtract(centre, cameraPosition)));
        occlusionValid = cameraDistance > radius * 1.001f;
    }
    const VxVector w = Subtract(centre, cameraPosition);
    const float ww = LengthSquared(w);
    const float radiusSquared = radius * radius;
    const auto hiddenAt = [&](float t) {
        const VxVector point = Add(centre, Scale(direction, t));
        const VxVector u = Subtract(point, cameraPosition);
        const float uu = LengthSquared(u);
        if (uu <= 1.0e-6f) {
            return false;
        }
        const float wu = core::Dot(w, u);
        const float discriminant = wu * wu - uu * (ww - radiusSquared);
        if (discriminant <= 0.0f) {
            return false;
        }
        const float length = std::sqrt(uu);
        const float nearHit = (wu - std::sqrt(discriminant)) / length;
        return nearHit > 1.0e-4f && nearHit < length - 1.0e-3f;
    };

    VisibleSpan spans[4];
    int spanCount = 0;
    float visibleMetres = 0.0f;
    if (!occlusionValid) {
        spans[spanCount++] = {tailT, tipT};
        visibleMetres = visibleLength;
    } else {
        float previousT = tailT;
        bool previousHidden = hiddenAt(tailT);
        bool spanOpen = !previousHidden;
        float spanStart = tailT;
        for (int slice = 1; slice <= kOcclusionSlices; ++slice) {
            const float t = tailT + visibleLength * static_cast<float>(slice) / kOcclusionSlices;
            const bool hidden = hiddenAt(t);
            if (hidden != previousHidden) {
                float low = previousT;
                float high = t;
                for (int step = 0; step < kBisectionSteps; ++step) {
                    const float middle = 0.5f * (low + high);
                    if (hiddenAt(middle) == previousHidden) {
                        low = middle;
                    } else {
                        high = middle;
                    }
                }
                const float boundary = 0.5f * (low + high);
                if (hidden) {
                    // Visible run ends here.
                    if (spanOpen && spanCount < 4) {
                        spans[spanCount++] = {spanStart, boundary};
                        visibleMetres += boundary - spanStart;
                    }
                    spanOpen = false;
                } else {
                    // A new visible run starts here.
                    spanStart = boundary;
                    spanOpen = true;
                }
                previousHidden = hidden;
            }
            previousT = t;
        }
        if (spanOpen && spanCount < 4) {
            spans[spanCount++] = {spanStart, tipT};
            visibleMetres += tipT - spanStart;
        }
    }
    m_OverlayVisibleMetres = visibleMetres;
    m_OverlaySpanCount = spanCount;
    if (spanCount == 0) {
        // Completely hidden behind the ball: correct for a near top-down view.
        ++m_OverlayHidden;
        return;
    }

    // Transform() treats Src as a point in the reference entity's local space,
    // so the ball is passed as the reference and its inverse matrix maps world
    // points back into that space.  The render context's world matrix, which
    // the renderer owns, is never touched.
    const VxMatrix inverseBall = ball->GetInverseWorldMatrix();
    const auto screenAt = [&](float t, ImVec2 &out) {
        VxVector screen;
        if (!ProjectToScreen(renderContext, inverseBall * Add(centre, Scale(direction, t)), ball,
                             screen)) {
            return false;
        }
        out = ImVec2(screen.x, screen.y);
        return true;
    };

    const ImGuiIO &io = ImGui::GetIO();
    const float scaleX = io.DisplaySize.x / static_cast<float>(renderContext->GetWidth());
    const float scaleY = io.DisplaySize.y / static_cast<float>(renderContext->GetHeight());

    // Pixels per metre at the arrow's depth: project the midpoint and the same
    // point one metre to the side of the view direction.  That is the true
    // perspective scale, independent of how the arrow itself is foreshortened.
    const VxVector middle = Add(centre, Scale(direction, 0.5f * (tailT + tipT)));
    float pixelsPerMetre = 0.0f;
    if (occlusionValid) {
        VxVector viewDirection(0.0f, 0.0f, 0.0f);
        if (Normalize(Subtract(middle, cameraPosition), viewDirection)) {
            VxVector up(0.0f, 1.0f, 0.0f);
            if (std::fabs(core::Dot(viewDirection, up)) > 0.95f) {
                up = VxVector(1.0f, 0.0f, 0.0f);
            }
            VxVector side(0.0f, 0.0f, 0.0f);
            if (core::Normalize(core::Cross(viewDirection, up), side)) {
                VxVector a;
                VxVector b;
                if (ProjectToScreen(renderContext, inverseBall * middle, ball, a) &&
                    ProjectToScreen(renderContext, inverseBall * Add(middle, side), ball, b)) {
                    const float dx = (b.x - a.x) * scaleX;
                    const float dy = (b.y - a.y) * scaleY;
                    pixelsPerMetre = std::sqrt(dx * dx + dy * dy);
                }
            }
        }
    }

    // Sizes are derived per span: normally from the true perspective scale,
    // and only when the camera is unusable from the span's own projection.
    const float metreScale = pixelsPerMetre;
    m_OverlayPixelsPerMetre = pixelsPerMetre;

    bool drewSomething = false;
    bool drewTip = false;
    ImVec2 tipScreen(0.0f, 0.0f);
    ImVec2 tipAxis(0.0f, 1.0f);

    for (int index = 0; index < spanCount; ++index) {
        ImVec2 a;
        ImVec2 b;
        if (!screenAt(spans[index].from, a) || !screenAt(spans[index].to, b)) {
            continue;
        }
        a = ImVec2(a.x * scaleX, a.y * scaleY);
        b = ImVec2(b.x * scaleX, b.y * scaleY);

        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float screenLength = std::sqrt(dx * dx + dy * dy);
        if (!(screenLength > 1.0f)) {
            continue;
        }
        if (a.x < -16.0f || a.y < -16.0f || a.x > io.DisplaySize.x + 16.0f ||
            a.y > io.DisplaySize.y + 16.0f) {
            continue;
        }

        // Without a usable camera the span's own projected length is the only
        // scale available.  Slightly wrong when the arrow points at the camera,
        // but it never leaves the arrow without a size.
        const float spanScale = metreScale > 1.0e-3f
                                        ? metreScale
                                        : screenLength / (spans[index].to - spans[index].from);
        const float shaftBase = std::clamp(core::kGravityArrowShaftBase * spanScale, 1.0f, 8.0f);
        const float shaftTip = std::clamp(core::kGravityArrowShaftTip * spanScale, 0.7f, 6.0f);
        const float headLength = std::clamp(core::kGravityArrowHeadLength * spanScale, 4.0f, 40.0f);
        const float headRadius = std::clamp(core::kGravityArrowHeadRadius * spanScale, 2.0f, 24.0f);

        const ImVec2 axis(dx / screenLength, dy / screenLength);
        const ImVec2 normal(-axis.y, axis.x);
        const bool endsAtTip = spans[index].to >= tipT - 1.0e-4f;
        const float spanHeadLength = endsAtTip ? (std::min)(headLength, screenLength * 0.45f) : 0.0f;
        const ImVec2 headBase(b.x - axis.x * spanHeadLength, b.y - axis.y * spanHeadLength);

        const auto shaftQuad = [&](float widening, ImU32 color) {
            const float half0 = shaftBase + widening;
            const float half1 = shaftTip + widening;
            draw->AddQuadFilled(ImVec2(a.x + normal.x * half0, a.y + normal.y * half0),
                                ImVec2(headBase.x + normal.x * half1, headBase.y + normal.y * half1),
                                ImVec2(headBase.x - normal.x * half1, headBase.y - normal.y * half1),
                                ImVec2(a.x - normal.x * half0, a.y - normal.y * half0), color);
        };
        const auto headTriangle = [&](float widening, ImU32 color) {
            const float half = headRadius + widening;
            draw->AddTriangleFilled(ImVec2(b.x + axis.x * widening, b.y + axis.y * widening),
                                    ImVec2(headBase.x + normal.x * half, headBase.y + normal.y * half),
                                    ImVec2(headBase.x - normal.x * half, headBase.y - normal.y * half),
                                    color);
        };

        shaftQuad(kOutline, kArrowEdge);
        if (endsAtTip) {
            headTriangle(kOutline, kArrowEdge);
        }
        shaftQuad(0.0f, kArrowColor);
        if (endsAtTip) {
            headTriangle(0.0f, kArrowColor);
            drewTip = true;
            tipScreen = b;
            tipAxis = axis;
            m_OverlayBase = VxVector(a.x, a.y, 0.0f);
            m_OverlayTip = VxVector(b.x, b.y, 0.0f);
        }
        drewSomething = true;
    }

    if (!drewSomething) {
        ++m_OverlayNoProjection;
        return;
    }
    ++m_OverlayDrawn;

    if (!drewTip) {
        // The tip is hidden behind the ball, so there is no point to hang the
        // magnitude label on: the arrow visibly runs into the ball instead.
        return;
    }

    char label[64];
    std::snprintf(label, sizeof(label), "g=%.4f", magnitude);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 textCentre(tipScreen.x + tipAxis.x * 16.0f, tipScreen.y + tipAxis.y * 16.0f);
    AddShadowedText(draw,
                    ImVec2(textCentre.x - textSize.x * 0.5f, textCentre.y - textSize.y * 0.5f), label);
}

// Shown only when the arrow cannot be drawn at all: it tells the three things
// that can fail (no ball, no gravity, projection) apart at a glance, and also
// proves that the ImGui draw list itself is live.
void FlexGravityMod::DrawOverlayStatus(ImDrawList *draw, CKRenderContext *renderContext) {
    char line[192];
    std::snprintf(line, sizeof(line), "FlexGravity overlay: frames=%d drawn=%d noball=%d noproj=%d",
                  m_OverlayFrames, m_OverlayDrawn, m_OverlayNoBall, m_OverlayNoProjection);
    AddShadowedText(draw, ImVec2(16.0f, 16.0f), line);

    std::snprintf(line, sizeof(line), "ball=(%.1f,%.1f,%.1f) g=(%.1f,%.1f,%.1f)",
                  m_OverlayBall.x, m_OverlayBall.y, m_OverlayBall.z, m_OverlayGravity.x,
                  m_OverlayGravity.y, m_OverlayGravity.z);
    AddShadowedText(draw, ImVec2(16.0f, 34.0f), line);

    std::snprintf(line, sizeof(line), "base=(%.1f,%.1f) tip=(%.1f,%.1f) display=%.0fx%.0f rc=%dx%d",
                  m_OverlayBase.x, m_OverlayBase.y, m_OverlayTip.x, m_OverlayTip.y,
                  m_OverlayDisplayWidth, m_OverlayDisplayHeight,
                  renderContext ? renderContext->GetWidth() : 0,
                  renderContext ? renderContext->GetHeight() : 0);
    AddShadowedText(draw, ImVec2(16.0f, 52.0f), line);
}

bool FlexGravityMod::ProjectToScreen(CKRenderContext *renderContext, const VxVector &localPoint,
                                     CK3dEntity *reference, VxVector &screenPoint) {
    VxVector source = localPoint;
    screenPoint = VxVector(0.0f, 0.0f, 0.0f);
    renderContext->Transform(&screenPoint, &source, reference);

    // Reject anything the transform could not express (behind the eye, NaN).
    const float limit = 100000.0f;
    if (!(screenPoint.x == screenPoint.x) || !(screenPoint.y == screenPoint.y)) {
        return false;
    }
    if (std::fabs(screenPoint.x) > limit || std::fabs(screenPoint.y) > limit) {
        return false;
    }
    return true;
}

// Array form of ProjectToScreen: the SDK's TransformVertices takes the whole
// array in one call and also returns the per-vertex clipping flags, which is
// what lets the overlays drop everything outside the frustum without touching
// the draw list.  `localPoints` are in `reference`'s space.
void FlexGravityMod::ProjectBatch(CKRenderContext *renderContext, CK3dEntity *reference,
                                  const VxVector *localPoints, size_t count,
                                  std::vector<VxVector4> &screen, std::vector<unsigned int> &clip) {
    screen.clear();
    clip.clear();
    if (!renderContext || !reference || !localPoints || count == 0) {
        return;
    }
    screen.resize(count);
    clip.assign(count, 0u);

    VxTransformData data{};
    data.InVertices = const_cast<VxVector *>(localPoints);
    data.InStride = sizeof(VxVector);
    data.ScreenVertices = screen.data();
    data.ScreenStride = sizeof(VxVector4);
    data.ClipFlags = clip.data();
    renderContext->TransformVertices(static_cast<int>(count), &data, reference);
}

void FlexGravityMod::PrintVizStatus(IBML *bml) {
    (void) bml;
    LogInfo("Overlay: hadFrame=%s frames=%d drawn=%d noBall=%d noProjection=%d hidden=%d",
            BoolText(m_OverlayHadFrame).c_str(), m_OverlayFrames, m_OverlayDrawn, m_OverlayNoBall,
            m_OverlayNoProjection, m_OverlayHidden);
    LogInfo("Overlay: imguiContext=%p bmlContext=%p display=%.0fx%.0f renderContext=%dx%d",
            static_cast<void *>(ImGui::GetCurrentContext()),
            static_cast<void *>(Bui::GetImGuiContext()), m_OverlayDisplayWidth,
            m_OverlayDisplayHeight, m_OverlayContextWidth, m_OverlayContextHeight);
    LogInfo("Overlay: ball=(%.2f,%.2f,%.2f) g=(%.2f,%.2f,%.2f) base=(%.2f,%.2f) tip=(%.2f,%.2f)",
            m_OverlayBall.x, m_OverlayBall.y, m_OverlayBall.z, m_OverlayGravity.x, m_OverlayGravity.y,
            m_OverlayGravity.z, m_OverlayBase.x, m_OverlayBase.y, m_OverlayTip.x, m_OverlayTip.y);
    LogInfo("Overlay: ballRadius=%.3f pixelsPerMetre=%.3f arrowLength=%.3f spans=%d visible=%.3f",
            m_OverlayBallRadius, m_OverlayPixelsPerMetre, core::kGravityArrowLength, m_OverlaySpanCount,
            m_OverlayVisibleMetres);
    LogInfo("Overlay: vertices=%d budgets=%d+%d vtxOffset=%s",
            m_OverlayVertices, core::kFieldLineVertexBudget, core::kEquipotentialVertexBudget,
            BoolText(m_OverlayVtxOffsetSupported).c_str());
    LogInfo("Overlay: batchProjection maxDeltaPx=%.4f (batch TransformVertices vs single Transform)",
            m_OverlayProjectionDelta);
    LogInfo("FieldLines: enabled=%s cached=%zu pending=%d traced=%d dropped=%d drawn=%d points=%d projected=%d vertices=%d lastTraceMs=%.2f spacing=%.3f cache=%s",
            BoolText(m_EnableFieldLines).c_str(), m_FieldLines.size(), m_FieldLinePending,
            m_FieldLineTraced, m_FieldLineDropped, m_FieldLineDrawn, m_FieldLinePoints,
            m_FieldLineProjected, m_FieldLineVertices, m_FieldLineLastTraceMs, m_FieldLineSpacing,
            BoolText(m_LineCacheValid).c_str());
    LogInfo("Equipotentials: enabled=%s samples=%d/%zu triangles=%d levels=%d step=%.3f cell=%.3f lastBuildMs=%.2f band=%d..%d projected=%d cache=%s",
            BoolText(m_EnableEquipotentials).c_str(), m_EquipotentialSamples,
            m_Equipotentials.values.size(), m_EquipotentialTriangles, m_EquipotentialLevels,
            m_Equipotentials.levelStep, m_Equipotentials.cellSize, m_Equipotentials.lastBuildMs,
            m_Equipotentials.bandFirst, m_Equipotentials.bandLast, m_EqProjected,
            BoolText(m_EqCacheValid).c_str());

    // The batch-vs-single projection check runs on the render thread (see
    // DrawOverlay); reading it here keeps the console path free of engine calls.
    SendMessage("overlay diagnostics written to ModLoader.log");
}
