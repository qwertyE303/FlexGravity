#pragma once

// Compiler for one field cell (a function expression) and for a CRegion Domain
// (a boolean expression).
//
// Function expressions keep the v1 grammar: the five implicit-multiplication
// rules, `^` right associative, 30 functions, and no relation operators.
//
// Domain is the v2 grammar: parentheses > comparison > not > and > or, chained
// comparisons (`2 < x < 5`), `and`/`&&`, `or`/`||`, `not`/`!`, and the six
// relation operators.  A comma no longer means AND (it is only a function
// argument separator).

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

// A flat boolean tree.  `a`/`b` are child node indices for And/Or/Not; for a
// Compare node `a` is the index into Domain::relations.  A flat tree keeps the
// whole Domain copyable without owning pointers.
enum class BoolKind : std::uint8_t { Compare, And, Or, Not };

struct BoolNode {
    BoolKind kind = BoolKind::Compare;
    int a = -1;
    int b = -1;
};

struct Domain {
    std::vector<BoolNode> nodes;
    std::vector<Relation> relations;
    int root = -1;
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

// Defensive limits for a malformed Domain cell.
inline constexpr int kMaxDomainNodes = 256;
inline constexpr int kMaxDomainDepth = 64;

bool CompileExpression(const std::string &text, unsigned variableMask, Program &program,
                       CompileError &error);
bool CompileDomain(const std::string &text, Domain &domain, CompileError &error);

// Short-circuit evaluation.  `nonFinite` is set when a relation that was
// actually evaluated had a NaN/Inf operand; that relation does not hold.
bool EvaluateDomain(const Domain &domain, const double *variables, bool *nonFinite);

} // namespace fg::core::expr
