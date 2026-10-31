// ===========================================================================
// BmlLegacyOverlay.h - lifecycle of the mod-owned Dear ImGui context used by
// the original BML build (the loader has no ImGui of its own).
// ===========================================================================
#pragma once

class IBML;

// Creates the ImGui context and registers the post-sprite render callback.
// Safe to call before a level exists: the callback is registered lazily once
// the render context is available.
void BmlLegacyOverlayInitialize(IBML *bml);

// Unregisters the callback and destroys the context, the ImGui textures and
// the backend state.  Called from IMod::OnUnload.
void BmlLegacyOverlayShutdown();

// Submits a frame that the post-sprite callback did not consume (some Virtools
// builds invoke the callback later than expected).  Returns true when a frame
// was pending and has now been submitted.  IMod::OnRender calls this as a
// fallback; it is a no-op when the normal path already ran.
bool BmlLegacyOverlayFlushPending();
