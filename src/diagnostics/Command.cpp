#include "mod/FlexGravityMod.h"

namespace {

enum class DebugAction {
    Status,
    List,
    FieldStatus,
    Viz,
    Custom,
    Expr,
    Domain,
    FreeMode,
    Reload,
    Help
};

struct DebugCommand {
    const char *name;
    const char *syntax;
    DebugAction action;
};

// One table drives dispatch, the help line and tab completion, so a new debug
// command is a single row here.
constexpr DebugCommand kDebugCommands[] = {
        {"status", "status", DebugAction::Status},
        {"list", "list", DebugAction::List},
        {"fstatus", "fstatus", DebugAction::FieldStatus},
        {"viz", "viz", DebugAction::Viz},
        {"custom", "custom [at x y z]", DebugAction::Custom},
        {"expr", "expr [at x y z] <expr>", DebugAction::Expr},
        {"domain", "domain [at x y z] <expression>", DebugAction::Domain},
        {"freemode", "freemode 0|1|true|false", DebugAction::FreeMode},
        {"reload", "reload", DebugAction::Reload},
        {"help", "help", DebugAction::Help},
};

constexpr size_t kDebugCommandCount = sizeof(kDebugCommands) / sizeof(kDebugCommands[0]);

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string DebugUsage() {
    std::string text = "FlexGravity debug: ";
    bool first = true;
    for (const DebugCommand &command : kDebugCommands) {
        if (!first) {
            text += " | ";
        }
        text += command.syntax;
        first = false;
    }
    return text;
}

const DebugCommand *FindDebugCommand(const std::string &name) {
    for (const DebugCommand &command : kDebugCommands) {
        if (name == command.name) {
            return &command;
        }
    }
    return nullptr;
}

std::vector<std::string> DebugCommandNames() {
    std::vector<std::string> names;
    names.reserve(kDebugCommandCount);
    for (const DebugCommand &command : kDebugCommands) {
        names.emplace_back(command.name);
    }
    return names;
}

std::string JoinFrom(const std::vector<std::string> &args, size_t first) {
    std::string text;
    for (size_t index = first; index < args.size(); ++index) {
        if (!text.empty()) {
            text += ' ';
        }
        text += args[index];
    }
    return text;
}

std::string Trim(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// The console splits the line on whitespace only (BML+'s ParseCommandLine does
// not treat quotes specially), so "x + 1" arrives as three tokens: join them
// back and strip one layer of quotes, the same convention BML's own commands
// use.
std::string JoinProbeText(const std::vector<std::string> &args, size_t first) {
    std::string text = Trim(JoinFrom(args, first));
    if (text.size() >= 2) {
        const char front = text.front();
        const char back = text.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
            text = Trim(text.substr(1, text.size() - 2));
        }
    }
    return text;
}

// "at <x> <y> <z>" is optional in front of every probe; everything after it is
// the expression / relation text.
bool SplitProbeArguments(const std::vector<std::string> &args, size_t first, VxVector &point,
                         bool &hasPoint, std::string &text, std::string &problem) {
    hasPoint = false;
    if (first < args.size() && Lowercase(args[first]) == "at") {
        if (args.size() < first + 4) {
            problem = "Usage: ... at <x> <y> <z>";
            return false;
        }
        float values[3] = {0.0f, 0.0f, 0.0f};
        for (int axis = 0; axis < 3; ++axis) {
            const std::string &token = args[first + 1 + static_cast<size_t>(axis)];
            char *end = nullptr;
            const float value = std::strtof(token.c_str(), &end);
            if (end == token.c_str() || (end && *end != '\0')) {
                problem = "Invalid float: " + token;
                return false;
            }
            values[axis] = value;
        }
        point = VxVector(values[0], values[1], values[2]);
        hasPoint = true;
        text = JoinProbeText(args, first + 4);
        return true;
    }
    text = JoinProbeText(args, first);
    return true;
}

} // namespace

// While a console command runs, log writes are allowed even with EnableLog off:
// the player asked for this output explicitly, and `list` has no in-game
// counterpart.  Outside a command, EnableLog alone decides whether anything is
// written at all.
namespace {

struct CommandLogScope {
    explicit CommandLogScope(FlexGravityMod *mod) : m_Mod(mod) { m_Mod->BeginCommandScope(); }
    ~CommandLogScope() { m_Mod->EndCommandScope(); }
    FlexGravityMod *m_Mod;
};

} // namespace

void FlexGravityCommand::Execute(IBML *bml, const std::vector<std::string> &args) {
    if (!m_Mod) {
        return;
    }
    CommandLogScope logScope(m_Mod);

    // fg
    if (args.size() < 2) {
        bml->SendIngameMessage("FlexGravity: usage: fg debug <command>");
        return;
    }

    // fg <group>
    if (Lowercase(args[1]) != "debug") {
        const std::string message = "FlexGravity: unknown group '" + args[1] + "'. Try: fg debug";
        bml->SendIngameMessage(message.c_str());
        return;
    }

    if (!m_Mod->IsDebugMode()) {
        bml->SendIngameMessage(
                "FlexGravity: debug commands are disabled (set Debug/DebugMode=1 in FlexGravity.cfg).");
        return;
    }

    // fg debug
    if (args.size() < 3) {
        bml->SendIngameMessage(DebugUsage().c_str());
        return;
    }

    // fg debug <command> [args...]  ->  re-index so handlers see the old layout
    std::vector<std::string> subArgs;
    subArgs.reserve(args.size() - 1);
    subArgs.push_back(args[0]);
    for (size_t index = 2; index < args.size(); ++index) {
        subArgs.push_back(args[index]);
    }

    const std::string sub = Lowercase(subArgs[1]);
    const DebugCommand *command = sub == "?" ? FindDebugCommand("help") : FindDebugCommand(sub);
    if (!command) {
        bml->SendIngameMessage("Unknown FlexGravity debug command. Try: fg debug help");
        return;
    }

    switch (command->action) {
        case DebugAction::Status:
            m_Mod->PrintStatus(bml);
            break;
        case DebugAction::List:
            m_Mod->PrintList(bml);
            break;
        case DebugAction::FieldStatus:
            m_Mod->PrintFieldStatus(bml);
            break;
        case DebugAction::Viz:
            m_Mod->PrintVizStatus(bml);
            break;
        case DebugAction::Custom:
        case DebugAction::Expr:
        case DebugAction::Domain: {
            VxVector point(0.0f, 0.0f, 0.0f);
            bool hasPoint = false;
            std::string text;
            std::string problem;
            if (!SplitProbeArguments(subArgs, 2, point, hasPoint, text, problem)) {
                bml->SendIngameMessage(("FlexGravity: " + problem).c_str());
                break;
            }
            if (command->action == DebugAction::Custom) {
                m_Mod->PrintCustomStatus(bml, hasPoint, point);
            } else if (command->action == DebugAction::Expr) {
                m_Mod->PrintExpressionProbe(bml, hasPoint, point, text);
            } else {
                m_Mod->PrintDomainProbe(bml, hasPoint, point, text);
            }
            break;
        }
        case DebugAction::FreeMode: {
            if (subArgs.size() < 3) {
                bml->SendIngameMessage(
                        ("FlexGravity: usage: fg debug freemode 0|1|true|false (currently " +
                         std::string(m_Mod->FreeModeEnabled() ? "on" : "off") + ")")
                                .c_str());
                break;
            }
            const std::string value = Lowercase(subArgs[2]);
            if (value == "1" || value == "true") {
                m_Mod->SetFreeMode(bml, true);
            } else if (value == "0" || value == "false") {
                m_Mod->SetFreeMode(bml, false);
            } else {
                bml->SendIngameMessage(
                        "FlexGravity: freemode expects 0, 1, true or false.");
            }
            break;
        }
        case DebugAction::Reload:
            m_Mod->Reload(bml);
            break;
        case DebugAction::Help:
            bml->SendIngameMessage(DebugUsage().c_str());
            break;
    }
}

const std::vector<std::string> FlexGravityCommand::GetTabCompletion(
        IBML *bml, const std::vector<std::string> &args) {
    (void) bml;

    if (args.size() <= 1) {
        return {"debug"};
    }
    if (args.size() == 2) {
        const std::string lowered = Lowercase(args[1]);
        if (lowered.empty()) {
            return {"debug"};
        }
        if (std::string("debug").compare(0, lowered.size(), lowered) == 0) {
            if (lowered == "debug" && m_Mod && m_Mod->IsDebugMode()) {
                return DebugCommandNames();
            }
            return {"debug"};
        }
        return {};
    }
    if (args.size() == 3) {
        if (m_Mod && m_Mod->IsDebugMode() && Lowercase(args[1]) == "debug") {
            return DebugCommandNames();
        }
        return {};
    }
    return {};
}
