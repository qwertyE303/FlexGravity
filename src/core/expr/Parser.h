#pragma once

// Compiler for one FG_Custom cell (a function expression) and for the Domain
// column (a comma-separated list of relations, ANDed).
//
// The grammar is the one in FG_Custom_Spec.md section 7.1, including implicit
// multiplication, which is inserted between tokens before parsing.

#include <cstdint>
#include <string>
#include <vector>

#include "core/expr/Expr.h"

namespace fg::core::expr {

enum class RelOp : std::uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct Relation {
    Program left;
    Program right;
    RelOp op = RelOp::Greater;
};

struct Domain {
    std::vector<Relation> relations;
};

struct CompileError {
    bool ok = true;
    std::string message;
    int position = -1; // byte offset into the source string, -1 when unknown
};

// Which variables a column may mention.  The specification keeps this a policy
// per column instead of hard-coding it, so it lives here as a mask.
inline constexpr unsigned kMaskRectangular = (1u << kVarX) | (1u << kVarY) | (1u << kVarZ);
inline constexpr unsigned kMaskSpherical = (1u << kVarR) | (1u << kVarTheta) | (1u << kVarPhi);
inline constexpr unsigned kMaskAll = (1u << kVariableCount) - 1u;

bool CompileExpression(const std::string &text, unsigned variableMask, Program &program,
                       CompileError &error);
bool CompileDomain(const std::string &text, Domain &domain, CompileError &error);

// Every relation must hold (comma = AND).  `nonFinite` is set when a side of a
// relation evaluated to NaN/Inf; such a relation never holds, which is what the
// specification asks for ("the row is inactive when the Domain is NaN").
bool EvaluateDomain(const Domain &domain, const double *variables, bool *nonFinite);

} // namespace fg::core::expr
