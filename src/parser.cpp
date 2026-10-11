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
        case TokenKind::AmpAmpEqual:
        case TokenKind::BarBarEqual:
        case TokenKind::QuestionQuestionEqual:
            return Precedence::Assignment;
        case TokenKind::Question:
            return Precedence::Conditional;
        case TokenKind::BarBar:
        case TokenKind::QuestionQuestion:
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
        case TokenKind::InstanceOf:
        case TokenKind::In:
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
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
            return Precedence::Call;
        case TokenKind::Dot:
        case TokenKind::QuestionDot:
        case TokenKind::OpenBracket:
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
    if (Check(TokenKind::Async)) {
        Lexer saved_lexer = lexer_;
        Token saved_tok = current_token_;
        Advance();
        if (Check(TokenKind::Function)) {
            return ParseFunctionDecl(/*is_async=*/true);
        }
        lexer_ = saved_lexer;
        current_token_ = saved_tok;
    }
    if (Check(TokenKind::Function)) {
        return ParseFunctionDecl(/*is_async=*/false);
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
    if (Match(TokenKind::Throw)) {
        return ParseThrow();
    }
    if (Match(TokenKind::Try)) {
        return ParseTryCatch();
    }
    if (Match(TokenKind::Class)) {
        return ParseClassDecl();
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
    // 1. Array destructuring: let [a, b, c] = expr;
    if (Match(TokenKind::OpenBracket)) {
        std::vector<std::string> names;
        if (!Check(TokenKind::CloseBracket)) {
            do {
                Token elem_tok = Consume(TokenKind::Identifier, "Expected identifier in array destructuring");
                names.push_back(elem_tok.text);
            } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBracket));
        }
        Consume(TokenKind::CloseBracket, "Expected ']' in array destructuring");
        Consume(TokenKind::Equal, "Expected '=' in destructuring declaration");
        auto rhs = ParseExpression();
        Match(TokenKind::Semicolon);

        static uint32_t s_destruct_arr_id = 0;
        std::string temp_var = "$d_arr_" + std::to_string(++s_destruct_arr_id);
        auto block = std::make_unique<BlockStmt>();
        block->AddStatement(std::make_unique<VarDeclStmt>(temp_var, std::move(rhs)));
        for (size_t i = 0; i < names.size(); ++i) {
            auto idx_access = std::make_unique<IndexAccessExpr>(
                std::make_unique<IdentifierExpr>(temp_var),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(static_cast<int32_t>(i)))
            );
            block->AddStatement(std::make_unique<VarDeclStmt>(names[i], std::move(idx_access)));
        }
        return block;
    }

    // 2. Object destructuring: let { a, b: c } = expr;
    if (Match(TokenKind::OpenBrace)) {
        std::vector<std::pair<std::string, std::string>> bindings;
        if (!Check(TokenKind::CloseBrace)) {
            do {
                Token prop_tok = Consume(TokenKind::Identifier, "Expected property in object destructuring");
                std::string local_var = prop_tok.text;
                if (Match(TokenKind::Colon)) {
                    Token alias_tok = Consume(TokenKind::Identifier, "Expected alias name in object destructuring");
                    local_var = alias_tok.text;
                }
                bindings.emplace_back(prop_tok.text, local_var);
            } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
        }
        Consume(TokenKind::CloseBrace, "Expected '}' in object destructuring");
        Consume(TokenKind::Equal, "Expected '=' in destructuring declaration");
        auto rhs = ParseExpression();
        Match(TokenKind::Semicolon);

        static uint32_t s_destruct_obj_id = 0;
        std::string temp_var = "$d_obj_" + std::to_string(++s_destruct_obj_id);
        auto block = std::make_unique<BlockStmt>();
        block->AddStatement(std::make_unique<VarDeclStmt>(temp_var, std::move(rhs)));
        for (const auto& [prop, local] : bindings) {
            auto prop_access = std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>(temp_var),
                prop
            );
            block->AddStatement(std::make_unique<VarDeclStmt>(local, std::move(prop_access)));
        }
        return block;
    }

    // 3. Regular variable declaration: let x = expr;
    Token name_tok = Consume(TokenKind::Identifier, "Expected identifier in variable declaration");
    std::unique_ptr<Expr> init = nullptr;
    if (Match(TokenKind::Equal)) {
        init = ParseExpression();
    }
    Match(TokenKind::Semicolon); // Optional semicolon
    return std::make_unique<VarDeclStmt>(name_tok.text, std::move(init));
}

std::unique_ptr<Stmt> Parser::ParseFunctionDecl(bool is_async) {
    Consume(TokenKind::Function, "Expected 'function'");
    bool is_generator = Match(TokenKind::Star);
    Token name_tok = Consume(TokenKind::Identifier, "Expected function name");
    Consume(TokenKind::OpenParen, "Expected '(' after function name");

    std::vector<std::string> params;
    std::string rest_param = "";
    if (!Check(TokenKind::CloseParen)) {
        if (Match(TokenKind::DotDotDot)) {
            Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
            rest_param = r.text;
        } else {
            Token param = Consume(TokenKind::Identifier, "Expected parameter name");
            params.push_back(param.text);
            while (Match(TokenKind::Comma)) {
                if (Match(TokenKind::DotDotDot)) {
                    Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                    rest_param = r.text;
                    break;
                }
                Token next_param = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                params.push_back(next_param.text);
            }
        }
    }
    Consume(TokenKind::CloseParen, "Expected ')' after parameters");

    std::unique_ptr<BlockStmt> body = ParseBlock();
    auto fn_decl = std::make_unique<FunctionDeclStmt>(name_tok.text, std::move(params), std::move(body));
    if (is_async) {
        fn_decl->SetAsync(true);
    }
    if (is_generator) {
        fn_decl->SetGenerator(true);
    }
    if (!rest_param.empty()) {
        fn_decl->SetRest(rest_param);
    }
    return fn_decl;
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

    // Speculative lookahead for "for ( [let|var|const]? (id | [a, b]) of iterable )"
    Lexer saved_lexer = lexer_;
    Token saved_tok = current_token_;

    bool is_for_of = false;
    bool is_destruct = false;
    std::string var_name = "";
    std::vector<std::string> destruct_vars;

    if (Match(TokenKind::Var) || Match(TokenKind::Let) || Match(TokenKind::Const)) {
        // Declaration specifier matched
    }

    bool is_for_in = false;
    if (Check(TokenKind::Identifier)) {
        Token id_tok = Advance();
        if (Match(TokenKind::Of)) {
            is_for_of = true;
            var_name = id_tok.text;
        } else if (Match(TokenKind::In)) {
            is_for_in = true;
            var_name = id_tok.text;
        }
    } else if (Match(TokenKind::OpenBracket)) {
        if (!Check(TokenKind::CloseBracket)) {
            do {
                Token el = Consume(TokenKind::Identifier, "Expected identifier in destructuring");
                destruct_vars.push_back(el.text);
            } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBracket));
        }
        Consume(TokenKind::CloseBracket, "Expected ']' in destructuring");
        if (Match(TokenKind::Of)) {
            is_for_of = true;
            is_destruct = true;
        }
    }

    if (is_for_in || is_for_of) {
        std::unique_ptr<Expr> iterable;
        if (is_for_in) {
            auto target_obj = ParseExpression();
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::move(target_obj));
            iterable = std::make_unique<CallExpr>(
                std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>("Object"), "keys"),
                std::move(args)
            );
        } else {
            iterable = ParseExpression();
        }
        Consume(TokenKind::CloseParen, "Expected ')' after for header");
        auto body = ParseStatement();

        static uint32_t s_for_of_id = 0;
        std::string iter_var = "$iter_" + std::to_string(++s_for_of_id);
        std::string step_var = "$step_" + std::to_string(s_for_of_id);

        auto outer_block = std::make_unique<BlockStmt>();

        // $iter = iterable[Symbol.iterator]()
        auto get_sym_iter = std::make_unique<PropertyAccessExpr>(
            std::make_unique<IdentifierExpr>("Symbol"),
            "iterator"
        );
        auto get_iter_fn = std::make_unique<IndexAccessExpr>(
            std::move(iterable),
            std::move(get_sym_iter)
        );
        auto call_iter = std::make_unique<CallExpr>(
            std::move(get_iter_fn),
            std::vector<std::unique_ptr<Expr>>{}
        );
        outer_block->AddStatement(std::make_unique<VarDeclStmt>(iter_var, std::move(call_iter)));

        // $step = $iter.next()
        auto call_next = [iter_var]() {
            auto get_next_fn = std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>(iter_var),
                "next"
            );
            return std::make_unique<CallExpr>(std::move(get_next_fn), std::vector<std::unique_ptr<Expr>>{});
        };
        outer_block->AddStatement(std::make_unique<VarDeclStmt>(step_var, call_next()));

        // condition: !$step.done
        auto cond = std::make_unique<UnaryExpr>(
            UnaryOp::LogicalNot,
            std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>(step_var),
                "done"
            )
        );

        // loop body block
        auto loop_body = std::make_unique<BlockStmt>();
        if (is_destruct) {
            std::string temp_val = "$item_" + std::to_string(s_for_of_id);
            loop_body->AddStatement(std::make_unique<VarDeclStmt>(
                temp_val,
                std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>(step_var), "value")
            ));
            for (size_t i = 0; i < destruct_vars.size(); ++i) {
                auto idx_access = std::make_unique<IndexAccessExpr>(
                    std::make_unique<IdentifierExpr>(temp_val),
                    std::make_unique<LiteralExpr>(TaggedValue::FromSmi(static_cast<int32_t>(i)))
                );
                loop_body->AddStatement(std::make_unique<VarDeclStmt>(destruct_vars[i], std::move(idx_access)));
            }
        } else {
            auto step_val = std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>(step_var),
                "value"
            );
            loop_body->AddStatement(std::make_unique<VarDeclStmt>(var_name, std::move(step_val)));
        }

        loop_body->AddStatement(std::move(body));

        // update: $step = $iter.next()
        auto update_step = std::make_unique<AssignExpr>(step_var, call_next());
        loop_body->AddStatement(std::make_unique<ExprStmt>(std::move(update_step)));

        outer_block->AddStatement(std::make_unique<WhileStmt>(std::move(cond), std::move(loop_body)));
        return outer_block;
    }

    // Standard for loop
    lexer_ = saved_lexer;
    current_token_ = saved_tok;

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

std::unique_ptr<Stmt> Parser::ParseThrow() {
    auto expr = ParseExpression();
    Match(TokenKind::Semicolon);
    return std::make_unique<ThrowStmt>(std::move(expr));
}

std::unique_ptr<Stmt> Parser::ParseTryCatch() {
    auto try_block = ParseBlock();
    std::string catch_param = "";
    std::unique_ptr<Stmt> catch_block = nullptr;
    if (Match(TokenKind::Catch)) {
        if (Match(TokenKind::OpenParen)) {
            Token param = Consume(TokenKind::Identifier, "Expected catch parameter identifier");
            catch_param = param.text;
            Consume(TokenKind::CloseParen, "Expected ')' after catch parameter");
        }
        catch_block = ParseBlock();
    }
    std::unique_ptr<Stmt> finally_block = nullptr;
    if (Match(TokenKind::Finally)) {
        finally_block = ParseBlock();
    }
    return std::make_unique<TryCatchStmt>(std::move(try_block), catch_param, std::move(catch_block), std::move(finally_block));
}

std::unique_ptr<Stmt> Parser::ParseClassDecl() {
    Token name_tok = Consume(TokenKind::Identifier, "Expected class name");
    std::string super_name = "";
    if (Match(TokenKind::Extends)) {
        Token super_tok = Consume(TokenKind::Identifier, "Expected super class name after 'extends'");
        super_name = super_tok.text;
    }
    Consume(TokenKind::OpenBrace, "Expected '{' in class declaration");
    std::vector<std::unique_ptr<FunctionDeclStmt>> methods;
    while (!Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        Token method_name = Consume(TokenKind::Identifier, "Expected method name");
        Consume(TokenKind::OpenParen, "Expected '(' after method name");
        std::vector<std::string> params;
        if (!Check(TokenKind::CloseParen)) {
            Token p = Consume(TokenKind::Identifier, "Expected parameter name");
            params.push_back(p.text);
            while (Match(TokenKind::Comma)) {
                Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                params.push_back(np.text);
            }
        }
        Consume(TokenKind::CloseParen, "Expected ')' after method parameters");
        auto body = ParseBlock();
        methods.push_back(std::make_unique<FunctionDeclStmt>(method_name.text, std::move(params), std::move(body)));
    }
    Consume(TokenKind::CloseBrace, "Expected '}' after class body");
    return std::make_unique<ClassDeclStmt>(name_tok.text, super_name, std::move(methods));
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
        case TokenKind::Backtick: {
            if (tok.template_exprs.empty()) {
                auto* s = new JSString(tok.template_quasis.empty() ? "" : tok.template_quasis[0]);
                return std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(s));
            }
            std::vector<std::unique_ptr<Expr>> expr_nodes;
            for (const auto& expr_str : tok.template_exprs) {
                Lexer sub_lexer(expr_str);
                Parser sub_parser(sub_lexer);
                expr_nodes.push_back(sub_parser.ParseExpression());
            }
            return std::make_unique<TemplateLiteralExpr>(std::move(tok.template_quasis), std::move(expr_nodes));
        }
        case TokenKind::DotDotDot: {
            return std::make_unique<SpreadExpr>(ParseExpression());
        }
        case TokenKind::Identifier: {
            if (Match(TokenKind::Arrow)) {
                std::vector<std::string> params = {tok.text};
                if (Check(TokenKind::OpenBrace)) {
                    auto body = ParseBlock();
                    return std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                } else {
                    auto arrow_expr = ParseExpression();
                    auto body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                    return std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                }
            }
            return std::make_unique<IdentifierExpr>(tok.text);
        }
        case TokenKind::OpenParen: {
            Lexer saved_lexer = lexer_;
            Token saved_tok = current_token_;

            // Check if this looks like arrow parameters: ( [param [, param]* [, ...rest]?]? ) =>
            bool is_arrow = false;
            std::vector<std::string> arrow_params;
            std::string arrow_rest;

            if (Check(TokenKind::CloseParen)) {
                Advance();
                if (Match(TokenKind::Arrow)) {
                    is_arrow = true;
                }
            } else {
                while (true) {
                    if (Match(TokenKind::DotDotDot)) {
                        if (Check(TokenKind::Identifier)) {
                            arrow_rest = Advance().text;
                            if (Check(TokenKind::CloseParen)) {
                                Advance();
                                if (Match(TokenKind::Arrow)) {
                                    is_arrow = true;
                                }
                            }
                        }
                        break;
                    }
                    if (Check(TokenKind::Identifier)) {
                        arrow_params.push_back(Advance().text);
                        if (Match(TokenKind::Comma)) {
                            continue;
                        }
                        if (Check(TokenKind::CloseParen)) {
                            Advance();
                            if (Match(TokenKind::Arrow)) {
                                is_arrow = true;
                            }
                            break;
                        }
                        break;
                    } else {
                        break;
                    }
                }
            }

            if (is_arrow) {
                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    auto arrow_expr = ParseExpression();
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }
                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(arrow_params), std::move(body));
                if (!arrow_rest.empty()) {
                    fn_expr->SetRest(arrow_rest);
                }
                return fn_expr;
            }

            // Fallback: restore lexer and current token, parse parenthesized expression
            lexer_ = saved_lexer;
            current_token_ = saved_tok;
            auto expr = ParseExpression();
            Consume(TokenKind::CloseParen, "Expected ')' after expression");
            return expr;
        }
        case TokenKind::OpenBrace: {
            // Object literal: { key: val, [expr]: val, ...spread }
            std::vector<ObjectLiteralExpr::Property> props;
            if (!Check(TokenKind::CloseBrace)) {
                do {
                    if (Match(TokenKind::DotDotDot)) {
                        auto spread_expr = ParseExpression();
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = "",
                            .computed_key = nullptr,
                            .value = std::make_unique<SpreadExpr>(std::move(spread_expr))
                        });
                        continue;
                    }
                    if (Match(TokenKind::OpenBracket)) {
                        auto computed_key = ParseExpression();
                        Consume(TokenKind::CloseBracket, "Expected ']' after computed property key");
                        Consume(TokenKind::Colon, "Expected ':' after computed property key");
                        auto val = ParseExpression();
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = "",
                            .computed_key = std::move(computed_key),
                            .value = std::move(val)
                        });
                        continue;
                    }
                    Token key_tok = Advance();
                    if (key_tok.kind != TokenKind::Identifier && key_tok.kind != TokenKind::String && key_tok.kind != TokenKind::Number) {
                        throw std::runtime_error("Expected property key in object literal");
                    }
                    Consume(TokenKind::Colon, "Expected ':' after property key");
                    auto val = ParseExpression();
                    props.push_back(ObjectLiteralExpr::Property{
                        .key = key_tok.text,
                        .computed_key = nullptr,
                        .value = std::move(val)
                    });
                } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
            }
            Consume(TokenKind::CloseBrace, "Expected '}' after object literal");
            return std::make_unique<ObjectLiteralExpr>(std::move(props));
        }
        case TokenKind::OpenBracket: {
            // Array literal: [ elem1, ...spread, elem2 ]
            std::vector<std::unique_ptr<Expr>> elements;
            if (!Check(TokenKind::CloseBracket)) {
                elements.push_back(ParseExpression());
                while (Match(TokenKind::Comma)) {
                    if (Check(TokenKind::CloseBracket)) break;
                    elements.push_back(ParseExpression());
                }
            }
            Consume(TokenKind::CloseBracket, "Expected ']' after array elements");
            return std::make_unique<ArrayLiteralExpr>(std::move(elements));
        }
        case TokenKind::Exclamation:
            return std::make_unique<UnaryExpr>(UnaryOp::LogicalNot, ParseExpression(Precedence::Unary));
        case TokenKind::Minus:
            return std::make_unique<UnaryExpr>(UnaryOp::Negate, ParseExpression(Precedence::Unary));
        case TokenKind::Plus:
            return std::make_unique<UnaryExpr>(UnaryOp::Plus, ParseExpression(Precedence::Unary));
        case TokenKind::Tilde:
            return std::make_unique<UnaryExpr>(UnaryOp::BitwiseNot, ParseExpression(Precedence::Unary));
        case TokenKind::TypeOf:
            return std::make_unique<UnaryExpr>(UnaryOp::TypeOf, ParseExpression(Precedence::Unary));
        case TokenKind::Delete:
            return std::make_unique<UnaryExpr>(UnaryOp::Delete, ParseExpression(Precedence::Unary));
        case TokenKind::PlusPlus: {
            Token id_tok = Consume(TokenKind::Identifier, "Expected identifier after '++'");
            auto plus_one = std::make_unique<BinaryExpr>(
                BinaryOp::Add,
                std::make_unique<IdentifierExpr>(id_tok.text),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))
            );
            return std::make_unique<AssignExpr>(id_tok.text, std::move(plus_one));
        }
        case TokenKind::MinusMinus: {
            Token id_tok = Consume(TokenKind::Identifier, "Expected identifier after '--'");
            auto minus_one = std::make_unique<BinaryExpr>(
                BinaryOp::Sub,
                std::make_unique<IdentifierExpr>(id_tok.text),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))
            );
            return std::make_unique<AssignExpr>(id_tok.text, std::move(minus_one));
        }
        case TokenKind::This:
            return std::make_unique<IdentifierExpr>("this");
        case TokenKind::New: {
            Token callee_tok = Consume(TokenKind::Identifier, "Expected constructor identifier after 'new'");
            std::unique_ptr<Expr> callee = std::make_unique<IdentifierExpr>(callee_tok.text);
            while (Match(TokenKind::Dot)) {
                Token prop = Consume(TokenKind::Identifier, "Expected property identifier after '.'");
                callee = std::make_unique<PropertyAccessExpr>(std::move(callee), prop.text);
            }
            std::vector<std::unique_ptr<Expr>> args;
            if (Match(TokenKind::OpenParen)) {
                if (!Check(TokenKind::CloseParen)) {
                    args.push_back(ParseExpression());
                    while (Match(TokenKind::Comma)) {
                        args.push_back(ParseExpression());
                    }
                }
                Consume(TokenKind::CloseParen, "Expected ')' after constructor argument list");
            }
            return std::make_unique<NewExpr>(std::move(callee), std::move(args));
        }
        case TokenKind::Yield: {
            bool is_delegate = Match(TokenKind::Star);
            std::unique_ptr<Expr> expr = nullptr;
            if (!Check(TokenKind::Semicolon) && !Check(TokenKind::CloseBrace) && !Check(TokenKind::CloseParen) && !Check(TokenKind::Comma) && !Check(TokenKind::EndOfFile)) {
                expr = ParseExpression();
            }
            return std::make_unique<YieldExpr>(std::move(expr), is_delegate);
        }
        case TokenKind::Await: {
            auto expr = ParseExpression(Precedence::Unary);
            return std::make_unique<AwaitExpr>(std::move(expr));
        }
        case TokenKind::Async: {
            if (Check(TokenKind::Function)) {
                Advance();
                bool is_generator = Match(TokenKind::Star);
                std::string name = "";
                if (Check(TokenKind::Identifier)) {
                    name = Advance().text;
                }
                Consume(TokenKind::OpenParen, "Expected '(' after 'function'");
                std::vector<std::string> params;
                std::string rest_param = "";
                if (!Check(TokenKind::CloseParen)) {
                    if (Match(TokenKind::DotDotDot)) {
                        Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                        rest_param = r.text;
                    } else {
                        Token p = Consume(TokenKind::Identifier, "Expected parameter name");
                        params.push_back(p.text);
                        while (Match(TokenKind::Comma)) {
                            if (Match(TokenKind::DotDotDot)) {
                                Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                                rest_param = r.text;
                                break;
                            }
                            Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                            params.push_back(np.text);
                        }
                    }
                }
                Consume(TokenKind::CloseParen, "Expected ')' after parameters");
                auto body = ParseBlock();
                auto fn_expr = std::make_unique<FunctionExpr>(name, std::move(params), std::move(body));
                fn_expr->SetAsync(true);
                if (is_generator) {
                    fn_expr->SetGenerator(true);
                }
                if (!rest_param.empty()) {
                    fn_expr->SetRest(rest_param);
                }
                return fn_expr;
            }
            if (Check(TokenKind::Arrow)) {
                Advance();
                std::vector<std::string> params = {"async"};
                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    auto arrow_expr = ParseExpression();
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }
                return std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
            }
            if (Check(TokenKind::Identifier)) {
                Token param_tok = Advance();
                Consume(TokenKind::Arrow, "Expected '=>' after async parameter");
                std::vector<std::string> params = {param_tok.text};
                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    auto arrow_expr = ParseExpression();
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }
                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetAsync(true);
                return fn_expr;
            }
            if (Check(TokenKind::OpenParen)) {
                Advance();
                std::vector<std::string> params;
                std::string rest_param = "";
                if (!Check(TokenKind::CloseParen)) {
                    if (Match(TokenKind::DotDotDot)) {
                        Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                        rest_param = r.text;
                    } else {
                        Token p = Consume(TokenKind::Identifier, "Expected parameter name");
                        params.push_back(p.text);
                        while (Match(TokenKind::Comma)) {
                            if (Match(TokenKind::DotDotDot)) {
                                Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                                rest_param = r.text;
                                break;
                            }
                            Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                            params.push_back(np.text);
                        }
                    }
                }
                Consume(TokenKind::CloseParen, "Expected ')' after async arrow parameters");
                Consume(TokenKind::Arrow, "Expected '=>' after async arrow parameters");
                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    auto arrow_expr = ParseExpression();
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }
                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetAsync(true);
                if (!rest_param.empty()) {
                    fn_expr->SetRest(rest_param);
                }
                return fn_expr;
            }
            return std::make_unique<IdentifierExpr>("async");
        }
        case TokenKind::Function: {
            bool is_generator = Match(TokenKind::Star);
            std::string name = "";
            if (Check(TokenKind::Identifier)) {
                name = Advance().text;
            }
            Consume(TokenKind::OpenParen, "Expected '(' after 'function'");
            std::vector<std::string> params;
            std::string rest_param = "";
            if (!Check(TokenKind::CloseParen)) {
                if (Match(TokenKind::DotDotDot)) {
                    Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                    rest_param = r.text;
                } else {
                    Token p = Consume(TokenKind::Identifier, "Expected parameter name");
                    params.push_back(p.text);
                    while (Match(TokenKind::Comma)) {
                        if (Match(TokenKind::DotDotDot)) {
                            Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                            rest_param = r.text;
                            break;
                        }
                        Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                        params.push_back(np.text);
                    }
                }
            }
            Consume(TokenKind::CloseParen, "Expected ')' after parameters");
            auto body = ParseBlock();
            auto fn_expr = std::make_unique<FunctionExpr>(name, std::move(params), std::move(body));
            if (is_generator) {
                fn_expr->SetGenerator(true);
            }
            if (!rest_param.empty()) {
                fn_expr->SetRest(rest_param);
            }
            return fn_expr;
        }
        default:
            throw std::runtime_error(std::string("SyntaxError: Unexpected token '") + tok.text + 
                                     "' at line " + std::to_string(tok.location.line) + 
                                     ", col " + std::to_string(tok.location.column));
    }
}

std::unique_ptr<Expr> Parser::ParseInfix(std::unique_ptr<Expr> lhs, Token op_token) {
    if (op_token.kind == TokenKind::Equal ||
        op_token.kind == TokenKind::PlusEqual ||
        op_token.kind == TokenKind::MinusEqual ||
        op_token.kind == TokenKind::StarEqual ||
        op_token.kind == TokenKind::SlashEqual ||
        op_token.kind == TokenKind::AmpAmpEqual ||
        op_token.kind == TokenKind::BarBarEqual ||
        op_token.kind == TokenKind::QuestionQuestionEqual) {

        auto rhs = ParseExpression(Precedence::Assignment);

        if (op_token.kind == TokenKind::Equal) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
                return std::make_unique<AssignExpr>(id->Name(), std::move(rhs));
            }
            if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
                std::string prop_name = prop->Property();
                auto obj = prop->ReleaseObject();
                return std::make_unique<PropertyAssignExpr>(std::move(obj), prop_name, std::move(rhs));
            }
            if (auto* idx = dynamic_cast<IndexAccessExpr*>(lhs.get())) {
                auto obj = idx->ReleaseObject();
                auto index = idx->ReleaseIndex();
                return std::make_unique<IndexAssignExpr>(std::move(obj), std::move(index), std::move(rhs));
            }
            throw std::runtime_error("Invalid assignment target");
        }

        BinaryOp bin_op;
        switch (op_token.kind) {
            case TokenKind::PlusEqual:               bin_op = BinaryOp::Add; break;
            case TokenKind::MinusEqual:              bin_op = BinaryOp::Sub; break;
            case TokenKind::StarEqual:               bin_op = BinaryOp::Mul; break;
            case TokenKind::SlashEqual:              bin_op = BinaryOp::Div; break;
            case TokenKind::AmpAmpEqual:             bin_op = BinaryOp::LogicalAnd; break;
            case TokenKind::BarBarEqual:             bin_op = BinaryOp::LogicalOr; break;
            case TokenKind::QuestionQuestionEqual:   bin_op = BinaryOp::NullishCoalescing; break;
            default: throw std::runtime_error("Unhandled compound assignment");
        }

        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            auto bin_expr = std::make_unique<BinaryExpr>(bin_op, std::make_unique<IdentifierExpr>(id->Name()), std::move(rhs));
            return std::make_unique<AssignExpr>(id->Name(), std::move(bin_expr));
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            if (auto* obj_id = dynamic_cast<IdentifierExpr*>(obj.get())) {
                auto get_prop = std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>(obj_id->Name()), prop_name);
                auto bin_expr = std::make_unique<BinaryExpr>(bin_op, std::move(get_prop), std::move(rhs));
                return std::make_unique<PropertyAssignExpr>(std::move(obj), prop_name, std::move(bin_expr));
            }
            auto bin_expr = std::make_unique<BinaryExpr>(bin_op, std::make_unique<PropertyAccessExpr>(std::move(obj), prop_name), std::move(rhs));
            return std::make_unique<PropertyAssignExpr>(nullptr, prop_name, std::move(bin_expr));
        }
        throw std::runtime_error("Invalid left-hand side in compound assignment");
    }

    if (op_token.kind == TokenKind::QuestionDot) {
        if (Match(TokenKind::OpenBracket)) {
            auto index = ParseExpression();
            Consume(TokenKind::CloseBracket, "Expected ']' after index in optional chaining");
            return std::make_unique<OptionalIndexAccessExpr>(std::move(lhs), std::move(index));
        }
        if (Match(TokenKind::OpenParen)) {
            std::vector<std::unique_ptr<Expr>> args;
            if (!Check(TokenKind::CloseParen)) {
                args.push_back(ParseExpression());
                while (Match(TokenKind::Comma)) {
                    args.push_back(ParseExpression());
                }
            }
            Consume(TokenKind::CloseParen, "Expected ')' after argument list in optional call");
            return std::make_unique<OptionalCallExpr>(std::move(lhs), std::move(args));
        }
        if (current_token_.text.empty() || (!std::isalpha(static_cast<unsigned char>(current_token_.text[0])) && current_token_.text[0] != '_' && current_token_.text[0] != '$')) {
            throw std::runtime_error("Expected property identifier after '?.'");
        }
        Token prop_tok = Advance();
        return std::make_unique<OptionalPropertyAccessExpr>(std::move(lhs), prop_tok.text);
    }

    if (op_token.kind == TokenKind::Dot) {
        if (current_token_.text.empty() || (!std::isalpha(static_cast<unsigned char>(current_token_.text[0])) && current_token_.text[0] != '_' && current_token_.text[0] != '$')) {
            throw std::runtime_error("Expected property identifier after '.'");
        }
        Token prop_tok = Advance();
        return std::make_unique<PropertyAccessExpr>(std::move(lhs), prop_tok.text);
    }

    if (op_token.kind == TokenKind::OpenBracket) {
        auto index = ParseExpression();
        Consume(TokenKind::CloseBracket, "Expected ']' after index");
        return std::make_unique<IndexAccessExpr>(std::move(lhs), std::move(index));
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

    if (op_token.kind == TokenKind::PlusPlus) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            std::string name = id->Name();
            auto plus_one = std::make_unique<BinaryExpr>(
                BinaryOp::Add,
                std::make_unique<IdentifierExpr>(name),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))
            );
            return std::make_unique<AssignExpr>(name, std::move(plus_one));
        }
        throw std::runtime_error("Invalid left-hand side in postfix ++");
    }

    if (op_token.kind == TokenKind::MinusMinus) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            std::string name = id->Name();
            auto minus_one = std::make_unique<BinaryExpr>(
                BinaryOp::Sub,
                std::make_unique<IdentifierExpr>(name),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))
            );
            return std::make_unique<AssignExpr>(name, std::move(minus_one));
        }
        throw std::runtime_error("Invalid left-hand side in postfix --");
    }

    if (op_token.kind == TokenKind::Question) {
        auto then_expr = ParseExpression();
        Consume(TokenKind::Colon, "Expected ':' in conditional expression");
        auto else_expr = ParseExpression(Precedence::Conditional);
        return std::make_unique<ConditionalExpr>(std::move(lhs), std::move(then_expr), std::move(else_expr));
    }

    Precedence prec = GetPrecedence(op_token.kind);
    auto rhs = ParseExpression(static_cast<Precedence>(static_cast<int>(prec) + 1));

    BinaryOp op;
    switch (op_token.kind) {
        case TokenKind::Plus:               op = BinaryOp::Add; break;
        case TokenKind::Minus:              op = BinaryOp::Sub; break;
        case TokenKind::Star:               op = BinaryOp::Mul; break;
        case TokenKind::Slash:              op = BinaryOp::Div; break;
        case TokenKind::Percent:            op = BinaryOp::Mod; break;
        case TokenKind::Ampersand:          op = BinaryOp::BitwiseAnd; break;
        case TokenKind::Bar:                op = BinaryOp::BitwiseOr; break;
        case TokenKind::Caret:              op = BinaryOp::BitwiseXor; break;
        case TokenKind::LessLess:           op = BinaryOp::ShiftLeft; break;
        case TokenKind::GreaterGreater:     op = BinaryOp::ShiftRight; break;
        case TokenKind::EqualEqual:         op = BinaryOp::Equal; break;
        case TokenKind::StrictEqual:        op = BinaryOp::StrictEqual; break;
        case TokenKind::NotEqual:           op = BinaryOp::NotEqual; break;
        case TokenKind::StrictNotEqual:     op = BinaryOp::StrictNotEqual; break;
        case TokenKind::Less:               op = BinaryOp::LessThan; break;
        case TokenKind::LessEqual:          op = BinaryOp::LessThanOrEqual; break;
        case TokenKind::Greater:            op = BinaryOp::GreaterThan; break;
        case TokenKind::GreaterEqual:       op = BinaryOp::GreaterThanOrEqual; break;
        case TokenKind::AmpAmp:             op = BinaryOp::LogicalAnd; break;
        case TokenKind::BarBar:             op = BinaryOp::LogicalOr; break;
        case TokenKind::QuestionQuestion:   op = BinaryOp::NullishCoalescing; break;
        case TokenKind::InstanceOf:         op = BinaryOp::InstanceOf; break;
        case TokenKind::In:                 op = BinaryOp::In; break;
        default:
            throw std::runtime_error("Unhandled binary operator");
    }

    return std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
}

} // namespace aether
