// ===========================================================================
// BmlCompat.h - the original Ballance Mod Loader (BML) 0.3.40 interface,
// declared against the Virtools 2.1 SDK headers instead of BML's trimmed
// virtools/ copies.
// ---------------------------------------------------------------------------
// Why a copy instead of #include <BML/IMod.h> from the BML SDK:
//   * the BML SDK ships its own "virtools/CKAll.h" with de-virtualized class
//     declarations.  This mod needs the real Virtools SDK (CKRenderContext's
//     drawing methods are virtual, and Ballance's CK2.dll does not export them
//     as symbols), so the two header trees cannot be mixed in one translation
//     unit;
//   * every class below is a pure interface: the vtable order is the ABI.  The
//     declarations are copied 1:1 from BML 0.3.40 (IMod.h / IBML.h / ICommand.h
//     / IConfig.h / ILogger.h), with only two edits: "virtools/CKAll.h" is
//     replaced by <CKAll.h>, and the protected IBML pointer is named m_BML so
//     the shared sources do not need a rename.
//   * ScriptHelper's functions are imported from BML.dll (BML.lib); ExecuteBB's
//     two force helpers are NOT in 0.3.40, so they are declared here and
//     implemented in BmlLegacyExecuteBB.cpp on top of ScriptHelper + the
//     physics_RT building-block GUIDs.
// ===========================================================================
#pragma once

#include <windows.h>

#include <CKAll.h>

#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <string>
#include <vector>

#ifndef BML_EXPORT
#define BML_EXPORT __declspec(dllimport)
#endif

#ifndef MOD_EXPORT
#define MOD_EXPORT extern "C" __declspec(dllexport)
#endif

// BML 0.3.40 version macros (BML/Version.h in the original SDK).
#define BML_MAJOR_VER 0
#define BML_MINOR_VER 3
#define BML_BUILD_VER 40
#define BML_VERSION "0.3.40"

class IBML;
class IMod;

// ---------------------------------------------------------------------------
// BMLVersion / IMod
// ---------------------------------------------------------------------------
struct BMLVersion {
    BMLVersion() : major(BML_MAJOR_VER), minor(BML_MINOR_VER), build(BML_BUILD_VER) {}
    BMLVersion(int mj, int mn, int bd) : major(mj), minor(mn), build(bd) {}
    int major, minor, build;

    bool operator<(const BMLVersion &o) const {
        if (major == o.major) {
            if (minor == o.minor) {
                return build < o.build;
            }
            return minor < o.minor;
        }
        return major < o.major;
    }

    bool operator>=(const BMLVersion &o) const { return !(*this < o); }
};

#define DECLARE_BML_VERSION \
    BMLVersion GetBMLVersion() override { return {BML_MAJOR_VER, BML_MINOR_VER, BML_BUILD_VER}; }

// ---------------------------------------------------------------------------
// IMessageReceiver  (vtable order is ABI: do not reorder)
// ---------------------------------------------------------------------------
class BML_EXPORT IMessageReceiver {
public:
    virtual void OnPreStartMenu() {};
    virtual void OnPostStartMenu() {};

    virtual void OnExitGame() {};

    virtual void OnPreLoadLevel() {};
    virtual void OnPostLoadLevel() {};

    virtual void OnStartLevel() {};

    virtual void OnPreResetLevel() {};
    virtual void OnPostResetLevel() {};

    virtual void OnPauseLevel() {};
    virtual void OnUnpauseLevel() {};

    virtual void OnPreExitLevel() {};
    virtual void OnPostExitLevel() {};

    virtual void OnPreNextLevel() {};
    virtual void OnPostNextLevel() {};

    virtual void OnDead() {};

    virtual void OnPreEndLevel() {};
    virtual void OnPostEndLevel() {};

    virtual void OnCounterActive() {};
    virtual void OnCounterInactive() {};

    virtual void OnBallNavActive() {};
    virtual void OnBallNavInactive() {};

    virtual void OnCamNavActive() {};
    virtual void OnCamNavInactive() {};

    virtual void OnBallOff() {};

    virtual void OnPreCheckpointReached() {};
    virtual void OnPostCheckpointReached() {};

    virtual void OnLevelFinish() {};

    virtual void OnGameOver() {};

    virtual void OnExtraPoint() {};

    virtual void OnPreSubLife() {};
    virtual void OnPostSubLife() {};

    virtual void OnPreLifeUp() {};
    virtual void OnPostLifeUp() {};
};

// ---------------------------------------------------------------------------
// ILogger / IConfig / ICommand
// ---------------------------------------------------------------------------
class BML_EXPORT ILogger {
public:
    virtual void Info(const char *fmt, ...) = 0;
    virtual void Warn(const char *fmt, ...) = 0;
    virtual void Error(const char *fmt, ...) = 0;
};

class BML_EXPORT IProperty {
public:
    virtual const char * GetString() = 0;
    virtual bool GetBoolean() = 0;
    virtual int GetInteger() = 0;
    virtual float GetFloat() = 0;
    virtual CKKEYBOARD GetKey() = 0;

    virtual void SetString(const char * value) = 0;
    virtual void SetBoolean(bool value) = 0;
    virtual void SetInteger(int value) = 0;
    virtual void SetFloat(float value) = 0;
    virtual void SetKey(CKKEYBOARD value) = 0;

    virtual void SetComment(const char * comment) = 0;
    virtual void SetDefaultString(const char * value) = 0;
    virtual void SetDefaultBoolean(bool value) = 0;
    virtual void SetDefaultInteger(int value) = 0;
    virtual void SetDefaultFloat(float value) = 0;
    virtual void SetDefaultKey(CKKEYBOARD value) = 0;

    enum PropertyType {
        STRING,
        BOOLEAN,
        INTEGER,
        KEY,
        FLOAT,
        NONE
    };
    virtual PropertyType GetType() = 0;
};

class BML_EXPORT IConfig {
public:
    virtual bool HasCategory(const char * category) = 0;
    virtual bool HasKey(const char * category, const char * key) = 0;

    virtual IProperty *GetProperty(const char * category, const char * key) = 0;
    virtual void SetCategoryComment(const char * category, const char * comment) = 0;
};

class BML_EXPORT ICommand {
public:
    virtual std::string GetName() = 0;
    virtual std::string GetAlias() = 0;
    virtual std::string GetDescription() = 0;
    virtual bool IsCheat() = 0;

    virtual void Execute(IBML *bml, const std::vector<std::string> &args) = 0;
    virtual const std::vector<std::string> GetTabCompletion(IBML *bml,
                                                           const std::vector<std::string> &args) = 0;

    static int ParseInteger(const std::string &str, int mn = INT_MIN, int mx = INT_MAX) {
        return (std::max)(mn, (std::min)(mx, atoi(str.c_str())));
    }

    static float ParseFloat(const std::string &str, float mn = FLT_MIN, float mx = FLT_MAX) {
        return (std::max)(mn, (std::min)(mx, (float) atof(str.c_str())));
    }

    static bool ParseBoolean(const std::string &str) { return str == "true" || str == "1"; }
};

// ---------------------------------------------------------------------------
// IBML  (vtable order is ABI: do not reorder)
// ---------------------------------------------------------------------------
class InputHook; // BML/Gui.h; never used by this mod.

class BML_EXPORT IBML : public IMessageReceiver {
public:
    virtual CKContext *GetCKContext() = 0;
    virtual CKRenderContext *GetRenderContext() = 0;

    virtual void ExitGame() = 0;

    virtual CKAttributeManager *GetAttributeManager() = 0;
    virtual CKBehaviorManager *GetBehaviorManager() = 0;
    virtual CKCollisionManager *GetCollisionManager() = 0;
    virtual InputHook *GetInputManager() = 0;
    virtual CKMessageManager *GetMessageManager() = 0;
    virtual CKPathManager *GetPathManager() = 0;
    virtual CKParameterManager *GetParameterManager() = 0;
    virtual CKRenderManager *GetRenderManager() = 0;
    virtual CKSoundManager *GetSoundManager() = 0;
    virtual CKTimeManager *GetTimeManager() = 0;

    virtual void AddTimer(CKDWORD delay, std::function<void()> callback) = 0;
    virtual void AddTimerLoop(CKDWORD delay, std::function<bool()> callback) = 0;
    virtual void AddTimer(float delay, std::function<void()> callback) = 0;
    virtual void AddTimerLoop(float delay, std::function<bool()> callback) = 0;

    virtual bool IsCheatEnabled() = 0;
    virtual void EnableCheat(bool enable) = 0;

    virtual void SendIngameMessage(const char * msg) = 0;

    virtual void RegisterCommand(ICommand *cmd) = 0;

    virtual void SetIC(CKBeObject *obj, bool hierarchy = false) = 0;
    virtual void RestoreIC(CKBeObject *obj, bool hierarchy = false) = 0;
    virtual void Show(CKBeObject *obj, CK_OBJECT_SHOWOPTION show, bool hierarchy = false) = 0;

    virtual bool IsIngame() = 0;
    virtual bool IsPaused() = 0;
    virtual bool IsPlaying() = 0;

    virtual CKDataArray *GetArrayByName(const char * name) = 0;
    virtual CKGroup *GetGroupByName(const char * name) = 0;
    virtual CKMaterial *GetMaterialByName(const char * name) = 0;
    virtual CKMesh *GetMeshByName(const char * name) = 0;
    virtual CK2dEntity *Get2dEntityByName(const char * name) = 0;
    virtual CK3dEntity *Get3dEntityByName(const char * name) = 0;
    virtual CK3dObject *Get3dObjectByName(const char * name) = 0;
    virtual CKCamera *GetCameraByName(const char * name) = 0;
    virtual CKTargetCamera *GetTargetCameraByName(const char * name) = 0;
    virtual CKLight *GetLightByName(const char * name) = 0;
    virtual CKTargetLight *GetTargetLightByName(const char * name) = 0;
    virtual CKSound *GetSoundByName(const char * name) = 0;
    virtual CKTexture *GetTextureByName(const char * name) = 0;
    virtual CKBehavior *GetScriptByName(const char * name) = 0;

    virtual void RegisterBallType(const char * ballFile, const char * ballId, const char * ballName, const char * objName,
                                  float friction, float elasticity, float mass, const char * collGroup,
                                  float linearDamp, float rotDamp, float force, float radius) = 0;
    virtual void RegisterFloorType(const char * floorName, float friction, float elasticity, float mass,
                                   const char * collGroup, bool enableColl) = 0;
    virtual void RegisterModulBall(const char * modulName, bool fixed, float friction, float elasticity,
                                   float mass, const char * collGroup, bool frozen, bool enableColl,
                                   bool calcMassCenter, float linearDamp, float rotDamp, float radius) = 0;
    virtual void RegisterModulConvex(const char * modulName, bool fixed, float friction, float elasticity,
                                     float mass, const char * collGroup, bool frozen, bool enableColl,
                                     bool calcMassCenter, float linearDamp, float rotDamp) = 0;
    virtual void RegisterTrafo(const char * modulName) = 0;
    virtual void RegisterModul(const char * modulName) = 0;

    virtual int GetModCount() = 0;
    virtual IMod *GetMod(int index) = 0;

    virtual float GetSRScore() = 0;
    virtual int GetHSScore() = 0;

    virtual void SkipRenderForNextTick() = 0;
};

// ---------------------------------------------------------------------------
// IMod  (vtable order is ABI: do not reorder)
// ---------------------------------------------------------------------------
class BML_EXPORT IMod : public IMessageReceiver {
public:
    IMod(IBML *bml) : m_BML(bml) {};
    virtual ~IMod();

    virtual const char * GetID() = 0;
    virtual const char * GetVersion() = 0;
    virtual const char * GetName() = 0;
    virtual const char * GetAuthor() = 0;
    virtual const char * GetDescription() = 0;
    virtual BMLVersion GetBMLVersion() = 0;

    virtual void OnLoad() {};
    virtual void OnUnload() {};
    virtual void OnModifyConfig(const char * category, const char * key, IProperty *prop) {};

    virtual void OnLoadObject(const char * filename, BOOL isMap, const char * masterName,
                              unsigned int filterClass, BOOL addtoscene, BOOL reuseMeshes,
                              BOOL reuseMaterials, BOOL dynamic, XObjectArray *objArray,
                              CKObject *masterObj) {};
    virtual void OnLoadScript(const char * filename, CKBehavior *script) {};

    virtual void OnProcess() {};
    virtual void OnRender(CK_RENDER_FLAGS flags) {};

    virtual void OnCheatEnabled(bool enable) {};

    virtual void OnPhysicalize(CK3dEntity *target, CKBOOL fixed, float friction, float elasticity, float mass,
                               const char * collGroup, CKBOOL startFrozen, CKBOOL enableColl,
                               CKBOOL calcMassCenter, float linearDamp, float rotDamp, const char * collSurface,
                               VxVector massCenter, int convexCnt, CKMesh **convexMesh, int ballCnt,
                               VxVector *ballCenter, float *ballRadius, int concaveCnt,
                               CKMesh **concaveMesh) {};
    virtual void OnUnphysicalize(CK3dEntity *target) {};

protected:
    virtual ILogger *GetLogger() final;
    virtual IConfig *GetConfig() final;

    IBML *m_BML;

private:
    ILogger *m_logger = nullptr;
    IConfig *m_config = nullptr;
};

// ---------------------------------------------------------------------------
// ScriptHelper: imported from BML.dll (the 0.3.40 SDK exports all of these).
// ---------------------------------------------------------------------------
namespace ScriptHelper {
BML_EXPORT CKBehavior *CreateBB(CKBehavior *script, CKGUID guid, bool target = false);
BML_EXPORT CKParameterLocal *CreateLocalParameter(CKBehavior *script, const char * name, CKGUID type);
BML_EXPORT CKParameterLocal *CreateParamObject(CKBehavior *script, const char * name, CKGUID guid,
                                               CKObject *value);
BML_EXPORT CKParameterLocal *CreateParamString(CKBehavior *script, const char * name, const char * value);
BML_EXPORT void SetParamObject(CKParameter *param, CKObject *value);

template <typename T>
CKParameterLocal *CreateParamValue(CKBehavior *script, const char * name, CKGUID guid, T value) {
    CKParameterLocal *param = CreateLocalParameter(script, name, guid);
    if (param) {
        param->SetValue(&value, sizeof(T));
    }
    return param;
}

template <typename T>
void SetParamValue(CKParameter *param, T value) {
    if (param) {
        param->SetValue(&value, sizeof(T));
    }
}

BML_EXPORT void DeleteBB(CKBehavior *script, CKBehavior *beh);
} // namespace ScriptHelper

// ---------------------------------------------------------------------------
// ExecuteBB force helpers: absent from BML 0.3.40, implemented in
// BmlLegacyExecuteBB.cpp with ScriptHelper + the physics_RT GUIDs.
// ---------------------------------------------------------------------------
namespace ExecuteBB {
CKBehavior *CreateSetPhysicsForce(CKBehavior *script, CK3dEntity *target = nullptr,
                                  VxVector position = VxVector(), CK3dEntity *posRef = nullptr,
                                  VxVector direction = VxVector(), CK3dEntity *directionRef = nullptr,
                                  float force = 0.0f);

// The original helper keeps a hidden wake-up behavior; ours takes the owner
// script explicitly so no dangling behavior survives a level change.
void PhysicsWakeUp(CKBehavior *ownerScript, CK3dEntity *target = nullptr);

// Drops the cached wake-up behavior.  Called from the level teardown path,
// before the owner script can be destroyed by the engine.
void ForgetCachedWakeUp();
} // namespace ExecuteBB
