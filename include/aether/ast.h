#ifndef AETHER_AST_H
#define AETHER_AST_H

#include "aether/common.h"
#include "aether/value.h"
#include <string>
#include <vector>
#include <memory>
#include <utility>

namespace aether {

class ASTVisitor;

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Exp,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    ShiftLeft,
    ShiftRight,
    ShiftRightUnsigned,
    Equal,
    StrictEqual,
    NotEqual,
    StrictNotEqual,
    LessThan,
    GreaterThan,
    LessThanOrEqual,
    GreaterThanOrEqual,
    LogicalAnd,
    LogicalOr,
    NullishCoalescing,
    InstanceOf,
    In,
    Comma
};

enum class UnaryOp {
    LogicalNot,
    Negate,
    BitwiseNot,
    Plus,
    TypeOf,
    Delete,
    Void
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void Accept(ASTVisitor* visitor) = 0;
};

// Expressions
class Expr : public ASTNode {};

class LiteralExpr : public Expr {
public:
    explicit LiteralExpr(TaggedValue val) : value_(val) {}
    TaggedValue Value() const noexcept { return value_; }
    void Accept(ASTVisitor* visitor) override;

private:
    TaggedValue value_;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(std::string_view name) : name_(name) {}
    const std::string& Name() const noexcept { return name_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(UnaryOp op, std::unique_ptr<Expr> expr)
        : op_(op), expr_(std::move(expr)) {}

    UnaryOp Op() const noexcept { return op_; }
    Expr* GetExpr() const noexcept { return expr_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    UnaryOp op_;
    std::unique_ptr<Expr> expr_;
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(BinaryOp op, std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

    BinaryOp Op() const noexcept { return op_; }
    Expr* Lhs() const noexcept { return lhs_.get(); }
    Expr* Rhs() const noexcept { return rhs_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    BinaryOp op_;
    std::unique_ptr<Expr> lhs_;
    std::unique_ptr<Expr> rhs_;
};

class AssignExpr : public Expr {
public:
    AssignExpr(std::string_view target, std::unique_ptr<Expr> val)
        : target_(target), value_(std::move(val)) {}

    const std::string& Target() const noexcept { return target_; }
    Expr* Value() const noexcept { return value_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string target_;
    std::unique_ptr<Expr> value_;
};

class PropertyAccessExpr : public Expr {
public:
    PropertyAccessExpr(std::unique_ptr<Expr> obj, std::string_view prop)
        : object_(std::move(obj)), property_(prop) {}

    Expr* Object() const noexcept { return object_.get(); }
    std::unique_ptr<Expr> ReleaseObject() noexcept { return std::move(object_); }
    const std::string& Property() const noexcept { return property_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::string property_;
};

class PropertyAssignExpr : public Expr {
public:
    PropertyAssignExpr(std::unique_ptr<Expr> obj, std::string_view prop, std::unique_ptr<Expr> val)
        : object_(std::move(obj)), property_(prop), value_(std::move(val)) {}

    Expr* Object() const noexcept { return object_.get(); }
    const std::string& Property() const noexcept { return property_; }
    Expr* Value() const noexcept { return value_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::string property_;
    std::unique_ptr<Expr> value_;
};

class CallExpr : public Expr {
public:
    CallExpr(std::unique_ptr<Expr> callee, std::vector<std::unique_ptr<Expr>> args)
        : callee_(std::move(callee)), args_(std::move(args)) {}

    Expr* Callee() const noexcept { return callee_.get(); }
    const std::vector<std::unique_ptr<Expr>>& Args() const noexcept { return args_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> callee_;
    std::vector<std::unique_ptr<Expr>> args_;
};

class ObjectLiteralExpr : public Expr {
public:
    struct Property {
        std::string key;
        std::unique_ptr<Expr> computed_key;
        std::unique_ptr<Expr> value;
    };

    explicit ObjectLiteralExpr(std::vector<Property> props)
        : properties_(std::move(props)) {}

    explicit ObjectLiteralExpr(std::vector<std::pair<std::string, std::unique_ptr<Expr>>> props) {
        for (auto& p : props) {
            properties_.push_back(Property{
                .key = std::move(p.first),
                .computed_key = nullptr,
                .value = std::move(p.second)
            });
        }
    }

    const std::vector<Property>& Properties() const noexcept {
        return properties_;
    }
    void Accept(ASTVisitor* visitor) override;

private:
    std::vector<Property> properties_;
};

class ArrayLiteralExpr : public Expr {
public:
    explicit ArrayLiteralExpr(std::vector<std::unique_ptr<Expr>> elements)
        : elements_(std::move(elements)) {}

    const std::vector<std::unique_ptr<Expr>>& Elements() const noexcept { return elements_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::vector<std::unique_ptr<Expr>> elements_;
};

class IndexAccessExpr : public Expr {
public:
    IndexAccessExpr(std::unique_ptr<Expr> object, std::unique_ptr<Expr> index)
        : object_(std::move(object)), index_(std::move(index)) {}

    Expr* Object() const noexcept { return object_.get(); }
    Expr* Index() const noexcept { return index_.get(); }
    std::unique_ptr<Expr> ReleaseObject() noexcept { return std::move(object_); }
    std::unique_ptr<Expr> ReleaseIndex() noexcept { return std::move(index_); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> index_;
};

class IndexAssignExpr : public Expr {
public:
    IndexAssignExpr(std::unique_ptr<Expr> object, std::unique_ptr<Expr> index, std::unique_ptr<Expr> value)
        : object_(std::move(object)), index_(std::move(index)), value_(std::move(value)) {}

    Expr* Object() const noexcept { return object_.get(); }
    Expr* Index() const noexcept { return index_.get(); }
    Expr* Value() const noexcept { return value_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> index_;
    std::unique_ptr<Expr> value_;
};

class CompoundPropertyAssignExpr : public Expr {
public:
    CompoundPropertyAssignExpr(BinaryOp op, std::unique_ptr<Expr> object, std::string_view property, std::unique_ptr<Expr> rhs)
        : op_(op), object_(std::move(object)), property_(property), rhs_(std::move(rhs)) {}

    BinaryOp Op() const noexcept { return op_; }
    Expr* Object() const noexcept { return object_.get(); }
    const std::string& Property() const noexcept { return property_; }
    Expr* Rhs() const noexcept { return rhs_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    BinaryOp op_;
    std::unique_ptr<Expr> object_;
    std::string property_;
    std::unique_ptr<Expr> rhs_;
};

class CompoundIndexAssignExpr : public Expr {
public:
    CompoundIndexAssignExpr(BinaryOp op, std::unique_ptr<Expr> object, std::unique_ptr<Expr> index, std::unique_ptr<Expr> rhs)
        : op_(op), object_(std::move(object)), index_(std::move(index)), rhs_(std::move(rhs)) {}

    BinaryOp Op() const noexcept { return op_; }
    Expr* Object() const noexcept { return object_.get(); }
    Expr* Index() const noexcept { return index_.get(); }
    Expr* Rhs() const noexcept { return rhs_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    BinaryOp op_;
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> index_;
    std::unique_ptr<Expr> rhs_;
};

enum class UpdateOp {
    Increment,
    Decrement
};

class UpdateExpr : public Expr {
public:
    UpdateExpr(UpdateOp op, std::string_view name, bool is_prefix)
        : op_(op), name_(name), is_prefix_(is_prefix) {}

    UpdateOp Op() const noexcept { return op_; }
    const std::string& Name() const noexcept { return name_; }
    bool IsPrefix() const noexcept { return is_prefix_; }
    void Accept(ASTVisitor* visitor) override;

private:
    UpdateOp op_;
    std::string name_;
    bool is_prefix_;
};

class NewExpr : public Expr {
public:
    NewExpr(std::unique_ptr<Expr> callee, std::vector<std::unique_ptr<Expr>> args)
        : callee_(std::move(callee)), args_(std::move(args)) {}

    Expr* Callee() const noexcept { return callee_.get(); }
    const std::vector<std::unique_ptr<Expr>>& Args() const noexcept { return args_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> callee_;
    std::vector<std::unique_ptr<Expr>> args_;
};

class ConditionalExpr : public Expr {
public:
    ConditionalExpr(std::unique_ptr<Expr> cond, std::unique_ptr<Expr> then_expr, std::unique_ptr<Expr> else_expr)
        : condition_(std::move(cond)), then_branch_(std::move(then_expr)), else_branch_(std::move(else_expr)) {}

    Expr* Condition() const noexcept { return condition_.get(); }
    Expr* ThenBranch() const noexcept { return then_branch_.get(); }
    Expr* ElseBranch() const noexcept { return else_branch_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<Expr> then_branch_;
    std::unique_ptr<Expr> else_branch_;
};

class TemplateLiteralExpr : public Expr {
public:
    TemplateLiteralExpr(std::vector<std::string> quasis, std::vector<std::unique_ptr<Expr>> exprs)
        : quasis_(std::move(quasis)), expressions_(std::move(exprs)) {}

    const std::vector<std::string>& Quasis() const noexcept { return quasis_; }
    const std::vector<std::unique_ptr<Expr>>& Expressions() const noexcept { return expressions_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::vector<std::string> quasis_;
    std::vector<std::unique_ptr<Expr>> expressions_;
};

class OptionalPropertyAccessExpr : public Expr {
public:
    OptionalPropertyAccessExpr(std::unique_ptr<Expr> obj, std::string_view prop)
        : object_(std::move(obj)), property_(prop) {}

    Expr* Object() const noexcept { return object_.get(); }
    const std::string& Property() const noexcept { return property_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::string property_;
};

class OptionalIndexAccessExpr : public Expr {
public:
    OptionalIndexAccessExpr(std::unique_ptr<Expr> obj, std::unique_ptr<Expr> index)
        : object_(std::move(obj)), index_(std::move(index)) {}

    Expr* Object() const noexcept { return object_.get(); }
    Expr* Index() const noexcept { return index_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> index_;
};

class OptionalCallExpr : public Expr {
public:
    OptionalCallExpr(std::unique_ptr<Expr> callee, std::vector<std::unique_ptr<Expr>> args)
        : callee_(std::move(callee)), args_(std::move(args)) {}

    Expr* Callee() const noexcept { return callee_.get(); }
    const std::vector<std::unique_ptr<Expr>>& Args() const noexcept { return args_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> callee_;
    std::vector<std::unique_ptr<Expr>> args_;
};

class SpreadExpr : public Expr {
public:
    explicit SpreadExpr(std::unique_ptr<Expr> expr) : expr_(std::move(expr)) {}

    Expr* GetExpr() const noexcept { return expr_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

class YieldExpr : public Expr {
public:
    explicit YieldExpr(std::unique_ptr<Expr> expr = nullptr, bool is_delegate = false)
        : expr_(std::move(expr)), is_delegate_(is_delegate) {}

    Expr* GetExpr() const noexcept { return expr_.get(); }
    bool IsDelegate() const noexcept { return is_delegate_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> expr_;
    bool is_delegate_{false};
};

class AwaitExpr : public Expr {
public:
    explicit AwaitExpr(std::unique_ptr<Expr> expr = nullptr)
        : expr_(std::move(expr)) {}

    Expr* GetExpr() const noexcept { return expr_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

class ImportMetaExpr : public Expr {
public:
    ImportMetaExpr() = default;
    void Accept(ASTVisitor* visitor) override;
};

class DynamicImportExpr : public Expr {
public:
    explicit DynamicImportExpr(std::unique_ptr<Expr> specifier)
        : specifier_(std::move(specifier)) {}

    Expr* Specifier() const noexcept { return specifier_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> specifier_;
};

// Statements
class Stmt : public ASTNode {};

class ExprStmt : public Stmt {
public:
    explicit ExprStmt(std::unique_ptr<Expr> expr) : expr_(std::move(expr)) {}
    Expr* GetExpr() const noexcept { return expr_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

class VarDeclStmt : public Stmt {
public:
    VarDeclStmt(std::string_view name, std::unique_ptr<Expr> init)
        : name_(name), initializer_(std::move(init)) {}

    const std::string& Name() const noexcept { return name_; }
    Expr* Initializer() const noexcept { return initializer_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::unique_ptr<Expr> initializer_;
};

class BlockStmt : public Stmt {
public:
    BlockStmt() = default;
    void AddStatement(std::unique_ptr<Stmt> stmt) {
        statements_.push_back(std::move(stmt));
    }
    const std::vector<std::unique_ptr<Stmt>>& Statements() const noexcept { return statements_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::vector<std::unique_ptr<Stmt>> statements_;
};

class IfStmt : public Stmt {
public:
    IfStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> then_branch, std::unique_ptr<Stmt> else_branch = nullptr)
        : condition_(std::move(cond)), then_branch_(std::move(then_branch)), else_branch_(std::move(else_branch)) {}

    Expr* Condition() const noexcept { return condition_.get(); }
    Stmt* ThenBranch() const noexcept { return then_branch_.get(); }
    Stmt* ElseBranch() const noexcept { return else_branch_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<Stmt> then_branch_;
    std::unique_ptr<Stmt> else_branch_;
};

class WhileStmt : public Stmt {
public:
    WhileStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> body)
        : condition_(std::move(cond)), body_(std::move(body)) {}

    Expr* Condition() const noexcept { return condition_.get(); }
    Stmt* Body() const noexcept { return body_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<Stmt> body_;
};

class ForStmt : public Stmt {
public:
    ForStmt(std::unique_ptr<Stmt> init, std::unique_ptr<Expr> cond, std::unique_ptr<Expr> update, std::unique_ptr<Stmt> body)
        : init_(std::move(init)), cond_(std::move(cond)), update_(std::move(update)), body_(std::move(body)) {}

    Stmt* Init() const noexcept { return init_.get(); }
    Expr* Cond() const noexcept { return cond_.get(); }
    Expr* Update() const noexcept { return update_.get(); }
    Stmt* Body() const noexcept { return body_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Stmt> init_;
    std::unique_ptr<Expr> cond_;
    std::unique_ptr<Expr> update_;
    std::unique_ptr<Stmt> body_;
};

class DoWhileStmt : public Stmt {
public:
    DoWhileStmt(std::unique_ptr<Stmt> body, std::unique_ptr<Expr> cond)
        : body_(std::move(body)), condition_(std::move(cond)) {}

    Stmt* Body() const noexcept { return body_.get(); }
    Expr* Condition() const noexcept { return condition_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Stmt> body_;
    std::unique_ptr<Expr> condition_;
};

class BreakStmt : public Stmt {
public:
    explicit BreakStmt(std::string_view label = "") : label_(label) {}
    const std::string& Label() const noexcept { return label_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string label_;
};

class ContinueStmt : public Stmt {
public:
    explicit ContinueStmt(std::string_view label = "") : label_(label) {}
    const std::string& Label() const noexcept { return label_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string label_;
};

struct SwitchCase {
    std::unique_ptr<Expr> test; // nullptr indicates 'default' case
    std::vector<std::unique_ptr<Stmt>> statements;
};

class SwitchStmt : public Stmt {
public:
    SwitchStmt(std::unique_ptr<Expr> discriminant, std::vector<SwitchCase> cases)
        : discriminant_(std::move(discriminant)), cases_(std::move(cases)) {}

    Expr* Discriminant() const noexcept { return discriminant_.get(); }
    const std::vector<SwitchCase>& Cases() const noexcept { return cases_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> discriminant_;
    std::vector<SwitchCase> cases_;
};

class DebuggerStmt : public Stmt {
public:
    DebuggerStmt() = default;
    void Accept(ASTVisitor* visitor) override;
};

class FunctionDeclStmt : public Stmt {
public:
    FunctionDeclStmt(std::string_view name, std::vector<std::string> params, std::unique_ptr<BlockStmt> body)
        : name_(name), params_(std::move(params)), body_(std::move(body)) {}

    const std::string& Name() const noexcept { return name_; }
    const std::vector<std::string>& Params() const noexcept { return params_; }
    BlockStmt* Body() const noexcept { return body_.get(); }

    bool HasRest() const noexcept { return has_rest_; }
    const std::string& RestParam() const noexcept { return rest_param_; }
    void SetRest(std::string_view r) { has_rest_ = true; rest_param_ = r; }

    bool IsGenerator() const noexcept { return is_generator_; }
    void SetGenerator(bool g) noexcept { is_generator_ = g; }

    bool IsAsync() const noexcept { return is_async_; }
    void SetAsync(bool a) noexcept { is_async_ = a; }

    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::vector<std::string> params_;
    std::unique_ptr<BlockStmt> body_;
    bool has_rest_{false};
    std::string rest_param_;
    bool is_generator_{false};
    bool is_async_{false};
};

class FunctionExpr : public Expr {
public:
    FunctionExpr(std::string_view name, std::vector<std::string> params, std::unique_ptr<BlockStmt> body)
        : name_(name), params_(std::move(params)), body_(std::move(body)) {}

    const std::string& Name() const noexcept { return name_; }
    const std::vector<std::string>& Params() const noexcept { return params_; }
    BlockStmt* Body() const noexcept { return body_.get(); }

    bool HasRest() const noexcept { return has_rest_; }
    const std::string& RestParam() const noexcept { return rest_param_; }
    void SetRest(std::string_view r) { has_rest_ = true; rest_param_ = r; }

    bool IsGenerator() const noexcept { return is_generator_; }
    void SetGenerator(bool g) noexcept { is_generator_ = g; }

    bool IsAsync() const noexcept { return is_async_; }
    void SetAsync(bool a) noexcept { is_async_ = a; }

    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::vector<std::string> params_;
    std::unique_ptr<BlockStmt> body_;
    bool has_rest_{false};
    std::string rest_param_;
    bool is_generator_{false};
    bool is_async_{false};
};

class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(std::unique_ptr<Expr> value) : value_(std::move(value)) {}
    Expr* Value() const noexcept { return value_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> value_;
};

class ThrowStmt : public Stmt {
public:
    explicit ThrowStmt(std::unique_ptr<Expr> expr) : expr_(std::move(expr)) {}
    Expr* GetExpr() const noexcept { return expr_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

class TryCatchStmt : public Stmt {
public:
    TryCatchStmt(std::unique_ptr<Stmt> try_block, std::string_view catch_param, std::unique_ptr<Stmt> catch_block, std::unique_ptr<Stmt> finally_block = nullptr)
        : try_block_(std::move(try_block)), catch_param_(catch_param), catch_block_(std::move(catch_block)), finally_block_(std::move(finally_block)) {}

    Stmt* TryBlock() const noexcept { return try_block_.get(); }
    const std::string& CatchParam() const noexcept { return catch_param_; }
    Stmt* CatchBlock() const noexcept { return catch_block_.get(); }
    Stmt* FinallyBlock() const noexcept { return finally_block_.get(); }
    void Accept(ASTVisitor* visitor) override;

private:
    std::unique_ptr<Stmt> try_block_;
    std::string catch_param_;
    std::unique_ptr<Stmt> catch_block_;
    std::unique_ptr<Stmt> finally_block_;
};

class ClassDeclStmt : public Stmt {
public:
    ClassDeclStmt(std::string_view name, std::string_view super_name, std::vector<std::unique_ptr<FunctionDeclStmt>> methods)
        : name_(name), super_name_(super_name), methods_(std::move(methods)) {}

    const std::string& Name() const noexcept { return name_; }
    const std::string& SuperName() const noexcept { return super_name_; }
    const std::vector<std::unique_ptr<FunctionDeclStmt>>& Methods() const noexcept { return methods_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::string super_name_;
    std::vector<std::unique_ptr<FunctionDeclStmt>> methods_;
};

struct ImportSpecifier {
    std::string imported_name;
    std::string local_name;
};

class ImportDeclStmt : public Stmt {
public:
    ImportDeclStmt(std::string_view module_specifier,
                   std::string_view default_import,
                   std::string_view namespace_import,
                   std::vector<ImportSpecifier> named_imports)
        : module_specifier_(module_specifier),
          default_import_(default_import),
          namespace_import_(namespace_import),
          named_imports_(std::move(named_imports)) {}

    const std::string& ModuleSpecifier() const noexcept { return module_specifier_; }
    const std::string& DefaultImport() const noexcept { return default_import_; }
    const std::string& NamespaceImport() const noexcept { return namespace_import_; }
    const std::vector<ImportSpecifier>& NamedImports() const noexcept { return named_imports_; }

    void Accept(ASTVisitor* visitor) override;

private:
    std::string module_specifier_;
    std::string default_import_;
    std::string namespace_import_;
    std::vector<ImportSpecifier> named_imports_;
};

struct ExportSpecifier {
    std::string local_name;
    std::string exported_name;
};

class ExportDeclStmt : public Stmt {
public:
    ExportDeclStmt(bool is_default,
                   std::unique_ptr<Stmt> decl,
                   std::unique_ptr<Expr> default_expr,
                   std::vector<ExportSpecifier> named_exports,
                   std::string_view from_specifier = "",
                   bool is_export_all = false,
                   std::string_view export_all_as = "")
        : is_default_(is_default),
          declaration_(std::move(decl)),
          default_expr_(std::move(default_expr)),
          named_exports_(std::move(named_exports)),
          from_specifier_(from_specifier),
          is_export_all_(is_export_all),
          export_all_as_(export_all_as) {}

    bool IsDefault() const noexcept { return is_default_; }
    Stmt* Declaration() const noexcept { return declaration_.get(); }
    Expr* DefaultExpr() const noexcept { return default_expr_.get(); }
    const std::vector<ExportSpecifier>& NamedExports() const noexcept { return named_exports_; }
    const std::string& FromSpecifier() const noexcept { return from_specifier_; }
    bool IsExportAll() const noexcept { return is_export_all_; }
    const std::string& ExportAllAs() const noexcept { return export_all_as_; }

    void Accept(ASTVisitor* visitor) override;

private:
    bool is_default_{false};
    std::unique_ptr<Stmt> declaration_;
    std::unique_ptr<Expr> default_expr_;
    std::vector<ExportSpecifier> named_exports_;
    std::string from_specifier_;
    bool is_export_all_{false};
    std::string export_all_as_;
};

class Program : public ASTNode {
public:
    Program() = default;
    void AddStatement(std::unique_ptr<Stmt> stmt) {
        statements_.push_back(std::move(stmt));
    }
    const std::vector<std::unique_ptr<Stmt>>& Statements() const noexcept { return statements_; }
    void Accept(ASTVisitor* visitor) override;

private:
    std::vector<std::unique_ptr<Stmt>> statements_;
};

// Visitor Interface
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;
    virtual void VisitLiteral(LiteralExpr* node) = 0;
    virtual void VisitIdentifier(IdentifierExpr* node) = 0;
    virtual void VisitUnary(UnaryExpr* node) = 0;
    virtual void VisitBinary(BinaryExpr* node) = 0;
    virtual void VisitAssign(AssignExpr* node) = 0;
    virtual void VisitPropertyAccess(PropertyAccessExpr* node) = 0;
    virtual void VisitPropertyAssign(PropertyAssignExpr* node) = 0;
    virtual void VisitCall(CallExpr* node) = 0;
    virtual void VisitNew(NewExpr* node) = 0;
    virtual void VisitFunctionExpr(FunctionExpr* node) = 0;
    virtual void VisitObjectLiteral(ObjectLiteralExpr* node) = 0;
    virtual void VisitArrayLiteral(ArrayLiteralExpr* node) = 0;
    virtual void VisitIndexAccess(IndexAccessExpr* node) = 0;
    virtual void VisitIndexAssign(IndexAssignExpr* node) = 0;
    virtual void VisitCompoundPropertyAssign(CompoundPropertyAssignExpr* node) = 0;
    virtual void VisitCompoundIndexAssign(CompoundIndexAssignExpr* node) = 0;
    virtual void VisitUpdate(UpdateExpr* node) = 0;
    virtual void VisitConditional(ConditionalExpr* node) = 0;
    virtual void VisitTemplateLiteral(TemplateLiteralExpr* node) = 0;
    virtual void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* node) = 0;
    virtual void VisitOptionalIndexAccess(OptionalIndexAccessExpr* node) = 0;
    virtual void VisitOptionalCall(OptionalCallExpr* node) = 0;
    virtual void VisitSpread(SpreadExpr* node) = 0;
    virtual void VisitYield(YieldExpr* node) = 0;
    virtual void VisitAwait(AwaitExpr* node) = 0;
    virtual void VisitImportMeta(ImportMetaExpr* node) = 0;
    virtual void VisitDynamicImport(DynamicImportExpr* node) = 0;

    virtual void VisitExprStmt(ExprStmt* node) = 0;
    virtual void VisitVarDecl(VarDeclStmt* node) = 0;
    virtual void VisitBlock(BlockStmt* node) = 0;
    virtual void VisitIf(IfStmt* node) = 0;
    virtual void VisitWhile(WhileStmt* node) = 0;
    virtual void VisitDoWhile(DoWhileStmt* node) = 0;
    virtual void VisitFor(ForStmt* node) = 0;
    virtual void VisitBreak(BreakStmt* node) = 0;
    virtual void VisitContinue(ContinueStmt* node) = 0;
    virtual void VisitSwitch(SwitchStmt* node) = 0;
    virtual void VisitDebugger(DebuggerStmt* node) = 0;
    virtual void VisitFunctionDecl(FunctionDeclStmt* node) = 0;
    virtual void VisitClassDecl(ClassDeclStmt* node) = 0;
    virtual void VisitImportDecl(ImportDeclStmt* node) = 0;
    virtual void VisitExportDecl(ExportDeclStmt* node) = 0;
    virtual void VisitThrow(ThrowStmt* node) = 0;
    virtual void VisitTryCatch(TryCatchStmt* node) = 0;
    virtual void VisitReturn(ReturnStmt* node) = 0;
    virtual void VisitProgram(Program* node) = 0;
};

inline void LiteralExpr::Accept(ASTVisitor* v) { v->VisitLiteral(this); }
inline void IdentifierExpr::Accept(ASTVisitor* v) { v->VisitIdentifier(this); }
inline void UnaryExpr::Accept(ASTVisitor* v) { v->VisitUnary(this); }
inline void BinaryExpr::Accept(ASTVisitor* v) { v->VisitBinary(this); }
inline void AssignExpr::Accept(ASTVisitor* v) { v->VisitAssign(this); }
inline void PropertyAccessExpr::Accept(ASTVisitor* v) { v->VisitPropertyAccess(this); }
inline void PropertyAssignExpr::Accept(ASTVisitor* v) { v->VisitPropertyAssign(this); }
inline void CallExpr::Accept(ASTVisitor* v) { v->VisitCall(this); }
inline void NewExpr::Accept(ASTVisitor* v) { v->VisitNew(this); }
inline void FunctionExpr::Accept(ASTVisitor* v) { v->VisitFunctionExpr(this); }
inline void ObjectLiteralExpr::Accept(ASTVisitor* v) { v->VisitObjectLiteral(this); }
inline void ArrayLiteralExpr::Accept(ASTVisitor* v) { v->VisitArrayLiteral(this); }
inline void IndexAccessExpr::Accept(ASTVisitor* v) { v->VisitIndexAccess(this); }
inline void IndexAssignExpr::Accept(ASTVisitor* v) { v->VisitIndexAssign(this); }
inline void CompoundPropertyAssignExpr::Accept(ASTVisitor* v) { v->VisitCompoundPropertyAssign(this); }
inline void CompoundIndexAssignExpr::Accept(ASTVisitor* v) { v->VisitCompoundIndexAssign(this); }
inline void UpdateExpr::Accept(ASTVisitor* v) { v->VisitUpdate(this); }
inline void ConditionalExpr::Accept(ASTVisitor* v) { v->VisitConditional(this); }
inline void TemplateLiteralExpr::Accept(ASTVisitor* v) { v->VisitTemplateLiteral(this); }
inline void OptionalPropertyAccessExpr::Accept(ASTVisitor* v) { v->VisitOptionalPropertyAccess(this); }
inline void OptionalIndexAccessExpr::Accept(ASTVisitor* v) { v->VisitOptionalIndexAccess(this); }
inline void OptionalCallExpr::Accept(ASTVisitor* v) { v->VisitOptionalCall(this); }
inline void SpreadExpr::Accept(ASTVisitor* v) { v->VisitSpread(this); }
inline void YieldExpr::Accept(ASTVisitor* v) { v->VisitYield(this); }
inline void AwaitExpr::Accept(ASTVisitor* v) { v->VisitAwait(this); }
inline void ImportMetaExpr::Accept(ASTVisitor* v) { v->VisitImportMeta(this); }
inline void DynamicImportExpr::Accept(ASTVisitor* v) { v->VisitDynamicImport(this); }
inline void ExprStmt::Accept(ASTVisitor* v) { v->VisitExprStmt(this); }
inline void VarDeclStmt::Accept(ASTVisitor* v) { v->VisitVarDecl(this); }
inline void BlockStmt::Accept(ASTVisitor* v) { v->VisitBlock(this); }
inline void IfStmt::Accept(ASTVisitor* v) { v->VisitIf(this); }
inline void WhileStmt::Accept(ASTVisitor* v) { v->VisitWhile(this); }
inline void DoWhileStmt::Accept(ASTVisitor* v) { v->VisitDoWhile(this); }
inline void ForStmt::Accept(ASTVisitor* v) { v->VisitFor(this); }
inline void BreakStmt::Accept(ASTVisitor* v) { v->VisitBreak(this); }
inline void ContinueStmt::Accept(ASTVisitor* v) { v->VisitContinue(this); }
inline void SwitchStmt::Accept(ASTVisitor* v) { v->VisitSwitch(this); }
inline void DebuggerStmt::Accept(ASTVisitor* v) { v->VisitDebugger(this); }
inline void FunctionDeclStmt::Accept(ASTVisitor* v) { v->VisitFunctionDecl(this); }
inline void ClassDeclStmt::Accept(ASTVisitor* v) { v->VisitClassDecl(this); }
inline void ImportDeclStmt::Accept(ASTVisitor* v) { v->VisitImportDecl(this); }
inline void ExportDeclStmt::Accept(ASTVisitor* v) { v->VisitExportDecl(this); }
inline void ThrowStmt::Accept(ASTVisitor* v) { v->VisitThrow(this); }
inline void TryCatchStmt::Accept(ASTVisitor* v) { v->VisitTryCatch(this); }
inline void ReturnStmt::Accept(ASTVisitor* v) { v->VisitReturn(this); }
inline void Program::Accept(ASTVisitor* v) { v->VisitProgram(this); }

} // namespace aether

#endif // AETHER_AST_H
