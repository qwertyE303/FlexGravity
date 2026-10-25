#include "mod/FlexGravityMod.h"

namespace {

// BML+ only reports properties that the configuration file declares, so every
// read below is a no-op when the key is missing (an old cfg keeps working, the
// matching overlay switch is simply always off).
bool ReadSwitch(IConfig *config, const char *key, const char *comment, bool &target) {
    IProperty *property = config->GetProperty("Debug", key);
    if (!property) {
        return false;
    }
    property->SetDefaultBoolean(false);
    property->SetComment(comment);
    target = property->GetBoolean();
    return true;
}

bool ReadFloat(IConfig *config, const char *key, const char *comment, float defaultValue, float &target) {
    IProperty *property = config->GetProperty("Debug", key);
    if (!property) {
        return false;
    }
    property->SetDefaultFloat(defaultValue);
    property->SetComment(comment);
    target = property->GetFloat();
    return true;
}

} // namespace

void FlexGravityMod::OnLoad() {
    IConfig *config = GetConfig();
    if (config) {
        config->SetCategoryComment("Debug", "FlexGravity diagnostics.");
        IProperty *debug = config->GetProperty("Debug", "DebugMode");
        if (debug) {
            debug->SetDefaultBoolean(false);
            debug->SetComment("Master debug switch: overlay, debug commands and every diagnostic "
                              "switch below only work while this is on.");
            m_DebugMode = debug->GetBoolean();
        }

        // Debug overlay.  All of these live in Debug and default to off/safe.
        ReadSwitch(config, "EnableLog",
                   "Write detailed diagnostics to ModLoader.log (requires DebugMode).",
                   m_EnableLog);
        ReadSwitch(config, "ShowGVector",
                   "Draw a blue arrow at the active ball: it points along the total gravity "
                   "(vanilla plus every active field) and is labelled with its magnitude.",
                   m_ShowGVector);
        ReadSwitch(config, "EnableFieldLines",
                   "Draw red gravity field lines around the camera, traced along the total "
                   "gravity and cached in world space as the ball moves.",
                   m_EnableFieldLines);
        ReadSwitch(config, "EnableEquipotentials",
                   "Draw translucent yellow equipotential surfaces around the camera: shells of "
                   "constant potential, one every EquipotentialDensity units.",
                   m_EnableEquipotentials);
        ReadFloat(config, "FieldLineDensity",
                  "Field lines per distance x100: 100 means one line per unit at g = 20.  "
                  "Spacing = (100 / density) * sqrt(20 / |g|).",
                  10.0f, m_FieldLineDensity);
        ReadFloat(config, "EquipotentialDensity",
                  "Equipotential surfaces per distance x100, same scale as FieldLineDensity.",
                  10.0f, m_EquipotentialDensity);
        ReadFloat(config, "DebugViewDistance",
                  "Radius in world units of the sphere around the camera that the overlay draws in.",
                  100.0f, m_DebugViewDistance);
    }

    // Logging starts here: LogInfo/LogWarn are gated by DebugMode && EnableLog,
    // and both are only known once the configuration above has been read.
    LogInfo("FlexGravity %s loading (BML+ %s)", FLEXGRAVITY_VERSION, BML_VERSION);

    // The command is always registered; the debug subcommands check DebugMode
    // themselves, so turning it on/off takes effect immediately.
    m_BML->RegisterCommand(new FlexGravityCommand(this));

    m_BML->SendIngameMessage("FlexGravity " FLEXGRAVITY_VERSION " loaded.");
    LogInfo("FlexGravity %s loaded; DebugMode=%s EnableLog=%s, overlay: gvector=%s lines=%s (density %g) "
            "equipotentials=%s (density %g) distance=%g, console: fg debug <command>",
            FLEXGRAVITY_VERSION, m_DebugMode ? "on" : "off", m_EnableLog ? "on" : "off",
            BoolText(m_ShowGVector).c_str(), BoolText(m_EnableFieldLines).c_str(), m_FieldLineDensity,
            BoolText(m_EnableEquipotentials).c_str(), m_EquipotentialDensity, m_DebugViewDistance);
}

void FlexGravityMod::OnModifyConfig(const char *category, const char *key, IProperty *prop) {
    if (!category || !key || !prop) {
        return;
    }
    if (_stricmp(category, "Debug") != 0) {
        return;
    }
    // Every line below only matters while detailed logging is on; the switches
    // themselves take effect immediately either way.
    const auto noteSwitch = [this](const char *name, bool value) {
        if (DebugLogging()) {
            LogInfo("Config: Debug.%s=%s", name, value ? "true" : "false");
        }
    };
    const auto noteFloat = [this](const char *name, float value) {
        if (DebugLogging()) {
            LogInfo("Config: Debug.%s=%g", name, value);
        }
    };

    if (_stricmp(key, "DebugMode") == 0) {
        m_DebugMode = prop->GetBoolean();
        noteSwitch("DebugMode", m_DebugMode);
        return;
    }
    if (_stricmp(key, "EnableLog") == 0) {
        m_EnableLog = prop->GetBoolean();
        noteSwitch("EnableLog", m_EnableLog);
        return;
    }
    if (_stricmp(key, "ShowGVector") == 0) {
        m_ShowGVector = prop->GetBoolean();
        noteSwitch("ShowGVector", m_ShowGVector);
        return;
    }
    if (_stricmp(key, "EnableFieldLines") == 0) {
        m_EnableFieldLines = prop->GetBoolean();
        noteSwitch("EnableFieldLines", m_EnableFieldLines);
        return;
    }
    if (_stricmp(key, "EnableEquipotentials") == 0) {
        m_EnableEquipotentials = prop->GetBoolean();
        noteSwitch("EnableEquipotentials", m_EnableEquipotentials);
        return;
    }
    if (_stricmp(key, "FieldLineDensity") == 0) {
        m_FieldLineDensity = prop->GetFloat();
        noteFloat("FieldLineDensity", m_FieldLineDensity);
        return;
    }
    if (_stricmp(key, "EquipotentialDensity") == 0) {
        m_EquipotentialDensity = prop->GetFloat();
        noteFloat("EquipotentialDensity", m_EquipotentialDensity);
        return;
    }
    if (_stricmp(key, "DebugViewDistance") == 0) {
        m_DebugViewDistance = prop->GetFloat();
        noteFloat("DebugViewDistance", m_DebugViewDistance);
    }
}

MOD_EXPORT IMod *BMLEntry(IBML *bml) {
    return new FlexGravityMod(bml);
}

MOD_EXPORT void BMLExit(IMod *mod) {
    delete mod;
}
