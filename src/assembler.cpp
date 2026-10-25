#include "aether/assembler.h"
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace aether {

ExecutableMemory::ExecutableMemory(const std::vector<uint8_t>& machine_code)
    : size_(machine_code.size()) {
#ifdef _WIN32
    address_ = VirtualAlloc(nullptr, size_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(address_ != nullptr && "VirtualAlloc failed for ExecutableMemory");
    std::memcpy(address_, machine_code.data(), size_);
    FlushInstructionCache(GetCurrentProcess(), address_, size_);
#else
    address_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(address_ != MAP_FAILED && "mmap failed for ExecutableMemory");
    std::memcpy(address_, machine_code.data(), size_);
#endif
}

ExecutableMemory::~ExecutableMemory() {
    if (address_) {
#ifdef _WIN32
        VirtualFree(address_, 0, MEM_RELEASE);
#else
        munmap(address_, size_);
#endif
        address_ = nullptr;
    }
}

void MacroAssembler::Emit32(uint32_t u32) {
    code_.push_back(static_cast<uint8_t>(u32 & 0xFF));
    code_.push_back(static_cast<uint8_t>((u32 >> 8) & 0xFF));
    code_.push_back(static_cast<uint8_t>((u32 >> 16) & 0xFF));
    code_.push_back(static_cast<uint8_t>((u32 >> 24) & 0xFF));
}

void MacroAssembler::Emit64(uint64_t u64) {
    Emit32(static_cast<uint32_t>(u64 & 0xFFFFFFFFULL));
    Emit32(static_cast<uint32_t>((u64 >> 32) & 0xFFFFFFFFULL));
}

void MacroAssembler::EmitRex(bool w, X86Reg r, X86Reg b) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (static_cast<uint8_t>(r) >= 8) rex |= 0x04;
    if (static_cast<uint8_t>(b) >= 8) rex |= 0x01;
    code_.push_back(rex);
}

void MacroAssembler::EmitModRM(uint8_t mod, X86Reg reg, X86Reg rm) {
    uint8_t reg_field = static_cast<uint8_t>(reg) & 7;
    uint8_t rm_field  = static_cast<uint8_t>(rm) & 7;
    code_.push_back(static_cast<uint8_t>((mod << 6) | (reg_field << 3) | rm_field));
}

void MacroAssembler::Prologue(size_t stack_frame_size) {
    // 1. Setup frame pointer
    Emit8(0x55); // push rbp
    Emit8(0x48); Emit8(0x89); Emit8(0xE5); // mov rbp, rsp

    // 2. Preserve callee-saved registers (Windows x64 ABI)
    Emit8(0x53); // push rbx
    Emit8(0x41); Emit8(0x54); // push r12
    Emit8(0x41); Emit8(0x55); // push r13
    Emit8(0x41); Emit8(0x56); // push r14
    Emit8(0x41); Emit8(0x57); // push r15
    Emit8(0x56); // push rsi
    Emit8(0x57); // push rdi

    // 3. Allocate local variable stack frame (16-byte aligned)
    size_t aligned_frame = (stack_frame_size + 15) & ~15;
    Emit8(0x48); Emit8(0x81); Emit8(0xEC); // sub rsp, imm32
    Emit32(static_cast<uint32_t>(aligned_frame));
}

void MacroAssembler::Epilogue() {
    // 1. Restore RSP from RBP minus callee-saved space (7 * 8 = 56 bytes)
    Emit8(0x48); Emit8(0x8D); Emit8(0x65); Emit8(static_cast<uint8_t>(-56)); // lea rsp, [rbp - 56]

    // 2. Restore callee-saved registers
    Emit8(0x5F); // pop rdi
    Emit8(0x5E); // pop rsi
    Emit8(0x41); Emit8(0x5F); // pop r15
    Emit8(0x41); Emit8(0x5E); // pop r14
    Emit8(0x41); Emit8(0x5D); // pop r13
    Emit8(0x41); Emit8(0x5C); // pop r12
    Emit8(0x5B); // pop rbx
    Emit8(0x5D); // pop rbp
    Emit8(0xC3); // ret
}

void MacroAssembler::MovRegImm64(X86Reg dst, uint64_t imm) {
    uint8_t r = static_cast<uint8_t>(dst);
    if (r >= 8) {
        Emit8(0x49);
        Emit8(static_cast<uint8_t>(0xB8 + (r & 7)));
    } else {
        Emit8(0x48);
        Emit8(static_cast<uint8_t>(0xB8 + r));
    }
    Emit64(imm);
}

void MacroAssembler::MovRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x89); // mov r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::MovRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x89); // mov r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::MovMemToReg(X86Reg dst, X86Reg base, int32_t disp) {
    EmitRex(true, dst, base);
    Emit8(0x8B); // mov r64, r/m64
    EmitModRM(0b10, dst, base); // disp32
    Emit32(static_cast<uint32_t>(disp));
}

void MacroAssembler::MovRegToMem(X86Reg base, int32_t disp, X86Reg src) {
    EmitRex(true, src, base);
    Emit8(0x89); // mov r/m64, r64
    EmitModRM(0b10, src, base); // disp32
    Emit32(static_cast<uint32_t>(disp));
}

void MacroAssembler::AddRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x01); // add r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::SubRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x29); // sub r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::ImulRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, dst, src);
    Emit8(0x0F); Emit8(0xAF); // imul r64, r/m64
    EmitModRM(0b11, dst, src);
}

void MacroAssembler::AndRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x21); // and r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::OrRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x09); // or r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::XorRegReg(X86Reg dst, X86Reg src) {
    EmitRex(true, src, dst);
    Emit8(0x31); // xor r/m64, r64
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::AddRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x01); // add r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::SubRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x29); // sub r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::ImulRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, dst, src);
    Emit8(0x0F); Emit8(0xAF); // imul r32, r/m32
    EmitModRM(0b11, dst, src);
}

void MacroAssembler::AndRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x21); // and r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::OrRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x09); // or r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::XorRegReg32(X86Reg dst, X86Reg src) {
    EmitRex(false, src, dst);
    Emit8(0x31); // xor r/m32, r32
    EmitModRM(0b11, src, dst);
}

void MacroAssembler::NegReg32(X86Reg dst) {
    EmitRex(false, static_cast<X86Reg>(3), dst);
    Emit8(0xF7); // neg r/m32
    EmitModRM(0b11, static_cast<X86Reg>(3), dst);
}

void MacroAssembler::NotReg32(X86Reg dst) {
    EmitRex(false, static_cast<X86Reg>(2), dst);
    Emit8(0xF7); // not r/m32
    EmitModRM(0b11, static_cast<X86Reg>(2), dst);
}

void MacroAssembler::ShlRegImm8(X86Reg dst, uint8_t imm) {
    EmitRex(false, static_cast<X86Reg>(4), dst);
    Emit8(0xC1); // shl r/m32, imm8
    EmitModRM(0b11, static_cast<X86Reg>(4), dst);
    Emit8(imm);
}

void MacroAssembler::SarRegImm8(X86Reg dst, uint8_t imm) {
    EmitRex(false, static_cast<X86Reg>(7), dst);
    Emit8(0xC1); // sar r/m32, imm8
    EmitModRM(0b11, static_cast<X86Reg>(7), dst);
    Emit8(imm);
}

void MacroAssembler::ShlRegCl(X86Reg dst) {
    EmitRex(false, static_cast<X86Reg>(4), dst);
    Emit8(0xD3); // shl r/m32, cl
    EmitModRM(0b11, static_cast<X86Reg>(4), dst);
}

void MacroAssembler::SarRegCl(X86Reg dst) {
    EmitRex(false, static_cast<X86Reg>(7), dst);
    Emit8(0xD3); // sar r/m32, cl
    EmitModRM(0b11, static_cast<X86Reg>(7), dst);
}

void MacroAssembler::ShrRegCl(X86Reg dst) {
    EmitRex(false, static_cast<X86Reg>(5), dst);
    Emit8(0xD3); // shr r/m32, cl
    EmitModRM(0b11, static_cast<X86Reg>(5), dst);
}

void MacroAssembler::AddRegImm32(X86Reg dst, int32_t imm) {
    EmitRex(false, static_cast<X86Reg>(0), dst);
    Emit8(0x81); // add r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(0), dst);
    Emit32(static_cast<uint32_t>(imm));
}

void MacroAssembler::SubRegImm32(X86Reg dst, int32_t imm) {
    EmitRex(false, static_cast<X86Reg>(5), dst);
    Emit8(0x81); // sub r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(5), dst);
    Emit32(static_cast<uint32_t>(imm));
}

void MacroAssembler::AndRegImm32(X86Reg dst, uint32_t imm) {
    EmitRex(false, static_cast<X86Reg>(4), dst);
    Emit8(0x81); // and r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(4), dst);
    Emit32(imm);
}

void MacroAssembler::OrRegImm32(X86Reg dst, uint32_t imm) {
    EmitRex(false, static_cast<X86Reg>(1), dst);
    Emit8(0x81); // or r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(1), dst);
    Emit32(imm);
}

void MacroAssembler::XorRegImm32(X86Reg dst, uint32_t imm) {
    EmitRex(false, static_cast<X86Reg>(6), dst);
    Emit8(0x81); // xor r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(6), dst);
    Emit32(imm);
}

void MacroAssembler::MovRegImm32(X86Reg dst, uint32_t imm) {
    uint8_t r = static_cast<uint8_t>(dst);
    if (r >= 8) {
        Emit8(0x41);
        Emit8(static_cast<uint8_t>(0xB8 + (r & 7)));
    } else {
        Emit8(static_cast<uint8_t>(0xB8 + r));
    }
    Emit32(imm);
}

void MacroAssembler::Cdq() {
    Emit8(0x99); // cdq: sign-extend EAX into EDX:EAX
}

void MacroAssembler::IdivReg32(X86Reg src) {
    uint8_t rm = static_cast<uint8_t>(src);
    if (rm >= 8) {
        Emit8(0x41);
    }
    Emit8(0xF7);
    EmitModRM(0b11, static_cast<X86Reg>(7), src);
}

void MacroAssembler::CmpRegReg32(X86Reg r1, X86Reg r2) {
    EmitRex(false, r2, r1);
    Emit8(0x39); // cmp r/m32, r32
    EmitModRM(0b11, r2, r1);
}

void MacroAssembler::TestRegReg(X86Reg r1, X86Reg r2) {
    EmitRex(true, r2, r1);
    Emit8(0x85); // test r/m64, r64
    EmitModRM(0b11, r2, r1);
}

void MacroAssembler::Movsxd(X86Reg dst, X86Reg src) {
    EmitRex(true, dst, src);
    Emit8(0x63); // movsxd r64, r/m32
    EmitModRM(0b11, dst, src);
}

void MacroAssembler::ShrRegImm8(X86Reg dst, uint8_t imm) {
    EmitRex(true, static_cast<X86Reg>(0), dst);
    Emit8(0xC1); // shr r/m64, imm8
    EmitModRM(0b11, static_cast<X86Reg>(5), dst);
    Emit8(imm);
}

void MacroAssembler::TestSmiAndBranchIfNot(X86Reg reg, JITLabel& label) {
    MovRegReg(X86Reg::R11, reg);
    ShrRegImm8(X86Reg::R11, 48);
    CmpRegImm32(X86Reg::R11, 0x7FF9);
    Jcc(Condition::NotEqual, label);
}

void MacroAssembler::CmpRegReg(X86Reg r1, X86Reg r2) {
    EmitRex(true, r2, r1);
    Emit8(0x39); // cmp r/m64, r64
    EmitModRM(0b11, r2, r1);
}

void MacroAssembler::CmpRegImm32(X86Reg r, int32_t imm) {
    EmitRex(false, static_cast<X86Reg>(7), r);
    Emit8(0x81); // cmp r/m32, imm32
    EmitModRM(0b11, static_cast<X86Reg>(7), r);
    Emit32(static_cast<uint32_t>(imm));
}

void MacroAssembler::CmpRegImm64(X86Reg r, int32_t imm) {
    EmitRex(true, static_cast<X86Reg>(7), r);
    Emit8(0x81); // cmp r/m64, imm32
    EmitModRM(0b11, static_cast<X86Reg>(7), r);
    Emit32(static_cast<uint32_t>(imm));
}

void MacroAssembler::Jmp(JITLabel& label) {
    Emit8(0xE9); // jmp rel32
    if (label.offset != -1) {
        int32_t rel = label.offset - static_cast<int32_t>(code_.size() + 4);
        Emit32(static_cast<uint32_t>(rel));
    } else {
        label.unresolved_branches.push_back(code_.size());
        Emit32(0);
    }
}

void MacroAssembler::Jcc(Condition cond, JITLabel& label) {
    Emit8(0x0F);
    Emit8(static_cast<uint8_t>(cond)); // jcc rel32
    if (label.offset != -1) {
        int32_t rel = label.offset - static_cast<int32_t>(code_.size() + 4);
        Emit32(static_cast<uint32_t>(rel));
    } else {
        label.unresolved_branches.push_back(code_.size());
        Emit32(0);
    }
}

void MacroAssembler::Bind(JITLabel& label) {
    label.offset = static_cast<int32_t>(code_.size());
    for (size_t patch_pc : label.unresolved_branches) {
        int32_t rel = label.offset - static_cast<int32_t>(patch_pc + 4);
        code_[patch_pc]     = static_cast<uint8_t>(rel & 0xFF);
        code_[patch_pc + 1] = static_cast<uint8_t>((rel >> 8) & 0xFF);
        code_[patch_pc + 2] = static_cast<uint8_t>((rel >> 16) & 0xFF);
        code_[patch_pc + 3] = static_cast<uint8_t>((rel >> 24) & 0xFF);
    }
    label.unresolved_branches.clear();
}

void MacroAssembler::CallReg(X86Reg reg) {
    if (static_cast<uint8_t>(reg) >= 8) {
        Emit8(0x41);
    }
    Emit8(0xFF);
    Emit8(static_cast<uint8_t>(0xD0 | (static_cast<uint8_t>(reg) & 7)));
}

void MacroAssembler::SubRspImm32(int32_t imm) {
    Emit8(0x48); Emit8(0x81); Emit8(0xEC);
    Emit32(static_cast<uint32_t>(imm));
}

void MacroAssembler::AddRspImm32(int32_t imm) {
    Emit8(0x48); Emit8(0x81); Emit8(0xC4);
    Emit32(static_cast<uint32_t>(imm));
}

} // namespace aether
