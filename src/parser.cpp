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
        case TokenKind::Comma:
            return Precedence::Comma;
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::StarStarEqual:
        case TokenKind::LessLessEqual:
        case TokenKind::GreaterGreaterEqual:
        case TokenKind::GreaterGreaterGreaterEqual:
        case TokenKind::AmpersandEqual:
        case TokenKind::BarEqual:
        case TokenKind::CaretEqual:
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
        case TokenKind::GreaterGreaterGreater:
            return Precedence::Shift;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return Precedence::Additive;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
            return Precedence::Multiplicative;
        case TokenKind::StarStar:
            return Precedence::Exponentiation;
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
    if (Match(TokenKind::Var)) {
        return ParseVarDecl(/*is_const=*/false);
    }
    if (Match(TokenKind::Let)) {
        return ParseVarDecl(/*is_const=*/false);
    }
    if (Match(TokenKind::Const)) {
        return ParseVarDecl(/*is_const=*/true);
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
    if (Match(TokenKind::Do)) {
        return ParseDoWhile();
    }
    if (Match(TokenKind::For)) {
        bool is_for_await = Match(TokenKind::Await);
        return ParseFor(is_for_await);
    }
    if (Match(TokenKind::Break)) {
        return ParseBreak();
    }
    if (Match(TokenKind::Continue)) {
        return ParseContinue();
    }
    if (Match(TokenKind::Switch)) {
        return ParseSwitch();
    }
    if (Match(TokenKind::Debugger)) {
        return ParseDebugger();
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
    if (Check(TokenKind::Import)) {
        Lexer saved_lexer = lexer_;
        Token saved_tok = current_token_;
        Advance();
        bool is_expr = Check(TokenKind::OpenParen) || Check(TokenKind::Dot);
        lexer_ = saved_lexer;
        current_token_ = saved_tok;
        if (is_expr) {
            return ParseExprStmt();
        }
        return ParseImportDecl();
    }
    if (Check(TokenKind::Export)) {
        return ParseExportDecl();
    }
    if (Check(TokenKind::OpenBrace)) {
        return ParseBlock();
    }
    if (Match(TokenKind::Semicolon)) {
        return std::make_unique<BlockStmt>(); // Empty statement
    }
    return ParseExprStmt();
}

std::unique_ptr<Stmt> Parser::ParseVarDecl(bool is_const) {
    auto parse_one_declarator = [&]() -> std::vector<std::unique_ptr<Stmt>> {
        std::vector<std::unique_ptr<Stmt>> stmts;
        // 1. Array destructuring: let [a, b, c = def] = expr;
        if (Match(TokenKind::OpenBracket)) {
            struct ArrayElem {
                std::string name;
                std::unique_ptr<Expr> default_val;
            };
            std::vector<ArrayElem> elems;
            if (!Check(TokenKind::CloseBracket)) {
                do {
                    Token elem_tok = Consume(TokenKind::Identifier, "Expected identifier in array destructuring");
                    std::unique_ptr<Expr> def_val = nullptr;
                    if (Match(TokenKind::Equal)) {
                        def_val = ParseExpression();
                    }
                    elems.push_back(ArrayElem{elem_tok.text, std::move(def_val)});
                } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBracket));
            }
            Consume(TokenKind::CloseBracket, "Expected ']' in array destructuring");
            Consume(TokenKind::Equal, "Expected '=' in destructuring declaration");
            auto rhs = ParseExpression();

            static uint32_t s_destruct_arr_id = 0;
            std::string temp_var = "$d_arr_" + std::to_string(++s_destruct_arr_id);
            stmts.push_back(std::make_unique<VarDeclStmt>(temp_var, std::move(rhs)));
            for (size_t i = 0; i < elems.size(); ++i) {
                auto idx_access = std::make_unique<IndexAccessExpr>(
                    std::make_unique<IdentifierExpr>(temp_var),
                    std::make_unique<LiteralExpr>(TaggedValue::FromSmi(static_cast<int32_t>(i)))
                );
                std::unique_ptr<Expr> init_expr;
                if (elems[i].default_val) {
                    init_expr = std::make_unique<BinaryExpr>(
                        BinaryOp::NullishCoalescing,
                        std::move(idx_access),
                        std::move(elems[i].default_val)
                    );
                } else {
                    init_expr = std::move(idx_access);
                }
                stmts.push_back(std::make_unique<VarDeclStmt>(elems[i].name, std::move(init_expr), is_const));
            }
            return stmts;
        }

        // 2. Object destructuring: let { a, b: c, active = true, coords: { lat, lng } } = expr;
        if (Match(TokenKind::OpenBrace)) {
            struct BindingItem {
                std::string prop;
                std::string local_var;
                std::unique_ptr<Expr> default_val;
                std::vector<BindingItem> nested_items;
                bool is_nested{false};
            };
            std::function<std::vector<BindingItem>()> parse_pattern = [&]() {
                std::vector<BindingItem> items;
                if (!Check(TokenKind::CloseBrace)) {
                    do {
                        Token prop_tok = Consume(TokenKind::Identifier, "Expected property in object destructuring");
                        BindingItem item;
                        item.prop = prop_tok.text;
                        item.local_var = prop_tok.text;
                        if (Match(TokenKind::Colon)) {
                            if (Match(TokenKind::OpenBrace)) {
                                item.is_nested = true;
                                item.nested_items = parse_pattern();
                            } else {
                                Token alias_tok = Consume(TokenKind::Identifier, "Expected alias name in object destructuring");
                                item.local_var = alias_tok.text;
                            }
                        }
                        if (Match(TokenKind::Equal)) {
                            item.default_val = ParseExpression();
                        }
                        items.push_back(std::move(item));
                    } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
                }
                Consume(TokenKind::CloseBrace, "Expected '}' in object destructuring");
                return items;
            };

            auto items = parse_pattern();
            Consume(TokenKind::Equal, "Expected '=' in destructuring declaration");
            auto rhs = ParseExpression();

            static uint32_t s_destruct_obj_id = 0;
            std::string temp_var = "$d_obj_" + std::to_string(++s_destruct_obj_id);
            stmts.push_back(std::make_unique<VarDeclStmt>(temp_var, std::move(rhs)));

            std::function<void(const std::string&, std::vector<BindingItem>&)> emit_bindings =
                [&](const std::string& parent_var, std::vector<BindingItem>& item_list) {
                for (auto& it : item_list) {
                    auto prop_access = std::make_unique<PropertyAccessExpr>(
                        std::make_unique<IdentifierExpr>(parent_var),
                        it.prop
                    );
                    if (it.is_nested) {
                        std::string sub_var = "$d_sub_" + std::to_string(++s_destruct_obj_id);
                        stmts.push_back(std::make_unique<VarDeclStmt>(sub_var, std::move(prop_access)));
                        emit_bindings(sub_var, it.nested_items);
                    } else {
                        std::unique_ptr<Expr> init_expr;
                        if (it.default_val) {
                            init_expr = std::make_unique<BinaryExpr>(
                                BinaryOp::NullishCoalescing,
                                std::move(prop_access),
                                std::move(it.default_val)
                            );
                        } else {
                            init_expr = std::move(prop_access);
                        }
                        stmts.push_back(std::make_unique<VarDeclStmt>(it.local_var, std::move(init_expr), is_const));
                    }
                }
            };

            emit_bindings(temp_var, items);
            return stmts;
        }

        // 3. Regular variable declaration: let x = expr;
        Token name_tok = Consume(TokenKind::Identifier, "Expected identifier in variable declaration");
        std::unique_ptr<Expr> init = nullptr;
        if (Match(TokenKind::Equal)) {
            init = ParseExpression();
        }
        stmts.push_back(std::make_unique<VarDeclStmt>(name_tok.text, std::move(init), is_const));
        return stmts;
    };

    std::vector<std::unique_ptr<Stmt>> all_stmts;
    auto first = parse_one_declarator();
    for (auto& s : first) {
        all_stmts.push_back(std::move(s));
    }

    while (Match(TokenKind::Comma)) {
        auto next = parse_one_declarator();
        for (auto& s : next) {
            all_stmts.push_back(std::move(s));
        }
    }

    Match(TokenKind::Semicolon); // Optional semicolon

    if (all_stmts.size() == 1) {
        return std::move(all_stmts[0]);
    }
    auto block = std::make_unique<BlockStmt>();
    for (auto& s : all_stmts) {
        block->AddStatement(std::move(s));
    }
    return block;
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

std::unique_ptr<Stmt> Parser::ParseDoWhile() {
    auto body = ParseStatement();
    Consume(TokenKind::While, "Expected 'while' after do-while body");
    Consume(TokenKind::OpenParen, "Expected '(' after 'while'");
    auto cond = ParseExpression();
    Consume(TokenKind::CloseParen, "Expected ')' after do-while condition");
    Match(TokenKind::Semicolon);
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(cond));
}

std::unique_ptr<Stmt> Parser::ParseBreak() {
    std::string label = "";
    if (Check(TokenKind::Identifier)) {
        label = Advance().text;
    }
    Match(TokenKind::Semicolon);
    return std::make_unique<BreakStmt>(label);
}

std::unique_ptr<Stmt> Parser::ParseContinue() {
    std::string label = "";
    if (Check(TokenKind::Identifier)) {
        label = Advance().text;
    }
    Match(TokenKind::Semicolon);
    return std::make_unique<ContinueStmt>(label);
}

std::unique_ptr<Stmt> Parser::ParseSwitch() {
    Consume(TokenKind::OpenParen, "Expected '(' after 'switch'");
    auto discriminant = ParseExpression();
    Consume(TokenKind::CloseParen, "Expected ')' after switch discriminant");
    Consume(TokenKind::OpenBrace, "Expected '{' before switch cases");

    std::vector<SwitchCase> cases;
    while (!Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        SwitchCase sc;
        if (Match(TokenKind::Case)) {
            sc.test = ParseExpression();
            Consume(TokenKind::Colon, "Expected ':' after case expression");
        } else if (Match(TokenKind::Default)) {
            Consume(TokenKind::Colon, "Expected ':' after default");
            sc.test = nullptr;
        } else {
            throw std::runtime_error("Expected 'case' or 'default' in switch statement");
        }

        while (!Check(TokenKind::Case) && !Check(TokenKind::Default) && !Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
            sc.statements.push_back(ParseStatement());
        }
        cases.push_back(std::move(sc));
    }
    Consume(TokenKind::CloseBrace, "Expected '}' after switch cases");
    return std::make_unique<SwitchStmt>(std::move(discriminant), std::move(cases));
}

std::unique_ptr<Stmt> Parser::ParseDebugger() {
    Match(TokenKind::Semicolon);
    return std::make_unique<DebuggerStmt>();
}

std::unique_ptr<Stmt> Parser::ParseFor(bool is_for_await) {
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
        if (is_for_await && is_for_in) {
            throw std::runtime_error("SyntaxError: for-in loop cannot be used with 'await'");
        }
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
        std::string target_var = "$target_" + std::to_string(++s_for_of_id);
        std::string iter_var = "$iter_" + std::to_string(s_for_of_id);
        std::string step_var = "$step_" + std::to_string(s_for_of_id);

        auto outer_block = std::make_unique<BlockStmt>();
        outer_block->AddStatement(std::make_unique<VarDeclStmt>(target_var, std::move(iterable)));

        if (is_for_await) {
            // ($target[Symbol.asyncIterator] !== undefined ? $target[Symbol.asyncIterator]() : $target[Symbol.iterator]())
            auto get_async_method = std::make_unique<IndexAccessExpr>(
                std::make_unique<IdentifierExpr>(target_var),
                std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>("Symbol"), "asyncIterator")
            );
            auto cond_has_async = std::make_unique<BinaryExpr>(
                BinaryOp::StrictNotEqual,
                std::move(get_async_method),
                std::make_unique<LiteralExpr>(TaggedValue::Undefined())
            );
            auto call_async = std::make_unique<CallExpr>(
                std::make_unique<IndexAccessExpr>(
                    std::make_unique<IdentifierExpr>(target_var),
                    std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>("Symbol"), "asyncIterator")
                ),
                std::vector<std::unique_ptr<Expr>>{}
            );
            auto call_sync = std::make_unique<CallExpr>(
                std::make_unique<IndexAccessExpr>(
                    std::make_unique<IdentifierExpr>(target_var),
                    std::make_unique<PropertyAccessExpr>(std::make_unique<IdentifierExpr>("Symbol"), "iterator")
                ),
                std::vector<std::unique_ptr<Expr>>{}
            );
            auto iter_init = std::make_unique<ConditionalExpr>(
                std::move(cond_has_async),
                std::move(call_async),
                std::move(call_sync)
            );
            outer_block->AddStatement(std::make_unique<VarDeclStmt>(iter_var, std::move(iter_init)));
        } else {
            auto get_sym_iter = std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>("Symbol"),
                "iterator"
            );
            auto get_iter_fn = std::make_unique<IndexAccessExpr>(
                std::make_unique<IdentifierExpr>(target_var),
                std::move(get_sym_iter)
            );
            auto call_iter = std::make_unique<CallExpr>(
                std::move(get_iter_fn),
                std::vector<std::unique_ptr<Expr>>{}
            );
            outer_block->AddStatement(std::make_unique<VarDeclStmt>(iter_var, std::move(call_iter)));
        }

        // $step = (await)? $iter.next()
        auto call_next = [iter_var, is_for_await]() -> std::unique_ptr<Expr> {
            auto get_next_fn = std::make_unique<PropertyAccessExpr>(
                std::make_unique<IdentifierExpr>(iter_var),
                "next"
            );
            auto call_expr = std::make_unique<CallExpr>(std::move(get_next_fn), std::vector<std::unique_ptr<Expr>>{});
            if (is_for_await) {
                return std::make_unique<AwaitExpr>(std::move(call_expr));
            }
            return call_expr;
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

        // update: $step = (await)? $iter.next()
        auto update_step = std::make_unique<AssignExpr>(step_var, call_next());
        loop_body->AddStatement(std::make_unique<ExprStmt>(std::move(update_step)));

        outer_block->AddStatement(std::make_unique<WhileStmt>(std::move(cond), std::move(loop_body)));
        return outer_block;
    }

    if (is_for_await) {
        throw std::runtime_error("SyntaxError: for-await loop requires 'of' iteration");
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
        update = ParseExpression(Precedence::Comma);
    }
    Consume(TokenKind::CloseParen, "Expected ')' after for loop update");

    auto body = ParseStatement();
    return std::make_unique<ForStmt>(std::move(init), std::move(cond), std::move(update), std::move(body));
}

std::unique_ptr<Stmt> Parser::ParseReturn() {
    std::unique_ptr<Expr> val = nullptr;
    if (!Check(TokenKind::Semicolon) && !Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        val = ParseExpression(Precedence::Comma);
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
    auto expr = ParseExpression(Precedence::Comma);
    Match(TokenKind::Semicolon);
    return std::make_unique<ExprStmt>(std::move(expr));
}

bool Parser::MatchContextualKeyword(std::string_view kw) {
    if (Check(TokenKind::Identifier) && current_token_.text == kw) {
        Advance();
        return true;
    }
    return false;
}

std::unique_ptr<Stmt> Parser::ParseImportDecl() {
    Consume(TokenKind::Import, "Expected 'import'");

    // Form 1: import "module-name";
    if (Check(TokenKind::String)) {
        Token spec_tok = Advance();
        Match(TokenKind::Semicolon);
        return std::make_unique<ImportDeclStmt>(spec_tok.text, "", "", std::vector<ImportSpecifier>{});
    }

    std::string default_import;
    std::string namespace_import;
    std::vector<ImportSpecifier> named_imports;

    // Form 2: import defaultExport ...
    if (Check(TokenKind::Identifier)) {
        Token def_tok = Advance();
        default_import = def_tok.text;
        if (Match(TokenKind::Comma)) {
            if (Match(TokenKind::Star)) {
                if (!MatchContextualKeyword("as")) {
                    Consume(TokenKind::Identifier, "Expected 'as' after '*'");
                }
                Token ns_tok = Consume(TokenKind::Identifier, "Expected namespace identifier");
                namespace_import = ns_tok.text;
            } else if (Match(TokenKind::OpenBrace)) {
                if (!Check(TokenKind::CloseBrace)) {
                    do {
                        Token orig = Advance();
                        std::string imported_name = orig.text;
                        std::string local_name = imported_name;
                        if (MatchContextualKeyword("as")) {
                            Token alias = Consume(TokenKind::Identifier, "Expected alias identifier");
                            local_name = alias.text;
                        }
                        named_imports.push_back(ImportSpecifier{imported_name, local_name});
                    } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
                }
                Consume(TokenKind::CloseBrace, "Expected '}' in import declaration");
            }
        }
    } else if (Match(TokenKind::Star)) {
        // Form 3: import * as ns from "module";
        if (!MatchContextualKeyword("as")) {
            Consume(TokenKind::Identifier, "Expected 'as' after '*'");
        }
        Token ns_tok = Consume(TokenKind::Identifier, "Expected namespace identifier");
        namespace_import = ns_tok.text;
    } else if (Match(TokenKind::OpenBrace)) {
        // Form 4: import { a, b as c } from "module";
        if (!Check(TokenKind::CloseBrace)) {
            do {
                Token orig = Advance();
                std::string imported_name = orig.text;
                std::string local_name = imported_name;
                if (MatchContextualKeyword("as")) {
                    Token alias = Consume(TokenKind::Identifier, "Expected alias identifier");
                    local_name = alias.text;
                }
                named_imports.push_back(ImportSpecifier{imported_name, local_name});
            } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
        }
        Consume(TokenKind::CloseBrace, "Expected '}' in import declaration");
    }

    if (!MatchContextualKeyword("from")) {
        Consume(TokenKind::Identifier, "Expected 'from' in import statement");
    }
    Token spec_tok = Consume(TokenKind::String, "Expected module specifier string after 'from'");
    Match(TokenKind::Semicolon);

    return std::make_unique<ImportDeclStmt>(spec_tok.text, default_import, namespace_import, std::move(named_imports));
}

std::unique_ptr<Stmt> Parser::ParseExportDecl() {
    Consume(TokenKind::Export, "Expected 'export'");

    // Form 1: export default ...
    if (Match(TokenKind::Default)) {
        if (Check(TokenKind::Async)) {
            Lexer saved_lexer = lexer_;
            Token saved_tok = current_token_;
            Advance();
            if (Check(TokenKind::Function)) {
                auto fn_decl = ParseFunctionDecl(/*is_async=*/true);
                return std::make_unique<ExportDeclStmt>(/*is_default=*/true, std::move(fn_decl), nullptr, std::vector<ExportSpecifier>{});
            }
            lexer_ = saved_lexer;
            current_token_ = saved_tok;
        }
        if (Check(TokenKind::Function)) {
            auto fn_decl = ParseFunctionDecl(/*is_async=*/false);
            return std::make_unique<ExportDeclStmt>(/*is_default=*/true, std::move(fn_decl), nullptr, std::vector<ExportSpecifier>{});
        }
        if (Check(TokenKind::Class)) {
            Advance();
            auto cls_decl = ParseClassDecl();
            return std::make_unique<ExportDeclStmt>(/*is_default=*/true, std::move(cls_decl), nullptr, std::vector<ExportSpecifier>{});
        }
        auto expr = ParseExpression();
        Match(TokenKind::Semicolon);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/true, nullptr, std::move(expr), std::vector<ExportSpecifier>{});
    }

    // Form 2: export * from "mod" OR export * as ns from "mod"
    if (Match(TokenKind::Star)) {
        std::string export_all_as;
        if (MatchContextualKeyword("as")) {
            Token as_tok = Consume(TokenKind::Identifier, "Expected identifier after 'as'");
            export_all_as = as_tok.text;
        }
        if (!MatchContextualKeyword("from")) {
            Consume(TokenKind::Identifier, "Expected 'from' after export *");
        }
        Token spec_tok = Consume(TokenKind::String, "Expected module specifier");
        Match(TokenKind::Semicolon);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, nullptr, nullptr, std::vector<ExportSpecifier>{}, spec_tok.text, /*is_export_all=*/true, export_all_as);
    }

    // Form 3: export { a, b as c } [from "mod"]
    if (Match(TokenKind::OpenBrace)) {
        std::vector<ExportSpecifier> named_exports;
        if (!Check(TokenKind::CloseBrace)) {
            do {
                Token orig = Advance();
                std::string local_name = orig.text;
                std::string export_name = local_name;
                if (MatchContextualKeyword("as")) {
                    Token as_tok = Advance();
                    export_name = as_tok.text;
                }
                named_exports.push_back(ExportSpecifier{local_name, export_name});
            } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
        }
        Consume(TokenKind::CloseBrace, "Expected '}' in export clause");
        std::string from_spec;
        if (MatchContextualKeyword("from")) {
            Token spec_tok = Consume(TokenKind::String, "Expected module specifier after 'from'");
            from_spec = spec_tok.text;
        }
        Match(TokenKind::Semicolon);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, nullptr, nullptr, std::move(named_exports), from_spec);
    }

    // Form 4: export var/let/const/function/class
    if (Match(TokenKind::Var) || Match(TokenKind::Let) || Match(TokenKind::Const)) {
        auto var_decl = ParseVarDecl();
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(var_decl), nullptr, std::vector<ExportSpecifier>{});
    }
    if (Check(TokenKind::Async)) {
        Lexer saved_lexer = lexer_;
        Token saved_tok = current_token_;
        Advance();
        if (Check(TokenKind::Function)) {
            auto fn_decl = ParseFunctionDecl(/*is_async=*/true);
            return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(fn_decl), nullptr, std::vector<ExportSpecifier>{});
        }
        lexer_ = saved_lexer;
        current_token_ = saved_tok;
    }
    if (Check(TokenKind::Function)) {
        auto fn_decl = ParseFunctionDecl(/*is_async=*/false);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(fn_decl), nullptr, std::vector<ExportSpecifier>{});
    }
    if (Match(TokenKind::Class)) {
        auto cls_decl = ParseClassDecl();
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(cls_decl), nullptr, std::vector<ExportSpecifier>{});
    }

    throw std::runtime_error("SyntaxError: Unexpected export syntax at line " + std::to_string(current_token_.location.line));
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
        case TokenKind::RegExp: {
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(new JSString(tok.text))));
            args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(new JSString(tok.regex_flags))));
            return std::make_unique<NewExpr>(std::make_unique<IdentifierExpr>("RegExp"), std::move(args));
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
            auto expr = ParseExpression(Precedence::Comma);
            Consume(TokenKind::CloseParen, "Expected ')' after expression");
            return expr;
        }
        case TokenKind::OpenBrace: {
            // Object literal: { key: val, [expr]: val, name() {}, [expr]() {}, ...spread }
            std::vector<ObjectLiteralExpr::Property> props;
            auto parse_method_params = [this](std::vector<std::string>& params, std::string& rest_param) {
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
            };

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
                        if (Match(TokenKind::OpenParen)) {
                            std::vector<std::string> params;
                            std::string rest_param;
                            parse_method_params(params, rest_param);
                            auto body = ParseBlock();
                            auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                            if (!rest_param.empty()) fn_expr->SetRest(rest_param);
                            props.push_back(ObjectLiteralExpr::Property{
                                .key = "",
                                .computed_key = std::move(computed_key),
                                .value = std::move(fn_expr)
                            });
                            continue;
                        }
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
                    if (Match(TokenKind::OpenParen)) {
                        std::vector<std::string> params;
                        std::string rest_param;
                        parse_method_params(params, rest_param);
                        auto body = ParseBlock();
                        auto fn_expr = std::make_unique<FunctionExpr>(key_tok.text, std::move(params), std::move(body));
                        if (!rest_param.empty()) fn_expr->SetRest(rest_param);
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = key_tok.text,
                            .computed_key = nullptr,
                            .value = std::move(fn_expr)
                        });
                        continue;
                    }
                    if (Match(TokenKind::Colon)) {
                        auto val = ParseExpression();
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = key_tok.text,
                            .computed_key = nullptr,
                            .value = std::move(val)
                        });
                        continue;
                    }
                    if (key_tok.kind == TokenKind::Identifier) {
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = key_tok.text,
                            .computed_key = nullptr,
                            .value = std::make_unique<IdentifierExpr>(key_tok.text)
                        });
                        continue;
                    }
                    throw std::runtime_error("Expected ':' after property key");
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
        case TokenKind::Void:
            return std::make_unique<UnaryExpr>(UnaryOp::Void, ParseExpression(Precedence::Unary));
        case TokenKind::PlusPlus: {
            Token id_tok = Consume(TokenKind::Identifier, "Expected identifier after '++'");
            return std::make_unique<UpdateExpr>(UpdateOp::Increment, id_tok.text, /*is_prefix=*/true);
        }
        case TokenKind::MinusMinus: {
            Token id_tok = Consume(TokenKind::Identifier, "Expected identifier after '--'");
            return std::make_unique<UpdateExpr>(UpdateOp::Decrement, id_tok.text, /*is_prefix=*/true);
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
        case TokenKind::Import: {
            if (Match(TokenKind::OpenParen)) {
                auto spec_expr = ParseExpression();
                Consume(TokenKind::CloseParen, "Expected ')' after dynamic import() specifier");
                return std::make_unique<DynamicImportExpr>(std::move(spec_expr));
            }
            if (Match(TokenKind::Dot)) {
                Token meta_tok = Consume(TokenKind::Identifier, "Expected 'meta' after 'import.'");
                if (meta_tok.text != "meta") {
                    throw std::runtime_error("SyntaxError: Expected 'meta' after 'import.'");
                }
                return std::make_unique<ImportMetaExpr>();
            }
            throw std::runtime_error("SyntaxError: Unexpected token 'import' in expression position");
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
        op_token.kind == TokenKind::PercentEqual ||
        op_token.kind == TokenKind::StarStarEqual ||
        op_token.kind == TokenKind::LessLessEqual ||
        op_token.kind == TokenKind::GreaterGreaterEqual ||
        op_token.kind == TokenKind::GreaterGreaterGreaterEqual ||
        op_token.kind == TokenKind::AmpersandEqual ||
        op_token.kind == TokenKind::BarEqual ||
        op_token.kind == TokenKind::CaretEqual ||
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
            if (auto* arr = dynamic_cast<ArrayLiteralExpr*>(lhs.get())) {
                std::vector<std::string> names;
                for (size_t i = 0; i < arr->Elements().size(); ++i) {
                    if (auto* id_elem = dynamic_cast<IdentifierExpr*>(arr->Elements()[i].get())) {
                        names.push_back(id_elem->Name());
                    }
                }
                return std::make_unique<ArrayDestructureAssignExpr>(std::move(names), std::move(rhs));
            }
            throw std::runtime_error("Invalid assignment target");
        }

        BinaryOp bin_op;
        switch (op_token.kind) {
            case TokenKind::PlusEqual:                    bin_op = BinaryOp::Add; break;
            case TokenKind::MinusEqual:                   bin_op = BinaryOp::Sub; break;
            case TokenKind::StarEqual:                    bin_op = BinaryOp::Mul; break;
            case TokenKind::SlashEqual:                   bin_op = BinaryOp::Div; break;
            case TokenKind::PercentEqual:                 bin_op = BinaryOp::Mod; break;
            case TokenKind::StarStarEqual:                bin_op = BinaryOp::Exp; break;
            case TokenKind::LessLessEqual:                bin_op = BinaryOp::ShiftLeft; break;
            case TokenKind::GreaterGreaterEqual:          bin_op = BinaryOp::ShiftRight; break;
            case TokenKind::GreaterGreaterGreaterEqual:   bin_op = BinaryOp::ShiftRightUnsigned; break;
            case TokenKind::AmpersandEqual:               bin_op = BinaryOp::BitwiseAnd; break;
            case TokenKind::BarEqual:                     bin_op = BinaryOp::BitwiseOr; break;
            case TokenKind::CaretEqual:                   bin_op = BinaryOp::BitwiseXor; break;
            case TokenKind::AmpAmpEqual:                  bin_op = BinaryOp::LogicalAnd; break;
            case TokenKind::BarBarEqual:                  bin_op = BinaryOp::LogicalOr; break;
            case TokenKind::QuestionQuestionEqual:        bin_op = BinaryOp::NullishCoalescing; break;
            default: throw std::runtime_error("Unhandled compound assignment");
        }

        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            auto bin_expr = std::make_unique<BinaryExpr>(bin_op, std::make_unique<IdentifierExpr>(id->Name()), std::move(rhs));
            return std::make_unique<AssignExpr>(id->Name(), std::move(bin_expr));
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            return std::make_unique<CompoundPropertyAssignExpr>(bin_op, std::move(obj), prop_name, std::move(rhs));
        }
        if (auto* idx = dynamic_cast<IndexAccessExpr*>(lhs.get())) {
            auto obj = idx->ReleaseObject();
            auto index = idx->ReleaseIndex();
            return std::make_unique<CompoundIndexAssignExpr>(bin_op, std::move(obj), std::move(index), std::move(rhs));
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
            return std::make_unique<UpdateExpr>(UpdateOp::Increment, id->Name(), /*is_prefix=*/false);
        }
        throw std::runtime_error("Invalid left-hand side in postfix ++");
    }

    if (op_token.kind == TokenKind::MinusMinus) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            return std::make_unique<UpdateExpr>(UpdateOp::Decrement, id->Name(), /*is_prefix=*/false);
        }
        throw std::runtime_error("Invalid left-hand side in postfix --");
    }

    if (op_token.kind == TokenKind::Question) {
        auto then_expr = ParseExpression();
        Consume(TokenKind::Colon, "Expected ':' in conditional expression");
        auto else_expr = ParseExpression(Precedence::Conditional);
        return std::make_unique<ConditionalExpr>(std::move(lhs), std::move(then_expr), std::move(else_expr));
    }

    if (op_token.kind == TokenKind::Comma) {
        auto rhs = ParseExpression(Precedence::Comma);
        return std::make_unique<BinaryExpr>(BinaryOp::Comma, std::move(lhs), std::move(rhs));
    }

    if (op_token.kind == TokenKind::StarStar) {
        // Exponentiation is right-associative: 2 ** 3 ** 2 is 2 ** (3 ** 2)
        auto rhs = ParseExpression(Precedence::Exponentiation);
        return std::make_unique<BinaryExpr>(BinaryOp::Exp, std::move(lhs), std::move(rhs));
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
        case TokenKind::GreaterGreaterGreater: op = BinaryOp::ShiftRightUnsigned; break;
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
