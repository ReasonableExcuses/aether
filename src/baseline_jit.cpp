#include "aether/baseline_jit.h"

namespace aether {

bool BaselineJIT::CanCompile(const BytecodeArray& bytecode) {
    const auto& code = bytecode.Code();
    size_t pc = 0;
    while (pc < code.size()) {
        Opcode op = static_cast<Opcode>(code[pc++]);
        switch (op) {
            case Opcode::LdaZero:
            case Opcode::LdaUndefined:
            case Opcode::LdaNull:
            case Opcode::LdaTrue:
            case Opcode::LdaFalse:
            case Opcode::LogicalNot:
            case Opcode::Return:
                break;
            case Opcode::LdaSmi:
                pc += 4;
                break;
            case Opcode::LdaConstant:
                pc += 2;
                break;
            case Opcode::Star:
            case Opcode::Ldar:
                pc += 1;
                break;
            case Opcode::Mov:
                pc += 2;
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
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
                pc += 1;
                break;
            case Opcode::Jump:
            case Opcode::JumpIfTrue:
            case Opcode::JumpIfFalse:
                pc += 4;
                break;
            default:
                return false;
        }
    }
    return true;
}

std::unique_ptr<ExecutableMemory> BaselineJIT::Compile(const BytecodeArray& bytecode) {
    if (!CanCompile(bytecode)) {
        return nullptr;
    }

    MacroAssembler masm;
    size_t stack_frame_size = (bytecode.RegisterCount() + 4) * 8;
    masm.Prologue(stack_frame_size);

    // Initialize parameters from RCX into virtual register slots
    for (uint32_t i = 0; i < bytecode.ParameterCount(); ++i) {
        masm.MovMemToReg(X86Reg::RAX, X86Reg::RCX, static_cast<int32_t>(i * 8));
        masm.MovRegToMem(X86Reg::RBP, GetRegisterOffset(static_cast<uint8_t>(i)), X86Reg::RAX);
    }

    // Label per bytecode offset
    std::vector<JITLabel> labels(bytecode.Code().size() + 1);

    size_t pc = 0;
    const auto& code = bytecode.Code();

    while (pc < code.size()) {
        masm.Bind(labels[pc]);
        Opcode op = static_cast<Opcode>(code[pc++]);

        switch (op) {
            case Opcode::LdaZero: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::FromSmi(0).RawBits());
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::FromSmi(imm).RawBits());
                break;
            }
            case Opcode::LdaTrue: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                break;
            }
            case Opcode::LdaFalse: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                break;
            }
            case Opcode::LdaUndefined: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::Undefined().RawBits());
                break;
            }
            case Opcode::LdaNull: {
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::Null().RawBits());
                break;
            }
            case Opcode::LdaConstant: {
                uint16_t idx = code[pc] | (code[pc + 1] << 8);
                pc += 2;
                masm.MovRegImm64(X86Reg::RAX, bytecode.Constants()[idx].RawBits());
                break;
            }
            case Opcode::Star: {
                uint8_t reg = code[pc++];
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
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.AddRegReg(X86Reg::RDX, X86Reg::R8);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RDX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.SubRegReg(X86Reg::RDX, X86Reg::R8);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RDX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Mul: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.ImulRegReg(X86Reg::RDX, X86Reg::R8);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RDX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Div: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RAX, X86Reg::RAX);
                masm.Movsxd(X86Reg::RCX, X86Reg::RCX);
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RAX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::Mod: {
                uint8_t reg = code[pc++];
                pc += 2;
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RAX, X86Reg::RAX);
                masm.Movsxd(X86Reg::RCX, X86Reg::RCX);
                masm.Cdq();
                masm.IdivReg32(X86Reg::RCX);
                masm.MovRegReg32(X86Reg::RAX, X86Reg::RDX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::BitwiseAnd: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.AndRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::BitwiseOr: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::BitwiseXor: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.XorRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::kTagSmi);
                masm.OrRegReg(X86Reg::RAX, X86Reg::RCX);
                break;
            }
            case Opcode::TestLessThan: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.CmpRegReg(X86Reg::RDX, X86Reg::R8);
                JITLabel is_true;
                JITLabel done;
                masm.Jcc(Condition::Less, is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                masm.Bind(done);
                break;
            }
            case Opcode::TestLessThanOrEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.CmpRegReg(X86Reg::RDX, X86Reg::R8);
                JITLabel is_true;
                JITLabel done;
                masm.Jcc(Condition::LessOrEqual, is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                masm.Bind(done);
                break;
            }
            case Opcode::TestGreaterThan: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.CmpRegReg(X86Reg::RDX, X86Reg::R8);
                JITLabel is_true;
                JITLabel done;
                masm.Jcc(Condition::Greater, is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                masm.Bind(done);
                break;
            }
            case Opcode::TestGreaterThanOrEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.Movsxd(X86Reg::RDX, X86Reg::RAX);
                masm.Movsxd(X86Reg::R8, X86Reg::RCX);
                masm.CmpRegReg(X86Reg::RDX, X86Reg::R8);
                JITLabel is_true;
                JITLabel done;
                masm.Jcc(Condition::GreaterOrEqual, is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                masm.Bind(done);
                break;
            }
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual: {
                uint8_t reg = code[pc++];
                masm.MovMemToReg(X86Reg::RCX, X86Reg::RBP, GetRegisterOffset(reg));
                masm.CmpRegReg(X86Reg::RAX, X86Reg::RCX);
                JITLabel is_true;
                JITLabel done;
                masm.Jcc(Condition::Equal, is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Jmp(done);
                masm.Bind(is_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
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
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::True().RawBits());
                masm.CmpRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.Jcc(Condition::Equal, labels[target_pc]);
                break;
            }
            case Opcode::JumpIfFalse: {
                int32_t offset = code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) | (code[pc + 3] << 24);
                pc += 4;
                size_t target_pc = pc + offset;
                // Check False
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::False().RawBits());
                masm.CmpRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.Jcc(Condition::Equal, labels[target_pc]);
                // Check Smi(0)
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::FromSmi(0).RawBits());
                masm.CmpRegReg(X86Reg::RAX, X86Reg::RCX);
                masm.Jcc(Condition::Equal, labels[target_pc]);
                break;
            }
            case Opcode::LogicalNot: {
                masm.MovRegImm64(X86Reg::RCX, TaggedValue::True().RawBits());
                masm.CmpRegReg(X86Reg::RAX, X86Reg::RCX);
                JITLabel was_true;
                JITLabel done;
                masm.Jcc(Condition::Equal, was_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::True().RawBits());
                masm.Jmp(done);
                masm.Bind(was_true);
                masm.MovRegImm64(X86Reg::RAX, TaggedValue::False().RawBits());
                masm.Bind(done);
                break;
            }
            case Opcode::Return: {
                masm.Epilogue();
                break;
            }
            default:
                break;
        }
    }

    masm.Epilogue();
    return std::make_unique<ExecutableMemory>(masm.Code());
}

} // namespace aether
