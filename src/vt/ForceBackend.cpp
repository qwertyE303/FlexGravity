#include "mod/FlexGravityMod.h"

CKBehavior *FlexGravityMod::GetOrCreateForceBehavior(ForceChannel &channel) {
    if (channel.behavior) {
        return channel.behavior;
    }
    if (m_ForceBackendFailed || !m_BML) {
        return nullptr;
    }

    CKBehavior *ownerScript = m_BML->GetScriptByName("Level_Init");
    if (!ownerScript) {
        m_ForceBackendFailed = true;
        LogWarn("Cannot create force behavior: Level_Init script not found");
        return nullptr;
    }

    CKBehavior *behavior = ExecuteBB::CreateSetPhysicsForce(ownerScript);
    if (!behavior) {
        m_ForceBackendFailed = true;
        LogWarn("Cannot create SetPhysicsForce behavior");
        return nullptr;
    }

    channel.behavior = behavior;
    if (!m_OwnerScript) {
        m_OwnerScript = ownerScript;
    }
    ++m_PendingChannelsCreated;
    return behavior;
}

void FlexGravityMod::ClearChannelForce(ForceChannel &channel) {
    // Never execute a behavior while the engine is dismantling the level.
    if (channel.behavior && channel.active && !m_TearingDown) {
        channel.behavior->ActivateInput(1);
        channel.behavior->Execute(0);
    }
    channel.active = false;
    channel.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
    channel.lastMass = 0.0f;
}

void FlexGravityMod::DropAllFieldForces() {
    // Forgets the state without touching the engine (used during teardown).
    for (auto &entry : m_ForceChannels) {
        entry.second.active = false;
        entry.second.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        entry.second.lastMass = 0.0f;
    }
    m_LastAffectedCount = -1;
}

void FlexGravityMod::ApplyChannelForce(CK3dEntity *entity, const BodyInfo &body,
                                       const VxVector &acceleration) {
    const float lengthSquared = LengthSquared(acceleration);
    if (lengthSquared <= kNoAccelerationSquared || !entity || body.mass <= 0.0f) {
        return;
    }

    auto it = m_ForceChannels.find(body.objectId);
    if (it == m_ForceChannels.end()) {
        ForceChannel channel;
        channel.objectId = body.objectId;
        it = m_ForceChannels.emplace(body.objectId, channel).first;
    }
    ForceChannel &channel = it->second;

    // A re-physicalized object (death respawn, ball type switch, ...) is a new
    // rigid body: the old behavior still points at the destroyed one, so the
    // first "set" after that would silently do nothing.  Recreate it here,
    // from the frame callback (never during script execution).
    if (channel.needsRebuild) {
        channel.needsRebuild = false;
        if (channel.behavior) {
            CKBehavior *owner = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
            if (owner && owner == m_OwnerScript) {
                ScriptHelper::DeleteBB(owner, channel.behavior);
            }
            channel.behavior = nullptr;
            ++m_PendingChannelsRebuilt;
        }
        channel.active = false;
        channel.lastAcceleration = VxVector(0.0f, 0.0f, 0.0f);
        channel.lastMass = 0.0f;
    }

    const VxVector delta = Subtract(channel.lastAcceleration, acceleration);
    if (channel.active && channel.lastMass == body.mass && LengthSquared(delta) <= kNoAccelerationSquared) {
        return;
    }

    CKBehavior *behavior = GetOrCreateForceBehavior(channel);
    if (!behavior) {
        return;
    }

    if (channel.active) {
        behavior->ActivateInput(1);
        behavior->Execute(0);
        channel.active = false;
    }

    CKParameterIn *targetParameter = behavior->GetTargetParameter();
    CKParameterIn *positionParameter = behavior->GetInputParameter(0);
    CKParameterIn *positionRefParameter = behavior->GetInputParameter(1);
    CKParameterIn *directionParameter = behavior->GetInputParameter(2);
    CKParameterIn *directionRefParameter = behavior->GetInputParameter(3);
    CKParameterIn *forceParameter = behavior->GetInputParameter(4);
    if (!targetParameter || !positionParameter || !positionRefParameter ||
        !directionParameter || !directionRefParameter || !forceParameter ||
        !targetParameter->GetDirectSource() || !positionParameter->GetDirectSource() ||
        !positionRefParameter->GetDirectSource() || !directionParameter->GetDirectSource() ||
        !directionRefParameter->GetDirectSource() || !forceParameter->GetDirectSource()) {
        if (!m_ForceBackendFailed) {
            m_ForceBackendFailed = true;
            LogWarn("SetPhysicsForce behavior has no direct parameter sources");
        }
        return;
    }

    ScriptHelper::SetParamObject(targetParameter->GetDirectSource(), entity);
    ScriptHelper::SetParamValue(positionParameter->GetDirectSource(), VxVector(0.0f, 0.0f, 0.0f));
    ScriptHelper::SetParamObject(positionRefParameter->GetDirectSource(), entity);
    ScriptHelper::SetParamValue(directionParameter->GetDirectSource(), acceleration);
    ScriptHelper::SetParamObject(directionRefParameter->GetDirectSource(), nullptr);

    const float magnitude = std::sqrt(lengthSquared);
    const float rawValue = body.mass * magnitude / kForceScale;
    ScriptHelper::SetParamValue(forceParameter->GetDirectSource(), rawValue);

    ExecuteBB::PhysicsWakeUp(entity);
    behavior->ActivateInput(0);
    behavior->Execute(0);

    channel.active = true;
    channel.lastAcceleration = acceleration;
    channel.lastMass = body.mass;

    if (DebugLogging()) {
        LogInfo("Field force: %s accel=(%.4f, %.4f, %.4f) raw=%.4f",
                body.objectName.c_str(), acceleration.x, acceleration.y, acceleration.z, rawValue);
    }
}

void FlexGravityMod::ClearAllFieldForces() {
    for (auto &entry : m_ForceChannels) {
        ClearChannelForce(entry.second);
    }
    m_LastAffectedCount = 0;
}

void FlexGravityMod::DestroyForceChannels() {
    CKBehavior *ownerScript = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    const bool canDelete = !m_TearingDown && ownerScript && ownerScript == m_OwnerScript;
    for (auto &entry : m_ForceChannels) {
        if (m_TearingDown) {
            entry.second.active = false;
            entry.second.lastMass = 0.0f;
        } else {
            ClearChannelForce(entry.second);
        }
        if (canDelete && entry.second.behavior) {
            ScriptHelper::DeleteBB(ownerScript, entry.second.behavior);
        }
    }
    m_ForceChannels.clear();
    m_OwnerScript = nullptr;
    m_LastAffectedCount = -1;
    m_ForceBackendFailed = false;
}

void FlexGravityMod::RebuildForceChannels() {
    // Called from the frame callback (never during script execution) right
    // after a level reset.  A SetPhysicsForce behavior keeps a handle to the
    // rigid body it was last applied to; after a reset that body is gone, so
    // the first "set" would silently do nothing.  Recreating the behaviors
    // restores the exact state of a freshly loaded level.
    CKBehavior *levelInit = m_BML ? m_BML->GetScriptByName("Level_Init") : nullptr;
    int deleted = 0;
    if (levelInit && levelInit == m_OwnerScript) {
        for (auto &entry : m_ForceChannels) {
            if (entry.second.behavior) {
                ScriptHelper::DeleteBB(levelInit, entry.second.behavior);
                entry.second.behavior = nullptr;
                ++deleted;
            }
        }
    }
    m_ForceChannels.clear();
    m_LastAffectedCount = -1;
    m_ForceBackendFailed = false;
    LogInfo("Rebuilt force channels after level reset (deleted %d)", deleted);
}

