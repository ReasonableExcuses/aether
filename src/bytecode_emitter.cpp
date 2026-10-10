#include "aether/bytecode_emitter.h"

namespace aether {

std::unique_ptr<BytecodeArray> BytecodeEmitter::Emit(ASTNode* root) {
    bytecode_ = std::make_unique<BytecodeArray>();
    scope_.clear();
    register_in_use_.clear();
    max_registers_ = 0;
    next_feedback_slot_ = 0;

    root->Accept(this);

    // If code does not end with Return or Halt, emit Return acc
    if (bytecode_->Code().empty() || 
        (bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Return) && 
         bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Halt))) {
        bytecode_->EmitOpcode(Opcode::Return);
    }

    bytecode_->SetRegisterCount(max_registers_);
    return std::move(bytecode_);
}

std::unique_ptr<BytecodeArray> BytecodeEmitter::EmitFunction(FunctionDeclStmt* func) {
    bytecode_ = std::make_unique<BytecodeArray>();
    scope_.clear();
    register_in_use_.clear();
    max_registers_ = 0;
    next_feedback_slot_ = 0;

    uint32_t param_count = static_cast<uint32_t>(func->Params().size());
    bytecode_->SetParameterCount(param_count);

    // Bind parameters to registers r0..r(param_count-1)
    for (uint32_t i = 0; i < param_count; ++i) {
        uint8_t reg = AllocateRegister();
        scope_[func->Params()[i]] = reg;
    }

    func->Body()->Accept(this);

    if (bytecode_->Code().empty() || 
        (bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Return) && 
         bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Halt))) {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
        bytecode_->EmitOpcode(Opcode::Return);
    }

    bytecode_->SetRegisterCount(max_registers_);
    return std::move(bytecode_);
}

uint8_t BytecodeEmitter::AllocateRegister() {
    for (size_t i = 0; i < register_in_use_.size(); ++i) {
        if (!register_in_use_[i]) {
            register_in_use_[i] = true;
            return static_cast<uint8_t>(i);
        }
    }
    size_t idx = register_in_use_.size();
    assert(idx < 255 && "Virtual register exhaustion");
    register_in_use_.push_back(true);
    if (register_in_use_.size() > max_registers_) {
        max_registers_ = static_cast<uint8_t>(register_in_use_.size());
    }
    return static_cast<uint8_t>(idx);
}

uint8_t BytecodeEmitter::AllocateRegisterRange(uint8_t count) {
    if (count == 0) return 0;
    // Find contiguous range of free registers
    for (size_t i = 0; i + count <= register_in_use_.size(); ++i) {
        bool all_free = true;
        for (size_t j = 0; j < count; ++j) {
            if (register_in_use_[i + j]) {
                all_free = false;
                break;
            }
        }
        if (all_free) {
            for (size_t j = 0; j < count; ++j) {
                register_in_use_[i + j] = true;
            }
            return static_cast<uint8_t>(i);
        }
    }

    // Allocate at end
    size_t start = register_in_use_.size();
    assert(start + count <= 255 && "Virtual register exhaustion");
    for (size_t j = 0; j < count; ++j) {
        register_in_use_.push_back(true);
    }
    if (register_in_use_.size() > max_registers_) {
        max_registers_ = static_cast<uint8_t>(register_in_use_.size());
    }
    return static_cast<uint8_t>(start);
}

void BytecodeEmitter::FreeRegister(uint8_t reg) {
    assert(reg < register_in_use_.size());
    register_in_use_[reg] = false;
}

void BytecodeEmitter::FreeRegisterRange(uint8_t start, uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        FreeRegister(static_cast<uint8_t>(start + i));
    }
}

void BytecodeEmitter::Bind(Label& label) {
    label.target_offset = static_cast<int32_t>(bytecode_->CurrentOffset());
    for (size_t jump_pc : label.forward_jumps) {
        int32_t rel_offset = label.target_offset - static_cast<int32_t>(jump_pc + 4);
        bytecode_->PatchI32(jump_pc, rel_offset);
    }
    label.forward_jumps.clear();
}

void BytecodeEmitter::EmitJump(Opcode jump_op, Label& label) {
    bytecode_->EmitOpcode(jump_op);
    size_t patch_offset = bytecode_->CurrentOffset();
    if (label.target_offset != -1) {
        int32_t rel_offset = label.target_offset - static_cast<int32_t>(patch_offset + 4);
        bytecode_->EmitI32(rel_offset);
    } else {
        label.forward_jumps.push_back(patch_offset);
        bytecode_->EmitI32(0); // Placeholder
    }
}

void BytecodeEmitter::VisitLiteral(LiteralExpr* node) {
    TaggedValue val = node->Value();
    if (val.IsSmi()) {
        bytecode_->EmitOpcode(Opcode::LdaSmi);
        bytecode_->EmitI32(val.AsSmi());
    } else if (val.IsBoolean()) {
        bytecode_->EmitOpcode(val.AsBoolean() ? Opcode::LdaTrue : Opcode::LdaFalse);
    } else if (val.IsUndefined()) {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
    } else if (val.IsNull()) {
        bytecode_->EmitOpcode(Opcode::LdaNull);
    } else {
        uint16_t idx = bytecode_->AddConstant(val);
        bytecode_->EmitOpcode(Opcode::LdaConstant);
        bytecode_->EmitU16(idx);
    }
}

JSFunction* BytecodeEmitter::LookupFunction(std::string_view name) {
    auto it = functions_.find(std::string(name));
    if (it != functions_.end()) {
        return it->second;
    }
    if (parent_emitter_) {
        return parent_emitter_->LookupFunction(name);
    }
    return nullptr;
}

void BytecodeEmitter::VisitIdentifier(IdentifierExpr* node) {
    auto it = scope_.find(node->Name());
    if (it != scope_.end()) {
        bytecode_->EmitOpcode(Opcode::Ldar);
        bytecode_->EmitU8(it->second);
        return;
    }

    JSFunction* fn = LookupFunction(node->Name());
    if (fn != nullptr) {
        uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(fn));
        bytecode_->EmitOpcode(Opcode::LdaConstant);
        bytecode_->EmitU16(const_idx);
        return;
    }

    assert(false && "Undeclared identifier in scope");
}

void BytecodeEmitter::VisitUnary(UnaryExpr* node) {
    node->GetExpr()->Accept(this);
    switch (node->Op()) {
        case UnaryOp::LogicalNot:
            bytecode_->EmitOpcode(Opcode::LogicalNot);
            break;
        case UnaryOp::Negate:
            bytecode_->EmitOpcode(Opcode::Negate);
            break;
        case UnaryOp::BitwiseNot:
            bytecode_->EmitOpcode(Opcode::BitwiseNot);
            break;
        case UnaryOp::Plus:
            // Unary + is a ToNumber conversion, numbers are unchanged
            break;
    }
}

void BytecodeEmitter::VisitAssign(AssignExpr* node) {
    node->Value()->Accept(this);
    auto it = scope_.find(node->Target());
    assert(it != scope_.end() && "Assignment to undeclared variable");
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(it->second);
}

void BytecodeEmitter::VisitPropertyAccess(PropertyAccessExpr* node) {
    node->Object()->Accept(this);
    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    auto* s = new JSString(node->Property());
    uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(s));
    uint16_t slot = next_feedback_slot_++;

    bytecode_->EmitOpcode(Opcode::LdaNamed);
    bytecode_->EmitU8(obj_reg);
    bytecode_->EmitU16(name_idx);
    bytecode_->EmitU16(slot);

    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitPropertyAssign(PropertyAssignExpr* node) {
    // 1. Evaluate object
    node->Object()->Accept(this);
    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    // 2. Evaluate value
    node->Value()->Accept(this);

    // 3. Emit StaNamed (acc -> obj.prop)
    auto* s = new JSString(node->Property());
    uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(s));
    uint16_t slot = next_feedback_slot_++;

    bytecode_->EmitOpcode(Opcode::StaNamed);
    bytecode_->EmitU8(obj_reg);
    bytecode_->EmitU16(name_idx);
    bytecode_->EmitU16(slot);

    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitCall(CallExpr* node) {
    // 1. Evaluate callee
    node->Callee()->Accept(this);
    uint8_t callee_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(callee_reg);

    // 2. Evaluate arguments into contiguous registers
    uint8_t arg_count = static_cast<uint8_t>(node->Args().size());
    uint8_t args_start = AllocateRegisterRange(arg_count);

    for (uint8_t i = 0; i < arg_count; ++i) {
        node->Args()[i]->Accept(this);
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
    }

    // 3. Emit Call
    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::Call);
    bytecode_->EmitU8(callee_reg);
    bytecode_->EmitU8(args_start);
    bytecode_->EmitU8(arg_count);
    bytecode_->EmitU16(slot);

    FreeRegisterRange(args_start, arg_count);
    FreeRegister(callee_reg);
}

void BytecodeEmitter::VisitObjectLiteral(ObjectLiteralExpr* node) {
    bytecode_->EmitOpcode(Opcode::CreateObject);
    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    for (const auto& [prop_name, expr] : node->Properties()) {
        expr->Accept(this);
        auto* s = new JSString(prop_name);
        uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(s));
        uint16_t slot = next_feedback_slot_++;

        bytecode_->EmitOpcode(Opcode::StaNamed);
        bytecode_->EmitU8(obj_reg);
        bytecode_->EmitU16(name_idx);
        bytecode_->EmitU16(slot);
    }

    bytecode_->EmitOpcode(Opcode::Ldar);
    bytecode_->EmitU8(obj_reg);
    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitBinary(BinaryExpr* node) {
    // 1. Evaluate LHS
    node->Lhs()->Accept(this);
    uint8_t lhs_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(lhs_reg);

    // 2. Evaluate RHS
    node->Rhs()->Accept(this);
    uint8_t rhs_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(rhs_reg);

    // 3. Load LHS back to acc
    bytecode_->EmitOpcode(Opcode::Ldar);
    bytecode_->EmitU8(lhs_reg);

    // 4. Perform operation
    uint16_t slot = next_feedback_slot_++;
    switch (node->Op()) {
        case BinaryOp::Add:
            bytecode_->EmitOpcode(Opcode::Add);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitU16(slot);
            break;
        case BinaryOp::Sub:
            bytecode_->EmitOpcode(Opcode::Sub);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitU16(slot);
            break;
        case BinaryOp::Mul:
            bytecode_->EmitOpcode(Opcode::Mul);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitU16(slot);
            break;
        case BinaryOp::Div:
            bytecode_->EmitOpcode(Opcode::Div);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitU16(slot);
            break;
        case BinaryOp::Mod:
            bytecode_->EmitOpcode(Opcode::Mod);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitU16(slot);
            break;
        case BinaryOp::BitwiseAnd:
            bytecode_->EmitOpcode(Opcode::BitwiseAnd);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::BitwiseOr:
            bytecode_->EmitOpcode(Opcode::BitwiseOr);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::BitwiseXor:
            bytecode_->EmitOpcode(Opcode::BitwiseXor);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::ShiftLeft:
            bytecode_->EmitOpcode(Opcode::ShiftLeft);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::ShiftRight:
            bytecode_->EmitOpcode(Opcode::ShiftRight);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::Equal:
            bytecode_->EmitOpcode(Opcode::TestEqual);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::StrictEqual:
            bytecode_->EmitOpcode(Opcode::TestStrictEqual);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::NotEqual:
            bytecode_->EmitOpcode(Opcode::TestEqual);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitOpcode(Opcode::LogicalNot);
            break;
        case BinaryOp::StrictNotEqual:
            bytecode_->EmitOpcode(Opcode::TestStrictEqual);
            bytecode_->EmitU8(rhs_reg);
            bytecode_->EmitOpcode(Opcode::LogicalNot);
            break;
        case BinaryOp::LessThan:
            bytecode_->EmitOpcode(Opcode::TestLessThan);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::GreaterThan:
            bytecode_->EmitOpcode(Opcode::TestGreaterThan);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::LessThanOrEqual:
            bytecode_->EmitOpcode(Opcode::TestLessThanOrEqual);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::GreaterThanOrEqual:
            bytecode_->EmitOpcode(Opcode::TestGreaterThanOrEqual);
            bytecode_->EmitU8(rhs_reg);
            break;
    }

    FreeRegister(rhs_reg);
    FreeRegister(lhs_reg);
}

void BytecodeEmitter::VisitExprStmt(ExprStmt* node) {
    node->GetExpr()->Accept(this);
}

void BytecodeEmitter::VisitVarDecl(VarDeclStmt* node) {
    uint8_t reg = AllocateRegister();
    scope_[node->Name()] = reg;

    if (node->Initializer()) {
        node->Initializer()->Accept(this);
    } else {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
    }

    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(reg);
}

void BytecodeEmitter::VisitBlock(BlockStmt* node) {
    for (const auto& stmt : node->Statements()) {
        stmt->Accept(this);
    }
}

void BytecodeEmitter::VisitIf(IfStmt* node) {
    node->Condition()->Accept(this);
    Label else_label;
    Label end_label;

    EmitJump(Opcode::JumpIfFalse, else_label);
    node->ThenBranch()->Accept(this);

    if (node->ElseBranch()) {
        EmitJump(Opcode::Jump, end_label);
        Bind(else_label);
        node->ElseBranch()->Accept(this);
        Bind(end_label);
    } else {
        Bind(else_label);
    }
}

void BytecodeEmitter::VisitWhile(WhileStmt* node) {
    Label loop_header;
    Label loop_exit;

    Bind(loop_header);
    node->Condition()->Accept(this);
    EmitJump(Opcode::JumpIfFalse, loop_exit);

    node->Body()->Accept(this);
    EmitJump(Opcode::Jump, loop_header);

    Bind(loop_exit);
}

void BytecodeEmitter::VisitFor(ForStmt* node) {
    if (node->Init()) {
        node->Init()->Accept(this);
    }

    Label loop_header;
    Label loop_exit;

    Bind(loop_header);
    if (node->Cond()) {
        node->Cond()->Accept(this);
        EmitJump(Opcode::JumpIfFalse, loop_exit);
    }

    node->Body()->Accept(this);

    if (node->Update()) {
        node->Update()->Accept(this);
    }

    EmitJump(Opcode::Jump, loop_header);
    Bind(loop_exit);
}

void BytecodeEmitter::VisitFunctionDecl(FunctionDeclStmt* node) {
    // 1. Allocate function object placeholder and register for recursive lookups
    auto* fn_obj = new JSFunction(node->Name(), nullptr);
    functions_[node->Name()] = fn_obj;

    // 2. Compile child routine
    BytecodeEmitter sub_emitter;
    sub_emitter.parent_emitter_ = this;
    auto fn_bc = sub_emitter.EmitFunction(node);

    // 3. Attach compiled bytecode
    BytecodeArray* raw_bc = fn_bc.get();
    fn_obj->SetBytecode(raw_bc);
    compiled_routines_.push_back(std::move(fn_bc));

    // 4. Register in current scope and emit constant load
    uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(fn_obj));
    uint8_t fn_reg = AllocateRegister();
    scope_[node->Name()] = fn_reg;

    bytecode_->EmitOpcode(Opcode::LdaConstant);
    bytecode_->EmitU16(const_idx);
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(fn_reg);
}

void BytecodeEmitter::VisitReturn(ReturnStmt* node) {
    if (node->Value()) {
        node->Value()->Accept(this);
    } else {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
    }
    bytecode_->EmitOpcode(Opcode::Return);
}

void BytecodeEmitter::VisitProgram(Program* node) {
    for (const auto& stmt : node->Statements()) {
        stmt->Accept(this);
    }
}

} // namespace aether
