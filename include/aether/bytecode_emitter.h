#ifndef AETHER_BYTECODE_EMITTER_H
#define AETHER_BYTECODE_EMITTER_H

#include "aether/common.h"
#include "aether/ast.h"
#include "aether/bytecode.h"
#include "aether/heap_object.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace aether {

struct Label {
    int32_t target_offset{-1};
    std::vector<size_t> forward_jumps;
};

/**
 * @brief Translates AST into optimized Accumulator-Register BytecodeArray.
 */
class BytecodeEmitter : public ASTVisitor {
public:
    BytecodeEmitter() = default;

    std::unique_ptr<BytecodeArray> Emit(ASTNode* root);
    std::unique_ptr<BytecodeArray> EmitFunction(FunctionDeclStmt* func);

    // ASTVisitor implementations
    void VisitLiteral(LiteralExpr* node) override;
    void VisitIdentifier(IdentifierExpr* node) override;
    void VisitUnary(UnaryExpr* node) override;
    void VisitBinary(BinaryExpr* node) override;
    void VisitAssign(AssignExpr* node) override;
    void VisitPropertyAccess(PropertyAccessExpr* node) override;
    void VisitPropertyAssign(PropertyAssignExpr* node) override;
    void VisitCall(CallExpr* node) override;
    void VisitNew(NewExpr* node) override;
    void VisitFunctionExpr(FunctionExpr* node) override;
    void VisitObjectLiteral(ObjectLiteralExpr* node) override;
    void VisitArrayLiteral(ArrayLiteralExpr* node) override;
    void VisitIndexAccess(IndexAccessExpr* node) override;
    void VisitIndexAssign(IndexAssignExpr* node) override;
    void VisitConditional(ConditionalExpr* node) override;
    void VisitTemplateLiteral(TemplateLiteralExpr* node) override;
    void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* node) override;
    void VisitOptionalIndexAccess(OptionalIndexAccessExpr* node) override;
    void VisitOptionalCall(OptionalCallExpr* node) override;
    void VisitSpread(SpreadExpr* node) override;
    void VisitYield(YieldExpr* node) override;

    void VisitExprStmt(ExprStmt* node) override;
    void VisitVarDecl(VarDeclStmt* node) override;
    void VisitBlock(BlockStmt* node) override;
    void VisitIf(IfStmt* node) override;
    void VisitWhile(WhileStmt* node) override;
    void VisitFor(ForStmt* node) override;
    void VisitFunctionDecl(FunctionDeclStmt* node) override;
    void VisitClassDecl(ClassDeclStmt* node) override;
    void VisitThrow(ThrowStmt* node) override;
    void VisitTryCatch(TryCatchStmt* node) override;
    void VisitReturn(ReturnStmt* node) override;
    void VisitProgram(Program* node) override;

    // Label and jump handling
    void Bind(Label& label);
    void EmitJump(Opcode jump_op, Label& label);

    // Register allocator
    uint8_t AllocateRegister();
    uint8_t AllocateRegisterRange(uint8_t count);
    void FreeRegister(uint8_t reg);
    void FreeRegisterRange(uint8_t start, uint8_t count);

    struct VarResolution {
        enum Kind { LocalReg, ContextSlot, GlobalOrFunction, GlobalValue, ThisVar, NotFound } kind{NotFound};
        uint8_t reg{0};
        uint8_t depth{0};
        uint16_t slot{0};
        JSFunction* fn{nullptr};
        TaggedValue global_val{TaggedValue::Undefined()};
    };

    VarResolution ResolveVariable(std::string_view name);
    void ScanAndSetupContextSlots(const std::vector<std::string>& params, ASTNode* body);

private:
    std::unique_ptr<BytecodeArray> bytecode_;
    std::unordered_map<std::string, uint8_t> scope_;
    std::unordered_map<std::string, uint16_t> context_slots_;
    std::vector<bool> register_in_use_;
    uint8_t max_registers_{0};
    uint16_t next_feedback_slot_{0};

    // Function lookup
    JSFunction* LookupFunction(std::string_view name);

    BytecodeEmitter* parent_emitter_{nullptr};
    std::unordered_map<std::string, JSFunction*> functions_;

    // Keep nested function bytecode arrays alive
    std::vector<std::unique_ptr<BytecodeArray>> compiled_routines_;
};

} // namespace aether

#endif // AETHER_BYTECODE_EMITTER_H
