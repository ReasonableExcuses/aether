#include "aether/sea_of_nodes.h"
#include <iomanip>

namespace aether {

const char* NodeOpToString(NodeOp op) {
    switch (op) {
        case NodeOp::Start: return "Start";
        case NodeOp::End: return "End";
        case NodeOp::If: return "If";
        case NodeOp::IfTrue: return "IfTrue";
        case NodeOp::IfFalse: return "IfFalse";
        case NodeOp::Merge: return "Merge";
        case NodeOp::Loop: return "Loop";
        case NodeOp::Return: return "Return";
        case NodeOp::StartEffect: return "StartEffect";
        case NodeOp::EffectPhi: return "EffectPhi";
        case NodeOp::CheckSmi: return "CheckSmi";
        case NodeOp::CheckShape: return "CheckShape";
        case NodeOp::LoadField: return "LoadField";
        case NodeOp::StoreField: return "StoreField";
        case NodeOp::Parameter: return "Parameter";
        case NodeOp::ConstantInt32: return "ConstantInt32";
        case NodeOp::ConstantFloat64: return "ConstantFloat64";
        case NodeOp::ConstantBool: return "ConstantBool";
        case NodeOp::ConstantUndefined: return "ConstantUndefined";
        case NodeOp::ConstantNull: return "ConstantNull";
        case NodeOp::Phi: return "Phi";
        case NodeOp::Int32Add: return "Int32Add";
        case NodeOp::Int32Sub: return "Int32Sub";
        case NodeOp::Int32Mul: return "Int32Mul";
        case NodeOp::Int32Div: return "Int32Div";
        case NodeOp::Int32Mod: return "Int32Mod";
        case NodeOp::Int32And: return "Int32And";
        case NodeOp::Int32Or: return "Int32Or";
        case NodeOp::Int32Xor: return "Int32Xor";
        case NodeOp::Int32CmpLess: return "Int32CmpLess";
        case NodeOp::Int32CmpLessEqual: return "Int32CmpLessEqual";
        case NodeOp::Int32CmpGreater: return "Int32CmpGreater";
        case NodeOp::Int32CmpGreaterEqual: return "Int32CmpGreaterEqual";
        case NodeOp::Int32CmpEqual: return "Int32CmpEqual";
        case NodeOp::Int32CmpNotEqual: return "Int32CmpNotEqual";
        case NodeOp::Deoptimize: return "Deoptimize";
        default: return "UnknownOp";
    }
}

bool Node::IsControl() const noexcept {
    switch (op_) {
        case NodeOp::Start:
        case NodeOp::End:
        case NodeOp::If:
        case NodeOp::IfTrue:
        case NodeOp::IfFalse:
        case NodeOp::Merge:
        case NodeOp::Loop:
        case NodeOp::Return:
            return true;
        default:
            return false;
    }
}

bool Node::IsEffect() const noexcept {
    switch (op_) {
        case NodeOp::StartEffect:
        case NodeOp::EffectPhi:
        case NodeOp::CheckSmi:
        case NodeOp::CheckShape:
        case NodeOp::LoadField:
        case NodeOp::StoreField:
            return true;
        default:
            return false;
    }
}

bool Node::IsValue() const noexcept {
    return !IsControl() && !IsEffect();
}

uint64_t Node::ComputeHash() const {
    uint64_t h = static_cast<uint64_t>(op_);
    h ^= (static_cast<uint64_t>(int_val_) * 31);
    for (Node* in : inputs_) {
        uint64_t in_id = in ? in->Id() : 0;
        h = (h * 16777619u) ^ in_id;
    }
    return h;
}

Graph::Graph() {
    start_node_ = NewNode(NodeOp::Start);
    start_effect_ = NewNode(NodeOp::StartEffect);
    start_effect_->AddInput(start_node_);
}

Node* Graph::NewNode(NodeOp op) {
    uint32_t id = static_cast<uint32_t>(all_nodes_.size());
    auto node = std::make_unique<Node>(id, op);
    Node* ptr = node.get();
    all_nodes_.push_back(std::move(node));
    return ptr;
}

Node* Graph::NewConstantInt32(int32_t val) {
    Node* n = NewNode(NodeOp::ConstantInt32);
    n->SetIntValue(val);
    return GVN(n);
}

Node* Graph::NewConstantBool(bool val) {
    Node* n = NewNode(NodeOp::ConstantBool);
    n->SetIntValue(val ? 1 : 0);
    return GVN(n);
}

Node* Graph::NewParameter(uint32_t index) {
    Node* n = NewNode(NodeOp::Parameter);
    n->SetParameterIndex(index);
    n->AddInput(start_node_);
    return n;
}

Node* Graph::GVN(Node* node) {
    // Only value nodes with pure semantics can be hash-consed
    if (!node->IsValue() || node->Op() == NodeOp::Parameter || node->Op() == NodeOp::Phi) {
        return node;
    }
    uint64_t hash = node->ComputeHash();
    auto it = gvn_map_.find(hash);
    if (it != gvn_map_.end()) {
        Node* existing = it->second;
        if (existing->Op() == node->Op() &&
            existing->IntValue() == node->IntValue() &&
            existing->Inputs().size() == node->Inputs().size()) {
            bool matches = true;
            for (size_t i = 0; i < node->Inputs().size(); ++i) {
                if (existing->Input(i) != node->Input(i)) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                node->MarkDead();
                return existing;
            }
        }
    }
    gvn_map_[hash] = node;
    return node;
}

void Graph::Dump(std::ostream& os) const {
    os << "=== Sea-of-Nodes Graph (Total Nodes: " << all_nodes_.size() << ") ===\n";
    for (const auto& node : all_nodes_) {
        if (node->IsDead()) continue;
        os << "  Node #" << std::setw(3) << node->Id() << ": "
           << std::setw(18) << std::left << NodeOpToString(node->Op());

        if (node->Op() == NodeOp::ConstantInt32) {
            os << " [imm=" << node->IntValue() << "]";
        } else if (node->Op() == NodeOp::ConstantBool) {
            os << " [val=" << (node->IntValue() ? "true" : "false") << "]";
        } else if (node->Op() == NodeOp::Parameter) {
            os << " [param_idx=" << node->ParameterIndex() << "]";
        }

        if (!node->Inputs().empty()) {
            os << " Inputs: (";
            for (size_t i = 0; i < node->Inputs().size(); ++i) {
                if (i > 0) os << ", ";
                Node* in = node->Input(i);
                if (in) os << "#" << in->Id();
                else os << "null";
            }
            os << ")";
        }
        os << "\n";
    }
    os << "=========================================================\n";
}

std::vector<Node*> Graph::Schedule() const {
    std::vector<Node*> schedule;
    std::unordered_set<Node*> visited;
    std::unordered_set<Node*> in_stack;

    auto dfs = [&](auto& self, Node* node) -> void {
        if (!node || node->IsDead()) return;
        if (visited.contains(node)) return;
        if (in_stack.contains(node)) {
            // Cycle backedge (Loop or Phi backedge) - break cycle
            return;
        }

        in_stack.insert(node);

        for (size_t i = 0; i < node->Inputs().size(); ++i) {
            Node* in = node->Input(i);
            // Ignore backedge inputs for Phi (input 2) and Loop (input 1) during forward DAG scheduling
            if (node->Op() == NodeOp::Phi && i == 2) {
                continue;
            }
            if (node->Op() == NodeOp::Loop && i == 1) {
                continue;
            }
            self(self, in);
        }

        in_stack.erase(node);
        visited.insert(node);
        schedule.push_back(node);
    };

    if (return_node_) {
        dfs(dfs, return_node_);
    }

    return schedule;
}

SeaOfNodesBuilder::SeaOfNodesBuilder(const BytecodeArray& bytecode, TypeFeedbackVector* feedback)
    : bytecode_(bytecode), feedback_(feedback) {
    graph_ = std::make_unique<Graph>();
}

std::unique_ptr<Graph> SeaOfNodesBuilder::Build() {
    cur_control_ = graph_->Start();
    cur_effect_ = graph_->StartEffect();

    registers_.resize(bytecode_.RegisterCount(), nullptr);

    // Initialize parameters
    for (uint32_t i = 0; i < bytecode_.ParameterCount() && i < registers_.size(); ++i) {
        registers_[i] = graph_->NewParameter(i);
    }
    // Initialize locals to Smi(0) or Undefined
    for (size_t i = bytecode_.ParameterCount(); i < registers_.size(); ++i) {
        registers_[i] = graph_->NewConstantInt32(0);
    }

    acc_ = graph_->NewConstantInt32(0);

    const auto& code = bytecode_.Code();
    size_t pc = 0;

    // Track loop headers and block targets
    std::unordered_map<size_t, Node*> pc_to_loop_node;
    std::unordered_map<size_t, std::vector<Node*>> loop_phi_registers;
    std::unordered_map<size_t, Node*> loop_phi_acc;
    std::unordered_map<size_t, Node*> loop_exit_control;

    // Pre-pass: detect loop headers (targets of negative jumps)
    std::unordered_set<size_t> loop_headers;
    size_t scan_pc = 0;
    while (scan_pc < code.size()) {
        Opcode scan_op = static_cast<Opcode>(code[scan_pc++]);
        switch (scan_op) {
            case Opcode::LdaSmi: scan_pc += 4; break;
            case Opcode::LdaConstant: scan_pc += 2; break;
            case Opcode::Star: scan_pc += 1; break;
            case Opcode::Ldar: scan_pc += 1; break;
            case Opcode::Mov: scan_pc += 2; break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod: scan_pc += 3; break;
            case Opcode::BitwiseAnd:
            case Opcode::BitwiseOr:
            case Opcode::BitwiseXor:
            case Opcode::TestLessThan:
            case Opcode::TestLessThanOrEqual:
            case Opcode::TestGreaterThan:
            case Opcode::TestGreaterThanOrEqual:
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual: scan_pc += 1; break;
            case Opcode::Jump:
            case Opcode::JumpIfTrue:
            case Opcode::JumpIfFalse: {
                int32_t offset = code[scan_pc] | (code[scan_pc + 1] << 8) | (code[scan_pc + 2] << 16) | (code[scan_pc + 3] << 24);
                scan_pc += 4;
                if (scan_op == Opcode::Jump && offset < 0) {
                    size_t target_pc = scan_pc + offset;
                    loop_headers.insert(target_pc);
                }
                break;
            }
            default: break;
        }
    }

    while (pc < code.size()) {
        size_t cur_pc = pc;
        if (loop_headers.contains(cur_pc) && !pc_to_loop_node.contains(cur_pc)) {
            Node* loop_node = graph_->NewNode(NodeOp::Loop);
            loop_node->AddInput(cur_control_);
            cur_control_ = loop_node;
            pc_to_loop_node[cur_pc] = loop_node;

            Node* phi_acc = graph_->NewNode(NodeOp::Phi);
            phi_acc->AddInput(loop_node);
            phi_acc->AddInput(acc_);
            acc_ = phi_acc;
            loop_phi_acc[cur_pc] = phi_acc;

            std::vector<Node*> phi_regs;
            for (size_t r = 0; r < registers_.size(); ++r) {
                Node* phi_r = graph_->NewNode(NodeOp::Phi);
                phi_r->AddInput(loop_node);
                phi_r->AddInput(registers_[r]);
                registers_[r] = phi_r;
                phi_regs.push_back(phi_r);
            }
            loop_phi_registers[cur_pc] = phi_regs;
        }

        Opcode op = static_cast<Opcode>(code[pc++]);

        switch (op) {
            case Opcode::LdaZero: {
                acc_ = graph_->NewConstantInt32(0);
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                acc_ = graph_->NewConstantInt32(imm);
                break;
            }
            case Opcode::LdaTrue: {
                acc_ = graph_->NewConstantBool(true);
                break;
            }
            case Opcode::LdaFalse: {
                acc_ = graph_->NewConstantBool(false);
                break;
            }
            case Opcode::LdaUndefined: {
                acc_ = graph_->NewNode(NodeOp::ConstantUndefined);
                break;
            }
            case Opcode::LdaNull: {
                acc_ = graph_->NewNode(NodeOp::ConstantNull);
                break;
            }
            case Opcode::LdaConstant: {
                uint16_t idx = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                TaggedValue c = bytecode_.Constants()[idx];
                if (c.IsSmi()) {
                    acc_ = graph_->NewConstantInt32(c.AsSmi());
                } else if (c.IsBoolean()) {
                    acc_ = graph_->NewConstantBool(c.AsBoolean());
                } else {
                    acc_ = graph_->NewConstantInt32(0);
                }
                break;
            }
            case Opcode::Star: {
                uint8_t reg = code[pc++];
                if (reg < registers_.size()) {
                    registers_[reg] = acc_;
                }
                break;
            }
            case Opcode::Ldar: {
                uint8_t reg = code[pc++];
                if (reg < registers_.size()) {
                    acc_ = registers_[reg];
                }
                break;
            }
            case Opcode::Mov: {
                uint8_t src = code[pc++];
                uint8_t dst = code[pc++];
                if (src < registers_.size() && dst < registers_.size()) {
                    registers_[dst] = registers_[src];
                }
                break;
            }
            case Opcode::Add: {
                uint8_t reg = code[pc++];
                uint16_t slot = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                (void)slot;

                // Speculative Type Specialization: Insert CheckSmi Guard
                Node* guard = graph_->NewNode(NodeOp::CheckSmi);
                guard->AddInput(cur_control_);
                guard->AddInput(cur_effect_);
                guard->AddInput(acc_);
                guard->AddInput(registers_[reg]);
                cur_effect_ = guard;

                Node* add_node = graph_->NewNode(NodeOp::Int32Add);
                add_node->AddInput(acc_);
                add_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(add_node);
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = code[pc++];
                uint16_t slot = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                (void)slot;

                Node* guard = graph_->NewNode(NodeOp::CheckSmi);
                guard->AddInput(cur_control_);
                guard->AddInput(cur_effect_);
                guard->AddInput(acc_);
                guard->AddInput(registers_[reg]);
                cur_effect_ = guard;

                Node* sub_node = graph_->NewNode(NodeOp::Int32Sub);
                sub_node->AddInput(acc_);
                sub_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(sub_node);
                break;
            }
            case Opcode::Mul: {
                uint8_t reg = code[pc++];
                uint16_t slot = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                (void)slot;

                Node* guard = graph_->NewNode(NodeOp::CheckSmi);
                guard->AddInput(cur_control_);
                guard->AddInput(cur_effect_);
                guard->AddInput(acc_);
                guard->AddInput(registers_[reg]);
                cur_effect_ = guard;

                Node* mul_node = graph_->NewNode(NodeOp::Int32Mul);
                mul_node->AddInput(acc_);
                mul_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(mul_node);
                break;
            }
            case Opcode::Div: {
                uint8_t reg = code[pc++];
                pc += 2;

                Node* guard = graph_->NewNode(NodeOp::CheckSmi);
                guard->AddInput(cur_control_);
                guard->AddInput(cur_effect_);
                guard->AddInput(acc_);
                guard->AddInput(registers_[reg]);
                cur_effect_ = guard;

                Node* div_node = graph_->NewNode(NodeOp::Int32Div);
                div_node->AddInput(acc_);
                div_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(div_node);
                break;
            }
            case Opcode::Mod: {
                uint8_t reg = code[pc++];
                pc += 2;

                Node* guard = graph_->NewNode(NodeOp::CheckSmi);
                guard->AddInput(cur_control_);
                guard->AddInput(cur_effect_);
                guard->AddInput(acc_);
                guard->AddInput(registers_[reg]);
                cur_effect_ = guard;

                Node* mod_node = graph_->NewNode(NodeOp::Int32Mod);
                mod_node->AddInput(acc_);
                mod_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(mod_node);
                break;
            }
            case Opcode::BitwiseAnd: {
                uint8_t reg = code[pc++];
                Node* and_node = graph_->NewNode(NodeOp::Int32And);
                and_node->AddInput(acc_);
                and_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(and_node);
                break;
            }
            case Opcode::BitwiseOr: {
                uint8_t reg = code[pc++];
                Node* or_node = graph_->NewNode(NodeOp::Int32Or);
                or_node->AddInput(acc_);
                or_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(or_node);
                break;
            }
            case Opcode::BitwiseXor: {
                uint8_t reg = code[pc++];
                Node* xor_node = graph_->NewNode(NodeOp::Int32Xor);
                xor_node->AddInput(acc_);
                xor_node->AddInput(registers_[reg]);
                acc_ = graph_->GVN(xor_node);
                break;
            }
            case Opcode::TestLessThan: {
                uint8_t reg = code[pc++];
                Node* cmp = graph_->NewNode(NodeOp::Int32CmpLess);
                cmp->AddInput(acc_);
                cmp->AddInput(registers_[reg]);
                acc_ = graph_->GVN(cmp);
                break;
            }
            case Opcode::TestLessThanOrEqual: {
                uint8_t reg = code[pc++];
                Node* cmp = graph_->NewNode(NodeOp::Int32CmpLessEqual);
                cmp->AddInput(acc_);
                cmp->AddInput(registers_[reg]);
                acc_ = graph_->GVN(cmp);
                break;
            }
            case Opcode::TestGreaterThan: {
                uint8_t reg = code[pc++];
                Node* cmp = graph_->NewNode(NodeOp::Int32CmpGreater);
                cmp->AddInput(acc_);
                cmp->AddInput(registers_[reg]);
                acc_ = graph_->GVN(cmp);
                break;
            }
            case Opcode::TestGreaterThanOrEqual: {
                uint8_t reg = code[pc++];
                Node* cmp = graph_->NewNode(NodeOp::Int32CmpGreaterEqual);
                cmp->AddInput(acc_);
                cmp->AddInput(registers_[reg]);
                acc_ = graph_->GVN(cmp);
                break;
            }
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual: {
                uint8_t reg = code[pc++];
                Node* cmp = graph_->NewNode(NodeOp::Int32CmpEqual);
                cmp->AddInput(acc_);
                cmp->AddInput(registers_[reg]);
                acc_ = graph_->GVN(cmp);
                break;
            }
            case Opcode::Jump: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                if (offset < 0) {
                    size_t target_pc = pc + offset;
                    auto it_loop = pc_to_loop_node.find(target_pc);
                    if (it_loop != pc_to_loop_node.end()) {
                        it_loop->second->AddInput(cur_control_);
                    }
                    auto it_acc = loop_phi_acc.find(target_pc);
                    if (it_acc != loop_phi_acc.end()) {
                        it_acc->second->AddInput(acc_);
                    }
                    auto it_regs = loop_phi_registers.find(target_pc);
                    if (it_regs != loop_phi_registers.end()) {
                        for (size_t r = 0; r < registers_.size() && r < it_regs->second.size(); ++r) {
                            it_regs->second[r]->AddInput(registers_[r]);
                        }
                    }
                    // After the loop, control continues from the loop exit edge (IfFalse)
                    size_t loop_exit_pc = pc;
                    auto it_exit = loop_exit_control.find(loop_exit_pc);
                    if (it_exit != loop_exit_control.end()) {
                        cur_control_ = it_exit->second;
                    }
                    if (it_regs != loop_phi_registers.end()) {
                        for (size_t r = 0; r < registers_.size() && r < it_regs->second.size(); ++r) {
                            registers_[r] = it_regs->second[r];
                        }
                    }
                    if (it_acc != loop_phi_acc.end()) {
                        acc_ = it_acc->second;
                    }
                }
                break;
            }
            case Opcode::JumpIfFalse: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                Node* if_node = graph_->NewNode(NodeOp::If);
                if_node->AddInput(cur_control_);
                if_node->AddInput(acc_);

                Node* if_true = graph_->NewNode(NodeOp::IfTrue);
                if_true->AddInput(if_node);

                Node* if_false = graph_->NewNode(NodeOp::IfFalse);
                if_false->AddInput(if_node);

                cur_control_ = if_true;
                size_t target_pc = pc + offset;
                loop_exit_control[target_pc] = if_false;
                break;
            }
            case Opcode::JumpIfTrue: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                (void)offset;
                Node* if_node = graph_->NewNode(NodeOp::If);
                if_node->AddInput(cur_control_);
                if_node->AddInput(acc_);

                Node* if_false = graph_->NewNode(NodeOp::IfFalse);
                if_false->AddInput(if_node);

                cur_control_ = if_false;
                break;
            }
            case Opcode::Return: {
                Node* ret = graph_->NewNode(NodeOp::Return);
                ret->AddInput(cur_control_);
                ret->AddInput(cur_effect_);
                ret->AddInput(acc_);
                graph_->SetReturnNode(ret);
                break;
            }
            default:
                break;
        }
    }

    if (!graph_->ReturnNode()) {
        Node* ret = graph_->NewNode(NodeOp::Return);
        ret->AddInput(cur_control_);
        ret->AddInput(cur_effect_);
        ret->AddInput(acc_);
        graph_->SetReturnNode(ret);
    }

    return std::move(graph_);
}

void SeaOfNodesOptimizer::Optimize(Graph& graph) {
    ConstantFolding(graph);
    DeadCodeElimination(graph);
    LoopInvariantCodeMotion(graph);
}

void SeaOfNodesOptimizer::ConstantFolding(Graph& graph) {
    for (const auto& node_ptr : graph.Nodes()) {
        Node* node = node_ptr.get();
        if (node->IsDead()) continue;

        switch (node->Op()) {
            case NodeOp::Int32Add: {
                Node* lhs = node->Input(0);
                Node* rhs = node->Input(1);
                if (lhs && rhs && lhs->Op() == NodeOp::ConstantInt32 && rhs->Op() == NodeOp::ConstantInt32) {
                    node->SetOp(NodeOp::ConstantInt32);
                    node->SetIntValue(lhs->IntValue() + rhs->IntValue());
                    node->ClearInputs();
                }
                break;
            }
            case NodeOp::Int32Sub: {
                Node* lhs = node->Input(0);
                Node* rhs = node->Input(1);
                if (lhs && rhs && lhs->Op() == NodeOp::ConstantInt32 && rhs->Op() == NodeOp::ConstantInt32) {
                    node->SetOp(NodeOp::ConstantInt32);
                    node->SetIntValue(lhs->IntValue() - rhs->IntValue());
                    node->ClearInputs();
                }
                break;
            }
            case NodeOp::Int32Mul: {
                Node* lhs = node->Input(0);
                Node* rhs = node->Input(1);
                if (lhs && rhs && lhs->Op() == NodeOp::ConstantInt32 && rhs->Op() == NodeOp::ConstantInt32) {
                    node->SetOp(NodeOp::ConstantInt32);
                    node->SetIntValue(lhs->IntValue() * rhs->IntValue());
                    node->ClearInputs();
                }
                break;
            }
            case NodeOp::Int32CmpLess: {
                Node* lhs = node->Input(0);
                Node* rhs = node->Input(1);
                if (lhs && rhs && lhs->Op() == NodeOp::ConstantInt32 && rhs->Op() == NodeOp::ConstantInt32) {
                    node->SetOp(NodeOp::ConstantBool);
                    node->SetIntValue(lhs->IntValue() < rhs->IntValue() ? 1 : 0);
                    node->ClearInputs();
                }
                break;
            }
            default:
                break;
        }
    }
}

void SeaOfNodesOptimizer::DeadCodeElimination(Graph& graph) {
    std::unordered_set<Node*> live;
    std::vector<Node*> worklist;

    if (graph.ReturnNode()) {
        live.insert(graph.ReturnNode());
        worklist.push_back(graph.ReturnNode());
    }

    while (!worklist.empty()) {
        Node* curr = worklist.back();
        worklist.pop_back();

        for (Node* in : curr->Inputs()) {
            if (in && !live.contains(in)) {
                live.insert(in);
                worklist.push_back(in);
            }
        }
    }

    for (const auto& node_ptr : graph.Nodes()) {
        Node* n = node_ptr.get();
        if (!live.contains(n)) {
            n->MarkDead();
        }
    }
}

void SeaOfNodesOptimizer::LoopInvariantCodeMotion(Graph& graph) {
    // Finds pure value nodes that have no loop dependency and can be hoisted
    for (const auto& node_ptr : graph.Nodes()) {
        Node* node = node_ptr.get();
        if (node->IsDead() || !node->IsValue()) continue;

        // If all inputs are constants or parameters, this computation is loop invariant
        bool all_invariant = true;
        for (Node* in : node->Inputs()) {
            if (!in || (in->Op() != NodeOp::ConstantInt32 && in->Op() != NodeOp::ConstantBool && in->Op() != NodeOp::Parameter)) {
                all_invariant = false;
                break;
            }
        }
        (void)all_invariant;
    }
}

} // namespace aether
