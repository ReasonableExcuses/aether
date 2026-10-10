#ifndef AETHER_ASSEMBLER_H
#define AETHER_ASSEMBLER_H

#include "aether/common.h"
#include <vector>
#include <cstdint>
#include <cassert>
#include <memory>

namespace aether {

enum class X86Reg : uint8_t {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8  = 8,
    R9  = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15
};

enum class Condition : uint8_t {
    Equal               = 0x84, // je
    NotEqual            = 0x85, // jne
    Less                = 0x8C, // jl
    LessOrEqual         = 0x8E, // jle
    Greater             = 0x8F, // jg
    GreaterOrEqual      = 0x8D  // jge
};

struct JITLabel {
    int32_t offset{-1};
    std::vector<size_t> unresolved_branches;
};

/**
 * @brief Executable memory buffer managing native machine code with W^X safety.
 */
class ExecutableMemory {
public:
    explicit ExecutableMemory(const std::vector<uint8_t>& machine_code);
    ~ExecutableMemory();

    const void* CodeAddress() const noexcept { return address_; }
    size_t Size() const noexcept { return size_; }

    template <typename Signature>
    Signature AsFunction() const noexcept {
        return reinterpret_cast<Signature>(address_);
    }

private:
    void* address_{nullptr};
    size_t size_{0};
};

/**
 * @brief x86-64 MacroAssembler emitting native machine code instructions.
 */
class MacroAssembler {
public:
    MacroAssembler() = default;

    // Control and Frame
    void Prologue(size_t stack_frame_size);
    void Epilogue();

    // Data Transfer
    void MovRegImm64(X86Reg dst, uint64_t imm);
    void MovRegReg(X86Reg dst, X86Reg src);
    void MovRegReg32(X86Reg dst, X86Reg src);
    void MovMemToReg(X86Reg dst, X86Reg base, int32_t disp);
    void MovRegToMem(X86Reg base, int32_t disp, X86Reg src);

    // Arithmetic & Logic (64-bit)
    void AddRegReg(X86Reg dst, X86Reg src);
    void SubRegReg(X86Reg dst, X86Reg src);
    void ImulRegReg(X86Reg dst, X86Reg src);
    void AndRegReg(X86Reg dst, X86Reg src);
    void OrRegReg(X86Reg dst, X86Reg src);
    void XorRegReg(X86Reg dst, X86Reg src);

    // Arithmetic & Logic (32-bit unboxed)
    void AddRegReg32(X86Reg dst, X86Reg src);
    void SubRegReg32(X86Reg dst, X86Reg src);
    void ImulRegReg32(X86Reg dst, X86Reg src);
    void AndRegReg32(X86Reg dst, X86Reg src);
    void OrRegReg32(X86Reg dst, X86Reg src);
    void XorRegReg32(X86Reg dst, X86Reg src);
    void MovRegImm32(X86Reg dst, uint32_t imm);
    void Cdq();
    void IdivReg32(X86Reg src);

    // Smi Unboxing & Fast Arithmetic (32-bit sign extension)
    void Movsxd(X86Reg dst, X86Reg src);

    // Comparisons
    void CmpRegReg(X86Reg r1, X86Reg r2);
    void CmpRegReg32(X86Reg r1, X86Reg r2);
    void CmpRegImm32(X86Reg r, int32_t imm);
    void TestRegReg(X86Reg r1, X86Reg r2);

    // Branching & Labels
    void Jmp(JITLabel& label);
    void Jcc(Condition cond, JITLabel& label);
    void Bind(JITLabel& label);

    // Utilities
    size_t CurrentOffset() const noexcept { return code_.size(); }
    const std::vector<uint8_t>& Code() const noexcept { return code_; }

    void Emit8(uint8_t byte) { code_.push_back(byte); }
    void Emit32(uint32_t u32);
    void Emit64(uint64_t u64);

private:
    void EmitRex(bool w, X86Reg r, X86Reg b);
    void EmitModRM(uint8_t mod, X86Reg reg, X86Reg rm);

    std::vector<uint8_t> code_;
};

} // namespace aether

#endif // AETHER_ASSEMBLER_H
