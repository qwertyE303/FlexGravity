// ===========================================================================
// BmlLegacyExecuteBB.cpp - the two ExecuteBB force helpers that BML 0.3.40
// does not ship (SetPhysicsForce / PhysicsWakeUp and their Create* variants).
// ---------------------------------------------------------------------------
// BML+'s ExecuteBB.cpp builds them out of ScriptHelper::CreateBB plus direct
// parameter sources; the building blocks themselves are Ballance's physics_RT
// blocks, so the same recipe works on the original loader:
//
//   TT_SETPHYSICSFORCE  CKGUID(0x56e20c57, 0x0b926068)   (BML/RegisterBB.h)
//   TT_PHYSICSWAKEUP    CKGUID(0x38b851b5, 0x72ca74ac)   (BML+ physics_RT.h)
//
// The wake-up behavior is cached per owner script and dropped on teardown,
// because a behavior belongs to the script object and dies with it.
// ===========================================================================
#include <BML/ExecuteBB.h>

#include <CKAll.h>

namespace {

const CKGUID kSetPhysicsForceGuid(0x56e20c57u, 0x0b926068u);
const CKGUID kPhysicsWakeUpGuid(0x38b851b5u, 0x72ca74acu);

CKBehavior *g_WakeUpOwner = nullptr;
CKBehavior *g_WakeUp = nullptr;

// CreateBB(..., target = true) only turns the target parameter on; it does not
// give it a direct source.  ScriptHelper::SetParamObject then dereferences that
// source without a check, so an unlinked target kills the process on the very
// first wake-up.  BML's own ExecuteBB::CreatePhysicsWakeUp links a local
// parameter right after creating the behavior, and so do we here.
void EnsureWakeUpBehavior(CKBehavior *ownerScript) {
    if (!ownerScript) {
        return;
    }
    if (g_WakeUpOwner == ownerScript && g_WakeUp != nullptr) {
        return;
    }
    g_WakeUpOwner = ownerScript;
    g_WakeUp = ScriptHelper::CreateBB(ownerScript, kPhysicsWakeUpGuid, true);
    if (!g_WakeUp) {
        return;
    }

    CKParameterIn *targetParameter = g_WakeUp->GetTargetParameter();
    if (!targetParameter) {
        return;
    }
    if (!targetParameter->GetDirectSource()) {
        targetParameter->SetDirectSource(
                ScriptHelper::CreateParamObject(ownerScript, "WakeUp Target", CKPGUID_3DENTITY, nullptr));
    }
}

} // namespace

namespace ExecuteBB {

CKBehavior *CreateSetPhysicsForce(CKBehavior *script, CK3dEntity *target, VxVector position,
                                  CK3dEntity *posRef, VxVector direction, CK3dEntity *directionRef,
                                  float force) {
    if (!script) {
        return nullptr;
    }

    CKBehavior *beh = ScriptHelper::CreateBB(script, kSetPhysicsForceGuid, true);
    if (!beh) {
        return nullptr;
    }

    CKParameterIn *targetParameter = beh->GetTargetParameter();
    if (targetParameter) {
        targetParameter->SetDirectSource(
                ScriptHelper::CreateParamObject(script, "Target", CKPGUID_3DENTITY, target));
    }

    CKParameterIn *positionParameter = beh->GetInputParameter(0);
    CKParameterIn *positionRefParameter = beh->GetInputParameter(1);
    CKParameterIn *directionParameter = beh->GetInputParameter(2);
    CKParameterIn *directionRefParameter = beh->GetInputParameter(3);
    CKParameterIn *forceParameter = beh->GetInputParameter(4);
    if (!positionParameter || !positionRefParameter || !directionParameter ||
        !directionRefParameter || !forceParameter) {
        return beh;
    }

    positionParameter->SetDirectSource(
            ScriptHelper::CreateParamValue(script, "Position", CKPGUID_VECTOR, position));
    positionRefParameter->SetDirectSource(
            ScriptHelper::CreateParamObject(script, "Pos Referential", CKPGUID_3DENTITY, posRef));
    directionParameter->SetDirectSource(
            ScriptHelper::CreateParamValue(script, "Direction", CKPGUID_VECTOR, direction));
    directionRefParameter->SetDirectSource(
            ScriptHelper::CreateParamObject(script, "Direction Ref", CKPGUID_3DENTITY, directionRef));
    forceParameter->SetDirectSource(
            ScriptHelper::CreateParamValue(script, "Force Value", CKPGUID_FLOAT, force));
    return beh;
}

void PhysicsWakeUp(CKBehavior *ownerScript, CK3dEntity *target) {
    if (!ownerScript || !target) {
        return;
    }
    EnsureWakeUpBehavior(ownerScript);
    if (!g_WakeUp) {
        return;
    }
    CKParameterIn *targetParameter = g_WakeUp->GetTargetParameter();
    if (!targetParameter) {
        return;
    }
    CKParameter *source = targetParameter->GetDirectSource();
    if (!source) {
        return;
    }
    ScriptHelper::SetParamObject(source, target);
    g_WakeUp->ActivateInput(0);
    g_WakeUp->Execute(0);
}

void ForgetCachedWakeUp() {
    g_WakeUpOwner = nullptr;
    g_WakeUp = nullptr;
}

} // namespace ExecuteBB
