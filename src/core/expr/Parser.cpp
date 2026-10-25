#include "core/expr/Parser.h"

#include <cmath>
#include <cstdlib>
#include <utility>

namespace fg::core::expr {
namespace {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class TokenKind {
    Number,
    Identifier,
    Plus,
    Minus,
    Star,
    Slash,
    Caret,
    LParen,
    RParen,
    Comma,
    RelOp,
    End
};

struct Token {
    TokenKind kind = TokenKind::End;
    double number = 0.0;
    std::string text; // identifiers, already lowercased
    RelOp op = RelOp::Equal;
    int position = 0;
};

bool FailAt(CompileError &error, const std::string &message, size_t position) {
    if (error.ok) {
        error.ok = false;
        error.message = message;
        error.position = static_cast<int>(position);
    }
    return false;
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAlphaNumeric(char c) {
    return IsAlpha(c) || IsDigit(c);
}

std::string Lowercase(const std::string &value) {
    std::string out = value;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool Tokenize(const std::string &text, std::vector<Token> &tokens, CompileError &error) {
    tokens.clear();
    const size_t size = text.size();
    size_t index = 0;

    while (index < size) {
        const unsigned char c = static_cast<unsigned char>(text[index]);

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++index;
            continue;
        }
        if (c >= 0x80) {
            return FailAt(error, "only ASCII characters are allowed", index);
        }

        // Numbers: 123, 1.5, .5, 5., 1e-3, 1E+3.  No hex, no underscores, no
        // Inf/NaN (those lex as identifiers and are rejected later).
        if (IsDigit(text[index]) || (text[index] == '.' && index + 1 < size && IsDigit(text[index + 1]))) {
            const size_t start = index;
            bool digits = false;
            while (index < size && IsDigit(text[index])) {
                ++index;
                digits = true;
            }
            if (index < size && text[index] == '.') {
                ++index;
                while (index < size && IsDigit(text[index])) {
                    ++index;
                    digits = true;
                }
            }
            if (!digits) {
                return FailAt(error, "malformed number", start);
            }
            if (index < size && (text[index] == 'e' || text[index] == 'E')) {
                size_t probe = index + 1;
                if (probe < size && (text[probe] == '+' || text[probe] == '-')) {
                    ++probe;
                }
                const size_t exponentStart = probe;
                while (probe < size && IsDigit(text[probe])) {
                    ++probe;
                }
                if (probe > exponentStart) {
                    index = probe;
                }
                // Otherwise leave the 'e' alone: "2e" is the number 2 times the
                // constant e, exactly like the implicit-multiplication rules say.
            }
            const std::string numberText = text.substr(start, index - start);
            Token token;
            token.kind = TokenKind::Number;
            token.number = std::strtod(numberText.c_str(), nullptr);
            token.position = static_cast<int>(start);
            tokens.push_back(token);
            continue;
        }

        if (IsAlpha(text[index]) || text[index] == '_') {
            const size_t start = index;
            while (index < size && (IsAlphaNumeric(text[index]) || text[index] == '_')) {
                ++index;
            }
            Token token;
            token.kind = TokenKind::Identifier;
            token.text = Lowercase(text.substr(start, index - start));
            token.position = static_cast<int>(start);
            tokens.push_back(token);
            continue;
        }

        Token token;
        token.position = static_cast<int>(index);
        switch (c) {
        case '+':
            token.kind = TokenKind::Plus;
            ++index;
            break;
        case '-':
            token.kind = TokenKind::Minus;
            ++index;
            break;
        case '*':
            if (index + 1 < size && text[index + 1] == '*') {
                return FailAt(error, "use '^' for powers, not '**'", index);
            }
            token.kind = TokenKind::Star;
            ++index;
            break;
        case '/':
            token.kind = TokenKind::Slash;
            ++index;
            break;
        case '^':
            token.kind = TokenKind::Caret;
            ++index;
            break;
        case '(':
            token.kind = TokenKind::LParen;
            ++index;
            break;
        case ')':
            token.kind = TokenKind::RParen;
            ++index;
            break;
        case ',':
            token.kind = TokenKind::Comma;
            ++index;
            break;
        case '=':
            if (index + 1 < size && text[index + 1] == '=') {
                return FailAt(error, "compare with '=' (not '==')", index);
            }
            token.kind = TokenKind::RelOp;
            token.op = RelOp::Equal;
            ++index;
            break;
        case '!':
            if (index + 1 < size && text[index + 1] == '=') {
                token.kind = TokenKind::RelOp;
                token.op = RelOp::NotEqual;
                index += 2;
                break;
            }
            return FailAt(error, "there is no '!' operator; use abs(x) for the absolute value", index);
        case '<':
            if (index + 1 < size && text[index + 1] == '=') {
                token.kind = TokenKind::RelOp;
                token.op = RelOp::LessEqual;
                index += 2;
            } else if (index + 1 < size && text[index + 1] == '>') {
                return FailAt(error, "use '!=' for not-equal (not '<>')", index);
            } else {
                token.kind = TokenKind::RelOp;
                token.op = RelOp::Less;
                ++index;
            }
            break;
        case '>':
            if (index + 1 < size && text[index + 1] == '=') {
                token.kind = TokenKind::RelOp;
                token.op = RelOp::GreaterEqual;
                index += 2;
            } else {
                token.kind = TokenKind::RelOp;
                token.op = RelOp::Greater;
                ++index;
            }
            break;
        case '%':
            return FailAt(error, "use mod(a,b) for the remainder (there is no '%' operator)", index);
        default:
            return FailAt(error, std::string("unexpected character '") + static_cast<char>(c) + "'", index);
        }
        tokens.push_back(token);
    }

    Token end;
    end.kind = TokenKind::End;
    end.position = static_cast<int>(size);
    tokens.push_back(end);
    return true;
}

// Implicit multiplication, exactly the five rules of the specification:
//   1. number or ")" followed by number / identifier / "(" -> "*"
//   2. variable or constant followed by "("               -> "*"
//   3. two identifiers                                    -> nothing (error later)
//   4. function name followed by "("                      -> a call, no "*"
//   5. function name followed by anything else            -> "needs parentheses"
bool InsertImplicitMultiplication(const std::vector<Token> &tokens, std::vector<Token> &expanded,
                                  CompileError &error) {
    expanded.clear();
    expanded.reserve(tokens.size() * 2);

    for (size_t index = 0; index < tokens.size(); ++index) {
        const Token &current = tokens[index];
        if (index > 0) {
            const Token &previous = tokens[index - 1];
            bool insert = false;
            switch (previous.kind) {
            case TokenKind::Number:
            case TokenKind::RParen:
                insert = current.kind == TokenKind::Number || current.kind == TokenKind::Identifier ||
                         current.kind == TokenKind::LParen;
                break;
            case TokenKind::Identifier: {
                if (FindFunction(previous.text) >= 0) {
                    if (current.kind != TokenKind::LParen) {
                        return FailAt(error, "function '" + previous.text + "' needs parentheses",
                                      static_cast<size_t>(previous.position));
                    }
                } else {
                    double constant = 0.0;
                    const bool isValue =
                            FindVariable(previous.text) >= 0 || IsConstant(previous.text, constant);
                    insert = isValue && current.kind == TokenKind::LParen;
                }
                break;
            }
            default:
                break;
            }
            if (insert) {
                Token star;
                star.kind = TokenKind::Star;
                star.position = current.position;
                expanded.push_back(star);
            }
        }
        expanded.push_back(current);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parser: recursive descent, emits postfix code directly
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(const std::vector<Token> &tokens, unsigned variableMask, CompileError &error)
        : m_tokens(tokens), m_mask(variableMask), m_error(error) {}

    bool ParseExpression(Program &out) {
        m_program = Program{};
        m_depth = 0;
        m_maxDepth = 0;
        if (!ParseAdditive()) {
            return false;
        }
        if (m_depth != 1) {
            return Fail("incomplete expression", Current().position);
        }
        out = m_program;
        out.maxDepth = m_maxDepth;
        return true;
    }

    const Token &Current() const { return m_tokens[m_index]; }

    bool AtEnd() const { return Current().kind == TokenKind::End; }

    void Advance() {
        if (m_index + 1 < m_tokens.size()) {
            ++m_index;
        }
    }

    bool Fail(const std::string &message, int position) {
        if (m_error.ok) {
            m_error.ok = false;
            m_error.message = message;
            m_error.position = position;
        }
        return false;
    }

private:
    bool ParseAdditive() {
        if (!m_error.ok) {
            return false;
        }
        if (!ParseMultiplicative()) {
            return false;
        }
        while (Current().kind == TokenKind::Plus || Current().kind == TokenKind::Minus) {
            const bool plus = Current().kind == TokenKind::Plus;
            Advance();
            if (!ParseMultiplicative()) {
                return false;
            }
            EmitOperator(plus ? OpCode::Add : OpCode::Subtract);
        }
        return m_error.ok;
    }

    bool ParseMultiplicative() {
        if (!m_error.ok) {
            return false;
        }
        if (!ParseUnary()) {
            return false;
        }
        while (Current().kind == TokenKind::Star || Current().kind == TokenKind::Slash) {
            const bool multiply = Current().kind == TokenKind::Star;
            Advance();
            if (!ParseUnary()) {
                return false;
            }
            EmitOperator(multiply ? OpCode::Multiply : OpCode::Divide);
        }
        return m_error.ok;
    }

    // Unary +/- bind tighter than * and / but looser than ^.
    bool ParseUnary() {
        if (!m_error.ok) {
            return false;
        }
        if (Current().kind == TokenKind::Plus) {
            Advance();
            return ParseUnary();
        }
        if (Current().kind == TokenKind::Minus) {
            Advance();
            if (!ParseUnary()) {
                return false;
            }
            EmitOperator(OpCode::Negate);
            return m_error.ok;
        }
        return ParsePower();
    }

    // ^ is right associative and its right operand may carry a sign (2^-1).
    bool ParsePower() {
        if (!m_error.ok) {
            return false;
        }
        if (!ParsePrimary()) {
            return false;
        }
        if (Current().kind == TokenKind::Caret) {
            Advance();
            if (!ParseUnary()) {
                return false;
            }
            EmitOperator(OpCode::Power);
        }
        return m_error.ok;
    }

    bool ParsePrimary() {
        if (!m_error.ok) {
            return false;
        }
        const Token &token = Current();
        switch (token.kind) {
        case TokenKind::Number:
            Advance();
            EmitNumber(token.number);
            return m_error.ok;

        case TokenKind::LParen:
            Advance();
            if (!ParseAdditive()) {
                return false;
            }
            if (Current().kind != TokenKind::RParen) {
                return Fail("missing ')'", Current().position);
            }
            Advance();
            return m_error.ok;

        case TokenKind::Identifier: {
            const std::string name = token.text;
            Advance();

            if (Current().kind == TokenKind::LParen) {
                const int functionIndex = FindFunction(name);
                if (functionIndex < 0) {
                    return Fail("unknown function '" + name + "'", token.position);
                }
                Advance();
                int argc = 0;
                if (Current().kind != TokenKind::RParen) {
                    while (true) {
                        if (!ParseAdditive()) {
                            return false;
                        }
                        ++argc;
                        if (argc > kMaxCallArgs) {
                            return Fail("too many arguments", token.position);
                        }
                        if (Current().kind == TokenKind::Comma) {
                            Advance();
                            if (Current().kind == TokenKind::RParen) {
                                return Fail("missing argument after ','", Current().position);
                            }
                            continue;
                        }
                        break;
                    }
                }
                if (Current().kind != TokenKind::RParen) {
                    return Fail("missing ')' in the call to '" + name + "'", Current().position);
                }
                Advance();
                const FunctionInfo &info = FunctionTable()[functionIndex];
                if (argc < info.minArgs || (info.maxArgs >= 0 && argc > info.maxArgs)) {
                    return Fail("wrong number of arguments for '" + name + "'", token.position);
                }
                EmitCall(info.id, argc);
                return m_error.ok;
            }

            const int variable = FindVariable(name);
            if (variable >= 0) {
                if ((m_mask & (1u << variable)) == 0) {
                    return Fail("variable '" + name + "' is not available in this column",
                                token.position);
                }
                EmitVariable(variable);
                return m_error.ok;
            }

            double constant = 0.0;
            if (IsConstant(name, constant)) {
                EmitNumber(constant);
                return m_error.ok;
            }

            if (FindFunction(name) >= 0) {
                return Fail("function '" + name + "' needs parentheses", token.position);
            }
            return Fail("unknown identifier '" + name + "'", token.position);
        }

        case TokenKind::RelOp:
            return Fail("relation operators (= != < <= > >=) are only allowed in Domain",
                        token.position);

        case TokenKind::End:
            return Fail("unexpected end of expression", token.position);

        default:
            return Fail("unexpected token", token.position);
        }
    }

    void EmitNumber(double value) {
        Instruction instruction;
        instruction.op = OpCode::PushNumber;
        instruction.number = value;
        Push(instruction);
    }

    void EmitVariable(int slot) {
        Instruction instruction;
        instruction.op = OpCode::PushVariable;
        instruction.operand = static_cast<std::uint8_t>(slot);
        Push(instruction);
    }

    void EmitOperator(OpCode op) {
        Instruction instruction;
        instruction.op = op;
        Push(instruction);
    }

    void EmitCall(FunctionId id, int argc) {
        Instruction instruction;
        instruction.op = OpCode::Call;
        instruction.operand = static_cast<std::uint8_t>(id);
        instruction.argc = static_cast<std::uint8_t>(argc);
        Push(instruction);
    }

    void Push(const Instruction &instruction) {
        if (m_program.code.size() >= static_cast<size_t>(kMaxInstructions)) {
            Fail("expression is too complex", Current().position);
            return;
        }
        m_program.code.push_back(instruction);

        switch (instruction.op) {
        case OpCode::PushNumber:
        case OpCode::PushVariable:
            ++m_depth;
            break;
        case OpCode::Negate:
            break;
        case OpCode::Call:
            m_depth -= static_cast<int>(instruction.argc) - 1;
            break;
        default:
            --m_depth;
            break;
        }

        if (m_depth > m_maxDepth) {
            m_maxDepth = m_depth;
        }
        if (m_depth > kMaxStackDepth) {
            Fail("expression is too deeply nested", Current().position);
        }
    }

    const std::vector<Token> &m_tokens;
    unsigned m_mask = kMaskAll;
    CompileError &m_error;
    size_t m_index = 0;
    Program m_program;
    int m_depth = 0;
    int m_maxDepth = 0;
};

bool CheckLength(const std::string &text, CompileError &error) {
    if (text.size() > static_cast<size_t>(kMaxExpressionBytes)) {
        error.ok = false;
        error.message = "expression is longer than " + std::to_string(kMaxExpressionBytes) + " bytes";
        error.position = kMaxExpressionBytes;
        return false;
    }
    return true;
}

} // namespace

bool CompileExpression(const std::string &text, unsigned variableMask, Program &program,
                       CompileError &error) {
    error = CompileError{};
    program = Program{};

    if (!CheckLength(text, error)) {
        return false;
    }

    std::vector<Token> tokens;
    if (!Tokenize(text, tokens, error)) {
        return false;
    }
    std::vector<Token> expanded;
    if (!InsertImplicitMultiplication(tokens, expanded, error)) {
        return false;
    }

    Parser parser(expanded, variableMask, error);
    if (!parser.ParseExpression(program)) {
        return false;
    }
    if (!parser.AtEnd()) {
        const Token &token = parser.Current();
        if (token.kind == TokenKind::Identifier) {
            return parser.Fail("missing operator between identifiers (write x*y)", token.position);
        }
        if (token.kind == TokenKind::RelOp) {
            return parser.Fail("relation operators are only allowed in Domain", token.position);
        }
        return parser.Fail("unexpected token after the expression", token.position);
    }
    return true;
}

bool CompileDomain(const std::string &text, Domain &domain, CompileError &error) {
    error = CompileError{};
    domain = Domain{};

    if (!CheckLength(text, error)) {
        return false;
    }

    std::vector<Token> tokens;
    if (!Tokenize(text, tokens, error)) {
        return false;
    }
    std::vector<Token> expanded;
    if (!InsertImplicitMultiplication(tokens, expanded, error)) {
        return false;
    }

    Parser parser(expanded, kMaskAll, error);

    while (true) {
        Relation relation;
        if (!parser.ParseExpression(relation.left)) {
            return false;
        }

        const Token &op = parser.Current();
        if (op.kind != TokenKind::RelOp) {
            return parser.Fail("each relation needs one of = != < <= > >=", op.position);
        }
        relation.op = op.op;
        const int opPosition = op.position;
        parser.Advance();

        if (!parser.ParseExpression(relation.right)) {
            return false;
        }
        domain.relations.push_back(std::move(relation));

        if (domain.relations.size() > static_cast<size_t>(kMaxRelations)) {
            return parser.Fail("too many relations", opPosition);
        }

        if (parser.Current().kind == TokenKind::Comma) {
            parser.Advance();
            if (parser.Current().kind == TokenKind::End) {
                return parser.Fail("empty relation after ','", parser.Current().position);
            }
            continue;
        }
        break;
    }

    if (!parser.AtEnd()) {
        const Token &token = parser.Current();
        if (token.kind == TokenKind::RelOp) {
            return parser.Fail("chained comparisons are not supported (use ',' for AND)",
                               token.position);
        }
        return parser.Fail("unexpected token after the last relation", token.position);
    }
    return true;
}

bool EvaluateDomain(const Domain &domain, const double *variables, bool *nonFinite) {
    if (nonFinite) {
        *nonFinite = false;
    }
    if (domain.relations.empty()) {
        return true;
    }

    for (const Relation &relation : domain.relations) {
        const double left = EvaluateProgram(relation.left, variables);
        const double right = EvaluateProgram(relation.right, variables);
        if (!IsFinite(left) || !IsFinite(right)) {
            if (nonFinite) {
                *nonFinite = true;
            }
            return false;
        }

        bool holds = false;
        switch (relation.op) {
        case RelOp::Equal:
            holds = std::fabs(left - right) <= 1e-6;
            break;
        case RelOp::NotEqual:
            holds = std::fabs(left - right) > 1e-6;
            break;
        case RelOp::Less:
            holds = left < right;
            break;
        case RelOp::LessEqual:
            holds = left <= right;
            break;
        case RelOp::Greater:
            holds = left > right;
            break;
        case RelOp::GreaterEqual:
            holds = left >= right;
            break;
        }
        if (!holds) {
            return false;
        }
    }
    return true;
}

} // namespace fg::core::expr
