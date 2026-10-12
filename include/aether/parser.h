#ifndef AETHER_PARSER_H
#define AETHER_PARSER_H

#include "aether/common.h"
#include "aether/lexer.h"
#include "aether/ast.h"
#include <memory>
#include <string>

namespace aether {

enum class Precedence {
    None = 0,
    Assignment,     // = += -= *= /=
    Conditional,    // ? :
    LogicalOr,      // ||
    LogicalAnd,     // &&
    BitwiseOr,      // |
    BitwiseXor,     // ^
    BitwiseAnd,     // &
    Equality,       // == != === !==
    Relational,     // < <= > >=
    Shift,          // << >>
    Additive,       // + -
    Multiplicative, // * / %
    Unary,          // ! - + ~
    Call,           // ()
    Member,         // .
    Primary
};

class Parser {
public:
    explicit Parser(Lexer& lexer);

    std::unique_ptr<Program> ParseProgram();
    std::unique_ptr<Stmt> ParseStatement();
    std::unique_ptr<Expr> ParseExpression(Precedence precedence = Precedence::Assignment);

private:
    Token Advance();
    Token Peek();
    bool Check(TokenKind kind);
    bool Match(TokenKind kind);
    Token Consume(TokenKind kind, std::string_view error_message);

    Precedence GetPrecedence(TokenKind kind);

    // Statement parsers
    std::unique_ptr<Stmt> ParseVarDecl();
    std::unique_ptr<Stmt> ParseFunctionDecl(bool is_async = false);
    std::unique_ptr<Stmt> ParseIf();
    std::unique_ptr<Stmt> ParseWhile();
    std::unique_ptr<Stmt> ParseFor(bool is_for_await = false);
    std::unique_ptr<Stmt> ParseReturn();
    std::unique_ptr<Stmt> ParseThrow();
    std::unique_ptr<Stmt> ParseTryCatch();
    std::unique_ptr<Stmt> ParseClassDecl();
    std::unique_ptr<BlockStmt> ParseBlock();
    std::unique_ptr<Stmt> ParseExprStmt();
    std::unique_ptr<Stmt> ParseImportDecl();
    std::unique_ptr<Stmt> ParseExportDecl();
    bool MatchContextualKeyword(std::string_view kw);

    // Expression parsers
    std::unique_ptr<Expr> ParsePrefix();
    std::unique_ptr<Expr> ParseInfix(std::unique_ptr<Expr> lhs, Token op_token);

    Lexer& lexer_;
    Token current_token_;
};

} // namespace aether

#endif // AETHER_PARSER_H
