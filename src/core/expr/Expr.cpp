#include "core/expr/Expr.h"

#include <cmath>
#include <cstring>

namespace fg::core::expr {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEuler = 2.71828182845904523536;

const FunctionInfo kFunctions[] = {
        {"sin", FunctionId::Sin, 1, 1},
        {"cos", FunctionId::Cos, 1, 1},
        {"tan", FunctionId::Tan, 1, 1},
        {"sec", FunctionId::Sec, 1, 1},
        {"csc", FunctionId::Csc, 1, 1},
        {"cot", FunctionId::Cot, 1, 1},
        {"asin", FunctionId::Asin, 1, 1},
        {"acos", FunctionId::Acos, 1, 1},
        {"atan", FunctionId::Atan, 1, 1},
        {"atan2", FunctionId::Atan2, 2, 2},
        {"sinh", FunctionId::Sinh, 1, 1},
        {"cosh", FunctionId::Cosh, 1, 1},
        {"tanh", FunctionId::Tanh, 1, 1},
        {"asinh", FunctionId::Asinh, 1, 1},
        {"acosh", FunctionId::Acosh, 1, 1},
        {"atanh", FunctionId::Atanh, 1, 1},
        {"ln", FunctionId::Ln, 1, 1},
        {"log10", FunctionId::Log10, 1, 1},
        {"log", FunctionId::Log, 2, 2},
        {"sqrt", FunctionId::Sqrt, 1, 1},
        {"cbrt", FunctionId::Cbrt, 1, 1},
        {"abs", FunctionId::Abs, 1, 1},
        {"sign", FunctionId::Sign, 1, 1},
        {"floor", FunctionId::Floor, 1, 1},
        {"ceil", FunctionId::Ceil, 1, 1},
        {"round", FunctionId::Round, 1, 1},
        {"trunc", FunctionId::Trunc, 1, 1},
        {"min", FunctionId::Min, 2, -1},
        {"max", FunctionId::Max, 2, -1},
        {"mod", FunctionId::Mod, 2, 2},
};

constexpr int kFunctionCount = sizeof(kFunctions) / sizeof(kFunctions[0]);

const char *const kVariableNames[kVariableCount] = {"x", "y", "z", "r", "theta", "phi"};

double NaN() {
    return std::nan("");
}

// The conventions the specification pins down:
//   log(a,b)  base first, requires a > 0, a != 1, b > 0, otherwise NaN
//   sign(0)   = 0
//   round     C round ("away from zero" on .5)
//   mod       fmod (the remainder keeps the sign of the dividend)
double ApplyCall(FunctionId id, const double *arguments, int argc) {
    switch (id) {
    case FunctionId::Sin:
        return std::sin(arguments[0]);
    case FunctionId::Cos:
        return std::cos(arguments[0]);
    case FunctionId::Tan:
        return std::tan(arguments[0]);
    case FunctionId::Sec:
        return 1.0 / std::cos(arguments[0]);
    case FunctionId::Csc:
        return 1.0 / std::sin(arguments[0]);
    case FunctionId::Cot:
        return 1.0 / std::tan(arguments[0]);
    case FunctionId::Asin:
        return std::asin(arguments[0]);
    case FunctionId::Acos:
        return std::acos(arguments[0]);
    case FunctionId::Atan:
        return std::atan(arguments[0]);
    case FunctionId::Atan2:
        return std::atan2(arguments[0], arguments[1]);
    case FunctionId::Sinh:
        return std::sinh(arguments[0]);
    case FunctionId::Cosh:
        return std::cosh(arguments[0]);
    case FunctionId::Tanh:
        return std::tanh(arguments[0]);
    case FunctionId::Asinh:
        return std::asinh(arguments[0]);
    case FunctionId::Acosh:
        return std::acosh(arguments[0]);
    case FunctionId::Atanh:
        return std::atanh(arguments[0]);
    case FunctionId::Ln:
        return std::log(arguments[0]);
    case FunctionId::Log10:
        return std::log10(arguments[0]);
    case FunctionId::Log: {
        const double base = arguments[0];
        const double value = arguments[1];
        if (!(base > 0.0) || base == 1.0 || !(value > 0.0)) {
            return NaN();
        }
        return std::log(value) / std::log(base);
    }
    case FunctionId::Sqrt:
        return std::sqrt(arguments[0]);
    case FunctionId::Cbrt:
        return std::cbrt(arguments[0]);
    case FunctionId::Abs:
        return std::fabs(arguments[0]);
    case FunctionId::Sign: {
        const double value = arguments[0];
        if (value > 0.0) {
            return 1.0;
        }
        return value < 0.0 ? -1.0 : 0.0;
    }
    case FunctionId::Floor:
        return std::floor(arguments[0]);
    case FunctionId::Ceil:
        return std::ceil(arguments[0]);
    case FunctionId::Round:
        return std::round(arguments[0]);
    case FunctionId::Trunc:
        return std::trunc(arguments[0]);
    case FunctionId::Min: {
        double value = arguments[0];
        for (int index = 1; index < argc; ++index) {
            if (arguments[index] < value) {
                value = arguments[index];
            }
        }
        return value;
    }
    case FunctionId::Max: {
        double value = arguments[0];
        for (int index = 1; index < argc; ++index) {
            if (arguments[index] > value) {
                value = arguments[index];
            }
        }
        return value;
    }
    case FunctionId::Mod:
        return std::fmod(arguments[0], arguments[1]);
    case FunctionId::Count:
        break;
    }
    return NaN();
}

} // namespace

const FunctionInfo *FunctionTable() {
    return kFunctions;
}

int FunctionCount() {
    return kFunctionCount;
}

int FindFunction(const std::string &lowercaseName) {
    for (int index = 0; index < kFunctionCount; ++index) {
        if (lowercaseName == kFunctions[index].name) {
            return index;
        }
    }
    return -1;
}

int FindVariable(const std::string &lowercaseName) {
    for (int slot = 0; slot < kVariableCount; ++slot) {
        if (lowercaseName == kVariableNames[slot]) {
            return slot;
        }
    }
    return -1;
}

bool IsConstant(const std::string &lowercaseName, double &value) {
    if (lowercaseName == "pi") {
        value = kPi;
        return true;
    }
    if (lowercaseName == "e") {
        value = kEuler;
        return true;
    }
    return false;
}

double EvaluateProgram(const Program &program, const double *variables) {
    if (program.code.empty()) {
        return 0.0;
    }

    double stack[kMaxStackDepth];
    int top = 0;

    for (const Instruction &instruction : program.code) {
        switch (instruction.op) {
        case OpCode::PushNumber:
            if (top >= kMaxStackDepth) {
                return NaN();
            }
            stack[top++] = instruction.number;
            break;
        case OpCode::PushVariable:
            if (top >= kMaxStackDepth || instruction.operand >= kVariableCount) {
                return NaN();
            }
            stack[top++] = variables[instruction.operand];
            break;
        case OpCode::Negate:
            if (top < 1) {
                return NaN();
            }
            stack[top - 1] = -stack[top - 1];
            break;
        case OpCode::Add:
        case OpCode::Subtract:
        case OpCode::Multiply:
        case OpCode::Divide:
        case OpCode::Power: {
            if (top < 2) {
                return NaN();
            }
            const double rhs = stack[--top];
            double &lhs = stack[top - 1];
            switch (instruction.op) {
            case OpCode::Add:
                lhs += rhs;
                break;
            case OpCode::Subtract:
                lhs -= rhs;
                break;
            case OpCode::Multiply:
                lhs *= rhs;
                break;
            case OpCode::Divide:
                lhs /= rhs; // IEEE: inf / NaN, never a trap
                break;
            default: // Power
                lhs = std::pow(lhs, rhs);
                break;
            }
            break;
        }
        case OpCode::Call: {
            const int argc = static_cast<int>(instruction.argc);
            if (argc <= 0 || argc > kMaxCallArgs || top < argc) {
                return NaN();
            }
            double arguments[kMaxCallArgs];
            for (int index = argc - 1; index >= 0; --index) {
                arguments[index] = stack[--top];
            }
            stack[top++] = ApplyCall(static_cast<FunctionId>(instruction.operand), arguments, argc);
            break;
        }
        }
    }

    return top > 0 ? stack[top - 1] : NaN();
}

bool IsFinite(double value) {
    return std::isfinite(value) != 0;
}

} // namespace fg::core::expr
