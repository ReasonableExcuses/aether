#ifndef AETHER_PARSER_H
#define AETHER_PARSER_H

#include "aether/common.h"
#include "aether/lexer.h"
#include "aether/ast.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>

namespace aether {

enum class Precedence {
    None = 0,
    Comma,          // ,
    Assignment,     // = += -= *= /= %= **= <<= >>= >>>= &= |= ^= &&= ||= ??=
    Conditional,    // ? :
    LogicalOr,      // ||
    LogicalAnd,     // &&
    BitwiseOr,      // |
    BitwiseXor,     // ^
    BitwiseAnd,     // &
    Equality,       // == != === !==
    Relational,     // < <= > >=
    Shift,          // << >> >>>
    Additive,       // + -
    Multiplicative, // * / %
    Exponentiation, // **
    Unary,          // ! - + ~ typeof void delete
    Call,           // ()
    Member,         // .
    Primary
};

class Parser {
public:
    explicit Parser(Lexer& lexer);

    // Parsing inhibits collection while AST literals are unpublished. Emit the
    // returned AST before the next explicit GC, or hold an enclosing NoGCScope.
    std::unique_ptr<Program> ParseProgram();
    std::unique_ptr<Stmt> ParseStatement();
    std::unique_ptr<Expr> ParseExpression(Precedence precedence = Precedence::Assignment);

    void SetModule(bool is_module = true) {
        is_module_ = is_module;
        if (is_module_) {
            strict_mode_ = true;
            lexer_.SetStrictMode(true);
        }
    }
    bool IsModule() const { return is_module_; }
    void SetStrictMode(bool s) {
        strict_mode_ = s;
        lexer_.SetStrictMode(s);
    }
    bool StrictMode() const { return strict_mode_; }

private:
    friend struct FunctionContextGuard;
    Token Advance();
    Token Peek();
    bool Check(TokenKind kind);
    bool Match(TokenKind kind);
    Token Consume(TokenKind kind, std::string_view error_message);

    Precedence GetPrecedence(TokenKind kind);

    // Statement parsers
    std::unique_ptr<Stmt> ParseVarDecl(bool is_const = false, VarDeclStmt::Kind kind = VarDeclStmt::Kind::Var);
    std::unique_ptr<Stmt> ParseFunctionDecl(bool is_async = false);
    std::unique_ptr<Stmt> ParseIf();
    std::unique_ptr<Stmt> ParseWhile();
    std::unique_ptr<Stmt> ParseDoWhile();
    std::unique_ptr<Stmt> ParseFor(bool is_for_await = false);
    std::unique_ptr<Stmt> ParseBreak();
    std::unique_ptr<Stmt> ParseContinue();
    std::unique_ptr<Stmt> ParseSwitch();
    std::unique_ptr<Stmt> ParseDebugger();
    std::unique_ptr<Stmt> ParseReturn();
    std::unique_ptr<Stmt> ParseThrow();
    std::unique_ptr<Stmt> ParseTryCatch();
    std::unique_ptr<Stmt> ParseClassDecl();
    struct ParsedClassBody {
        struct Field {
            std::string name;
            std::unique_ptr<Expr> initializer;
        };
        std::vector<std::unique_ptr<FunctionDeclStmt>> methods;
        std::vector<Field> fields;
    };
    ParsedClassBody ParseClassElements(std::string_view super_name);
    std::unique_ptr<Stmt> ParseWith();
    std::unique_ptr<BlockStmt> ParseBlock();
    std::unique_ptr<Stmt> ParseExprStmt();
    std::unique_ptr<Stmt> ParseImportDecl();
    std::unique_ptr<Stmt> ParseExportDecl();
    bool MatchContextualKeyword(std::string_view kw);
    void ValidateSubstatement(const Stmt* stmt, bool is_if = false);

    // Expression parsers
    std::unique_ptr<Expr> ParsePrefix(Precedence precedence = Precedence::Assignment);
    std::unique_ptr<Expr> ParseInfix(std::unique_ptr<Expr> lhs, Token op_token);
    std::unique_ptr<CallExpr> MakeTaggedTemplateCall(std::unique_ptr<Expr> tag, Token op_token);
    void ConsumeSemicolon();

    Lexer& lexer_;
    Token current_token_;
    bool in_derived_constructor_{false};
    bool strict_mode_{false};
    bool allow_in_{true};
    bool in_generator_{false};
    bool in_async_{false};
    bool is_module_{false};
    uint32_t function_depth_{0};
    std::vector<std::unordered_set<std::string>> declared_private_names_stack_;
};

} // namespace aether

#endif // AETHER_PARSER_H
