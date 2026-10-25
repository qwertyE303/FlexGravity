#pragma once

// Engine-free expression kernel for the FG_Custom rows.
//
// A source string is compiled once (Lexer + Parser) into a flat postfix program;
// evaluating it is a stack walk with no allocation and no recursion.  Nothing in
// this file (or in Parser.h) knows about Virtools, so it is unit tested without
// the game.

#include <cstdint>
#include <string>
#include <vector>

namespace fg::core::expr {

// The six variables, in the order the specification lists them.
enum VariableSlot : int {
    kVarX = 0,
    kVarY = 1,
    kVarZ = 2,
    kVarR = 3,
    kVarTheta = 4,
    kVarPhi = 5,
    kVariableCount = 6
};

// Defensive compile-time limits, not gameplay parameters: they only bound the
// work a malformed (or hostile) array cell can request.
inline constexpr int kMaxStackDepth = 96;
inline constexpr int kMaxInstructions = 4096;
inline constexpr int kMaxCallArgs = 16;
inline constexpr int kMaxRelations = 64;
inline constexpr int kMaxExpressionBytes = 4096;

enum class OpCode : std::uint8_t {
    PushNumber,
    PushVariable,
    Negate,
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Call
};

enum class FunctionId : std::uint8_t {
    Sin,
    Cos,
    Tan,
    Sec,
    Csc,
    Cot,
    Asin,
    Acos,
    Atan,
    Atan2,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
    Ln,
    Log10,
    Log,
    Sqrt,
    Cbrt,
    Abs,
    Sign,
    Floor,
    Ceil,
    Round,
    Trunc,
    Min,
    Max,
    Mod,
    Count
};

struct Instruction {
    OpCode op = OpCode::PushNumber;
    std::uint8_t operand = 0; // variable slot, or FunctionId for Call
    std::uint8_t argc = 0;    // Call: number of arguments
    double number = 0.0;      // PushNumber
};

struct Program {
    std::vector<Instruction> code;
    int maxDepth = 0; // deepest the operand stack ever gets; bounded by construction
};

struct FunctionInfo {
    const char *name; // lowercase, ASCII
    FunctionId id;
    int minArgs;
    int maxArgs; // -1 = variadic (minArgs .. kMaxCallArgs)
};

const FunctionInfo *FunctionTable();
int FunctionCount();

// Lowercase name lookup; -1 when nothing matches.
int FindFunction(const std::string &lowercaseName);

// Variable slot for a lowercase name, or -1.  The per-column policy is applied
// by the caller (the parser), not here.
int FindVariable(const std::string &lowercaseName);

// Constants are case-insensitive too: pi, e.
bool IsConstant(const std::string &lowercaseName, double &value);

// Postfix evaluation.  `variables` holds kVariableCount entries in VariableSlot
// order.  Returns NaN when the program is malformed (never crashes).
double EvaluateProgram(const Program &program, const double *variables);

bool IsFinite(double value);

} // namespace fg::core::expr
