#include "aether/parser.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include <stdexcept>
#include <algorithm>

namespace aether {

struct FunctionContextGuard {
    Parser& parser;
    bool prev_gen;
    bool prev_async;
    FunctionContextGuard(Parser& p, bool is_gen, bool is_async)
        : parser(p), prev_gen(p.in_generator_), prev_async(p.in_async_) {
        parser.in_generator_ = is_gen;
        parser.in_async_ = is_async;
        parser.function_depth_++;
    }
    ~FunctionContextGuard() {
        parser.in_generator_ = prev_gen;
        parser.in_async_ = prev_async;
        parser.function_depth_--;
    }
};

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

static bool IsReservedWord(std::string_view word) {
    static const std::unordered_set<std::string_view> reserved = {
        "break", "case", "catch", "class", "const", "continue",
        "debugger", "default", "delete", "do", "else", "enum", "export", "extends",
        "false", "finally", "for", "function", "if", "import", "in",
        "instanceof", "new", "null", "return", "super", "switch", "this",
        "throw", "true", "try", "typeof", "var", "void", "while", "with"
    };
    return reserved.find(word) != reserved.end();
}

static bool IsStrictReservedWord(std::string_view word) {
    static const std::unordered_set<std::string_view> strict_reserved = {
        "implements", "interface", "let", "package", "private", "protected", "public", "static", "yield"
    };
    return strict_reserved.find(word) != strict_reserved.end();
}

static bool IsAllDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

struct YieldAwaitChecker : public ASTVisitor {
    bool found_yield{false};
    bool found_await{false};

    void VisitLiteral(LiteralExpr*) override {}
    void VisitIdentifier(IdentifierExpr*) override {}
    void VisitUnary(UnaryExpr* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
    void VisitBinary(BinaryExpr* n) override {
        if (n->Lhs()) n->Lhs()->Accept(this);
        if (n->Rhs()) n->Rhs()->Accept(this);
    }
    void VisitAssign(AssignExpr* n) override { if (n->Value()) n->Value()->Accept(this); }
    void VisitArrayDestructureAssign(ArrayDestructureAssignExpr* n) override { if (n->Value()) n->Value()->Accept(this); }
    void VisitPropertyAccess(PropertyAccessExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
    void VisitPrivateAccess(PrivateAccessExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
    void VisitPropertyAssign(PropertyAssignExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Value()) n->Value()->Accept(this);
    }
    void VisitCall(CallExpr* n) override {
        if (n->Callee()) n->Callee()->Accept(this);
        for (const auto& a : n->Args()) if (a) a->Accept(this);
    }
    void VisitNew(NewExpr* n) override {
        if (n->Callee()) n->Callee()->Accept(this);
        for (const auto& a : n->Args()) if (a) a->Accept(this);
    }
    void VisitFunctionExpr(FunctionExpr*) override {}
    void VisitObjectLiteral(ObjectLiteralExpr* n) override {
        for (const auto& p : n->Properties()) {
            if (p.computed_key) p.computed_key->Accept(this);
            if (p.value) p.value->Accept(this);
        }
    }
    void VisitArrayLiteral(ArrayLiteralExpr* n) override {
        for (const auto& e : n->Elements()) if (e) e->Accept(this);
    }
    void VisitIndexAccess(IndexAccessExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Index()) n->Index()->Accept(this);
    }
    void VisitIndexAssign(IndexAssignExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Index()) n->Index()->Accept(this);
        if (n->Value()) n->Value()->Accept(this);
    }
    void VisitCompoundAssign(CompoundAssignExpr* n) override { if (n->Rhs()) n->Rhs()->Accept(this); }
    void VisitCompoundPropertyAssign(CompoundPropertyAssignExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Rhs()) n->Rhs()->Accept(this);
    }
    void VisitCompoundPrivateAssign(CompoundPrivateAssignExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Rhs()) n->Rhs()->Accept(this);
    }
    void VisitCompoundIndexAssign(CompoundIndexAssignExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Index()) n->Index()->Accept(this);
        if (n->Rhs()) n->Rhs()->Accept(this);
    }
    void VisitUpdate(UpdateExpr*) override {}
    void VisitUpdateProperty(UpdatePropertyExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
    void VisitUpdateIndex(UpdateIndexExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Index()) n->Index()->Accept(this);
    }
    void VisitConditional(ConditionalExpr* n) override {
        if (n->Condition()) n->Condition()->Accept(this);
        if (n->ThenBranch()) n->ThenBranch()->Accept(this);
        if (n->ElseBranch()) n->ElseBranch()->Accept(this);
    }
    void VisitTemplateLiteral(TemplateLiteralExpr* n) override {
        for (const auto& e : n->Expressions()) if (e) e->Accept(this);
    }
    void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
    void VisitOptionalIndexAccess(OptionalIndexAccessExpr* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Index()) n->Index()->Accept(this);
    }
    void VisitOptionalCall(OptionalCallExpr* n) override {
        if (n->Callee()) n->Callee()->Accept(this);
        for (const auto& a : n->Args()) if (a) a->Accept(this);
    }
    void VisitSpread(SpreadExpr* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
    void VisitYield(YieldExpr*) override { found_yield = true; }
    void VisitAwait(AwaitExpr*) override { found_await = true; }
    void VisitImportMeta(ImportMetaExpr*) override {}
    void VisitDynamicImport(DynamicImportExpr* n) override { if (n->Specifier()) n->Specifier()->Accept(this); }
    void VisitExprStmt(ExprStmt* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
    void VisitVarDecl(VarDeclStmt* n) override { if (n->Initializer()) n->Initializer()->Accept(this); }
    void VisitBlock(BlockStmt* n) override { for (const auto& s : n->Statements()) if (s) s->Accept(this); }
    void VisitIf(IfStmt* n) override {
        if (n->Condition()) n->Condition()->Accept(this);
        if (n->ThenBranch()) n->ThenBranch()->Accept(this);
        if (n->ElseBranch()) n->ElseBranch()->Accept(this);
    }
    void VisitWhile(WhileStmt* n) override {
        if (n->Condition()) n->Condition()->Accept(this);
        if (n->Body()) n->Body()->Accept(this);
    }
    void VisitDoWhile(DoWhileStmt* n) override {
        if (n->Body()) n->Body()->Accept(this);
        if (n->Condition()) n->Condition()->Accept(this);
    }
    void VisitFor(ForStmt* n) override {
        if (n->Init()) n->Init()->Accept(this);
        if (n->Cond()) n->Cond()->Accept(this);
        if (n->Update()) n->Update()->Accept(this);
        if (n->Body()) n->Body()->Accept(this);
    }
    void VisitBreak(BreakStmt*) override {}
    void VisitContinue(ContinueStmt*) override {}
    void VisitSwitch(SwitchStmt* n) override {
        if (n->Discriminant()) n->Discriminant()->Accept(this);
        for (const auto& c : n->Cases()) {
            if (c.test) c.test->Accept(this);
            for (const auto& s : c.statements) if (s) s->Accept(this);
        }
    }
    void VisitDebugger(DebuggerStmt*) override {}
    void VisitFunctionDecl(FunctionDeclStmt*) override {}
    void VisitClassDecl(ClassDeclStmt*) override {}
    void VisitImportDecl(ImportDeclStmt*) override {}
    void VisitExportDecl(ExportDeclStmt*) override {}
    void VisitWith(WithStmt* n) override {
        if (n->Object()) n->Object()->Accept(this);
        if (n->Body()) n->Body()->Accept(this);
    }
    void VisitThrow(ThrowStmt* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
    void VisitTryCatch(TryCatchStmt* n) override {
        if (n->TryBlock()) n->TryBlock()->Accept(this);
        if (n->CatchBlock()) n->CatchBlock()->Accept(this);
        if (n->FinallyBlock()) n->FinallyBlock()->Accept(this);
    }
    void VisitReturn(ReturnStmt* n) override { if (n->Value()) n->Value()->Accept(this); }
    void VisitProgram(Program* n) override { for (const auto& s : n->Statements()) if (s) s->Accept(this); }
};

Token Parser::Consume(TokenKind kind, std::string_view error_message) {
    if (Check(kind)) {
        if (kind == TokenKind::Identifier && current_token_.has_escape && IsReservedWord(current_token_.text)) {
            throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
        }
        return Advance();
    }
    throw std::runtime_error(std::string("SyntaxError: ") + std::string(error_message) + 
                             " at line " + std::to_string(current_token_.location.line) + 
                             ", col " + std::to_string(current_token_.location.column) + 
                             " (found '" + current_token_.text + "')");
}

void Parser::ConsumeSemicolon() {
    if (Match(TokenKind::Semicolon)) {
        return;
    }
    if (Check(TokenKind::CloseBrace) || Check(TokenKind::EndOfFile) || current_token_.has_leading_newline) {
        return;
    }
    throw std::runtime_error("SyntaxError: Unexpected token '" + current_token_.text + "' at line " +
                             std::to_string(current_token_.location.line) + ", col " +
                             std::to_string(current_token_.location.column));
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
        case TokenKind::Backtick:
            return Precedence::Call;
        case TokenKind::Dot:
        case TokenKind::QuestionDot:
        case TokenKind::OpenBracket:
            return Precedence::Member;
        default:
            return Precedence::None;
    }
}

static bool ContainsOptionalChain(const Expr* expr) {
    if (!expr) return false;
    if (dynamic_cast<const OptionalPropertyAccessExpr*>(expr) ||
        dynamic_cast<const OptionalIndexAccessExpr*>(expr) ||
        dynamic_cast<const OptionalCallExpr*>(expr)) {
        return true;
    }
    if (auto* prop = dynamic_cast<const PropertyAccessExpr*>(expr)) {
        return ContainsOptionalChain(prop->Object());
    }
    if (auto* idx = dynamic_cast<const IndexAccessExpr*>(expr)) {
        return ContainsOptionalChain(idx->Object());
    }
    if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
        return ContainsOptionalChain(call->Callee());
    }
    return false;
}

static bool IsOptionalChain(const Expr* expr) {
    if (!expr) return false;
    return dynamic_cast<const OptionalPropertyAccessExpr*>(expr) != nullptr ||
           dynamic_cast<const OptionalIndexAccessExpr*>(expr) != nullptr ||
           dynamic_cast<const OptionalCallExpr*>(expr) != nullptr;
}

std::unique_ptr<Program> Parser::ParseProgram() {
    NoGCScope compilation;
    auto prog = std::make_unique<Program>();
    bool in_directive_prologue = true;
    while (!Check(TokenKind::EndOfFile)) {
        Token first = Peek();
        auto statement = ParseStatement();
        if (in_directive_prologue && first.kind == TokenKind::String) {
            if (first.text == "use strict") {
                strict_mode_ = true;
            }
        } else {
            in_directive_prologue = false;
        }
        prog->AddStatement(std::move(statement));
    }
    prog->SetStrict(strict_mode_);
    return prog;
}

std::unique_ptr<Stmt> Parser::ParseStatement() {
    NoGCScope compilation;
    if (Match(TokenKind::Var)) {
        return ParseVarDecl(/*is_const=*/false, VarDeclStmt::Kind::Var);
    }
    if (Match(TokenKind::Let)) {
        return ParseVarDecl(/*is_const=*/false, VarDeclStmt::Kind::Let);
    }
    if (Match(TokenKind::Const)) {
        return ParseVarDecl(/*is_const=*/true, VarDeclStmt::Kind::Const);
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
    if (Match(TokenKind::With)) {
        if (strict_mode_) {
            throw std::runtime_error("SyntaxError: Strict mode code may not include a with statement");
        }
        return ParseWith();
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

std::unique_ptr<Stmt> Parser::ParseVarDecl(bool is_const, VarDeclStmt::Kind kind) {
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
                stmts.push_back(std::make_unique<VarDeclStmt>(elems[i].name, std::move(init_expr), is_const, kind));
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
                        stmts.push_back(std::make_unique<VarDeclStmt>(it.local_var, std::move(init_expr), is_const, kind));
                    }
                }
            };

            emit_bindings(temp_var, items);
            return stmts;
        }

        // 3. Regular variable declaration: let x = expr;
        Token name_tok;
        if (Check(TokenKind::Identifier)) {
            name_tok = Advance();
        } else if (!in_async_ && !is_module_ && Check(TokenKind::Await)) {
            name_tok = Advance();
        } else if (!strict_mode_ && !in_generator_ && Check(TokenKind::Yield)) {
            name_tok = Advance();
        } else {
            name_tok = Consume(TokenKind::Identifier, "Expected identifier in variable declaration");
        }
        std::unique_ptr<Expr> init = nullptr;
        if (Match(TokenKind::Equal)) {
            init = ParseExpression();
            if (dynamic_cast<SpreadExpr*>(init.get())) {
                throw std::runtime_error("SyntaxError: Unexpected token '...'");
            }
        }
        stmts.push_back(std::make_unique<VarDeclStmt>(name_tok.text, std::move(init), is_const, kind));
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
    Token name_tok;
    if (Check(TokenKind::Identifier)) {
        name_tok = Advance();
    } else if (Check(TokenKind::Await) && !is_async && !in_async_ && !is_module_) {
        name_tok = Advance();
    } else if (Check(TokenKind::Yield) && !is_generator && !in_generator_ && !strict_mode_) {
        name_tok = Advance();
    } else {
        name_tok = Consume(TokenKind::Identifier, "Expected function name");
    }
    Consume(TokenKind::OpenParen, "Expected '(' after function name");

    auto parse_fn_param = [&]() -> Token {
        if (Check(TokenKind::Identifier)) return Advance();
        if (Check(TokenKind::Await) && !is_async && !in_async_ && !is_module_) return Advance();
        if (Check(TokenKind::Yield) && !is_generator && !in_generator_ && !strict_mode_) return Advance();
        return Consume(TokenKind::Identifier, "Expected parameter name");
    };

    std::vector<std::string> params;
    std::vector<std::unique_ptr<Expr>> param_defaults;
    std::string rest_param = "";
    if (!Check(TokenKind::CloseParen)) {
        if (Match(TokenKind::DotDotDot)) {
            Token r = parse_fn_param();
            if (Check(TokenKind::Equal)) throw std::runtime_error("SyntaxError: Rest parameter may not have a default initializer");
            if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
            rest_param = r.text;
        } else {
            Token param = parse_fn_param();
            std::unique_ptr<Expr> def = nullptr;
            if (Match(TokenKind::Equal)) {
                def = ParseExpression();
            }
            params.push_back(param.text);
            param_defaults.push_back(std::move(def));
            while (Match(TokenKind::Comma)) {
                if (Check(TokenKind::CloseParen)) break;
                if (Match(TokenKind::DotDotDot)) {
                    Token r = parse_fn_param();
                    if (Check(TokenKind::Equal)) throw std::runtime_error("SyntaxError: Rest parameter may not have a default initializer");
                    if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
                    rest_param = r.text;
                    break;
                }
                Token next_param = parse_fn_param();
                std::unique_ptr<Expr> next_def = nullptr;
                if (Match(TokenKind::Equal)) {
                    next_def = ParseExpression();
                }
                params.push_back(next_param.text);
                param_defaults.push_back(std::move(next_def));
            }
        }
    }
    Consume(TokenKind::CloseParen, "Expected ')' after parameters");

    std::unique_ptr<BlockStmt> body;
    {
        FunctionContextGuard guard(*this, is_generator, is_async);
        body = ParseBlock();
    }
    auto fn_decl = std::make_unique<FunctionDeclStmt>(name_tok.text, std::move(params), std::move(body));
    fn_decl->SetParamDefaults(std::move(param_defaults));
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
            throw std::runtime_error("SyntaxError: Expected 'case' or 'default' in switch statement");
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
        bool is_of = Match(TokenKind::Of) || (Check(TokenKind::Identifier) && current_token_.text == "of" && (Advance(), true));
        if (is_of) {
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
        bool is_of = Match(TokenKind::Of) || (Check(TokenKind::Identifier) && current_token_.text == "of" && (Advance(), true));
        if (is_of) {
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
        bool saved_allow_in = allow_in_;
        allow_in_ = false;
        if (Match(TokenKind::Var)) {
            init = ParseVarDecl(false, VarDeclStmt::Kind::Var);
        } else if (Match(TokenKind::Let)) {
            init = ParseVarDecl(false, VarDeclStmt::Kind::Let);
        } else if (Match(TokenKind::Const)) {
            init = ParseVarDecl(true, VarDeclStmt::Kind::Const);
        } else {
            init = ParseExprStmt();
        }
        allow_in_ = saved_allow_in;
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

Parser::ParsedClassBody Parser::ParseClassElements(std::string_view super_name) {
    bool prev_strict = strict_mode_;
    strict_mode_ = true;
    Consume(TokenKind::OpenBrace, "Expected '{' in class body");

    // Pre-scan class body for declared private names
    std::unordered_set<std::string> declared_in_this_class;
    Lexer scan_lexer = lexer_;
    Token scan_cur = current_token_;
    int scan_brace_depth = 1;
    while (scan_brace_depth > 0 && scan_cur.kind != TokenKind::EndOfFile) {
        if (scan_cur.kind == TokenKind::OpenBrace) {
            scan_brace_depth++;
        } else if (scan_cur.kind == TokenKind::CloseBrace) {
            scan_brace_depth--;
            if (scan_brace_depth == 0) break;
        } else if (scan_brace_depth == 1 && scan_cur.kind == TokenKind::Hash) {
            Token next = scan_lexer.NextToken();
            if (next.kind == TokenKind::Identifier) {
                Token after_next = scan_lexer.PeekToken();
                if (after_next.kind != TokenKind::In) {
                    declared_in_this_class.insert(next.text);
                }
            }
            scan_cur = scan_lexer.NextToken();
            continue;
        }
        scan_cur = scan_lexer.NextToken();
    }
    declared_private_names_stack_.push_back(std::move(declared_in_this_class));

    ParsedClassBody body;
    while (!Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
        if (Match(TokenKind::Semicolon)) continue;

        bool is_static = false;
        if (Check(TokenKind::Static) || (Check(TokenKind::Identifier) && current_token_.text == "static")) {
            Token next = lexer_.PeekToken();
            if (next.kind != TokenKind::OpenParen && next.kind != TokenKind::Equal &&
                next.kind != TokenKind::Semicolon && next.kind != TokenKind::CloseBrace) {
                Advance();
                is_static = true;
            }
        }

        bool is_async = false;
        if (Check(TokenKind::Async) || (Check(TokenKind::Identifier) && current_token_.text == "async")) {
            Token next = lexer_.PeekToken();
            if (next.kind != TokenKind::OpenParen && next.kind != TokenKind::Equal &&
                next.kind != TokenKind::Semicolon && next.kind != TokenKind::CloseBrace) {
                Advance();
                is_async = true;
            }
        }

        bool is_generator = false;
        if (Match(TokenKind::Star)) {
            is_generator = true;
        }

        bool is_getter = false;
        bool is_setter = false;
        if (!is_async && !is_generator && Check(TokenKind::Identifier) &&
            (current_token_.text == "get" || current_token_.text == "set")) {
            Token next = lexer_.PeekToken();
            if (next.kind != TokenKind::OpenParen && next.kind != TokenKind::Equal &&
                next.kind != TokenKind::Semicolon && next.kind != TokenKind::CloseBrace) {
                Token gs = Advance();
                if (gs.text == "get") is_getter = true;
                else is_setter = true;
            }
        }

        std::string method_name_text;
        Token first = Advance();
        if (first.kind == TokenKind::Hash) {
            Token private_name = Consume(TokenKind::Identifier, "Expected private name after '#'");
            method_name_text = "#" + private_name.text;
        } else {
            method_name_text = first.text;
        }

        if (is_getter) {
            method_name_text = (method_name_text.front() == '#' ? "__get__#" + method_name_text.substr(1) : "__get__" + method_name_text);
        } else if (is_setter) {
            method_name_text = (method_name_text.front() == '#' ? "__set__#" + method_name_text.substr(1) : "__set__" + method_name_text);
        }

        if (is_static) {
            method_name_text = "__static__" + method_name_text;
        }

        if (Check(TokenKind::Semicolon) || Check(TokenKind::Equal) || !Check(TokenKind::OpenParen)) {
            std::unique_ptr<Expr> initializer;
            if (Match(TokenKind::Equal)) initializer = ParseExpression(Precedence::Comma);
            Match(TokenKind::Semicolon);
            body.fields.push_back({method_name_text, std::move(initializer)});
            continue;
        }

        Consume(TokenKind::OpenParen, "Expected '(' after method name");
        std::vector<std::string> params;
        std::vector<std::unique_ptr<Expr>> param_defaults;
        if (!Check(TokenKind::CloseParen)) {
            Token p = Consume(TokenKind::Identifier, "Expected parameter name");
            std::unique_ptr<Expr> def = nullptr;
            if (Match(TokenKind::Equal)) {
                def = ParseExpression();
            }
            params.push_back(p.text);
            param_defaults.push_back(std::move(def));
            while (Match(TokenKind::Comma)) {
                Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                std::unique_ptr<Expr> next_def = nullptr;
                if (Match(TokenKind::Equal)) {
                    next_def = ParseExpression();
                }
                params.push_back(np.text);
                param_defaults.push_back(std::move(next_def));
            }
        }
        Consume(TokenKind::CloseParen, "Expected ')' after method parameters");

        bool prev_in_derived = in_derived_constructor_;
        if (method_name_text == "constructor") {
            in_derived_constructor_ = !super_name.empty();
        } else {
            in_derived_constructor_ = false;
        }

        std::unique_ptr<BlockStmt> block_body;
        {
            FunctionContextGuard guard(*this, is_generator, is_async);
            block_body = ParseBlock();
        }
        in_derived_constructor_ = prev_in_derived;

        auto method_decl = std::make_unique<FunctionDeclStmt>(method_name_text, std::move(params), std::move(block_body));
        method_decl->SetParamDefaults(std::move(param_defaults));
        method_decl->SetAsync(is_async);
        method_decl->SetGenerator(is_generator);
        body.methods.push_back(std::move(method_decl));
    }

    Consume(TokenKind::CloseBrace, "Expected '}' after class body");
    declared_private_names_stack_.pop_back();
    strict_mode_ = prev_strict;
    return body;
}

std::unique_ptr<Stmt> Parser::ParseClassDecl() {
    Token name_tok = Consume(TokenKind::Identifier, "Expected class name");
    std::string super_name = "";
    if (Match(TokenKind::Extends)) {
        Token super_tok = Consume(TokenKind::Identifier, "Expected super class name after 'extends'");
        super_name = super_tok.text;
    }
    auto body = ParseClassElements(super_name);
    auto class_node = std::make_unique<ClassDeclStmt>(name_tok.text, super_name, std::move(body.methods));
    for (auto& f : body.fields) {
        class_node->AddField(f.name, std::move(f.initializer));
    }
    return class_node;
}

std::unique_ptr<Stmt> Parser::ParseWith() {
    Consume(TokenKind::OpenParen, "Expected '(' after 'with'");
    auto expr = ParseExpression();
    Consume(TokenKind::CloseParen, "Expected ')' after with expression");
    auto body = ParseStatement();
    return std::make_unique<WithStmt>(std::move(expr), std::move(body));
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
    if (dynamic_cast<SpreadExpr*>(expr.get())) {
        throw std::runtime_error("SyntaxError: Unexpected token '...'");
    }
    ConsumeSemicolon();
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
    if (Match(TokenKind::Var)) {
        auto var_decl = ParseVarDecl(false, VarDeclStmt::Kind::Var);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(var_decl), nullptr, std::vector<ExportSpecifier>{});
    }
    if (Match(TokenKind::Let)) {
        auto var_decl = ParseVarDecl(false, VarDeclStmt::Kind::Let);
        return std::make_unique<ExportDeclStmt>(/*is_default=*/false, std::move(var_decl), nullptr, std::vector<ExportSpecifier>{});
    }
    if (Match(TokenKind::Const)) {
        auto var_decl = ParseVarDecl(true, VarDeclStmt::Kind::Const);
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
    NoGCScope compilation;
    auto lhs = ParsePrefix(precedence);

    while (precedence <= GetPrecedence(Peek().kind)) {
        if ((Peek().kind == TokenKind::PlusPlus || Peek().kind == TokenKind::MinusMinus) &&
            Peek().has_leading_newline) {
            break;
        }
        Token op_token = Advance();
        lhs = ParseInfix(std::move(lhs), op_token);
    }

    return lhs;
}

std::unique_ptr<Expr> Parser::ParsePrefix(Precedence precedence) {
    Token tok = Advance();
    switch (tok.kind) {
        case TokenKind::Number: {
            if (tok.is_bigint) {
                std::string s = tok.text;
                if (!s.empty() && (s.back() == 'n' || s.back() == 'N')) {
                    s.pop_back();
                }
                return std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(AllocateRuntime<JSBigInt>(s)));
            }
            if (tok.is_integer) {
                return std::make_unique<LiteralExpr>(TaggedValue::FromSmi(tok.int_value));
            }
            return std::make_unique<LiteralExpr>(TaggedValue::FromDouble(tok.number_value));
        }
        case TokenKind::String: {
            auto* s = AllocateRuntime<JSString>(tok.text);
            return std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(s));
        }
        case TokenKind::RegExp: {
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(tok.text))));
            args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(tok.regex_flags))));
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
            for (bool inv : tok.template_has_invalid_escape) {
                if (inv) {
                    throw std::runtime_error("SyntaxError: Invalid escape sequence in template literal");
                }
            }
            if (tok.template_exprs.empty()) {
                auto* s = AllocateRuntime<JSString>(tok.template_quasis.empty() ? "" : tok.template_quasis[0]);
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
            if (tok.has_escape && IsReservedWord(tok.text)) {
                throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
            }
            if (Check(TokenKind::Arrow)) {
                if (precedence > Precedence::Assignment) {
                    throw std::runtime_error("SyntaxError: Unexpected token '=>'");
                }
                if (current_token_.has_leading_newline) {
                    throw std::runtime_error("SyntaxError: Line terminator not allowed before '=>'");
                }
                if (IsReservedWord(tok.text)) {
                    throw std::runtime_error("SyntaxError: Unexpected reserved word: " + tok.text);
                }
                if (tok.text == "eval" || tok.text == "arguments") {
                    if (strict_mode_ || is_module_) {
                        throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                    }
                }
                if (IsStrictReservedWord(tok.text)) {
                    if (strict_mode_ || is_module_) {
                        throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + tok.text);
                    }
                }
                if (tok.text == "await" && (in_async_ || is_module_)) {
                    throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                }
                if (tok.text == "yield" && (in_generator_ || strict_mode_)) {
                    throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                }

                Advance(); // Consume '=>'
                std::vector<std::string> params = {tok.text};
                bool prev_gen = in_generator_;
                bool prev_async = in_async_;
                in_generator_ = false;
                in_async_ = false;

                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    bool saved_allow_in = allow_in_;
                    allow_in_ = true;
                    auto arrow_expr = ParseExpression();
                    allow_in_ = saved_allow_in;
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }

                in_generator_ = prev_gen;
                in_async_ = prev_async;

                bool body_is_strict = strict_mode_ || is_module_;
                if (!body_is_strict && body && !body->Statements().empty()) {
                    if (auto* es = dynamic_cast<ExprStmt*>(body->Statements().front().get())) {
                        if (auto* lit = dynamic_cast<LiteralExpr*>(es->GetExpr())) {
                            if (lit->Value().IsHeapObject() && lit->Value().AsHeapObject()->IsString()) {
                                if (static_cast<JSString*>(lit->Value().AsHeapObject())->Value() == "use strict") {
                                    body_is_strict = true;
                                }
                            }
                        }
                    }
                }
                if (body_is_strict) {
                    if (tok.text == "eval" || tok.text == "arguments") {
                        throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                    }
                    if (IsStrictReservedWord(tok.text)) {
                        throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + tok.text);
                    }
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetArrow(true);
                return fn_expr;
            }
            return std::make_unique<IdentifierExpr>(tok.text);
        }
        case TokenKind::OpenParen: {
            Lexer saved_lexer = lexer_;
            Token saved_tok = current_token_;

            // Depth-balanced scan to check if matching ')' is followed by '=>'
            bool is_arrow = false;
            bool arrow_has_newline = false;
            int paren_depth = 1;
            int brace_depth = 0;
            int bracket_depth = 0;

            Lexer lookahead_lexer = lexer_;
            Token scan_tok = current_token_;
            while (scan_tok.kind != TokenKind::EndOfFile) {
                if (scan_tok.kind == TokenKind::OpenParen) {
                    paren_depth++;
                } else if (scan_tok.kind == TokenKind::CloseParen) {
                    paren_depth--;
                    if (paren_depth == 0) {
                        Token next_tok = lookahead_lexer.NextToken();
                        if (next_tok.kind == TokenKind::Arrow) {
                            is_arrow = true;
                            arrow_has_newline = next_tok.has_leading_newline;
                        }
                        break;
                    }
                } else if (scan_tok.kind == TokenKind::OpenBrace) {
                    brace_depth++;
                } else if (scan_tok.kind == TokenKind::CloseBrace) {
                    if (brace_depth > 0) brace_depth--;
                } else if (scan_tok.kind == TokenKind::OpenBracket) {
                    bracket_depth++;
                } else if (scan_tok.kind == TokenKind::CloseBracket) {
                    if (bracket_depth > 0) bracket_depth--;
                }
                scan_tok = lookahead_lexer.NextToken();
            }

            if (is_arrow) {
                if (precedence > Precedence::Assignment) {
                    throw std::runtime_error("SyntaxError: Unexpected token '=>'");
                }
                if (arrow_has_newline) {
                    throw std::runtime_error("SyntaxError: Line terminator not allowed before '=>'");
                }

                std::vector<std::string> arrow_params;
                std::vector<std::unique_ptr<Expr>> arrow_defaults;
                std::string arrow_rest;
                std::vector<std::unique_ptr<Stmt>> destructuring_stmts;
                std::unordered_set<std::string> param_names;
                bool has_destructuring = false;
                bool has_defaults = false;
                static uint32_t s_arrow_id = 0;

                auto add_param_name = [&](const std::string& name) {
                    if (param_names.count(name)) {
                        throw std::runtime_error("SyntaxError: Duplicate parameter name '" + name + "' not allowed in arrow function");
                    }
                    param_names.insert(name);
                };

                // Helper to parse pattern destructuring
                std::function<void(const std::string&, bool)> parse_pattern_binding;
                parse_pattern_binding = [&](const std::string& source_var, bool is_rest) {
                    has_destructuring = true;
                    if (Check(TokenKind::OpenBracket)) {
                        Advance(); // Consume '['
                        std::string iter_var = "$d_iter_" + std::to_string(++s_arrow_id);
                        std::string step_var = "$d_step_" + std::to_string(s_arrow_id);

                        // var $d_iter = source_var[Symbol.iterator]();
                        auto get_sym_iter = std::make_unique<PropertyAccessExpr>(
                            std::make_unique<IdentifierExpr>("Symbol"), "iterator");
                        auto get_iter_fn = std::make_unique<IndexAccessExpr>(
                            std::make_unique<IdentifierExpr>(source_var), std::move(get_sym_iter));
                        auto call_iter = std::make_unique<CallExpr>(
                            std::move(get_iter_fn), std::vector<std::unique_ptr<Expr>>{});
                        destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(iter_var, std::move(call_iter)));
                        destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(step_var, std::make_unique<LiteralExpr>(TaggedValue::Undefined())));

                        bool has_rest_elem = false;

                        while (!Check(TokenKind::CloseBracket) && !Check(TokenKind::EndOfFile)) {
                            if (Match(TokenKind::Comma)) {
                                // Elision: $d_step = $d_iter.next();
                                auto next_fn = std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(iter_var), "next");
                                auto call_next = std::make_unique<CallExpr>(std::move(next_fn), std::vector<std::unique_ptr<Expr>>{});
                                auto step_assign = std::make_unique<AssignExpr>(
                                    step_var, std::move(call_next));
                                destructuring_stmts.push_back(std::make_unique<ExprStmt>(std::move(step_assign)));
                                continue;
                            }
                            if (Match(TokenKind::DotDotDot)) {
                                has_rest_elem = true;
                                std::string rest_arr = "$d_rest_" + std::to_string(++s_arrow_id);
                                // var rest_arr = [];
                                destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(
                                    rest_arr, std::make_unique<ArrayLiteralExpr>(std::vector<std::unique_ptr<Expr>>{})));

                                // while (!($d_step = $d_iter.next()).done) rest_arr.push($d_step.value);
                                auto next_fn = std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(iter_var), "next");
                                auto call_next = std::make_unique<CallExpr>(std::move(next_fn), std::vector<std::unique_ptr<Expr>>{});
                                auto step_assign = std::make_unique<AssignExpr>(
                                    step_var, std::move(call_next));
                                auto done_access = std::make_unique<PropertyAccessExpr>(
                                    std::move(step_assign), "done");
                                auto loop_cond = std::make_unique<UnaryExpr>(UnaryOp::LogicalNot, std::move(done_access));

                                auto push_callee = std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(rest_arr), "push");
                                std::vector<std::unique_ptr<Expr>> push_args;
                                push_args.push_back(std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(step_var), "value"));
                                auto push_call = std::make_unique<CallExpr>(std::move(push_callee), std::move(push_args));
                                auto loop_body = std::make_unique<BlockStmt>();
                                loop_body->AddStatement(std::make_unique<ExprStmt>(std::move(push_call)));

                                destructuring_stmts.push_back(std::make_unique<WhileStmt>(
                                    std::move(loop_cond), std::move(loop_body)));

                                if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                                    Token r_tok = Advance();
                                    if (r_tok.has_escape && IsReservedWord(r_tok.text)) {
                                        throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                                    }
                                    if (IsReservedWord(r_tok.text)) {
                                        throw std::runtime_error("SyntaxError: Unexpected reserved word: " + r_tok.text);
                                    }
                                    if (r_tok.text == "eval" || r_tok.text == "arguments") {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                        }
                                    }
                                    if (IsStrictReservedWord(r_tok.text)) {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + r_tok.text);
                                        }
                                    }
                                    if (r_tok.text == "await" && (in_async_ || is_module_)) {
                                        throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                                    }
                                    if (r_tok.text == "yield" && (in_generator_ || strict_mode_)) {
                                        throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                                    }
                                    add_param_name(r_tok.text);
                                    if (Check(TokenKind::Equal)) {
                                        throw std::runtime_error("SyntaxError: Rest element cannot have default initializer");
                                    }
                                    destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(
                                        r_tok.text, std::make_unique<IdentifierExpr>(rest_arr)));
                                } else if (Check(TokenKind::OpenBracket) || Check(TokenKind::OpenBrace)) {
                                    parse_pattern_binding(rest_arr, false);
                                }
                                break;
                            }

                            // Normal element: $d_step = $d_iter.next();
                            auto next_fn = std::make_unique<PropertyAccessExpr>(
                                std::make_unique<IdentifierExpr>(iter_var), "next");
                            auto call_next = std::make_unique<CallExpr>(std::move(next_fn), std::vector<std::unique_ptr<Expr>>{});
                            auto step_assign = std::make_unique<AssignExpr>(
                                step_var, std::move(call_next));
                            destructuring_stmts.push_back(std::make_unique<ExprStmt>(std::move(step_assign)));

                            // var $d_val = ($d_step.done ? undefined : $d_step.value)
                            std::string val_var = "$d_val_" + std::to_string(++s_arrow_id);
                            auto done_cond = std::make_unique<PropertyAccessExpr>(
                                std::make_unique<IdentifierExpr>(step_var), "done");
                            auto val_expr = std::make_unique<PropertyAccessExpr>(
                                std::make_unique<IdentifierExpr>(step_var), "value");
                            auto access_expr = std::make_unique<ConditionalExpr>(
                                std::move(done_cond),
                                std::make_unique<LiteralExpr>(TaggedValue::Undefined()),
                                std::move(val_expr));
                            destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(val_var, std::move(access_expr)));

                            if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                                Token el_tok = Advance();
                                if (el_tok.has_escape && IsReservedWord(el_tok.text)) {
                                    throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                                }
                                if (IsReservedWord(el_tok.text)) {
                                    throw std::runtime_error("SyntaxError: Unexpected reserved word: " + el_tok.text);
                                }
                                if (el_tok.text == "eval" || el_tok.text == "arguments") {
                                    if (strict_mode_ || is_module_) {
                                        throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                    }
                                }
                                if (IsStrictReservedWord(el_tok.text)) {
                                    if (strict_mode_ || is_module_) {
                                        throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + el_tok.text);
                                    }
                                }
                                if (el_tok.text == "await" && (in_async_ || is_module_)) {
                                    throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                                }
                                if (el_tok.text == "yield" && (in_generator_ || strict_mode_)) {
                                    throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                                }
                                add_param_name(el_tok.text);
                                std::unique_ptr<Expr> init_expr;
                                if (Match(TokenKind::Equal)) {
                                    auto def_expr = ParseExpression();
                                    YieldAwaitChecker yac;
                                    def_expr->Accept(&yac);
                                    if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                                    if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                                    if (auto* fe = dynamic_cast<FunctionExpr*>(def_expr.get())) {
                                        if (fe->Name().empty() && !fe->HasStaticName()) fe->SetName(el_tok.text);
                                    }
                                    auto cond = std::make_unique<BinaryExpr>(
                                        BinaryOp::StrictEqual,
                                        std::make_unique<IdentifierExpr>(val_var),
                                        std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                                    init_expr = std::make_unique<ConditionalExpr>(
                                        std::move(cond), std::move(def_expr), std::make_unique<IdentifierExpr>(val_var));
                                } else {
                                    init_expr = std::make_unique<IdentifierExpr>(val_var);
                                }
                                destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(el_tok.text, std::move(init_expr)));
                            } else if (Check(TokenKind::OpenBracket) || Check(TokenKind::OpenBrace)) {
                                std::string sub_var = "$d_sub_" + std::to_string(++s_arrow_id);
                                size_t insert_idx = destructuring_stmts.size();
                                parse_pattern_binding(sub_var, false);
                                std::unique_ptr<Expr> init_expr;
                                if (Match(TokenKind::Equal)) {
                                    auto def_expr = ParseExpression();
                                    YieldAwaitChecker yac;
                                    def_expr->Accept(&yac);
                                    if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                                    if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                                    auto cond = std::make_unique<BinaryExpr>(
                                        BinaryOp::StrictEqual,
                                        std::make_unique<IdentifierExpr>(val_var),
                                        std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                                    init_expr = std::make_unique<ConditionalExpr>(
                                        std::move(cond), std::move(def_expr), std::make_unique<IdentifierExpr>(val_var));
                                } else {
                                    init_expr = std::make_unique<IdentifierExpr>(val_var);
                                }
                                destructuring_stmts.insert(destructuring_stmts.begin() + insert_idx,
                                                           std::make_unique<VarDeclStmt>(sub_var, std::move(init_expr)));
                            } else {
                                throw std::runtime_error("SyntaxError: Unexpected token in array destructuring pattern");
                            }

                            if (Match(TokenKind::Comma)) {
                                continue;
                            }
                        }
                        Consume(TokenKind::CloseBracket, "Expected ']' in array destructuring pattern");

                        // If pattern ended without a rest element, close the iterator:
                        // if ((step_var === undefined || !step_var.done) && typeof iter_var.return === "function") iter_var.return();
                        if (!has_rest_elem) {
                            auto is_step_undef = std::make_unique<BinaryExpr>(
                                BinaryOp::StrictEqual,
                                std::make_unique<IdentifierExpr>(step_var),
                                std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                            auto not_done = std::make_unique<UnaryExpr>(
                                UnaryOp::LogicalNot,
                                std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(step_var), "done"));
                            auto step_not_done = std::make_unique<BinaryExpr>(
                                BinaryOp::LogicalOr,
                                std::move(is_step_undef),
                                std::move(not_done));
                            auto ret_prop = std::make_unique<PropertyAccessExpr>(
                                std::make_unique<IdentifierExpr>(iter_var), "return");
                            auto ret_type = std::make_unique<UnaryExpr>(UnaryOp::TypeOf, std::move(ret_prop));
                            auto is_fn = std::make_unique<BinaryExpr>(
                                BinaryOp::StrictEqual,
                                std::move(ret_type),
                                std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(AllocateRuntime<JSString>("function"))));
                            auto close_cond = std::make_unique<BinaryExpr>(
                                BinaryOp::LogicalAnd,
                                std::move(step_not_done),
                                std::move(is_fn));
                            auto call_ret = std::make_unique<CallExpr>(
                                std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(iter_var), "return"),
                                std::vector<std::unique_ptr<Expr>>{});
                            auto if_close = std::make_unique<IfStmt>(
                                std::move(close_cond),
                                std::make_unique<ExprStmt>(std::move(call_ret)),
                                nullptr);
                            destructuring_stmts.push_back(std::move(if_close));
                        }
                    } else if (Check(TokenKind::OpenBrace)) {
                        Advance(); // Consume '{'

                        // RequireObjectCoercible(source_var):
                        // if (source_var === undefined || source_var === null) throw new TypeError("Cannot destructure undefined or null");
                        auto cond_undef = std::make_unique<BinaryExpr>(
                            BinaryOp::StrictEqual,
                            std::make_unique<IdentifierExpr>(source_var),
                            std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                        auto cond_null = std::make_unique<BinaryExpr>(
                            BinaryOp::StrictEqual,
                            std::make_unique<IdentifierExpr>(source_var),
                            std::make_unique<LiteralExpr>(TaggedValue::Null()));
                        auto is_nullish = std::make_unique<BinaryExpr>(
                            BinaryOp::LogicalOr,
                            std::move(cond_undef),
                            std::move(cond_null));
                        std::vector<std::unique_ptr<Expr>> err_args;
                        err_args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Cannot destructure undefined or null"))));
                        auto new_err = std::make_unique<NewExpr>(
                            std::make_unique<IdentifierExpr>("TypeError"),
                            std::move(err_args));
                        auto throw_stmt = std::make_unique<ThrowStmt>(std::move(new_err));
                        destructuring_stmts.push_back(std::make_unique<IfStmt>(
                            std::move(is_nullish),
                            std::move(throw_stmt),
                            nullptr));

                        struct ExtractedProp {
                            bool is_computed{false};
                            std::string key_or_var;
                        };
                        std::vector<ExtractedProp> extracted_props;

                        while (!Check(TokenKind::CloseBrace) && !Check(TokenKind::EndOfFile)) {
                            if (Match(TokenKind::DotDotDot)) {
                                if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                                    Token r_tok = Advance();
                                    if (r_tok.has_escape && IsReservedWord(r_tok.text)) {
                                        throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                                    }
                                    if (IsReservedWord(r_tok.text)) {
                                        throw std::runtime_error("SyntaxError: Unexpected reserved word: " + r_tok.text);
                                    }
                                    if (r_tok.text == "eval" || r_tok.text == "arguments") {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                        }
                                    }
                                    if (IsStrictReservedWord(r_tok.text)) {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + r_tok.text);
                                        }
                                    }
                                    if (r_tok.text == "await" && (in_async_ || is_module_)) {
                                        throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                                    }
                                    if (r_tok.text == "yield" && (in_generator_ || strict_mode_)) {
                                        throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                                    }
                                    add_param_name(r_tok.text);
                                    if (Check(TokenKind::Equal)) {
                                        throw std::runtime_error("SyntaxError: Rest element cannot have default initializer");
                                    }
                                    if (Check(TokenKind::Comma)) {
                                        throw std::runtime_error("SyntaxError: Rest property may not have a trailing comma");
                                    }

                                    // var rest = Object.assign({}, source_var);
                                    auto assign_callee = std::make_unique<PropertyAccessExpr>(
                                        std::make_unique<IdentifierExpr>("Object"), "assign");
                                    std::vector<std::unique_ptr<Expr>> assign_args;
                                    assign_args.push_back(std::make_unique<ObjectLiteralExpr>(std::vector<ObjectLiteralExpr::Property>{}));
                                    assign_args.push_back(std::make_unique<IdentifierExpr>(source_var));
                                    auto assign_call = std::make_unique<CallExpr>(std::move(assign_callee), std::move(assign_args));
                                    destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(r_tok.text, std::move(assign_call)));

                                    // delete rest[key] for each extracted property
                                    for (const auto& ep : extracted_props) {
                                        std::unique_ptr<Expr> del_target;
                                        if (ep.is_computed) {
                                            del_target = std::make_unique<IndexAccessExpr>(
                                                std::make_unique<IdentifierExpr>(r_tok.text),
                                                std::make_unique<IdentifierExpr>(ep.key_or_var));
                                        } else {
                                            del_target = std::make_unique<PropertyAccessExpr>(
                                                std::make_unique<IdentifierExpr>(r_tok.text),
                                                ep.key_or_var);
                                        }
                                        destructuring_stmts.push_back(std::make_unique<ExprStmt>(
                                            std::make_unique<UnaryExpr>(UnaryOp::Delete, std::move(del_target))));
                                    }
                                } else {
                                    throw std::runtime_error("SyntaxError: Expected identifier after '...' in object pattern");
                                }
                                break;
                            }

                            bool is_computed = false;
                            std::string prop_name;
                            std::string local_name;
                            std::string computed_key_var;
                            bool prop_has_escape = false;

                            if (Check(TokenKind::OpenBracket)) {
                                Advance(); // consume '['
                                is_computed = true;
                                bool saved_allow_in = allow_in_;
                                allow_in_ = true;
                                auto key_expr = ParseExpression();
                                allow_in_ = saved_allow_in;
                                Consume(TokenKind::CloseBracket, "Expected ']' after computed property name");
                                computed_key_var = "$d_key_" + std::to_string(++s_arrow_id);
                                destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(computed_key_var, std::move(key_expr)));
                                extracted_props.push_back({true, computed_key_var});

                                Consume(TokenKind::Colon, "Expected ':' after computed property in destructuring pattern");
                            } else {
                                if (Check(TokenKind::Identifier)) {
                                    Token p_tok = Advance();
                                    prop_name = p_tok.text;
                                    local_name = prop_name;
                                    prop_has_escape = p_tok.has_escape;
                                } else if (Check(TokenKind::String) || Check(TokenKind::Number)) {
                                    prop_name = Advance().text;
                                    local_name = prop_name;
                                } else {
                                    throw std::runtime_error("SyntaxError: Expected property name in object pattern");
                                }
                                extracted_props.push_back({false, prop_name});
                            }

                            std::unique_ptr<Expr> prop_acc;
                            if (is_computed) {
                                prop_acc = std::make_unique<IndexAccessExpr>(
                                    std::make_unique<IdentifierExpr>(source_var),
                                    std::make_unique<IdentifierExpr>(computed_key_var));
                            } else {
                                prop_acc = std::make_unique<PropertyAccessExpr>(
                                    std::make_unique<IdentifierExpr>(source_var),
                                    prop_name);
                            }

                            std::string val_var = "$d_val_" + std::to_string(++s_arrow_id);
                            destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(val_var, std::move(prop_acc)));

                            if (is_computed || Match(TokenKind::Colon)) {
                                if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                                    Token id_tok = Advance();
                                    local_name = id_tok.text;
                                    if (id_tok.has_escape && IsReservedWord(local_name)) {
                                        throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                                    }
                                    if (IsReservedWord(local_name)) {
                                        throw std::runtime_error("SyntaxError: Unexpected reserved word: " + local_name);
                                    }
                                    if (local_name == "eval" || local_name == "arguments") {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                        }
                                    }
                                    if (IsStrictReservedWord(local_name)) {
                                        if (strict_mode_ || is_module_) {
                                            throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + local_name);
                                        }
                                    }
                                    if (local_name == "await" && (in_async_ || is_module_)) {
                                        throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                                    }
                                    if (local_name == "yield" && (in_generator_ || strict_mode_)) {
                                        throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                                    }
                                    add_param_name(local_name);

                                    std::unique_ptr<Expr> init_expr;
                                    if (Match(TokenKind::Equal)) {
                                        auto def_expr = ParseExpression();
                                        YieldAwaitChecker yac;
                                        def_expr->Accept(&yac);
                                        if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                                        if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                                        if (auto* fe = dynamic_cast<FunctionExpr*>(def_expr.get())) {
                                            if (fe->Name().empty() && !fe->HasStaticName()) fe->SetName(local_name);
                                        }
                                        auto cond = std::make_unique<BinaryExpr>(
                                            BinaryOp::StrictEqual,
                                            std::make_unique<IdentifierExpr>(val_var),
                                            std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                                        init_expr = std::make_unique<ConditionalExpr>(
                                            std::move(cond), std::move(def_expr), std::make_unique<IdentifierExpr>(val_var));
                                    } else {
                                        init_expr = std::make_unique<IdentifierExpr>(val_var);
                                    }
                                    destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(local_name, std::move(init_expr)));
                                } else if (Check(TokenKind::OpenBracket) || Check(TokenKind::OpenBrace)) {
                                    std::string sub_var = "$d_sub_" + std::to_string(++s_arrow_id);
                                    size_t insert_idx = destructuring_stmts.size();
                                    parse_pattern_binding(sub_var, false);
                                    std::unique_ptr<Expr> init_expr;
                                    if (Match(TokenKind::Equal)) {
                                        auto def_expr = ParseExpression();
                                        YieldAwaitChecker yac;
                                        def_expr->Accept(&yac);
                                        if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                                        if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                                        auto cond = std::make_unique<BinaryExpr>(
                                            BinaryOp::StrictEqual,
                                            std::make_unique<IdentifierExpr>(val_var),
                                            std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                                        init_expr = std::make_unique<ConditionalExpr>(
                                            std::move(cond), std::move(def_expr), std::make_unique<IdentifierExpr>(val_var));
                                    } else {
                                        init_expr = std::make_unique<IdentifierExpr>(val_var);
                                    }
                                    destructuring_stmts.insert(destructuring_stmts.begin() + insert_idx,
                                                               std::make_unique<VarDeclStmt>(sub_var, std::move(init_expr)));
                                } else {
                                    throw std::runtime_error("SyntaxError: Expected binding after ':' in object pattern");
                                }
                            } else {
                                if (IsAllDigits(prop_name)) {
                                    throw std::runtime_error("SyntaxError: Numeric property in object pattern requires a colon");
                                }
                                if (prop_has_escape && IsReservedWord(prop_name)) {
                                    throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                                }
                                if (IsReservedWord(local_name)) {
                                    throw std::runtime_error("SyntaxError: Unexpected reserved word: " + local_name);
                                }
                                if (local_name == "eval" || local_name == "arguments") {
                                    if (strict_mode_ || is_module_) {
                                        throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                    }
                                }
                                if (IsStrictReservedWord(local_name)) {
                                    if (strict_mode_ || is_module_) {
                                        throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + local_name);
                                    }
                                }
                                if (local_name == "await" && (in_async_ || is_module_)) {
                                    throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                                }
                                if (local_name == "yield" && (in_generator_ || strict_mode_)) {
                                    throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                                }
                                add_param_name(local_name);

                                std::unique_ptr<Expr> init_expr;
                                if (Match(TokenKind::Equal)) {
                                    auto def_expr = ParseExpression();
                                    YieldAwaitChecker yac;
                                    def_expr->Accept(&yac);
                                    if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                                    if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                                    if (auto* fe = dynamic_cast<FunctionExpr*>(def_expr.get())) {
                                        if (fe->Name().empty() && !fe->HasStaticName()) fe->SetName(local_name);
                                    }
                                    auto cond = std::make_unique<BinaryExpr>(
                                        BinaryOp::StrictEqual,
                                        std::make_unique<IdentifierExpr>(val_var),
                                        std::make_unique<LiteralExpr>(TaggedValue::Undefined()));
                                    init_expr = std::make_unique<ConditionalExpr>(
                                        std::move(cond), std::move(def_expr), std::make_unique<IdentifierExpr>(val_var));
                                } else {
                                    init_expr = std::make_unique<IdentifierExpr>(val_var);
                                }
                                destructuring_stmts.push_back(std::make_unique<VarDeclStmt>(local_name, std::move(init_expr)));
                            }

                            if (Match(TokenKind::Comma)) {
                                continue;
                            }
                        }
                        Consume(TokenKind::CloseBrace, "Expected '}' in object destructuring pattern");
                    }
                };

                // Now loop over arrow parameters:
                while (!Check(TokenKind::CloseParen) && !Check(TokenKind::EndOfFile)) {
                    if (Match(TokenKind::DotDotDot)) {
                        if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                            Token r = Advance();
                            if (r.has_escape && IsReservedWord(r.text)) {
                                throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                            }
                            if (IsReservedWord(r.text)) {
                                throw std::runtime_error("SyntaxError: Unexpected reserved word: " + r.text);
                            }
                            if (r.text == "eval" || r.text == "arguments") {
                                if (strict_mode_ || is_module_) {
                                    throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                                }
                            }
                            if (IsStrictReservedWord(r.text)) {
                                if (strict_mode_ || is_module_) {
                                    throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + r.text);
                                }
                            }
                            if (r.text == "await" && (in_async_ || is_module_)) {
                                throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                            }
                            if (r.text == "yield" && (in_generator_ || strict_mode_)) {
                                throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                            }
                            add_param_name(r.text);
                            if (Check(TokenKind::Equal)) throw std::runtime_error("SyntaxError: Rest parameter may not have a default initializer");
                            if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
                            arrow_rest = r.text;
                        } else if (Check(TokenKind::OpenBracket) || Check(TokenKind::OpenBrace)) {
                            std::string synth_rest = "$rest_" + std::to_string(++s_arrow_id);
                            arrow_rest = synth_rest;
                            parse_pattern_binding(synth_rest, true);
                            if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
                        }
                        break;
                    }

                    if (Check(TokenKind::OpenBracket) || Check(TokenKind::OpenBrace)) {
                        std::string synth_p = "$p_" + std::to_string(++s_arrow_id);
                        arrow_params.push_back(synth_p);
                        parse_pattern_binding(synth_p, false);
                        std::unique_ptr<Expr> def = nullptr;
                        if (Match(TokenKind::Equal)) {
                            def = ParseExpression();
                            YieldAwaitChecker yac;
                            def->Accept(&yac);
                            if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                            if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                            has_defaults = true;
                        }
                        arrow_defaults.push_back(std::move(def));
                    } else if (Check(TokenKind::Identifier) || Check(TokenKind::Yield) || Check(TokenKind::Await)) {
                        Token p = Advance();
                        if (p.has_escape && IsReservedWord(p.text)) {
                            throw std::runtime_error("SyntaxError: Escaped characters not allowed in reserved words");
                        }
                        if (IsReservedWord(p.text)) {
                            throw std::runtime_error("SyntaxError: Unexpected reserved word: " + p.text);
                        }
                        if (p.text == "eval" || p.text == "arguments") {
                            if (strict_mode_ || is_module_) {
                                throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                            }
                        }
                        if (IsStrictReservedWord(p.text)) {
                            if (strict_mode_ || is_module_) {
                                throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + p.text);
                            }
                        }
                        if (p.text == "await" && (in_async_ || is_module_)) {
                            throw std::runtime_error("SyntaxError: 'await' cannot be used as parameter");
                        }
                        if (p.text == "yield" && (in_generator_ || strict_mode_)) {
                            throw std::runtime_error("SyntaxError: 'yield' cannot be used as parameter");
                        }
                        add_param_name(p.text);
                        std::unique_ptr<Expr> def = nullptr;
                        if (Match(TokenKind::Equal)) {
                            def = ParseExpression();
                            YieldAwaitChecker yac;
                            def->Accept(&yac);
                            if (yac.found_yield) throw std::runtime_error("SyntaxError: Yield expression not allowed in parameter defaults");
                            if (yac.found_await) throw std::runtime_error("SyntaxError: Await expression not allowed in parameter defaults");
                            has_defaults = true;
                        }
                        arrow_params.push_back(p.text);
                        arrow_defaults.push_back(std::move(def));
                    } else {
                        break;
                    }

                    if (Match(TokenKind::Comma)) {
                        if (Check(TokenKind::CloseParen)) break;
                        continue;
                    } else {
                        break;
                    }
                }

                Consume(TokenKind::CloseParen, "Expected ')' after arrow parameters");
                Consume(TokenKind::Arrow, "Expected '=>' after arrow parameters");

                bool prev_gen = in_generator_;
                bool prev_async = in_async_;
                in_generator_ = false;
                in_async_ = false;

                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    bool saved_allow_in = allow_in_;
                    allow_in_ = true;
                    auto arrow_expr = ParseExpression();
                    allow_in_ = saved_allow_in;
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }

                in_generator_ = prev_gen;
                in_async_ = prev_async;

                bool body_is_strict = strict_mode_ || is_module_;
                if (!body_is_strict && body && !body->Statements().empty()) {
                    if (auto* es = dynamic_cast<ExprStmt*>(body->Statements().front().get())) {
                        if (auto* lit = dynamic_cast<LiteralExpr*>(es->GetExpr())) {
                            if (lit->Value().IsHeapObject() && lit->Value().AsHeapObject()->IsString()) {
                                if (static_cast<JSString*>(lit->Value().AsHeapObject())->Value() == "use strict") {
                                    body_is_strict = true;
                                }
                            }
                        }
                    }
                }
                if (body_is_strict) {
                    if (has_destructuring || has_defaults || !arrow_rest.empty()) {
                        throw std::runtime_error("SyntaxError: Illegal 'use strict' directive in function with non-simple parameter list");
                    }
                    for (const auto& p : param_names) {
                        if (p == "eval" || p == "arguments") {
                            throw std::runtime_error("SyntaxError: Unexpected eval or arguments in strict mode");
                        }
                        if (IsStrictReservedWord(p)) {
                            throw std::runtime_error("SyntaxError: Unexpected strict reserved word: " + p);
                        }
                    }
                }

                if (!destructuring_stmts.empty()) {
                    body->Statements().insert(
                        body->Statements().begin(),
                        std::make_move_iterator(destructuring_stmts.begin()),
                        std::make_move_iterator(destructuring_stmts.end())
                    );
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(arrow_params), std::move(body));
                fn_expr->SetArrow(true);
                if (!arrow_defaults.empty()) {
                    fn_expr->SetParamDefaults(std::move(arrow_defaults));
                }
                if (!arrow_rest.empty()) {
                    fn_expr->SetRest(arrow_rest);
                }
                return fn_expr;
            }

            // Fallback: restore lexer and current token, parse parenthesized expression
            lexer_ = saved_lexer;
            current_token_ = saved_tok;
            bool saved_allow_in = allow_in_;
            allow_in_ = true;
            auto expr = ParseExpression(Precedence::Comma);
            allow_in_ = saved_allow_in;
            Consume(TokenKind::CloseParen, "Expected ')' after expression");
            if (expr) {
                expr->SetGrouped(true);
            }
            return expr;
        }
        case TokenKind::OpenBrace: {
            // Object literal: { key: val, [expr]: val, name() {}, [expr]() {}, ...spread }
            std::vector<ObjectLiteralExpr::Property> props;
            auto parse_method_params = [this](std::vector<std::string>& params, std::string& rest_param) {
                if (!Check(TokenKind::CloseParen)) {
                    if (Match(TokenKind::DotDotDot)) {
                        Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                        if (Check(TokenKind::Equal)) throw std::runtime_error("SyntaxError: Rest parameter may not have a default initializer");
                        if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
                        rest_param = r.text;
                    } else {
                        Token p = Consume(TokenKind::Identifier, "Expected parameter name");
                        if (Match(TokenKind::Equal)) {
                            ParseExpression();
                        }
                        params.push_back(p.text);
                        while (Match(TokenKind::Comma)) {
                            if (Check(TokenKind::CloseParen)) break;
                            if (Match(TokenKind::DotDotDot)) {
                                Token r = Consume(TokenKind::Identifier, "Expected rest parameter name");
                                if (Check(TokenKind::Equal)) throw std::runtime_error("SyntaxError: Rest parameter may not have a default initializer");
                                if (Check(TokenKind::Comma)) throw std::runtime_error("SyntaxError: Rest parameter may not have a trailing comma");
                                rest_param = r.text;
                                break;
                            }
                            Token np = Consume(TokenKind::Identifier, "Expected parameter name after ','");
                            if (Match(TokenKind::Equal)) {
                                ParseExpression();
                            }
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
                    bool is_accessor = (key_tok.text == "get" || key_tok.text == "set") &&
                                       !Check(TokenKind::Colon) && !Check(TokenKind::Comma) &&
                                       !Check(TokenKind::CloseBrace) && !Check(TokenKind::OpenParen);
                    if (is_accessor) {
                        bool is_get = (key_tok.text == "get");
                        std::unique_ptr<Expr> computed_key = nullptr;
                        std::string name = "";
                        if (Match(TokenKind::OpenBracket)) {
                            computed_key = ParseExpression();
                            Consume(TokenKind::CloseBracket, "Expected ']' after computed accessor key");
                        } else {
                            Token name_tok = Advance();
                            name = name_tok.text;
                        }
                        Consume(TokenKind::OpenParen, "Expected '(' after accessor name");
                        std::vector<std::string> params;
                        std::string rest_param;
                        parse_method_params(params, rest_param);
                        auto body = ParseBlock();
                        auto fn_expr = std::make_unique<FunctionExpr>(name, std::move(params), std::move(body));
                        if (!rest_param.empty()) fn_expr->SetRest(rest_param);
                        props.push_back(ObjectLiteralExpr::Property{
                            .key = std::move(name),
                            .computed_key = std::move(computed_key),
                            .value = std::move(fn_expr),
                            .is_getter = is_get,
                            .is_setter = !is_get
                        });
                        continue;
                    }

                    bool is_valid_key = (key_tok.kind == TokenKind::String || key_tok.kind == TokenKind::Number ||
                                         (!key_tok.text.empty() && (std::isalpha(static_cast<unsigned char>(key_tok.text[0])) ||
                                                                    key_tok.text[0] == '_' || key_tok.text[0] == '$')));
                    if (!is_valid_key) {
                        throw std::runtime_error("SyntaxError: Expected property key in object literal");
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
                    throw std::runtime_error("SyntaxError: Expected ':' after property key");
                } while (Match(TokenKind::Comma) && !Check(TokenKind::CloseBrace));
            }
            Consume(TokenKind::CloseBrace, "Expected '}' after object literal");
            return std::make_unique<ObjectLiteralExpr>(std::move(props));
        }
        case TokenKind::OpenBracket: {
            // Array literal: [ elem1, ...spread, elem2, , elem3 ]
            std::vector<std::unique_ptr<Expr>> elements;
            while (!Check(TokenKind::CloseBracket) && !Check(TokenKind::EndOfFile)) {
                if (Match(TokenKind::Comma)) {
                    elements.push_back(nullptr);
                } else {
                    elements.push_back(ParseExpression(Precedence::Assignment));
                    if (!Check(TokenKind::CloseBracket)) {
                        Consume(TokenKind::Comma, "Expected ',' or ']' after array element");
                    }
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
        case TokenKind::Delete: {
            auto operand = ParseExpression(Precedence::Unary);
            if (strict_mode_) {
                if (dynamic_cast<IdentifierExpr*>(operand.get())) {
                    throw std::runtime_error("SyntaxError: Delete of an unqualified identifier in strict mode");
                }
            }
            return std::make_unique<UnaryExpr>(UnaryOp::Delete, std::move(operand));
        }
        case TokenKind::Void:
            return std::make_unique<UnaryExpr>(UnaryOp::Void, ParseExpression(Precedence::Unary));
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus: {
            UpdateOp uop = (tok.kind == TokenKind::PlusPlus) ? UpdateOp::Increment : UpdateOp::Decrement;
            auto operand = ParseExpression(Precedence::Call);
            if (ContainsOptionalChain(operand.get())) {
                throw std::runtime_error("SyntaxError: Invalid left-hand side in prefix " + tok.text);
            }
            if (strict_mode_) {
                if (auto* id = dynamic_cast<IdentifierExpr*>(operand.get())) {
                    if (id->Name() == "eval" || id->Name() == "arguments") {
                        throw std::runtime_error("SyntaxError: Invalid assignment target in strict mode");
                    }
                }
            }
            if (auto* id = dynamic_cast<IdentifierExpr*>(operand.get())) {
                if (id->Name() == "this" || id->Name() == "new.target") {
                    throw std::runtime_error("SyntaxError: Invalid left-hand side in prefix " + tok.text);
                }
                return std::make_unique<UpdateExpr>(uop, id->Name(), /*is_prefix=*/true);
            }
            if (auto* prop = dynamic_cast<PropertyAccessExpr*>(operand.get())) {
                std::string prop_name = prop->Property();
                auto obj = prop->ReleaseObject();
                return std::make_unique<UpdatePropertyExpr>(uop, std::move(obj), prop_name, /*is_prefix=*/true);
            }
            if (auto* idx = dynamic_cast<IndexAccessExpr*>(operand.get())) {
                auto obj = idx->ReleaseObject();
                auto index = idx->ReleaseIndex();
                return std::make_unique<UpdateIndexExpr>(uop, std::move(obj), std::move(index), /*is_prefix=*/true);
            }
            throw std::runtime_error("SyntaxError: Invalid left-hand side in prefix " + tok.text);
        }
        case TokenKind::This:
            return std::make_unique<IdentifierExpr>("this");
        case TokenKind::Super:
            return std::make_unique<IdentifierExpr>("super");
        case TokenKind::Hash: {
            if (declared_private_names_stack_.empty()) {
                throw std::runtime_error("SyntaxError: Unexpected private identifier outside class");
            }
            Token name_tok = Consume(TokenKind::Identifier, "Expected identifier after '#'");
            std::string field_name = name_tok.text;

            bool declared = false;
            for (auto it = declared_private_names_stack_.rbegin(); it != declared_private_names_stack_.rend(); ++it) {
                if (it->count(field_name)) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                throw std::runtime_error("SyntaxError: Private field '#" + field_name + "' must be declared in an enclosing class");
            }
            if (!allow_in_) {
                throw std::runtime_error("SyntaxError: Private field in expression is not allowed here");
            }
            if (!Check(TokenKind::In)) {
                throw std::runtime_error("SyntaxError: Private identifier must be followed by 'in'");
            }
            return std::make_unique<PrivateIdentifierExpr>(field_name);
        }
        case TokenKind::Class: {
            std::string name = "";
            if (Check(TokenKind::Identifier)) {
                name = Advance().text;
            }
            std::string super_name = "";
            if (Match(TokenKind::Extends)) {
                Token super_tok = Consume(TokenKind::Identifier, "Expected super class name after 'extends'");
                super_name = super_tok.text;
            }
            auto body = ParseClassElements(super_name);
            auto class_expr = std::make_unique<ClassExpr>(name, super_name, std::move(body.methods));
            for (auto& f : body.fields) {
                class_expr->AddField(f.name, std::move(f.initializer));
            }
            return class_expr;
        }
        case TokenKind::New: {
            if (Match(TokenKind::Dot)) {
                Token target_tok = Consume(TokenKind::Identifier, "Expected 'target' after 'new.'");
                if (target_tok.has_escape || target_tok.text != "target") {
                    throw std::runtime_error("SyntaxError: The 'target' contextual keyword must not contain Unicode escape sequences");
                }
                return std::make_unique<IdentifierExpr>("new.target");
            }
            std::unique_ptr<Expr> callee = ParsePrefix();
            while (true) {
                if (Match(TokenKind::Dot)) {
                    Token prop = Consume(TokenKind::Identifier, "Expected property identifier after '.'");
                    callee = std::make_unique<PropertyAccessExpr>(std::move(callee), prop.text);
                } else if (Match(TokenKind::OpenBracket)) {
                    auto index = ParseExpression(Precedence::Comma);
                    Consume(TokenKind::CloseBracket, "Expected ']' after index");
                    callee = std::make_unique<IndexAccessExpr>(std::move(callee), std::move(index));
                } else if (Check(TokenKind::Backtick)) {
                    Token backtick_tok = Advance();
                    callee = MakeTaggedTemplateCall(std::move(callee), backtick_tok);
                } else {
                    break;
                }
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
            if (in_generator_) {
                bool is_delegate = Match(TokenKind::Star);
                std::unique_ptr<Expr> expr = nullptr;
                if (!Check(TokenKind::Semicolon) && !Check(TokenKind::CloseBrace) && !Check(TokenKind::CloseParen) && !Check(TokenKind::Comma) && !Check(TokenKind::EndOfFile)) {
                    expr = ParseExpression();
                }
                return std::make_unique<YieldExpr>(std::move(expr), is_delegate);
            }
            if (strict_mode_) {
                throw std::runtime_error("SyntaxError: Unexpected reserved word 'yield'");
            }
            if (Check(TokenKind::Arrow)) {
                if (current_token_.has_leading_newline) {
                    throw std::runtime_error("SyntaxError: Line terminator not allowed before '=>'");
                }
                Advance(); // Consume '=>'
                std::vector<std::string> params = {"yield"};
                bool prev_gen = in_generator_;
                bool prev_async = in_async_;
                in_generator_ = false;
                in_async_ = false;

                std::unique_ptr<BlockStmt> body;
                if (Check(TokenKind::OpenBrace)) {
                    body = ParseBlock();
                } else {
                    bool saved_allow_in = allow_in_;
                    allow_in_ = true;
                    auto arrow_expr = ParseExpression();
                    allow_in_ = saved_allow_in;
                    body = std::make_unique<BlockStmt>();
                    body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                }

                in_generator_ = prev_gen;
                in_async_ = prev_async;

                bool body_is_strict = strict_mode_ || is_module_;
                if (!body_is_strict && body && !body->Statements().empty()) {
                    if (auto* es = dynamic_cast<ExprStmt*>(body->Statements().front().get())) {
                        if (auto* lit = dynamic_cast<LiteralExpr*>(es->GetExpr())) {
                            if (lit->Value().IsHeapObject() && lit->Value().AsHeapObject()->IsString()) {
                                if (static_cast<JSString*>(lit->Value().AsHeapObject())->Value() == "use strict") {
                                    body_is_strict = true;
                                }
                            }
                        }
                    }
                }
                if (body_is_strict) {
                    throw std::runtime_error("SyntaxError: Unexpected reserved word 'yield'");
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetArrow(true);
                return fn_expr;
            }
            return std::make_unique<IdentifierExpr>("yield");
        }
        case TokenKind::Await: {
            if (in_async_ || (is_module_ && function_depth_ == 0)) {
                auto expr = ParseExpression(Precedence::Unary);
                return std::make_unique<AwaitExpr>(std::move(expr));
            }
            if (is_module_) {
                throw std::runtime_error("SyntaxError: 'await' is a reserved word in module code");
            }
            return std::make_unique<IdentifierExpr>("await");
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

                std::unique_ptr<BlockStmt> body;
                {
                    FunctionContextGuard guard(*this, is_generator, true);
                    body = ParseBlock();
                }

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
                {
                    FunctionContextGuard guard(*this, false, true);
                    if (Check(TokenKind::OpenBrace)) {
                        body = ParseBlock();
                    } else {
                        auto arrow_expr = ParseExpression();
                        body = std::make_unique<BlockStmt>();
                        body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                    }
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetArrow(true);
                fn_expr->SetAsync(true);
                return fn_expr;
            }
            if (Check(TokenKind::Identifier)) {
                Token param_tok = Advance();
                Consume(TokenKind::Arrow, "Expected '=>' after async parameter");
                std::vector<std::string> params = {param_tok.text};

                std::unique_ptr<BlockStmt> body;
                {
                    FunctionContextGuard guard(*this, false, true);
                    if (Check(TokenKind::OpenBrace)) {
                        body = ParseBlock();
                    } else {
                        auto arrow_expr = ParseExpression();
                        body = std::make_unique<BlockStmt>();
                        body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                    }
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetArrow(true);
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
                {
                    FunctionContextGuard guard(*this, false, true);
                    if (Check(TokenKind::OpenBrace)) {
                        body = ParseBlock();
                    } else {
                        auto arrow_expr = ParseExpression();
                        body = std::make_unique<BlockStmt>();
                        body->AddStatement(std::make_unique<ReturnStmt>(std::move(arrow_expr)));
                    }
                }

                auto fn_expr = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
                fn_expr->SetArrow(true);
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
            } else if (Check(TokenKind::Await) && !in_async_ && !is_module_) {
                name = Advance().text;
            } else if (Check(TokenKind::Yield) && !is_generator && !in_generator_ && !strict_mode_) {
                name = Advance().text;
            }
            Consume(TokenKind::OpenParen, "Expected '(' after 'function'");

            auto parse_fn_expr_param = [&]() -> Token {
                if (Check(TokenKind::Identifier)) return Advance();
                if (Check(TokenKind::Await) && !in_async_ && !is_module_) return Advance();
                if (Check(TokenKind::Yield) && !is_generator && !in_generator_ && !strict_mode_) return Advance();
                return Consume(TokenKind::Identifier, "Expected parameter name");
            };

            std::vector<std::string> params;
            std::vector<std::unique_ptr<Expr>> param_defaults;
            std::string rest_param = "";
            if (!Check(TokenKind::CloseParen)) {
                if (Match(TokenKind::DotDotDot)) {
                    Token r = parse_fn_expr_param();
                    rest_param = r.text;
                } else {
                    Token p = parse_fn_expr_param();
                    std::unique_ptr<Expr> def = nullptr;
                    if (Match(TokenKind::Equal)) {
                        def = ParseExpression();
                    }
                    params.push_back(p.text);
                    param_defaults.push_back(std::move(def));
                    while (Match(TokenKind::Comma)) {
                        if (Match(TokenKind::DotDotDot)) {
                            Token r = parse_fn_expr_param();
                            rest_param = r.text;
                            break;
                        }
                        Token np = parse_fn_expr_param();
                        std::unique_ptr<Expr> next_def = nullptr;
                        if (Match(TokenKind::Equal)) {
                            next_def = ParseExpression();
                        }
                        params.push_back(np.text);
                        param_defaults.push_back(std::move(next_def));
                    }
                }
            }
            Consume(TokenKind::CloseParen, "Expected ')' after parameters");

            std::unique_ptr<BlockStmt> body;
            {
                FunctionContextGuard guard(*this, is_generator, false);
                body = ParseBlock();
            }
            auto fn_expr = std::make_unique<FunctionExpr>(name, std::move(params), std::move(body));
            fn_expr->SetParamDefaults(std::move(param_defaults));
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

        if (strict_mode_) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
                if (id->Name() == "eval" || id->Name() == "arguments") {
                    throw std::runtime_error("SyntaxError: Invalid assignment target in strict mode");
                }
            }
        }

        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            if (id->Name() == "this" || id->Name() == "new.target") {
                throw std::runtime_error("SyntaxError: Invalid left-hand side in assignment");
            }
        }

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
            if (auto* priv = dynamic_cast<PrivateAccessExpr*>(lhs.get())) {
                auto obj = priv->ReleaseObject();
                return std::make_unique<PropertyAssignExpr>(std::move(obj), "__private__" + priv->Name(), std::move(rhs));
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
            throw std::runtime_error("SyntaxError: Invalid left-hand side in assignment");
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
            default: throw std::runtime_error("SyntaxError: Unhandled compound assignment");
        }

        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            if (auto* fn = dynamic_cast<FunctionExpr*>(rhs.get()); fn && fn->Name().empty() && !fn->HasStaticName()) {
                // NamedEvaluation for an anonymous function assigned through
                // a logical assignment uses the LHS binding name.
                fn->SetName(id->Name());
            }
            return std::make_unique<CompoundAssignExpr>(bin_op, id->Name(), std::move(rhs));
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            return std::make_unique<CompoundPropertyAssignExpr>(bin_op, std::move(obj), prop_name, std::move(rhs));
        }
        if (auto* priv = dynamic_cast<PrivateAccessExpr*>(lhs.get())) {
            auto obj = priv->ReleaseObject();
            return std::make_unique<CompoundPrivateAssignExpr>(bin_op, std::move(obj), priv->Name(), std::move(rhs));
        }
        if (auto* idx = dynamic_cast<IndexAccessExpr*>(lhs.get())) {
            auto obj = idx->ReleaseObject();
            auto index = idx->ReleaseIndex();
            return std::make_unique<CompoundIndexAssignExpr>(bin_op, std::move(obj), std::move(index), std::move(rhs));
        }
        throw std::runtime_error("SyntaxError: Invalid left-hand side in compound assignment");
    }

    if (op_token.kind == TokenKind::QuestionDot) {
        if (Check(TokenKind::Backtick)) {
            throw std::runtime_error("SyntaxError: Tagged template cannot be used with optional chaining");
        }
        if (Match(TokenKind::OpenBracket)) {
            auto index = ParseExpression(Precedence::Comma);
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
            return std::make_unique<OptionalCallExpr>(std::move(lhs), std::move(args), true);
        }
        if (current_token_.text.empty() || (!std::isalpha(static_cast<unsigned char>(current_token_.text[0])) && current_token_.text[0] != '_' && current_token_.text[0] != '$')) {
            throw std::runtime_error("SyntaxError: Expected property identifier after '?.'");
        }
        Token prop_tok = Advance();
        return std::make_unique<OptionalPropertyAccessExpr>(std::move(lhs), prop_tok.text);
    }

    if (op_token.kind == TokenKind::Dot) {
        if (Match(TokenKind::Hash)) {
            Token private_name = Consume(TokenKind::Identifier, "Expected private name after '.'");
            return std::make_unique<PrivateAccessExpr>(std::move(lhs), private_name.text);
        }
        if (current_token_.text.empty() || (!std::isalpha(static_cast<unsigned char>(current_token_.text[0])) && current_token_.text[0] != '_' && current_token_.text[0] != '$')) {
            throw std::runtime_error("SyntaxError: Expected property identifier after '.'");
        }
        Token prop_tok = Advance();
        if (IsOptionalChain(lhs.get())) {
            return std::make_unique<OptionalPropertyAccessExpr>(std::move(lhs), prop_tok.text);
        }
        return std::make_unique<PropertyAccessExpr>(std::move(lhs), prop_tok.text);
    }

    if (op_token.kind == TokenKind::OpenBracket) {
        auto index = ParseExpression(Precedence::Comma);
        Consume(TokenKind::CloseBracket, "Expected ']' after index");
        if (IsOptionalChain(lhs.get())) {
            return std::make_unique<OptionalIndexAccessExpr>(std::move(lhs), std::move(index));
        }
        return std::make_unique<IndexAccessExpr>(std::move(lhs), std::move(index));
    }

    if (op_token.kind == TokenKind::OpenParen) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            if (id->Name() == "super" && !in_derived_constructor_) {
                throw std::runtime_error("SyntaxError: 'super()' is only valid in derived class constructors");
            }
        }
        std::vector<std::unique_ptr<Expr>> args;
        if (!Check(TokenKind::CloseParen)) {
            args.push_back(ParseExpression());
            while (Match(TokenKind::Comma)) {
                args.push_back(ParseExpression());
            }
        }
        Consume(TokenKind::CloseParen, "Expected ')' after argument list");
        if (IsOptionalChain(lhs.get())) {
            return std::make_unique<OptionalCallExpr>(std::move(lhs), std::move(args), false);
        }
        return std::make_unique<CallExpr>(std::move(lhs), std::move(args));
    }

    if (op_token.kind == TokenKind::Backtick) {
        return MakeTaggedTemplateCall(std::move(lhs), op_token);
    }

    if (op_token.kind == TokenKind::PlusPlus) {
        if (ContainsOptionalChain(lhs.get())) {
            throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix ++");
        }
        if (strict_mode_) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
                if (id->Name() == "eval" || id->Name() == "arguments") {
                    throw std::runtime_error("SyntaxError: Invalid assignment target in strict mode");
                }
            }
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            if (id->Name() == "this" || id->Name() == "new.target") {
                throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix ++");
            }
            return std::make_unique<UpdateExpr>(UpdateOp::Increment, id->Name(), /*is_prefix=*/false);
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            return std::make_unique<UpdatePropertyExpr>(UpdateOp::Increment, std::move(obj), prop_name, /*is_prefix=*/false);
        }
        if (auto* idx = dynamic_cast<IndexAccessExpr*>(lhs.get())) {
            auto obj = idx->ReleaseObject();
            auto index = idx->ReleaseIndex();
            return std::make_unique<UpdateIndexExpr>(UpdateOp::Increment, std::move(obj), std::move(index), /*is_prefix=*/false);
        }
        throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix ++");
    }

    if (op_token.kind == TokenKind::MinusMinus) {
        if (ContainsOptionalChain(lhs.get())) {
            throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix --");
        }
        if (strict_mode_) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
                if (id->Name() == "eval" || id->Name() == "arguments") {
                    throw std::runtime_error("SyntaxError: Invalid assignment target in strict mode");
                }
            }
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(lhs.get())) {
            if (id->Name() == "this" || id->Name() == "new.target") {
                throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix --");
            }
            return std::make_unique<UpdateExpr>(UpdateOp::Decrement, id->Name(), /*is_prefix=*/false);
        }
        if (auto* prop = dynamic_cast<PropertyAccessExpr*>(lhs.get())) {
            std::string prop_name = prop->Property();
            auto obj = prop->ReleaseObject();
            return std::make_unique<UpdatePropertyExpr>(UpdateOp::Decrement, std::move(obj), prop_name, /*is_prefix=*/false);
        }
        if (auto* idx = dynamic_cast<IndexAccessExpr*>(lhs.get())) {
            auto obj = idx->ReleaseObject();
            auto index = idx->ReleaseIndex();
            return std::make_unique<UpdateIndexExpr>(UpdateOp::Decrement, std::move(obj), std::move(index), /*is_prefix=*/false);
        }
        throw std::runtime_error("SyntaxError: Invalid left-hand side in postfix --");
    }

    if (op_token.kind == TokenKind::In && !allow_in_) {
        throw std::runtime_error("SyntaxError: Unexpected token 'in' in contexts where it is disallowed at line " + std::to_string(op_token.location.line));
    }

    if (op_token.kind == TokenKind::Question) {
        bool saved_allow_in = allow_in_;
        allow_in_ = true;
        auto then_expr = ParseExpression();
        allow_in_ = saved_allow_in;
        Consume(TokenKind::Colon, "Expected ':' in conditional expression");
        auto else_expr = ParseExpression(Precedence::Conditional);
        return std::make_unique<ConditionalExpr>(std::move(lhs), std::move(then_expr), std::move(else_expr));
    }

    if (op_token.kind == TokenKind::Comma) {
        auto rhs = ParseExpression(Precedence::Comma);
        return std::make_unique<BinaryExpr>(BinaryOp::Comma, std::move(lhs), std::move(rhs));
    }

    if (op_token.kind == TokenKind::StarStar) {
        if ((dynamic_cast<UnaryExpr*>(lhs.get()) && !lhs->IsGrouped()) ||
            (dynamic_cast<AwaitExpr*>(lhs.get()) && !lhs->IsGrouped())) {
            throw std::runtime_error("SyntaxError: Unary operator used immediately before exponentiation expression without parentheses at line " + std::to_string(op_token.location.line));
        }
        // Exponentiation is right-associative: 2 ** 3 ** 2 is 2 ** (3 ** 2)
        auto rhs = ParseExpression(Precedence::Exponentiation);
        return std::make_unique<BinaryExpr>(BinaryOp::Exp, std::move(lhs), std::move(rhs));
    }

    if (op_token.kind == TokenKind::QuestionQuestion) {
        if (auto* b_lhs = dynamic_cast<BinaryExpr*>(lhs.get())) {
            if (!b_lhs->IsGrouped() && (b_lhs->Op() == BinaryOp::LogicalAnd || b_lhs->Op() == BinaryOp::LogicalOr)) {
                throw std::runtime_error("SyntaxError: Cannot chain nullish coalescing operator '??' directly with '&&' or '||' without parentheses at line " + std::to_string(op_token.location.line));
            }
        }
    } else if (op_token.kind == TokenKind::AmpAmp || op_token.kind == TokenKind::BarBar) {
        if (auto* b_lhs = dynamic_cast<BinaryExpr*>(lhs.get())) {
            if (!b_lhs->IsGrouped() && b_lhs->Op() == BinaryOp::NullishCoalescing) {
                throw std::runtime_error("SyntaxError: Cannot chain logical operator directly with nullish coalescing operator '??' without parentheses at line " + std::to_string(op_token.location.line));
            }
        }
    }

    Precedence prec = GetPrecedence(op_token.kind);
    auto rhs = ParseExpression(static_cast<Precedence>(static_cast<int>(prec) + 1));

    if (op_token.kind == TokenKind::QuestionQuestion) {
        if (auto* b_rhs = dynamic_cast<BinaryExpr*>(rhs.get())) {
            if (!b_rhs->IsGrouped() && (b_rhs->Op() == BinaryOp::LogicalAnd || b_rhs->Op() == BinaryOp::LogicalOr)) {
                throw std::runtime_error("SyntaxError: Cannot chain nullish coalescing operator '??' directly with '&&' or '||' without parentheses at line " + std::to_string(op_token.location.line));
            }
        }
    } else if (op_token.kind == TokenKind::AmpAmp || op_token.kind == TokenKind::BarBar) {
        if (auto* b_rhs = dynamic_cast<BinaryExpr*>(rhs.get())) {
            if (!b_rhs->IsGrouped() && b_rhs->Op() == BinaryOp::NullishCoalescing) {
                throw std::runtime_error("SyntaxError: Cannot chain logical operator directly with nullish coalescing operator '??' without parentheses at line " + std::to_string(op_token.location.line));
            }
        }
    }

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

std::unique_ptr<CallExpr> Parser::MakeTaggedTemplateCall(std::unique_ptr<Expr> tag, Token op_token) {
    if (ContainsOptionalChain(tag.get())) {
        throw std::runtime_error("SyntaxError: Tagged template cannot be used with optional chaining");
    }
    std::vector<std::unique_ptr<Expr>> args;
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    JSObject* arr_proto = Realm::Current() ? Realm::Current()->GetArrayPrototype() : nullptr;

    auto* template_arr = AllocateRuntime<JSArray>(root, op_token.template_quasis.size());
    if (arr_proto) template_arr->SetPrototype(arr_proto);

    auto* raw_arr = AllocateRuntime<JSArray>(root, op_token.template_raw_quasis.size());
    if (arr_proto) raw_arr->SetPrototype(arr_proto);

    for (size_t i = 0; i < op_token.template_quasis.size(); ++i) {
        if (i < op_token.template_has_invalid_escape.size() && op_token.template_has_invalid_escape[i]) {
            template_arr->SetElement(i, TaggedValue::Undefined());
        } else {
            template_arr->SetElement(i, TaggedValue::FromHeapObject(AllocateRuntime<JSString>(op_token.template_quasis[i])));
        }
    }

    for (size_t i = 0; i < op_token.template_raw_quasis.size(); ++i) {
        raw_arr->SetElement(i, TaggedValue::FromHeapObject(AllocateRuntime<JSString>(op_token.template_raw_quasis[i])));
    }

    uint32_t dummy_shape_id = 9999;
    template_arr->SetNamedProperty("raw", TaggedValue::FromHeapObject(raw_arr), dummy_shape_id);
    template_arr->SetOwnPropertyAttributes("raw", PropertyAttributes::None);
    raw_arr->Freeze();
    template_arr->Freeze();

    args.push_back(std::make_unique<LiteralExpr>(TaggedValue::FromHeapObject(template_arr)));
    for (const auto& expr_str : op_token.template_exprs) {
        Lexer sub_lexer(expr_str);
        Parser sub_parser(sub_lexer);
        args.push_back(sub_parser.ParseExpression());
    }
    return std::make_unique<CallExpr>(std::move(tag), std::move(args));
}

} // namespace aether
