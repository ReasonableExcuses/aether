#include "aether/optimizing_jit.h"
#include "aether/baseline_jit.h"
#include <iostream>

namespace aether {

bool OptimizingJIT::CanCompile(const BytecodeArray& bytecode) {
    return BaselineJIT::CanCompile(bytecode);
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

    // 3. Machine Code Generation using MacroAssembler
    MacroAssembler masm;
    size_t stack_frame_size = (bytecode.RegisterCount() + 4) * 8;
    masm.Prologue(stack_frame_size);

    // Unbox incoming parameters from RCX directly into unboxed 32-bit slots
    for (uint32_t i = 0; i < bytecode.ParameterCount(); ++i) {
        masm.MovMemToReg(X86Reg::RAX, X86Reg::RCX, static_cast<int32_t>(i * 8));
        // Extract 32-bit Smi payload into unboxed 32-bit slot
        masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
        masm.MovRegToMem(X86Reg::RBP, GetRegisterOffset(static_cast<uint8_t>(i)), X86Reg::RAX);
    }

    std::vector<JITLabel> labels(bytecode.Code().size() + 1);
    JITLabel deopt_bailout_label;

    size_t pc = 0;
    const auto& code = bytecode.Code();

    while (pc < code.size()) {
        masm.Bind(labels[pc]);
        Opcode op = static_cast<Opcode>(code[pc++]);

        switch (op) {
            case Opcode::LdaZero: {
                // Load unboxed 0 directly into EAX
                masm.MovRegImm32(X86Reg::RAX, 0);
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                masm.MovRegImm32(X86Reg::RAX, static_cast<uint32_t>(imm));
                break;
            }
            case Opcode::LdaTrue: {
                masm.MovRegImm32(X86Reg::RAX, 1);
                break;
            }
            case Opcode::LdaFalse: {
                masm.MovRegImm32(X86Reg::RAX, 0);
                break;
            }
            case Opcode::LdaConstant: {
                uint16_t idx = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                TaggedValue c = bytecode.Constants()[idx];
                if (c.IsSmi()) {
                    masm.MovRegImm32(X86Reg::RAX, static_cast<uint32_t>(c.AsSmi()));
                } else {
                    masm.MovRegImm64(X86Reg::RAX, c.RawBits());
                }
                break;
            }
            case Opcode::Star: {
                uint8_t reg = code[pc++];
                // Store unboxed 32-bit value in local stack slot
                masm.MovRegToMem(X86Reg::RBP, GetRegisterOffset(reg), X86Reg::RAX);
                break;
            }
            case Opcode::Ldar: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetRegisterOffset(reg));
                break;
            }
            case Opcode::Mov: {
                uint8_t src = code[pc++];
                uint8_t dst = code[pc++];
                masm.MovMemToReg(X86Reg::RAX, X86Reg::RBP, GetRegisterOffset(src));
                masm.MovRegToMem(X86Reg::RBP, GetRegisterOffset(dst), X86Reg::RAX);
                break;
            }
            case Opcode::Add: {
                uint8_t reg = code[pc++];
                pc += 2; // slot
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                // Fast unboxed 32-bit addition: add eax, ecx
                masm.AddRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.SubRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Mul: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.ImulRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Div: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                break;
            }
            case Opcode::Mod: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RDX);
                break;
            }
            case Opcode::BitwiseAnd: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.AndRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::BitwiseOr: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.OrRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::BitwiseXor: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.XorRegReg32(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::TestLessThan: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Less, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                break;
            }
            case Opcode::TestLessThanOrEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::LessOrEqual, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                break;
            }
            case Opcode::TestGreaterThan: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Greater, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                break;
            }
            case Opcode::TestGreaterThanOrEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::GreaterOrEqual, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                break;
            }
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg32(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true, done;
                masm.Jcc(Condition::Equal, is_true);
                masm.MovRegImm32(X86Reg::RAX, 0);
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm32(X86Reg::RAX, 1);
                masm.Bind(done);
                break;
            }
            case Opcode::Jump: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                size_t target_pc = pc + offset;
                masm.Jmp(labels[target_pc]);
                break;
            }
            case Opcode::JumpIfTrue: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                size_t target_pc = pc + offset;
                // EAX != 0
                masm.TestRegReg(X86Reg::RAX, X86Reg::RAX);
                masm.Jcc(Condition::NotEqual, labels[target_pc]);
                break;
            }
            case Opcode::JumpIfFalse: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                size_t target_pc = pc + offset;
                // EAX == 0
                masm.TestRegReg(X86Reg::RAX, X86Reg::RAX);
                masm.Jcc(Condition::Equal, labels[target_pc]);
                break;
            }
            case Opcode::Return: {
                // Box unboxed 32-bit result in EAX into NaN-boxed Smi
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.Epilogue();
                break;
            }
            default:
                break;
        }
    }

    // Default epilogue
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
