#include "aether/bytecode_emitter.h"
#include "aether/runtime.h"

namespace aether {

class CaptureScanner : public ASTVisitor {
public:
    std::unordered_set<std::string> identifiers_in_nested_functions;
    bool inside_nested_function{false};

    void VisitLiteral(LiteralExpr*) override {}
    void VisitIdentifier(IdentifierExpr* node) override {
        if (inside_nested_function) {
            identifiers_in_nested_functions.insert(node->Name());
        }
    }
    void VisitUnary(UnaryExpr* node) override { node->GetExpr()->Accept(this); }
    void VisitBinary(BinaryExpr* node) override {
        node->Lhs()->Accept(this);
        node->Rhs()->Accept(this);
    }
    void VisitAssign(AssignExpr* node) override {
        if (inside_nested_function) {
            identifiers_in_nested_functions.insert(node->Target());
        }
        node->Value()->Accept(this);
    }
    void VisitPropertyAccess(PropertyAccessExpr* node) override { node->Object()->Accept(this); }
    void VisitPropertyAssign(PropertyAssignExpr* node) override {
        node->Object()->Accept(this);
        node->Value()->Accept(this);
    }
    void VisitCall(CallExpr* node) override {
        node->Callee()->Accept(this);
        for (const auto& a : node->Args()) a->Accept(this);
    }
    void VisitNew(NewExpr* node) override {
        node->Callee()->Accept(this);
        for (const auto& a : node->Args()) a->Accept(this);
    }
    void VisitFunctionExpr(FunctionExpr* node) override {
        bool prev = inside_nested_function;
        inside_nested_function = true;
        node->Body()->Accept(this);
        inside_nested_function = prev;
    }
    void VisitObjectLiteral(ObjectLiteralExpr* node) override {
        for (const auto& p : node->Properties()) {
            if (p.computed_key) p.computed_key->Accept(this);
            if (p.value) p.value->Accept(this);
        }
    }
    void VisitArrayLiteral(ArrayLiteralExpr* node) override {
        for (const auto& el : node->Elements()) el->Accept(this);
    }
    void VisitIndexAccess(IndexAccessExpr* node) override {
        node->Object()->Accept(this);
        node->Index()->Accept(this);
    }
    void VisitIndexAssign(IndexAssignExpr* node) override {
        node->Object()->Accept(this);
        node->Index()->Accept(this);
        node->Value()->Accept(this);
    }
    void VisitConditional(ConditionalExpr* node) override {
        node->Condition()->Accept(this);
        node->ThenBranch()->Accept(this);
        node->ElseBranch()->Accept(this);
    }
    void VisitTemplateLiteral(TemplateLiteralExpr* node) override {
        for (const auto& e : node->Expressions()) e->Accept(this);
    }
    void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* node) override {
        node->Object()->Accept(this);
    }
    void VisitOptionalIndexAccess(OptionalIndexAccessExpr* node) override {
        node->Object()->Accept(this);
        node->Index()->Accept(this);
    }
    void VisitOptionalCall(OptionalCallExpr* node) override {
        node->Callee()->Accept(this);
        for (const auto& a : node->Args()) a->Accept(this);
    }
    void VisitSpread(SpreadExpr* node) override {
        node->GetExpr()->Accept(this);
    }
    void VisitYield(YieldExpr* node) override {
        if (node->GetExpr()) node->GetExpr()->Accept(this);
    }
    void VisitAwait(AwaitExpr* node) override {
        if (node->GetExpr()) node->GetExpr()->Accept(this);
    }
    void VisitExprStmt(ExprStmt* node) override { node->GetExpr()->Accept(this); }
    void VisitVarDecl(VarDeclStmt* node) override {
        if (node->Initializer()) node->Initializer()->Accept(this);
    }
    void VisitBlock(BlockStmt* node) override {
        for (const auto& s : node->Statements()) s->Accept(this);
    }
    void VisitIf(IfStmt* node) override {
        node->Condition()->Accept(this);
        node->ThenBranch()->Accept(this);
        if (node->ElseBranch()) node->ElseBranch()->Accept(this);
    }
    void VisitWhile(WhileStmt* node) override {
        node->Condition()->Accept(this);
        node->Body()->Accept(this);
    }
    void VisitFor(ForStmt* node) override {
        if (node->Init()) node->Init()->Accept(this);
        if (node->Cond()) node->Cond()->Accept(this);
        if (node->Update()) node->Update()->Accept(this);
        node->Body()->Accept(this);
    }
    void VisitFunctionDecl(FunctionDeclStmt* node) override {
        bool prev = inside_nested_function;
        inside_nested_function = true;
        node->Body()->Accept(this);
        inside_nested_function = prev;
    }
    void VisitClassDecl(ClassDeclStmt* node) override {
        for (const auto& m : node->Methods()) {
            m->Accept(this);
        }
    }
    void VisitThrow(ThrowStmt* node) override { node->GetExpr()->Accept(this); }
    void VisitTryCatch(TryCatchStmt* node) override {
        node->TryBlock()->Accept(this);
        if (node->CatchBlock()) node->CatchBlock()->Accept(this);
        if (node->FinallyBlock()) node->FinallyBlock()->Accept(this);
    }
    void VisitReturn(ReturnStmt* node) override {
        if (node->Value()) node->Value()->Accept(this);
    }
    void VisitProgram(Program* node) override {
        for (const auto& s : node->Statements()) s->Accept(this);
    }
};

void BytecodeEmitter::ScanAndSetupContextSlots(const std::vector<std::string>& params, ASTNode* body) {
    context_slots_.clear();
    if (!body) return;
    CaptureScanner scanner;
    body->Accept(&scanner);

    for (const auto& p : params) {
        if (scanner.identifiers_in_nested_functions.count(p)) {
            if (!context_slots_.count(p)) {
                uint16_t slot = static_cast<uint16_t>(context_slots_.size());
                context_slots_[p] = slot;
            }
        }
    }

    if (auto* block = dynamic_cast<BlockStmt*>(body)) {
        for (const auto& stmt : block->Statements()) {
            if (auto* v = dynamic_cast<VarDeclStmt*>(stmt.get())) {
                if (scanner.identifiers_in_nested_functions.count(v->Name())) {
                    if (!context_slots_.count(v->Name())) {
                        uint16_t slot = static_cast<uint16_t>(context_slots_.size());
                        context_slots_[v->Name()] = slot;
                    }
                }
            } else if (auto* f = dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
                if (scanner.identifiers_in_nested_functions.count(f->Name())) {
                    if (!context_slots_.count(f->Name())) {
                        uint16_t slot = static_cast<uint16_t>(context_slots_.size());
                        context_slots_[f->Name()] = slot;
                    }
                }
            }
        }
    } else if (auto* prog = dynamic_cast<Program*>(body)) {
        for (const auto& stmt : prog->Statements()) {
            if (auto* v = dynamic_cast<VarDeclStmt*>(stmt.get())) {
                if (scanner.identifiers_in_nested_functions.count(v->Name())) {
                    if (!context_slots_.count(v->Name())) {
                        uint16_t slot = static_cast<uint16_t>(context_slots_.size());
                        context_slots_[v->Name()] = slot;
                    }
                }
            } else if (auto* f = dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
                if (scanner.identifiers_in_nested_functions.count(f->Name())) {
                    if (!context_slots_.count(f->Name())) {
                        uint16_t slot = static_cast<uint16_t>(context_slots_.size());
                        context_slots_[f->Name()] = slot;
                    }
                }
            }
        }
    }
}

BytecodeEmitter::VarResolution BytecodeEmitter::ResolveVariable(std::string_view name) {
    if (name == "this") {
        return VarResolution{VarResolution::ThisVar, 0, 0, 0, nullptr};
    }
    if (name == "undefined") {
        return VarResolution{VarResolution::GlobalValue, 0, 0, 0, nullptr, TaggedValue::Undefined()};
    }
    auto cs_it = context_slots_.find(std::string(name));
    if (cs_it != context_slots_.end()) {
        return VarResolution{VarResolution::ContextSlot, 0, 0, cs_it->second, nullptr};
    }
    auto reg_it = scope_.find(std::string(name));
    if (reg_it != scope_.end()) {
        return VarResolution{VarResolution::LocalReg, reg_it->second, 0, 0, nullptr};
    }
    uint8_t depth = context_slots_.empty() ? 0 : 1;
    BytecodeEmitter* parent = parent_emitter_;
    while (parent != nullptr) {
        auto p_cs = parent->context_slots_.find(std::string(name));
        if (p_cs != parent->context_slots_.end()) {
            return VarResolution{VarResolution::ContextSlot, 0, depth, p_cs->second, nullptr};
        }
        if (!parent->context_slots_.empty()) {
            depth++;
        }
        parent = parent->parent_emitter_;
    }
    JSFunction* fn = LookupFunction(name);
    if (fn != nullptr) {
        return VarResolution{VarResolution::GlobalOrFunction, 0, 0, 0, fn, TaggedValue::Undefined()};
    }
    if (Realm::Current()) {
        TaggedValue g_val = Realm::Current()->GetGlobalProperty(name);
        if (!g_val.IsUndefined()) {
            return VarResolution{VarResolution::GlobalValue, 0, 0, 0, nullptr, g_val};
        }
    }
    return VarResolution{VarResolution::NotFound, 0, 0, 0, nullptr, TaggedValue::Undefined()};
}

std::unique_ptr<BytecodeArray> BytecodeEmitter::Emit(ASTNode* root) {
    bytecode_ = std::make_unique<BytecodeArray>();
    scope_.clear();
    context_slots_.clear();
    register_in_use_.clear();
    max_registers_ = 0;
    next_feedback_slot_ = 0;

    ScanAndSetupContextSlots({}, root);

    if (!context_slots_.empty()) {
        bytecode_->EmitOpcode(Opcode::CreateFunctionContext);
        bytecode_->EmitU16(static_cast<uint16_t>(context_slots_.size()));
    }

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
    context_slots_.clear();
    register_in_use_.clear();
    max_registers_ = 0;
    next_feedback_slot_ = 0;

    uint32_t param_count = static_cast<uint32_t>(func->Params().size());
    bytecode_->SetParameterCount(param_count);

    ScanAndSetupContextSlots(func->Params(), func->Body());

    // Bind parameters to registers r0..r(param_count-1)
    for (uint32_t i = 0; i < param_count; ++i) {
        uint8_t reg = AllocateRegister();
        scope_[func->Params()[i]] = reg;
    }

    if (func->HasRest()) {
        uint8_t rest_reg = AllocateRegister();
        scope_[func->RestParam()] = rest_reg;
        bytecode_->EmitOpcode(Opcode::CreateRestArray);
        bytecode_->EmitU8(rest_reg);
        bytecode_->EmitU16(static_cast<uint16_t>(param_count));
    }

    if (!context_slots_.empty()) {
        bytecode_->EmitOpcode(Opcode::CreateFunctionContext);
        bytecode_->EmitU16(static_cast<uint16_t>(context_slots_.size()));

        for (uint32_t i = 0; i < param_count; ++i) {
            const auto& pname = func->Params()[i];
            auto cs_it = context_slots_.find(pname);
            if (cs_it != context_slots_.end()) {
                bytecode_->EmitOpcode(Opcode::Ldar);
                bytecode_->EmitU8(scope_[pname]);
                bytecode_->EmitOpcode(Opcode::StaContextSlot);
                bytecode_->EmitU8(0);
                bytecode_->EmitU16(cs_it->second);
            }
        }
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
    auto res = ResolveVariable(node->Name());
    if (res.kind == VarResolution::ThisVar) {
        bytecode_->EmitOpcode(Opcode::LdaThis);
        return;
    }
    if (res.kind == VarResolution::LocalReg) {
        bytecode_->EmitOpcode(Opcode::Ldar);
        bytecode_->EmitU8(res.reg);
        return;
    }
    if (res.kind == VarResolution::ContextSlot) {
        bytecode_->EmitOpcode(Opcode::LdaContextSlot);
        bytecode_->EmitU8(res.depth);
        bytecode_->EmitU16(res.slot);
        return;
    }
    if (res.kind == VarResolution::GlobalOrFunction) {
        uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(res.fn));
        bytecode_->EmitOpcode(Opcode::LdaConstant);
        bytecode_->EmitU16(const_idx);
        return;
    }
    if (res.kind == VarResolution::GlobalValue) {
        if (res.global_val.IsUndefined()) {
            bytecode_->EmitOpcode(Opcode::LdaUndefined);
            return;
        }
        uint16_t const_idx = bytecode_->AddConstant(res.global_val);
        bytecode_->EmitOpcode(Opcode::LdaConstant);
        bytecode_->EmitU16(const_idx);
        return;
    }

    assert(false && "Undeclared identifier in scope");
}

void BytecodeEmitter::VisitUnary(UnaryExpr* node) {
    if (node->Op() == UnaryOp::Delete) {
        if (auto* mem = dynamic_cast<PropertyAccessExpr*>(node->GetExpr())) {
            mem->Object()->Accept(this);
            uint8_t obj_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(obj_reg);
            uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(new JSString(mem->Property())));
            bytecode_->EmitOpcode(Opcode::DeleteNamed);
            bytecode_->EmitU8(obj_reg);
            bytecode_->EmitU16(name_idx);
            FreeRegister(obj_reg);
            return;
        }
        if (auto* idx = dynamic_cast<IndexAccessExpr*>(node->GetExpr())) {
            idx->Object()->Accept(this);
            uint8_t obj_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(obj_reg);
            idx->Index()->Accept(this);
            uint8_t key_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(key_reg);
            bytecode_->EmitOpcode(Opcode::DeleteKeyed);
            bytecode_->EmitU8(obj_reg);
            bytecode_->EmitU8(key_reg);
            FreeRegister(key_reg);
            FreeRegister(obj_reg);
            return;
        }
        node->GetExpr()->Accept(this);
        bytecode_->EmitOpcode(Opcode::LdaTrue);
        return;
    }

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
        case UnaryOp::TypeOf:
            bytecode_->EmitOpcode(Opcode::TypeOf);
            break;
        case UnaryOp::Delete:
            break;
    }
}

void BytecodeEmitter::VisitAssign(AssignExpr* node) {
    node->Value()->Accept(this);
    auto res = ResolveVariable(node->Target());
    if (res.kind == VarResolution::LocalReg) {
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(res.reg);
        return;
    }
    if (res.kind == VarResolution::ContextSlot) {
        bytecode_->EmitOpcode(Opcode::StaContextSlot);
        bytecode_->EmitU8(res.depth);
        bytecode_->EmitU16(res.slot);
        return;
    }
    assert(false && "Assignment to undeclared variable");
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
    if (auto* prop = dynamic_cast<PropertyAccessExpr*>(node->Callee())) {
        // 1. Evaluate receiver object
        prop->Object()->Accept(this);
        uint8_t this_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(this_reg);

        // 2. Load property from this_reg into callee_reg
        auto* s = new JSString(prop->Property());
        uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(s));
        uint16_t slot = next_feedback_slot_++;
        bytecode_->EmitOpcode(Opcode::LdaNamed);
        bytecode_->EmitU8(this_reg);
        bytecode_->EmitU16(name_idx);
        bytecode_->EmitU16(slot);

        uint8_t callee_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(callee_reg);

        // 3. Evaluate arguments into contiguous registers
        uint8_t arg_count = static_cast<uint8_t>(node->Args().size());
        uint8_t args_start = AllocateRegisterRange(arg_count);
        for (uint8_t i = 0; i < arg_count; ++i) {
            node->Args()[i]->Accept(this);
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
        }

        // 4. Emit CallMethod
        uint16_t call_slot = next_feedback_slot_++;
        bytecode_->EmitOpcode(Opcode::CallMethod);
        bytecode_->EmitU8(callee_reg);
        bytecode_->EmitU8(this_reg);
        bytecode_->EmitU8(args_start);
        bytecode_->EmitU8(arg_count);
        bytecode_->EmitU16(call_slot);

        FreeRegisterRange(args_start, arg_count);
        FreeRegister(callee_reg);
        FreeRegister(this_reg);
        return;
    }

    if (auto* idx = dynamic_cast<IndexAccessExpr*>(node->Callee())) {
        // 1. Evaluate receiver object
        idx->Object()->Accept(this);
        uint8_t this_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(this_reg);

        // 2. Evaluate key
        idx->Index()->Accept(this);
        uint8_t key_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(key_reg);

        // 3. Load keyed property
        uint16_t slot = next_feedback_slot_++;
        bytecode_->EmitOpcode(Opcode::LdaKeyed);
        bytecode_->EmitU8(this_reg);
        bytecode_->EmitU8(key_reg);
        bytecode_->EmitU16(slot);

        uint8_t callee_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(callee_reg);

        // 4. Evaluate arguments
        uint8_t arg_count = static_cast<uint8_t>(node->Args().size());
        uint8_t args_start = AllocateRegisterRange(arg_count);
        for (uint8_t i = 0; i < arg_count; ++i) {
            node->Args()[i]->Accept(this);
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
        }

        // 5. Emit CallMethod
        uint16_t call_slot = next_feedback_slot_++;
        bytecode_->EmitOpcode(Opcode::CallMethod);
        bytecode_->EmitU8(callee_reg);
        bytecode_->EmitU8(this_reg);
        bytecode_->EmitU8(args_start);
        bytecode_->EmitU8(arg_count);
        bytecode_->EmitU16(call_slot);

        FreeRegisterRange(args_start, arg_count);
        FreeRegister(callee_reg);
        FreeRegister(key_reg);
        FreeRegister(this_reg);
        return;
    }

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

    for (const auto& prop : node->Properties()) {
        if (prop.computed_key != nullptr) {
            // Evaluates computed property key expression
            prop.computed_key->Accept(this);
            uint8_t key_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(key_reg);

            // Evaluates value expression
            prop.value->Accept(this);
            uint16_t slot = next_feedback_slot_++;
            bytecode_->EmitOpcode(Opcode::StaKeyed);
            bytecode_->EmitU8(obj_reg);
            bytecode_->EmitU8(key_reg);
            bytecode_->EmitU16(slot);
            FreeRegister(key_reg);
        } else if (prop.key.empty() || dynamic_cast<SpreadExpr*>(prop.value.get()) != nullptr) {
            Expr* target_expr = dynamic_cast<SpreadExpr*>(prop.value.get()) ? dynamic_cast<SpreadExpr*>(prop.value.get())->GetExpr() : prop.value.get();
            target_expr->Accept(this);
            uint8_t src_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(src_reg);
            bytecode_->EmitOpcode(Opcode::ObjectSpread);
            bytecode_->EmitU8(obj_reg);
            bytecode_->EmitU8(src_reg);
            FreeRegister(src_reg);
        } else {
            prop.value->Accept(this);
            auto* s = new JSString(prop.key);
            uint16_t name_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(s));
            uint16_t slot = next_feedback_slot_++;

            bytecode_->EmitOpcode(Opcode::StaNamed);
            bytecode_->EmitU8(obj_reg);
            bytecode_->EmitU16(name_idx);
            bytecode_->EmitU16(slot);
        }
    }

    bytecode_->EmitOpcode(Opcode::Ldar);
    bytecode_->EmitU8(obj_reg);
    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitArrayLiteral(ArrayLiteralExpr* node) {
    bool has_spread = false;
    for (const auto& el : node->Elements()) {
        if (dynamic_cast<SpreadExpr*>(el.get()) != nullptr) {
            has_spread = true;
            break;
        }
    }

    if (!has_spread) {
        uint8_t count = static_cast<uint8_t>(node->Elements().size());
        uint8_t args_start = AllocateRegisterRange(count);

        for (uint8_t i = 0; i < count; ++i) {
            node->Elements()[i]->Accept(this);
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
        }

        bytecode_->EmitOpcode(Opcode::CreateArray);
        bytecode_->EmitU8(args_start);
        bytecode_->EmitU16(count);

        FreeRegisterRange(args_start, count);
        return;
    }

    uint8_t arr_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::CreateArray);
    bytecode_->EmitU8(0);
    bytecode_->EmitU16(0);
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(arr_reg);

    for (const auto& el : node->Elements()) {
        if (auto* sp = dynamic_cast<SpreadExpr*>(el.get())) {
            sp->GetExpr()->Accept(this);
            uint8_t src_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(src_reg);
            bytecode_->EmitOpcode(Opcode::ArraySpread);
            bytecode_->EmitU8(arr_reg);
            bytecode_->EmitU8(src_reg);
            FreeRegister(src_reg);
        } else {
            el->Accept(this);
            uint8_t val_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(val_reg);
            bytecode_->EmitOpcode(Opcode::ArrayPush);
            bytecode_->EmitU8(arr_reg);
            bytecode_->EmitU8(val_reg);
            FreeRegister(val_reg);
        }
    }

    bytecode_->EmitOpcode(Opcode::Ldar);
    bytecode_->EmitU8(arr_reg);
    FreeRegister(arr_reg);
}

void BytecodeEmitter::VisitIndexAccess(IndexAccessExpr* node) {
    node->Object()->Accept(this);
    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    node->Index()->Accept(this);
    uint8_t key_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(key_reg);

    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::LdaKeyed);
    bytecode_->EmitU8(obj_reg);
    bytecode_->EmitU8(key_reg);
    bytecode_->EmitU16(slot);

    FreeRegister(key_reg);
    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitIndexAssign(IndexAssignExpr* node) {
    node->Object()->Accept(this);
    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    node->Index()->Accept(this);
    uint8_t key_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(key_reg);

    node->Value()->Accept(this);

    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::StaKeyed);
    bytecode_->EmitU8(obj_reg);
    bytecode_->EmitU8(key_reg);
    bytecode_->EmitU16(slot);

    FreeRegister(key_reg);
    FreeRegister(obj_reg);
}

void BytecodeEmitter::VisitConditional(ConditionalExpr* node) {
    node->Condition()->Accept(this);
    Label else_label;
    Label end_label;
    EmitJump(Opcode::JumpIfFalse, else_label);
    node->ThenBranch()->Accept(this);
    EmitJump(Opcode::Jump, end_label);
    Bind(else_label);
    node->ElseBranch()->Accept(this);
    Bind(end_label);
}

void BytecodeEmitter::VisitBinary(BinaryExpr* node) {
    if (node->Op() == BinaryOp::LogicalAnd) {
        node->Lhs()->Accept(this);
        Label short_circuit;
        EmitJump(Opcode::JumpIfFalse, short_circuit);
        node->Rhs()->Accept(this);
        Bind(short_circuit);
        return;
    }
    if (node->Op() == BinaryOp::LogicalOr) {
        node->Lhs()->Accept(this);
        Label short_circuit;
        EmitJump(Opcode::JumpIfTrue, short_circuit);
        node->Rhs()->Accept(this);
        Bind(short_circuit);
        return;
    }
    if (node->Op() == BinaryOp::NullishCoalescing) {
        node->Lhs()->Accept(this);
        Label short_circuit;
        EmitJump(Opcode::JumpIfNotNullish, short_circuit);
        node->Rhs()->Accept(this);
        Bind(short_circuit);
        return;
    }

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
        case BinaryOp::InstanceOf:
            bytecode_->EmitOpcode(Opcode::TestInstanceOf);
            bytecode_->EmitU8(rhs_reg);
            break;
        case BinaryOp::In:
            bytecode_->EmitOpcode(Opcode::TestIn);
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
    auto cs_it = context_slots_.find(node->Name());
    if (cs_it != context_slots_.end()) {
        if (node->Initializer()) {
            node->Initializer()->Accept(this);
        } else {
            bytecode_->EmitOpcode(Opcode::LdaUndefined);
        }
        bytecode_->EmitOpcode(Opcode::StaContextSlot);
        bytecode_->EmitU8(0);
        bytecode_->EmitU16(cs_it->second);
    } else {
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
    fn_obj->SetGenerator(node->IsGenerator());
    fn_obj->SetAsync(node->IsAsync());
    functions_[node->Name()] = fn_obj;

    // 2. Compile child routine
    BytecodeEmitter sub_emitter;
    sub_emitter.parent_emitter_ = this;
    auto fn_bc = sub_emitter.EmitFunction(node);

    // 3. Attach compiled bytecode
    std::shared_ptr<BytecodeArray> shared_bc = std::move(fn_bc);
    fn_obj->SetBytecode(shared_bc);
    for (auto& routine : sub_emitter.compiled_routines_) {
        compiled_routines_.push_back(std::move(routine));
    }

    // 4. Register in current scope and emit constant load / closure creation
    uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(fn_obj));
    auto cs_it = context_slots_.find(node->Name());
    if (cs_it != context_slots_.end()) {
        bytecode_->EmitOpcode(Opcode::CreateClosure);
        bytecode_->EmitU16(const_idx);
        bytecode_->EmitOpcode(Opcode::StaContextSlot);
        bytecode_->EmitU8(0);
        bytecode_->EmitU16(cs_it->second);
    } else {
        uint8_t fn_reg = AllocateRegister();
        scope_[node->Name()] = fn_reg;

        if (parent_emitter_ != nullptr || !context_slots_.empty()) {
            bytecode_->EmitOpcode(Opcode::CreateClosure);
            bytecode_->EmitU16(const_idx);
        } else {
            bytecode_->EmitOpcode(Opcode::LdaConstant);
            bytecode_->EmitU16(const_idx);
        }
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(fn_reg);
    }
}

void BytecodeEmitter::VisitNew(NewExpr* node) {
    // 1. Evaluate callee constructor
    node->Callee()->Accept(this);
    uint8_t ctor_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(ctor_reg);

    // 2. Evaluate arguments into contiguous registers
    uint8_t arg_count = static_cast<uint8_t>(node->Args().size());
    uint8_t args_start = AllocateRegisterRange(arg_count);
    for (uint8_t i = 0; i < arg_count; ++i) {
        node->Args()[i]->Accept(this);
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
    }

    // 3. Emit Construct
    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::Construct);
    bytecode_->EmitU8(ctor_reg);
    bytecode_->EmitU8(args_start);
    bytecode_->EmitU8(arg_count);
    bytecode_->EmitU16(slot);

    FreeRegisterRange(args_start, arg_count);
    FreeRegister(ctor_reg);
}

void BytecodeEmitter::VisitFunctionExpr(FunctionExpr* node) {
    auto* fn_obj = new JSFunction(node->Name(), nullptr);
    fn_obj->SetGenerator(node->IsGenerator());
    fn_obj->SetAsync(node->IsAsync());

    BytecodeEmitter sub_emitter;
    sub_emitter.parent_emitter_ = this;
    sub_emitter.bytecode_ = std::make_unique<BytecodeArray>();
    uint32_t param_count = static_cast<uint32_t>(node->Params().size());
    sub_emitter.bytecode_->SetParameterCount(param_count);

    sub_emitter.ScanAndSetupContextSlots(node->Params(), node->Body());

    for (uint32_t i = 0; i < param_count; ++i) {
        uint8_t reg = sub_emitter.AllocateRegister();
        sub_emitter.scope_[node->Params()[i]] = reg;
    }

    if (node->HasRest()) {
        uint8_t rest_reg = sub_emitter.AllocateRegister();
        sub_emitter.scope_[node->RestParam()] = rest_reg;
        sub_emitter.bytecode_->EmitOpcode(Opcode::CreateRestArray);
        sub_emitter.bytecode_->EmitU8(rest_reg);
        sub_emitter.bytecode_->EmitU16(static_cast<uint16_t>(param_count));
    }

    if (!sub_emitter.context_slots_.empty()) {
        sub_emitter.bytecode_->EmitOpcode(Opcode::CreateFunctionContext);
        sub_emitter.bytecode_->EmitU16(static_cast<uint16_t>(sub_emitter.context_slots_.size()));

        for (uint32_t i = 0; i < param_count; ++i) {
            const auto& pname = node->Params()[i];
            auto cs_it = sub_emitter.context_slots_.find(pname);
            if (cs_it != sub_emitter.context_slots_.end()) {
                sub_emitter.bytecode_->EmitOpcode(Opcode::Ldar);
                sub_emitter.bytecode_->EmitU8(sub_emitter.scope_[pname]);
                sub_emitter.bytecode_->EmitOpcode(Opcode::StaContextSlot);
                sub_emitter.bytecode_->EmitU8(0);
                sub_emitter.bytecode_->EmitU16(cs_it->second);
            }
        }
    }

    node->Body()->Accept(&sub_emitter);

    if (sub_emitter.bytecode_->Code().empty() || 
        (sub_emitter.bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Return) && 
         sub_emitter.bytecode_->Code().back() != static_cast<uint8_t>(Opcode::Halt))) {
        sub_emitter.bytecode_->EmitOpcode(Opcode::LdaUndefined);
        sub_emitter.bytecode_->EmitOpcode(Opcode::Return);
    }
    sub_emitter.bytecode_->SetRegisterCount(sub_emitter.max_registers_);

    std::shared_ptr<BytecodeArray> shared_bc = std::move(sub_emitter.bytecode_);
    fn_obj->SetBytecode(shared_bc);
    for (auto& routine : sub_emitter.compiled_routines_) {
        compiled_routines_.push_back(std::move(routine));
    }

    uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(fn_obj));
    bytecode_->EmitOpcode(Opcode::CreateClosure);
    bytecode_->EmitU16(const_idx);
}

void BytecodeEmitter::VisitClassDecl(ClassDeclStmt* node) {
    FunctionDeclStmt* ctor_stmt = nullptr;
    for (const auto& m : node->Methods()) {
        if (m->Name() == "constructor") {
            ctor_stmt = m.get();
            break;
        }
    }

    std::unique_ptr<FunctionDeclStmt> default_ctor;
    if (!ctor_stmt) {
        default_ctor = std::make_unique<FunctionDeclStmt>(node->Name(), std::vector<std::string>{}, std::make_unique<BlockStmt>());
        ctor_stmt = default_ctor.get();
    }

    auto* ctor_fn = new JSFunction(node->Name(), nullptr);
    functions_[node->Name()] = ctor_fn;

    BytecodeEmitter sub_emitter;
    sub_emitter.parent_emitter_ = this;
    auto ctor_bc = sub_emitter.EmitFunction(ctor_stmt);
    std::shared_ptr<BytecodeArray> shared_ctor_bc = std::move(ctor_bc);
    ctor_fn->SetBytecode(shared_ctor_bc);
    for (auto& routine : sub_emitter.compiled_routines_) {
        compiled_routines_.push_back(std::move(routine));
    }

    auto* proto_shape = new Shape(0, nullptr, "", TaggedValue::Null());
    auto* proto_obj = new JSObject(proto_shape);
    if (!node->SuperName().empty()) {
        JSFunction* super_fn = LookupFunction(node->SuperName());
        if (super_fn && super_fn->GetPrototype()) {
            proto_obj->SetPrototype(super_fn->GetPrototype());
        }
    }
    ctor_fn->SetPrototype(proto_obj);

    uint32_t dummy_id = 1;
    for (const auto& m : node->Methods()) {
        if (m->Name() == "constructor") continue;
        auto* method_fn = new JSFunction(m->Name(), nullptr);
        BytecodeEmitter m_emitter;
        m_emitter.parent_emitter_ = this;
        auto m_bc = m_emitter.EmitFunction(m.get());
        std::shared_ptr<BytecodeArray> shared_m_bc = std::move(m_bc);
        method_fn->SetBytecode(shared_m_bc);
        for (auto& routine : m_emitter.compiled_routines_) {
            compiled_routines_.push_back(std::move(routine));
        }

        proto_obj->SetNamedProperty(m->Name(), TaggedValue::FromHeapObject(method_fn), dummy_id);
    }

    uint16_t const_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(ctor_fn));
    uint8_t fn_reg = AllocateRegister();
    scope_[node->Name()] = fn_reg;

    bytecode_->EmitOpcode(Opcode::LdaConstant);
    bytecode_->EmitU16(const_idx);
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(fn_reg);
}

void BytecodeEmitter::VisitThrow(ThrowStmt* node) {
    node->GetExpr()->Accept(this);
    bytecode_->EmitOpcode(Opcode::Throw);
}

void BytecodeEmitter::VisitTryCatch(TryCatchStmt* node) {
    Label catch_label;
    Label end_label;

    bytecode_->EmitOpcode(Opcode::PushExceptionHandler);
    size_t handler_patch_offset = bytecode_->CurrentOffset();
    catch_label.forward_jumps.push_back(handler_patch_offset);
    bytecode_->EmitI32(0);

    node->TryBlock()->Accept(this);
    bytecode_->EmitOpcode(Opcode::PopExceptionHandler);

    if (node->FinallyBlock()) {
        node->FinallyBlock()->Accept(this);
    }
    EmitJump(Opcode::Jump, end_label);

    Bind(catch_label);
    if (node->CatchBlock()) {
        if (!node->CatchParam().empty()) {
            uint8_t err_reg = AllocateRegister();
            scope_[node->CatchParam()] = err_reg;
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(err_reg);
        }
        node->CatchBlock()->Accept(this);
    }
    if (node->FinallyBlock()) {
        node->FinallyBlock()->Accept(this);
    }

    Bind(end_label);
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

void BytecodeEmitter::VisitTemplateLiteral(TemplateLiteralExpr* node) {
    assert(node->Quasis().size() == node->Expressions().size() + 1);

    auto* s0 = new JSString(node->Quasis()[0]);
    uint16_t c0 = bytecode_->AddConstant(TaggedValue::FromHeapObject(s0));
    bytecode_->EmitOpcode(Opcode::LdaConstant);
    bytecode_->EmitU16(c0);

    for (size_t i = 0; i < node->Expressions().size(); ++i) {
        uint8_t lhs_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(lhs_reg);

        node->Expressions()[i]->Accept(this);
        uint8_t rhs_reg = AllocateRegister();
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(rhs_reg);

        bytecode_->EmitOpcode(Opcode::Ldar);
        bytecode_->EmitU8(lhs_reg);
        uint16_t add_slot1 = next_feedback_slot_++;
        bytecode_->EmitOpcode(Opcode::Add);
        bytecode_->EmitU8(rhs_reg);
        bytecode_->EmitU16(add_slot1);

        FreeRegister(rhs_reg);
        FreeRegister(lhs_reg);

        if (!node->Quasis()[i + 1].empty()) {
            uint8_t str_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(str_reg);

            auto* q_str = new JSString(node->Quasis()[i + 1]);
            uint16_t q_idx = bytecode_->AddConstant(TaggedValue::FromHeapObject(q_str));
            uint8_t q_reg = AllocateRegister();
            bytecode_->EmitOpcode(Opcode::LdaConstant);
            bytecode_->EmitU16(q_idx);
            bytecode_->EmitOpcode(Opcode::Star);
            bytecode_->EmitU8(q_reg);

            bytecode_->EmitOpcode(Opcode::Ldar);
            bytecode_->EmitU8(str_reg);
            uint16_t add_slot2 = next_feedback_slot_++;
            bytecode_->EmitOpcode(Opcode::Add);
            bytecode_->EmitU8(q_reg);
            bytecode_->EmitU16(add_slot2);

            FreeRegister(q_reg);
            FreeRegister(str_reg);
        }
    }
}

void BytecodeEmitter::VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* node) {
    node->Object()->Accept(this);
    Label is_nullish;
    Label done;
    EmitJump(Opcode::JumpIfNullish, is_nullish);

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

    EmitJump(Opcode::Jump, done);

    Bind(is_nullish);
    bytecode_->EmitOpcode(Opcode::LdaUndefined);

    Bind(done);
}

void BytecodeEmitter::VisitOptionalIndexAccess(OptionalIndexAccessExpr* node) {
    node->Object()->Accept(this);
    Label is_nullish;
    Label done;
    EmitJump(Opcode::JumpIfNullish, is_nullish);

    uint8_t obj_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(obj_reg);

    node->Index()->Accept(this);
    uint8_t idx_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(idx_reg);

    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::LdaKeyed);
    bytecode_->EmitU8(obj_reg);
    bytecode_->EmitU8(idx_reg);
    bytecode_->EmitU16(slot);

    FreeRegister(idx_reg);
    FreeRegister(obj_reg);
    EmitJump(Opcode::Jump, done);

    Bind(is_nullish);
    bytecode_->EmitOpcode(Opcode::LdaUndefined);

    Bind(done);
}

void BytecodeEmitter::VisitOptionalCall(OptionalCallExpr* node) {
    node->Callee()->Accept(this);
    Label is_nullish;
    Label done;
    EmitJump(Opcode::JumpIfNullish, is_nullish);

    uint8_t callee_reg = AllocateRegister();
    bytecode_->EmitOpcode(Opcode::Star);
    bytecode_->EmitU8(callee_reg);

    uint8_t arg_count = static_cast<uint8_t>(node->Args().size());
    uint8_t args_start = AllocateRegisterRange(arg_count);
    for (uint8_t i = 0; i < arg_count; ++i) {
        node->Args()[i]->Accept(this);
        bytecode_->EmitOpcode(Opcode::Star);
        bytecode_->EmitU8(static_cast<uint8_t>(args_start + i));
    }

    uint16_t slot = next_feedback_slot_++;
    bytecode_->EmitOpcode(Opcode::Call);
    bytecode_->EmitU8(callee_reg);
    bytecode_->EmitU8(args_start);
    bytecode_->EmitU8(arg_count);
    bytecode_->EmitU16(slot);

    FreeRegisterRange(args_start, arg_count);
    FreeRegister(callee_reg);
    EmitJump(Opcode::Jump, done);

    Bind(is_nullish);
    bytecode_->EmitOpcode(Opcode::LdaUndefined);

    Bind(done);
}

void BytecodeEmitter::VisitSpread(SpreadExpr* node) {
    node->GetExpr()->Accept(this);
}

void BytecodeEmitter::VisitYield(YieldExpr* node) {
    if (node->GetExpr()) {
        node->GetExpr()->Accept(this);
    } else {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
    }
    bytecode_->EmitOpcode(Opcode::Yield);
}

void BytecodeEmitter::VisitAwait(AwaitExpr* node) {
    if (node->GetExpr()) {
        node->GetExpr()->Accept(this);
    } else {
        bytecode_->EmitOpcode(Opcode::LdaUndefined);
    }
    bytecode_->EmitOpcode(Opcode::Await);
}

} // namespace aether
