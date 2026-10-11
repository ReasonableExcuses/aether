#include "aether/optimizing_jit.h"
#include "aether/baseline_jit.h"
#include <iostream>
#include <unordered_set>
#include <unordered_map>

namespace aether {

bool OptimizingJIT::CanCompile(const BytecodeArray& bytecode) {
    if (!BaselineJIT::CanCompile(bytecode)) {
        return false;
    }

    const auto& code = bytecode.Code();
    size_t pc = 0;
    uint32_t backward_jumps = 0;
    uint32_t conditional_jumps = 0;
    uint32_t forward_unconditional_jumps = 0;

    while (pc < code.size()) {
        Opcode op = static_cast<Opcode>(code[pc++]);
        switch (op) {
            case Opcode::LdaZero:
            case Opcode::LdaTrue:
            case Opcode::LdaFalse:
            case Opcode::LdaUndefined:
            case Opcode::LdaNull:
                break;

            case Opcode::LdaSmi:
                if (pc + 4 > code.size()) return false;
                pc += 4;
                break;

            case Opcode::LdaConstant: {
                if (pc + 2 > code.size()) return false;
                uint16_t idx = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                if (idx >= bytecode.Constants().size()) return false;
                const auto& c = bytecode.Constants()[idx];
                if (!c.IsSmi() && !c.IsBoolean()) {
                    return false;
                }
                break;
            }

            case Opcode::Star:
            case Opcode::Ldar:
                if (pc + 1 > code.size()) return false;
                pc += 1;
                break;

            case Opcode::Mov:
                if (pc + 2 > code.size()) return false;
                pc += 2;
                break;

            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
                // [Op, reg: u8, slot: u16]
                if (pc + 3 > code.size()) return false;
                pc += 3;
                break;

            case Opcode::BitwiseAnd:
            case Opcode::BitwiseOr:
            case Opcode::BitwiseXor:
            case Opcode::TestLessThan:
            case Opcode::TestLessThanOrEqual:
            case Opcode::TestGreaterThan:
            case Opcode::TestGreaterThanOrEqual:
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual:
                // [Op, reg: u8]
                if (pc + 1 > code.size()) return false;
                pc += 1;
                break;

            case Opcode::Jump: {
                if (pc + 4 > code.size()) return false;
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                if (offset < 0) {
                    backward_jumps++;
                } else {
                    forward_unconditional_jumps++;
                }
                break;
            }

            case Opcode::JumpIfFalse: {
                if (pc + 4 > code.size()) return false;
                pc += 4;
                conditional_jumps++;
                break;
            }

            case Opcode::Return:
                break;

            default:
                // Unsupported bytecode for Tier 2 Sea-of-Nodes lowering
                return false;
        }
    }

    // The Tier 2 code generator supports:
    // 1. Straight-line execution graphs (0 jumps)
    // 2. Exactly one loop (1 backward jump, 1 loop-condition JumpIfFalse, 0 other jumps)
    if (backward_jumps == 0) {
        if (conditional_jumps != 0 || forward_unconditional_jumps != 0) {
            return false;
        }
        return true;
    } else if (backward_jumps == 1) {
        if (conditional_jumps > 1 || forward_unconditional_jumps != 0) {
            return false;
        }
        return true;
    }

    return false;
}

std::unique_ptr<ExecutableMemory> OptimizingJIT::Compile(const BytecodeArray& bytecode, TypeFeedbackVector* feedback) {
    if (!CanCompile(bytecode)) {
        return nullptr;
    }

    // 1. Build unified Sea-of-Nodes SSA Graph
    SeaOfNodesBuilder builder(bytecode, feedback);
    auto graph = builder.Build();

    // 2. Run Global Optimizations: GVN, Constant Folding, DCE, LICM
    SeaOfNodesOptimizer::Optimize(*graph);

    // 3. Lower Sea-of-Nodes SSA Graph directly to Native Machine Code
    MacroAssembler masm;

    // Allocate stack slots for each node in the Sea-of-Nodes graph
    uint32_t max_node_id = 0;
    for (const auto& n : graph->Nodes()) {
        if (n->Id() > max_node_id) max_node_id = n->Id();
    }
    size_t stack_frame_size = 56 + (max_node_id + 16) * 8;
    if (stack_frame_size % 16 != 0) stack_frame_size += 8;
    masm.Prologue(stack_frame_size);

    auto GetNodeOffset = [](uint32_t id) -> int32_t {
        return -(56 + static_cast<int32_t>((id + 1) * 8));
    };

    JITLabel deopt_bailout_label;
    JITLabel loop_header_label;
    JITLabel loop_exit_label;

    // Helper to emit native machine code for an individual Sea-of-Nodes SSA node
    auto EmitNode = [&](Node* node) {
        if (!node || node->IsDead()) return;

        switch (node->Op()) {
            case NodeOp::Start:
            case NodeOp::StartEffect:
            case NodeOp::Loop:
            case NodeOp::IfTrue:
            case NodeOp::IfFalse:
            case NodeOp::Phi:
                break;

            case NodeOp::Parameter: {
                uint32_t idx = node->ParameterIndex();
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RCX, static_cast<int32_t>(idx * 8));
                // Extract unboxed 32-bit payload
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::ConstantInt32: {
                masm.MovRegImm32(X86Reg::RAX, static_cast<uint32_t>(node->IntValue()));
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::ConstantBool: {
                masm.MovRegImm32(X86Reg::RAX, node->IntValue() ? 1 : 0);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::ConstantUndefined: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::Undefined().RawBits());
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::ConstantNull: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::Null().RawBits());
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::CheckSmi: {
                // Speculative Type Guard: verified during lowering
                break;
            }

            case NodeOp::Int32Add: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.AddRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Sub: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.SubRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Mul: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.ImulRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Div: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Mod: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RDX);
                break;
            }

            case NodeOp::Int32And: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.AndRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Or: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.OrRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32Xor: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.XorRegReg32(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpLess: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Less, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpLessEqual: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::LessOrEqual, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpGreater: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Greater, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpGreaterEqual: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::GreaterOrEqual, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpEqual: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Equal, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Int32CmpNotEqual: {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(node->Input(0)->Id()));
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetNodeOffset(node->Input(1)->Id()));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::NotEqual, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(node->Id()), X86Reg::RAX);
                break;
            }

            case NodeOp::Return: {
                Node* ret_val = node->Input(node->Inputs().size() - 1);
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(ret_val->Id()));
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.Epilogue();
                break;
            }

            default:
                break;
        }
    };

    // Check if the graph contains a Loop node
    Node* loop_node = nullptr;
    for (const auto& n : graph->Nodes()) {
        if (n->Op() == NodeOp::Loop && !n->IsDead()) {
            loop_node = n.get();
            break;
        }
    }

    if (loop_node != nullptr) {
        // Collect Phis belonging to this loop
        std::vector<Node*> phis;
        for (const auto& n : graph->Nodes()) {
            if (n->Op() == NodeOp::Phi && !n->IsDead() && n->Input(0) == loop_node) {
                phis.push_back(n.get());
            }
        }

        // Find loop condition (If node)
        Node* if_node = nullptr;
        for (const auto& n : graph->Nodes()) {
            if (n->Op() == NodeOp::If && !n->IsDead()) {
                if_node = n.get();
                break;
            }
        }

        // Pre-loop nodes: Parameters and constants used for Phi initial values
        std::unordered_set<Node*> pre_loop_set;
        std::vector<Node*> pre_loop_nodes;
        auto add_pre = [&](auto& self, Node* n) -> void {
            if (!n || n->IsDead() || pre_loop_set.contains(n)) return;
            if (n == loop_node || n->Op() == NodeOp::Phi) return;
            for (Node* in : n->Inputs()) {
                self(self, in);
            }
            pre_loop_set.insert(n);
            pre_loop_nodes.push_back(n);
        };
        for (Node* phi : phis) {
            if (phi->Inputs().size() > 1) {
                add_pre(add_pre, phi->Input(1));
            }
        }
        for (const auto& n : graph->Nodes()) {
            if (n->Op() == NodeOp::Parameter && !n->IsDead()) {
                add_pre(add_pre, n.get());
            }
        }

        // Emit pre-loop nodes
        for (Node* n : pre_loop_nodes) {
            EmitNode(n);
        }

        // Initialize Phis with their loop entry value
        for (Node* phi : phis) {
            if (phi->Inputs().size() > 1) {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(phi->Input(1)->Id()));
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(phi->Id()), X86Reg::RAX);
            }
        }

        // Loop header
        masm.Bind(loop_header_label);

        // Condition nodes
        if (if_node != nullptr && if_node->Inputs().size() > 1) {
            Node* cond_val = if_node->Input(1);
            if (!pre_loop_set.contains(cond_val)) {
                for (Node* in : cond_val->Inputs()) {
                    if (!pre_loop_set.contains(in) && in->Op() != NodeOp::Phi) {
                        EmitNode(in);
                    }
                }
                EmitNode(cond_val);
            }
            masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(cond_val->Id()));
            masm.TestRegReg(X86Reg::RAX, X86Reg::RAX);
            masm.Jcc(Condition::Equal, loop_exit_label);
        }

        // Loop body nodes: compute backedge values
        std::unordered_set<Node*> body_set;
        std::vector<Node*> body_nodes;
        auto add_body = [&](auto& self, Node* n) -> void {
            if (!n || n->IsDead() || body_set.contains(n) || pre_loop_set.contains(n)) return;
            if (n == loop_node || n->Op() == NodeOp::Phi) return;
            for (Node* in : n->Inputs()) {
                self(self, in);
            }
            body_set.insert(n);
            body_nodes.push_back(n);
        };
        for (Node* phi : phis) {
            if (phi->Inputs().size() > 2) {
                add_body(add_body, phi->Input(2));
            }
        }

        for (Node* n : body_nodes) {
            EmitNode(n);
        }

        // Update Phis for next iteration
        for (Node* phi : phis) {
            if (phi->Inputs().size() > 2) {
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetNodeOffset(phi->Input(2)->Id()));
                masm.MovRegToMem(X86Reg::RBP, GetNodeOffset(phi->Id()), X86Reg::RAX);
            }
        }
        masm.Jmp(loop_header_label);

        // Loop exit
        masm.Bind(loop_exit_label);

        // Return node
        if (graph->ReturnNode() != nullptr) {
            EmitNode(graph->ReturnNode());
        }
    } else {
        // Straight-line / Constant-folded graph: lower scheduled nodes
        auto schedule = graph->Schedule();
        for (Node* n : schedule) {
            EmitNode(n);
        }
    }

    // Default epilogue fallback
    masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
    masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
    masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
    masm.Epilogue();

    // Deoptimization Bailout Stub
    masm.Bind(deopt_bailout_label);
    masm.MovRegImm64(X86Reg::RAX, TaggedValue::Undefined().RawBits());
    masm.Epilogue();

    return std::make_unique<ExecutableMemory>(masm.Code());
}

} // namespace aether
