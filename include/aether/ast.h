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
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    ShiftLeft,
    ShiftRight,
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
    InstanceOf
};

enum class UnaryOp {
    LogicalNot,
    Negate,
    BitwiseNot,
    Plus,
    TypeOf
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

    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::vector<std::string> params_;
    std::unique_ptr<BlockStmt> body_;
    bool has_rest_{false};
    std::string rest_param_;
    bool is_generator_{false};
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

    void Accept(ASTVisitor* visitor) override;

private:
    std::string name_;
    std::vector<std::string> params_;
    std::unique_ptr<BlockStmt> body_;
    bool has_rest_{false};
    std::string rest_param_;
    bool is_generator_{false};
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
    virtual void VisitConditional(ConditionalExpr* node) = 0;
    virtual void VisitTemplateLiteral(TemplateLiteralExpr* node) = 0;
    virtual void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* node) = 0;
    virtual void VisitOptionalIndexAccess(OptionalIndexAccessExpr* node) = 0;
    virtual void VisitOptionalCall(OptionalCallExpr* node) = 0;
    virtual void VisitSpread(SpreadExpr* node) = 0;
    virtual void VisitYield(YieldExpr* node) = 0;

    virtual void VisitExprStmt(ExprStmt* node) = 0;
    virtual void VisitVarDecl(VarDeclStmt* node) = 0;
    virtual void VisitBlock(BlockStmt* node) = 0;
    virtual void VisitIf(IfStmt* node) = 0;
    virtual void VisitWhile(WhileStmt* node) = 0;
    virtual void VisitFor(ForStmt* node) = 0;
    virtual void VisitFunctionDecl(FunctionDeclStmt* node) = 0;
    virtual void VisitClassDecl(ClassDeclStmt* node) = 0;
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
inline void ConditionalExpr::Accept(ASTVisitor* v) { v->VisitConditional(this); }
inline void TemplateLiteralExpr::Accept(ASTVisitor* v) { v->VisitTemplateLiteral(this); }
inline void OptionalPropertyAccessExpr::Accept(ASTVisitor* v) { v->VisitOptionalPropertyAccess(this); }
inline void OptionalIndexAccessExpr::Accept(ASTVisitor* v) { v->VisitOptionalIndexAccess(this); }
inline void OptionalCallExpr::Accept(ASTVisitor* v) { v->VisitOptionalCall(this); }
inline void SpreadExpr::Accept(ASTVisitor* v) { v->VisitSpread(this); }
inline void YieldExpr::Accept(ASTVisitor* v) { v->VisitYield(this); }
inline void ExprStmt::Accept(ASTVisitor* v) { v->VisitExprStmt(this); }
inline void VarDeclStmt::Accept(ASTVisitor* v) { v->VisitVarDecl(this); }
inline void BlockStmt::Accept(ASTVisitor* v) { v->VisitBlock(this); }
inline void IfStmt::Accept(ASTVisitor* v) { v->VisitIf(this); }
inline void WhileStmt::Accept(ASTVisitor* v) { v->VisitWhile(this); }
inline void ForStmt::Accept(ASTVisitor* v) { v->VisitFor(this); }
inline void FunctionDeclStmt::Accept(ASTVisitor* v) { v->VisitFunctionDecl(this); }
inline void ClassDeclStmt::Accept(ASTVisitor* v) { v->VisitClassDecl(this); }
inline void ThrowStmt::Accept(ASTVisitor* v) { v->VisitThrow(this); }
inline void TryCatchStmt::Accept(ASTVisitor* v) { v->VisitTryCatch(this); }
inline void ReturnStmt::Accept(ASTVisitor* v) { v->VisitReturn(this); }
inline void Program::Accept(ASTVisitor* v) { v->VisitProgram(this); }

} // namespace aether

#endif // AETHER_AST_H
