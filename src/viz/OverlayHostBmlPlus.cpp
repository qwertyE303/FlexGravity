// ===========================================================================
// OverlayHostBmlPlus.cpp - BML+ side of the host boundary.
// ---------------------------------------------------------------------------
// The loader creates the ImGui context and calls NewFrame/Render itself, so
// this file only forwards the context queries and the context scope.  It is
// not part of the legacy target.
// ===========================================================================
#include "viz/OverlayHost.h"

#include <BML/Bui.h>

#include <imgui.h>

namespace {

ImGuiContext *g_Previous = nullptr;

} // namespace

namespace OverlayHost {

bool HasContext() { return Bui::GetImGuiContext() != nullptr; }

bool BeginFrame() { return true; }

void EndFrame() {}

void *CurrentContext() { return static_cast<void *>(Bui::GetImGuiContext()); }

void EnterContext() {
    g_Previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(Bui::GetImGuiContext());
}

void LeaveContext() { ImGui::SetCurrentContext(g_Previous); }

} // namespace OverlayHost
