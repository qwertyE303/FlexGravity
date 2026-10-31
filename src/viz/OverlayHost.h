// ===========================================================================
// OverlayHost.h - the only place the overlay knows which loader it runs on.
// ---------------------------------------------------------------------------
// BML+ owns the Dear ImGui context and its frame: the loader calls NewFrame
// before OnProcess and submits the draw data after sprites.  The original BML
// has no ImGui at all, so the legacy build creates the context itself, opens
// and closes the frame, and submits it from a post-sprite callback.
//
// Everything else in viz/Overlay.cpp is shared and calls this interface.
// ===========================================================================
#pragma once

namespace OverlayHost {

// True when a usable ImGui context exists.  BML+: the loader's context.
// Legacy: our own, created on first use.
bool HasContext();

// Starts one overlay frame.  BML+: the loader already did NewFrame, so this is
// a no-op that returns true.  Legacy: NewFrame (false when it cannot start).
bool BeginFrame();

// Ends the frame started by BeginFrame.  BML+: no-op (the loader renders).
// Legacy: ImGui::Render, then the post-sprite callback submits the draw data.
void EndFrame();

// Diagnostic pointer for the overlay status log.
void *CurrentContext();

// Makes the host context current for the duration of a Scope.  BML+ has to do
// this because the mod DLL and the loader do not share a "current context";
// legacy owns the only context, so it is a no-op.
void EnterContext();
void LeaveContext();

struct Scope {
    Scope() { EnterContext(); }
    ~Scope() { LeaveContext(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
};

} // namespace OverlayHost
