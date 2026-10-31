#include "mod/FlexGravityMod.h"

#if defined(FLEXGRAVITY_LEGACY_BML)
#include "bml-old/BmlLegacyOverlay.h"
#endif

// Every teardown callback funnels through here.  While the engine is tearing
// the level down, no behavior may be executed and no force may be unset: doing
// so collides with the engine destroying the rigid bodies and hangs the main
// thread.  `destroyChannels == false` therefore only forgets our own state.
void FlexGravityMod::TearDownLevel(bool destroyChannels, bool clearScan) {
    m_TearingDown = true;

    if (destroyChannels) {
        DestroyForceChannels();
    } else {
        DropAllFieldForces();
    }

    m_Bodies.clear();
    m_BallRadii.clear();
#if defined(FLEXGRAVITY_LEGACY_BML)
    // No field-line/equipotential state exists on the original loader, but the
    // cached wake-up behavior belongs to Level_Init and must be forgotten
    // before the engine destroys that script.
    ExecuteBB::ForgetCachedWakeUp();
#else
    // Traced field lines only make sense for the level they were traced in, and
    // the equipotential lattice was sampled from that level's fields.
    DropFieldLines();
    DropEquipotentials();
#endif
    m_PendingPhysicalizeCount = 0;
    m_PendingFixedCount = 0;
    m_PendingChannelsCreated = 0;
    m_PendingChannelsRebuilt = 0;
    m_LastFieldUpdate = -1.0f;

    // A level reset is followed by OnStartLevel, which rescans; keeping the
    // previous scan around would only keep stale object ids alive.
    if (clearScan) {
        m_Scan = ScanInfo{};
    }
}

void FlexGravityMod::OnUnload() {
    TearDownLevel(true, true);
#if defined(FLEXGRAVITY_LEGACY_BML)
    // Destroys the mod-owned ImGui context and unregisters the render callback.
    BmlLegacyOverlayShutdown();
#endif
}

void FlexGravityMod::OnStartLevel() {
    m_TearingDown = false;

    // Overlay diagnostics are per level: a warning written in the menu must not
    // suppress the one that would explain a missing arrow in the level.
    m_OverlayGateWarned = false;
    m_OverlayDrawWarned = false;
    m_OverlayGateFrames = 0;
    m_OverlayProcessFrames = 0;
    m_OverlayHadFrame = false;

    CKBehavior *levelInit = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    if (!m_ForceChannels.empty()) {
        if (levelInit && levelInit == m_OwnerScript) {
            // Same Level_Init object: this was a level reset, and the force
            // behaviors survived with a handle to the previous rigid body.
            // They are rebuilt on the next frame, outside script execution.
            m_PendingChannelRebuild = true;
        } else {
            // The level was reloaded (not just reset): the old script is gone
            // and every behavior we created with it is dangling.
            if (DebugLogging()) {
                LogInfo("Level init changed; dropping %zu stale force channel(s)",
                        m_ForceChannels.size());
            }
            m_ForceChannels.clear();
        }
    }
    m_OwnerScript = levelInit;

    m_ForceBackendFailed = false;
    m_LastAffectedCount = -1;

    Scan();
    BuildSourceIndex();

    CK3dEntity *ball = FindActiveBall();
    if (ball) {
        if (DebugLogging()) {
            bool found = false;
            const float mass = GetBodyMass(ball, &found);
            LogInfo("Active ball at level start: %s id=%s mass=%.4f known=%s",
                    SafeName(ball).c_str(), IdText(ball).c_str(), mass, BoolText(found).c_str());
        }
    } else {
        LogWarn("No active ball found at level start");
    }
}

void FlexGravityMod::OnPreLoadLevel() {
    if (DebugLogging()) {
        LogInfo("OnPreLoadLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    }
    TearDownLevel(true, true);
}

void FlexGravityMod::OnPreExitLevel() {
    if (DebugLogging()) {
        LogInfo("OnPreExitLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    }
    TearDownLevel(true, true);
}

#if defined(FLEXGRAVITY_LEGACY_BML)
void FlexGravityMod::OnRender(CK_RENDER_FLAGS flags) {
    (void) flags;
    static bool warned = false;
    if (BmlLegacyOverlayFlushPending() && !warned) {
        warned = true;
        LogAlways("overlay: submitted from OnRender (the post-sprite callback did not fire)");
    }
}
#endif

void FlexGravityMod::OnProcess() {
    if (m_TearingDown) {
        return;
    }
    if (m_PendingChannelRebuild) {
        m_PendingChannelRebuild = false;
        RebuildForceChannels();
    }
    if (ShouldUpdateFields()) {
        UpdateFields();
    }
    FlushLogCounters();
    // ImGui frame order in BML 0.3.13: PreProcess(NewFrame) -> PostProcess
    // (this callback) -> ImGui::Render().  Drawing here is inside the frame;
    // OnRender runs after Render and its draw commands are discarded.
    DrawOverlayFrame();
}

void FlexGravityMod::OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float friction, float elasticity, float mass,
                                   const char *collGroup, CKBOOL startFrozen, CKBOOL enableColl,
                                   CKBOOL calcMassCenter, float linearDamp, float rotDamp, const char *collSurface,
                                   VxVector massCenter, int convexCnt, CKMesh **convexMesh, int ballCnt,
                                   VxVector *ballCenter, float *ballRadius, int concaveCnt,
                                   CKMesh **concaveMesh) {
    (void) friction;
    (void) elasticity;
    (void) collGroup;
    (void) startFrozen;
    (void) enableColl;
    (void) calcMassCenter;
    (void) linearDamp;
    (void) rotDamp;
    (void) collSurface;
    (void) massCenter;
    (void) convexCnt;
    (void) convexMesh;
    (void) ballCnt;
    (void) ballCenter;
    (void) ballRadius;
    (void) concaveCnt;
    (void) concaveMesh;

    if (!target) {
        return;
    }

    BodyInfo body;
    body.objectId = target->GetID();
    body.objectName = SafeName(target);
    body.mass = mass;
    body.fixed = fixed != FALSE;
    body.physicalized = true;
    m_Bodies[body.objectId] = body;

    // The level script passes the collision sphere radius to the Physicalize
    // building block, and BML forwards it here.  Remember it: that is the real
    // ball radius, straight out of the Virtools data.
    if (ballCnt > 0 && ballRadius && ballRadius[0] > 0.0f) {
        m_BallRadii[body.objectId] = ballRadius[0];
    }

    if (body.fixed) {
        ++m_PendingFixedCount;
    } else {
        // Counting instead of logging: a debris-heavy level physicalizes 150+
        // bodies, and one line each buried every other diagnostic.
        ++m_PendingPhysicalizeCount;
    }

    auto channel = m_ForceChannels.find(body.objectId);
    if (channel != m_ForceChannels.end()) {
        // The rigid body was replaced (respawn, ball switch, ...).  The force
        // behavior still holds the previous body, so it has to be rebuilt
        // before it is used again.
        channel->second.active = false;
        channel->second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel->second.lastMass = 0.0f;
        if (channel->second.behavior) {
            channel->second.needsRebuild = true;
        }
    }
}

void FlexGravityMod::OnUnphysicalize(CK3dEntity *target) {
    if (!target) {
        return;
    }

    const CK_ID objectId = target->GetID();
    auto it = m_Bodies.find(objectId);
    if (it != m_Bodies.end()) {
        it->second.physicalized = false;
    }

    auto channel = m_ForceChannels.find(objectId);
    if (channel != m_ForceChannels.end()) {
        // The rigid body is already being destroyed; calling into the behavior
        // here (UnsetPhysicsForce) hangs the engine.  Just forget the state.
        channel->second.active = false;
        channel->second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel->second.lastMass = 0.0f;
    }
}

void FlexGravityMod::OnDead() {
    if (DebugLogging()) {
        LogInfo("OnDead: dropping field forces");
    }
    ClearAllFieldForces();
}

void FlexGravityMod::OnPreResetLevel() {
    if (DebugLogging()) {
        LogInfo("OnPreResetLevel: tearing down (%zu channel(s))", m_ForceChannels.size());
    }
    TearDownLevel(false, false);
}

// Replaces the per-object lines that used to dominate the log: one summary per
// frame at most, and only when something actually happened.
void FlexGravityMod::FlushLogCounters() {
    if (m_PendingPhysicalizeCount > 0 || m_PendingFixedCount > 0) {
        LogInfo("Physicalize: %d dynamic body/bodies, %d fixed ignored", m_PendingPhysicalizeCount,
                m_PendingFixedCount);
        m_PendingPhysicalizeCount = 0;
        m_PendingFixedCount = 0;
    }
    if (m_PendingChannelsCreated > 0 || m_PendingChannelsRebuilt > 0) {
        LogInfo("Force channels: created %d, rebuilt %d", m_PendingChannelsCreated,
                m_PendingChannelsRebuilt);
        m_PendingChannelsCreated = 0;
        m_PendingChannelsRebuilt = 0;
    }
}

