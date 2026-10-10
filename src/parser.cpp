#include "aether/parser.h"
#include "aether/heap_object.h"
#include <stdexcept>

namespace aether {

Parser::Parser(Lexer& lexer)
    : lexer_(lexer), current_token_(lexer.NextToken()) {}

Token Parser::Advance() {
    Token prev = current_token_;
    current_token_ = lexer_.NextToken();
    return prev;
}

Token Parser::Peek() {
    return current_token_;
}

bool Parser::Check(TokenKind kind) {
    return current_token_.kind == kind;
}

bool Parser::Match(TokenKind kind) {
    if (Check(kind)) {
        Advance();
        return true;
    }
    return false;
}

Token Parser::Consume(TokenKind kind, std::string_view error_message) {
    if (Check(kind)) {
        return Advance();
    }
    throw std::runtime_error(std::string("SyntaxError: ") + std::string(error_message) + 
                             " at line " + std::to_string(current_token_.location.line) + 
                             ", col " + std::to_string(current_token_.location.column) + 
                             " (found '" + current_token_.text + "')");
}

Precedence Parser::GetPrecedence(TokenKind kind) {
    switch (kind) {
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
            return Precedence::Assignment;
        case TokenKind::BarBar:
            return Precedence::LogicalOr;
        case TokenKind::AmpAmp:
            return Precedence::LogicalAnd;
        case TokenKind::Bar:
            return Precedence::BitwiseOr;
        case TokenKind::Caret:
            return Precedence::BitwiseXor;
        case TokenKind::Ampersand:
            return Precedence::BitwiseAnd;
        case TokenKind::EqualEqual:
        case TokenKind::StrictEqual:
        case TokenKind::NotEqual:
        case TokenKind::StrictNotEqual:
            return Precedence::Equality;
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
            return Precedence::Relational;
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
            return Precedence::Shift;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return Precedence::Additive;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            return Precedence::Multiplicative;
        case TokenKind::OpenParen:
            return Precedence::Call;
        case TokenKind::Dot:
            return Precedence::Member;
        default:
            return Precedence::None;
    }
}

std::unique_ptr<Program> Parser::ParseProgram() {
    auto prog = std::make_unique<Program>();
    while (!Check(TokenKind::EndOfFile)) {
        prog->AddStatement(ParseStatement());
    }
    return prog;
}

std::unique_ptr<Stmt> Parser::ParseStatement() {
    if (Match(TokenKind::Var) || Match(TokenKind::Let) || Match(TokenKind::Const)) {
        return ParseVarDecl();
    }
    if (Check(TokenKind::Function)) {
        return ParseFunctionDecl();
    }
    if (Match(TokenKind::If)) {
        return ParseIf();
    }
    if (Match(TokenKind::While)) {
        return ParseWhile();
    }
    if (Match(TokenKind::For)) {
        return ParseFor();
    }
    if (Match(TokenKind::Return)) {
        return ParseReturn();
    }
    if (Check(TokenKind::OpenBrace)) {
        return ParseBlock();
    }
    if (Match(TokenKind::Semicolon)) {
        return std::make_unique<BlockStmt>(); // Empty statement
    }
    return ParseExprStmt();
}

std::unique_ptr<Stmt> Parser::ParseVarDecl() {
    Token name_tok = Consume(TokenKind::Identifier, "Expected identifier in variable declaration");
    std::unique_ptr<Expr> init = nullptr;
    if (Match(TokenKind::Equal)) {
        init = ParseExpression();
    }
    Match(TokenKind::Semicolon); // Optional semicolon
    return std::make_unique<VarDeclStmt>(name_tok.text, std::move(init));
}

std::unique_ptr<Stmt> Parser::ParseFunctionDecl() {
    Consume(TokenKind::Function, "Expected 'function'");
    Token name_tok = Consume(TokenKind::Identifier, "Expected function name");
    Consume(TokenKind::OpenParen, "Expected '(' after function name");

    std::vector<std::string> params;
    if (!Check(TokenKind::CloseParen)) {
        Token param = Consume(TokenKind::Identifier, "Expected parameter name");
        params.push_back(param.text);
        while (Match(TokenKind::Comma)) {
            Token next_param = Consume(TokenKind::Identifier, "Expected parameter name after ','");
            params.push_back(next_param.text);
        }
    }
    Consume(TokenKind::CloseParen, "Expected ')' after parameters");

    std::unique_ptr<BlockStmt> body = ParseBlock();
    return std::make_unique<FunctionDeclStmt>(name_tok.text, std::move(params), std::move(body));
}

std::unique_ptr<Stmt> Parser::ParseIf() {
    Consume(TokenKind::OpenParen, "Expected '(' after 'if'");
    auto cond = ParseExpression();
    Consume(TokenKind::CloseParen, "Expected ')' after if condition");

    auto then_branch = ParseStatement();
    std::unique_ptr<Stmt> else_branch = nullptr;
    if (Match(TokenKind::Else)) {
        else_branch = ParseStatement();
    }
    return std::make_unique<IfStmt>(std::move(cond), std::move(then_branch), std::move(else_branch));
}

std::unique_ptr<Stmt> Parser::ParseWhile() {
    Consume(TokenKind::OpenParen, "Expected '(' after 'while'");
    auto cond = ParseExpression();
    Consume(TokenKind::CloseParen, "Expected ')' after while condition");

    auto body = ParseStatement();
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
}

std::unique_ptr<Stmt> Parser::ParseFor() {
    Consume(TokenKind::OpenParen, "Expected '(' after 'for'");

    std::unique_ptr<Stmt> init = nullptr;
    if (!Check(TokenKind::Semicolon)) {
        if (Match(TokenKind::Var) || Match(TokenKind::Let)) {
            init = ParseVarDecl();
        } else {
            init = ParseExprStmt();
        }
    } else {
        Advance(); // Semicolon
    }

    std::unique_ptr<Expr> cond = nullptr;
    if (!Check(TokenKind::Semicolon)) {
        cond = ParseExpression();
    }
    Consume(TokenKind::Semicolon, "Expected ';' after for loop condition");

    std::unique_ptr<Expr> update = nullptr;
    if (!Check(TokenKind::CloseParen)) {
        update = ParseExpression();
    }
    Consume(TokenKind::CloseParen, "Expected ')' after for loop update");

    auto body = ParseStatement();
    return std::make_unique<ForStmt>(std::move(init), std::move(cond), std::move(update), std::move(body));
}

std::unique_ptr<Stmt> Parser::ParseReturn() {
    std::unique_ptr<Expr> val = nullptr;
    if (!Check(TokenKind::Semicolon) && !Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        val = ParseExpression();
    }
    Match(TokenKind::Semicolon);
    return std::make_unique<ReturnStmt>(std::move(val));
}

std::unique_ptr<BlockStmt> Parser::ParseBlock() {
    Consume(TokenKind::OpenBrace, "Expected '{'");
    auto block = std::make_unique<BlockStmt>();
    while (!Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        block->AddStatement(ParseStatement());
    }
    Consume(TokenKind::CloseBrace, "Expected '}'");
    return block;
}

std::unique_ptr<Stmt> Parser::ParseExprStmt() {
    auto expr = ParseExpression();
    Match(TokenKind::Semicolon);
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::unique_ptr<Expr> Parser::ParseExpression(Precedence precedence) {
    auto lhs = ParsePrefix();

    while (precedence <= GetPrecedence(Peek().kind)) {
        Token op_token = Advance();
        lhs = ParseInfix(std::move(lhs), op_token);
    }

    return lhs;
}

std::unique_ptr<Expr> Parser::ParsePrefix() {
    Token tok = Advance();
    switch (tok.kind) {
        case TokenKind::Number: {
            if (tok.is_integer) {
                return std::make_unique<LiteralExpr>(TaggedValue::FromSmi(tok.int_value));
            }
            return std::make_unique<LiteralExpr>(TaggedValue::FromDouble(tok.number_value));
        }
        case TokenKind::String: {
            auto* s = new JSString(tok.text);
            return std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(s));
        }
        case TokenKind::True:
            return std::make_unique<LiteralExpr>(TaggedValue::True());
        case TokenKind::False:
            return std::make_unique<LiteralExpr>(TaggedValue::False());
        case TokenKind::Null:
            return std::make_unique<LiteralExpr>(TaggedValue::Null());
        case TokenKind::Undefined:
            return std::make_unique<LiteralExpr>(TaggedValue::Undefined());
        case TokenKind::Identifier:
            return std::make_unique<IdentifierExpr>(tok.text);
        case TokenKind::OpenParen: {
            auto expr = ParseExpression();
            Consume(TokenKind::CloseParen, "Expected ')' after expression");
            return expr;
        }
        case TokenKind::OpenBrace: {
            // Object literal: { key: val, ... }
            std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props;
            if (!Check(TokenKind::CloseBrace)) {
                do {
                    Token key_tok = Advance();
                    if (key_tok.kind != TokenKind::Identifier && key_tok.kind != TokenKind::String) {
                        throw std::runtime_error("Expected property key in object literal");
                    }
                    Consume(TokenKind::Colon, "Expected ':' after property key");
                    auto val = ParseExpression();
                    props.emplace_back(key_tok.text, std::move(val));
                } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
            }
            Consume(TokenKind::CloseBrace, "Expected '}' after object literal");
            return std::make_unique<ObjectLiteralExpr>(std::move(props));
        }
        case TokenKind::Exclamation:
            return std::make_unique<UnaryExpr>(UnaryOp::LogicalNot, ParseExpression(Precedence::Unary));
        case TokenKind::Minus:
            return std::make_unique<UnaryExpr>(UnaryOp::Negate, ParseExpression(Precedence::Unary));
        case TokenKind::Plus:
            return std::make_unique<UnaryExpr>(UnaryOp::Plus, ParseExpression(Precedence::Unary));
        case TokenKind::Tilde:
            return std::make_unique<UnaryExpr>(UnaryOp::BitwiseNot, ParseExpression(Precedence::Unary));
        default:
            throw std::runtime_error(std::string("SyntaxError: Unexpected token '") + tok.text + 
                                     "' at line " + std::to_string(tok.location.line) + 
                                     ", col " + std::to_string(tok.location.column));
    }
}

std::unique_ptr<Expr> Parser::ParseInfix(std::unique_ptr<Expr> lhs, Token op_token) {
    if (op_token.kind == TokenKind::Equal) {
        // Assignment
        auto rhs = ParseExpression(Precedence::Assignment);
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            return std::make_unique<AssignExpr>(id->Name(), std::move(rhs));
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            return std::make_unique<PropertyAssignExpr>(std::move(obj), prop_name, std::move(rhs));
        }
        throw std::runtime_error("Invalid assignment target");
    }

    if (op_token.kind == TokenKind::Dot) {
        Token prop_tok = Consume(TokenKind::Identifier, "Expected property identifier after '.'");
        return std::make_unique<PropertyAccessExpr>(std::move(lhs), prop_tok.text);
    }

    if (op_token.kind == TokenKind::OpenParen) {
        std::vector<std::unique_ptr<Expr>> args;
        if (!Check(TokenKind::CloseParen)) {
            args.push_back(ParseExpression());
            while (Match(TokenKind::Comma)) {
                args.push_back(ParseExpression());
            }
        }
        Consume(TokenKind::CloseParen, "Expected ')' after argument list");
        return std::make_unique<CallExpr>(std::move(lhs), std::move(args));
    }

    Precedence prec = GetPrecedence(op_token.kind);
    auto rhs = ParseExpression(static_cast<Precedence>(static_cast<int>(prec) + 1));

    BinaryOp op;
    switch (op_token.kind) {
        case TokenKind::Plus:           op = BinaryOp::Add; break;
        case TokenKind::Minus:          op = BinaryOp::Sub; break;
        case TokenKind::Star:           op = BinaryOp::Mul; break;
        case TokenKind::Slash:          op = BinaryOp::Div; break;
        case TokenKind::Percent:        op = BinaryOp::Mod; break;
        case TokenKind::Ampersand:      op = BinaryOp::BitwiseAnd; break;
        case TokenKind::Bar:            op = BinaryOp::BitwiseOr; break;
        case TokenKind::Caret:          op = BinaryOp::BitwiseXor; break;
        case TokenKind::LessLess:       op = BinaryOp::ShiftLeft; break;
        case TokenKind::GreaterGreater: op = BinaryOp::ShiftRight; break;
        case TokenKind::EqualEqual:     op = BinaryOp::Equal; break;
        case TokenKind::StrictEqual:    op = BinaryOp::StrictEqual; break;
        case TokenKind::NotEqual:       op = BinaryOp::NotEqual; break;
        case TokenKind::StrictNotEqual: op = BinaryOp::StrictNotEqual; break;
        case TokenKind::Less:           op = BinaryOp::LessThan; break;
        case TokenKind::LessEqual:      op = BinaryOp::LessThanOrEqual; break;
        case TokenKind::Greater:        op = BinaryOp::GreaterThan; break;
        case TokenKind::GreaterEqual:   op = BinaryOp::GreaterThanOrEqual; break;
        default:
            throw std::runtime_error("Unhandled binary operator");
    }

    return std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
}

} // namespace aether
