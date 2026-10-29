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
    And,
    Or,
    Not,
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
            token.text = Lowercase(text.substr(start, index - start));
            // Domain keywords are reserved everywhere: `1 and y` must not turn
            // into `1*and*y`, and a field column may not use them as names.
            if (token.text == "and") {
                token.kind = TokenKind::And;
            } else if (token.text == "or") {
                token.kind = TokenKind::Or;
            } else if (token.text == "not") {
                token.kind = TokenKind::Not;
            } else {
                token.kind = TokenKind::Identifier;
            }
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
            token.kind = TokenKind::Not;
            ++index;
            break;
        case '&':
            if (index + 1 < size && text[index + 1] == '&') {
                token.kind = TokenKind::And;
                index += 2;
                break;
            }
            return FailAt(error, "use 'and' or '&&' for logical AND", index);
        case '|':
            if (index + 1 < size && text[index + 1] == '|') {
                token.kind = TokenKind::Or;
                index += 2;
                break;
            }
            return FailAt(error, "use 'or' or '||' for logical OR", index);
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
            // `and` / `or` / `not` are keywords, never values: never glue them
            // to a neighbouring value with an implicit '*'.
            if (current.kind == TokenKind::And || current.kind == TokenKind::Or ||
                current.kind == TokenKind::Not || previous.kind == TokenKind::And ||
                previous.kind == TokenKind::Or || previous.kind == TokenKind::Not) {
                expanded.push_back(current);
                continue;
            }
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
    Parser(const std::vector<Token> &tokens, unsigned variableMask, CompileError &error,
           size_t &index)
        : m_tokens(tokens), m_mask(variableMask), m_error(error), m_index(index) {}

    // Parses one arithmetic expression starting at the shared cursor and leaves
    // the cursor on the first token that does not belong to it.  Domain reuses
    // this for the two sides of every comparison.
    bool ParseArithmetic(Program &out) {
        if (!m_error.ok) {
            return false;
        }
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

    size_t Index() const { return m_index; }
    void SetIndex(size_t index) { m_index = index; }

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

        case TokenKind::And:
        case TokenKind::Or:
        case TokenKind::Not:
            return Fail("'and' / 'or' / 'not' are only allowed in Domain", token.position);

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
    size_t &m_index;
    Program m_program;
    int m_depth = 0;
    int m_maxDepth = 0;
};

// Boolean-domain parser on top of the arithmetic Parser.  `(x+1)>0` and
// `(x>0)` both start with '(' — the first is a comparison whose left side is a
// parenthesised arithmetic expression, the second is a parenthesised boolean.
// Primary therefore tries the boolean group and backtracks to a comparison.
class DomainParser {
public:
    DomainParser(Parser &arith, Domain &domain, CompileError &error)
        : m_arith(arith), m_domain(domain), m_error(error) {}

    bool Parse(int &root) { return ParseOr(root); }

private:
    bool Fail(const std::string &message) {
        if (m_error.ok) {
            m_error.ok = false;
            m_error.message = message;
            m_error.position = m_arith.Current().position;
        }
        return false;
    }

    int AddNode(const BoolNode &node) {
        m_domain.nodes.push_back(node);
        return static_cast<int>(m_domain.nodes.size()) - 1;
    }

    int AddCompare(Relation relation) {
        m_domain.relations.push_back(std::move(relation));
        BoolNode node;
        node.kind = BoolKind::Compare;
        node.a = static_cast<int>(m_domain.relations.size()) - 1;
        return AddNode(node);
    }

    int AddBinary(BoolKind kind, int a, int b) {
        BoolNode node;
        node.kind = kind;
        node.a = a;
        node.b = b;
        return AddNode(node);
    }

    bool TooLarge() const {
        return m_domain.nodes.size() > static_cast<size_t>(kMaxDomainNodes);
    }

    bool ParseOr(int &out) {
        int left = -1;
        if (!ParseAnd(left)) {
            return false;
        }
        while (m_arith.Current().kind == TokenKind::Or) {
            m_arith.Advance();
            int right = -1;
            if (!ParseAnd(right)) {
                return false;
            }
            left = AddBinary(BoolKind::Or, left, right);
            if (TooLarge()) {
                return Fail("Domain has too many terms");
            }
        }
        out = left;
        return true;
    }

    bool ParseAnd(int &out) {
        int left = -1;
        if (!ParseNot(left)) {
            return false;
        }
        while (m_arith.Current().kind == TokenKind::And) {
            m_arith.Advance();
            int right = -1;
            if (!ParseNot(right)) {
                return false;
            }
            left = AddBinary(BoolKind::And, left, right);
            if (TooLarge()) {
                return Fail("Domain has too many terms");
            }
        }
        out = left;
        return true;
    }

    // `not` binds the comparison that follows it: `not x=1` is `not (x=1)`.
    bool ParseNot(int &out) {
        if (m_arith.Current().kind == TokenKind::Not) {
            m_arith.Advance();
            if (++m_depth > kMaxDomainDepth) {
                return Fail("Domain is nested too deeply");
            }
            int child = -1;
            const bool ok = ParseNot(child);
            --m_depth;
            if (!ok) {
                return false;
            }
            BoolNode node;
            node.kind = BoolKind::Not;
            node.a = child;
            out = AddNode(node);
            return true;
        }
        return ParsePrimary(out);
    }

    bool ParsePrimary(int &out) {
        if (m_arith.Current().kind == TokenKind::LParen) {
            const size_t savedIndex = m_arith.Index();
            const CompileError savedError = m_error;
            const size_t savedNodes = m_domain.nodes.size();
            const size_t savedRelations = m_domain.relations.size();

            m_arith.Advance();
            if (++m_depth > kMaxDomainDepth) {
                --m_depth;
                return Fail("Domain is nested too deeply");
            }
            int inner = -1;
            const bool parsed = ParseOr(inner);
            const bool closed = parsed && m_arith.Current().kind == TokenKind::RParen;
            if (closed) {
                m_arith.Advance();
                // A comparison operator after ')' means the parentheses were
                // arithmetic, not boolean: `(x+1) > 0`.
                if (m_arith.Current().kind != TokenKind::RelOp) {
                    --m_depth;
                    out = inner;
                    return true;
                }
            }
            --m_depth;
            m_arith.SetIndex(savedIndex);
            m_error = savedError;
            m_domain.nodes.resize(savedNodes);
            m_domain.relations.resize(savedRelations);
        }
        return ParseComparison(out);
    }

    bool ParseComparison(int &out) {
        Program left;
        if (!m_arith.ParseArithmetic(left)) {
            return false;
        }
        if (m_arith.Current().kind != TokenKind::RelOp) {
            return Fail("expected one of = != < <= > >=");
        }

        int result = -1;
        while (m_arith.Current().kind == TokenKind::RelOp) {
            Relation relation;
            relation.op = m_arith.Current().op;
            m_arith.Advance();
            relation.left = std::move(left);
            if (!m_arith.ParseArithmetic(relation.right)) {
                return false;
            }
            // A chained comparison keeps the previous right-hand side as the
            // next left-hand side: 2 < x < 5  ==  (2 < x) and (x < 5).
            Program next = relation.right;

            const int node = AddCompare(std::move(relation));
            result = result < 0 ? node : AddBinary(BoolKind::And, result, node);
            if (TooLarge()) {
                return Fail("Domain has too many terms");
            }
            left = std::move(next);
        }
        out = result;
        return true;
    }

    Parser &m_arith;
    Domain &m_domain;
    CompileError &m_error;
    int m_depth = 0;
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

    size_t index = 0;
    Parser parser(expanded, variableMask, error, index);
    if (!parser.ParseArithmetic(program)) {
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

    if (text.empty()) {
        error.ok = false;
        error.message = "Domain is empty";
        error.position = 0;
        return false;
    }
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

    size_t index = 0;
    Parser parser(expanded, kMaskAll, error, index);
    DomainParser domainParser(parser, domain, error);
    int root = -1;
    if (!domainParser.Parse(root)) {
        return false;
    }
    if (!parser.AtEnd()) {
        const Token &token = parser.Current();
        if (token.kind == TokenKind::Comma) {
            return parser.Fail("a comma no longer means AND; use 'and'", token.position);
        }
        if (token.kind == TokenKind::Identifier) {
            return parser.Fail("missing logical operator between terms", token.position);
        }
        return parser.Fail("unexpected token after the Domain expression", token.position);
    }
    domain.root = root;
    return true;
}

namespace {

bool RelationHolds(const Relation &relation, const double *variables, bool *nonFinite) {
    const double left = EvaluateProgram(relation.left, variables);
    const double right = EvaluateProgram(relation.right, variables);
    if (!IsFinite(left) || !IsFinite(right)) {
        if (nonFinite) {
            *nonFinite = true;
        }
        return false;
    }

    switch (relation.op) {
    case RelOp::Equal:
        return std::fabs(left - right) <= 1e-6;
    case RelOp::NotEqual:
        return std::fabs(left - right) > 1e-6;
    case RelOp::Less:
        return left < right;
    case RelOp::LessEqual:
        return left <= right;
    case RelOp::Greater:
        return left > right;
    case RelOp::GreaterEqual:
        return left >= right;
    }
    return false;
}

bool EvaluateNode(const Domain &domain, int index, const double *variables, bool *nonFinite,
                  int depth) {
    if (index < 0 || index >= static_cast<int>(domain.nodes.size()) ||
        depth > kMaxDomainDepth) {
        return false;
    }
    const BoolNode &node = domain.nodes[static_cast<size_t>(index)];
    switch (node.kind) {
    case BoolKind::Compare:
        if (node.a < 0 || node.a >= static_cast<int>(domain.relations.size())) {
            return false;
        }
        return RelationHolds(domain.relations[static_cast<size_t>(node.a)], variables, nonFinite);
    case BoolKind::And:
        if (!EvaluateNode(domain, node.a, variables, nonFinite, depth + 1)) {
            return false;
        }
        return EvaluateNode(domain, node.b, variables, nonFinite, depth + 1);
    case BoolKind::Or:
        if (EvaluateNode(domain, node.a, variables, nonFinite, depth + 1)) {
            return true;
        }
        return EvaluateNode(domain, node.b, variables, nonFinite, depth + 1);
    case BoolKind::Not:
        return !EvaluateNode(domain, node.a, variables, nonFinite, depth + 1);
    }
    return false;
}

} // namespace

bool EvaluateDomain(const Domain &domain, const double *variables, bool *nonFinite) {
    if (nonFinite) {
        *nonFinite = false;
    }
    if (domain.root < 0) {
        return true;
    }
    return EvaluateNode(domain, domain.root, variables, nonFinite, 0);
}

} // namespace fg::core::expr
