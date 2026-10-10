#ifndef AETHER_BYTECODE_H
#define AETHER_BYTECODE_H

#include "aether/common.h"
#include "aether/value.h"
#include <vector>
#include <string>
#include <memory>

namespace aether {

class ExecutableMemory;

/**
 * @brief Opcode enumerations for AetherVM Accumulator-Register Bytecode ISA.
 */
enum class Opcode : uint8_t {
    // Constant & Literal Loads
    LdaConstant,        // [Op, const_idx: u16]        acc = Constants[const_idx]
    LdaZero,            // [Op]                        acc = Smi(0)
    LdaSmi,             // [Op, imm: i32]              acc = Smi(imm)
    LdaUndefined,       // [Op]                        acc = Undefined
    LdaNull,            // [Op]                        acc = Null
    LdaTrue,            // [Op]                        acc = True
    LdaFalse,           // [Op]                        acc = False

    // Register Transfers
    Ldar,               // [Op, reg: u8]               acc = Reg[reg]
    Star,               // [Op, reg: u8]               Reg[reg] = acc
    Mov,                // [Op, src: u8, dst: u8]      Reg[dst] = Reg[src]

    // Binary Arithmetic (acc = acc <op> Reg[reg])
    Add,                // [Op, reg: u8, slot: u16]    acc = acc + Reg[reg]
    Sub,                // [Op, reg: u8, slot: u16]    acc = acc - Reg[reg]
    Mul,                // [Op, reg: u8, slot: u16]    acc = acc * Reg[reg]
    Div,                // [Op, reg: u8, slot: u16]    acc = acc / Reg[reg]
    Mod,                // [Op, reg: u8, slot: u16]    acc = acc % Reg[reg]

    // Bitwise Operations
    BitwiseAnd,         // [Op, reg: u8]               acc = acc & Reg[reg]
    BitwiseOr,          // [Op, reg: u8]               acc = acc | Reg[reg]
    BitwiseXor,         // [Op, reg: u8]               acc = acc ^ Reg[reg]
    ShiftLeft,          // [Op, reg: u8]               acc = acc << Reg[reg]
    ShiftRight,         // [Op, reg: u8]               acc = acc >> Reg[reg]

    // Comparisons (acc = (acc <op> Reg[reg]))
    TestEqual,              // [Op, reg: u8]           acc = (acc == Reg[reg])
    TestStrictEqual,        // [Op, reg: u8]           acc = (acc === Reg[reg])
    TestLessThan,           // [Op, reg: u8]           acc = (acc < Reg[reg])
    TestGreaterThan,        // [Op, reg: u8]           acc = (acc > Reg[reg])
    TestLessThanOrEqual,    // [Op, reg: u8]           acc = (acc <= Reg[reg])
    TestGreaterThanOrEqual, // [Op, reg: u8]           acc = (acc >= Reg[reg])

    // Control Flow
    Jump,               // [Op, offset: i32]           pc += offset
    JumpIfTrue,         // [Op, offset: i32]           if (ToBoolean(acc)) pc += offset
    JumpIfFalse,        // [Op, offset: i32]           if (!ToBoolean(acc)) pc += offset

    // Object & Property Access (Inline Cache enabled)
    LdaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  acc = obj.name
    StaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  obj.name = acc

    // Unary Operations
    LogicalNot,             // [Op]                    acc = !ToBoolean(acc)
    Negate,                 // [Op]                    acc = -acc
    BitwiseNot,             // [Op]                    acc = ~acc

    // Function Calls & Object Instantiation
    Call,                   // [Op, callee: u8, args: u8, count: u8, slot: u16]
    CreateObject,           // [Op]                    acc = new JSObject()

    // Execution & Control
    Return,             // [Op]                        return acc
    Halt,               // [Op]                        halt execution
    Count               // Total opcode count marker
};

const char* OpcodeToString(Opcode op);

/**
 * @brief Represents a compiled bytecode routine along with constant pool and metadata.
 */
class BytecodeArray {
public:
    BytecodeArray(uint32_t register_count = 0, uint32_t parameter_count = 0)
        : register_count_(register_count), parameter_count_(parameter_count) {}

    const std::vector<uint8_t>& Code() const noexcept { return bytes_; }
    const std::vector<TaggedValue>& Constants() const noexcept { return constants_; }
    uint32_t RegisterCount() const noexcept { return register_count_; }
    uint32_t ParameterCount() const noexcept { return parameter_count_; }

    void SetRegisterCount(uint32_t count) noexcept { register_count_ = count; }
    void SetParameterCount(uint32_t count) noexcept { parameter_count_ = count; }

    // Emission primitives
    size_t CurrentOffset() const noexcept { return bytes_.size(); }

    void EmitByte(uint8_t b) { bytes_.push_back(b); }
    void EmitOpcode(Opcode op) { bytes_.push_back(static_cast<uint8_t>(op)); }
    void EmitU8(uint8_t u8) { bytes_.push_back(u8); }
    void EmitU16(uint16_t u16);
    void EmitI32(int32_t i32);

    void PatchI32(size_t offset, int32_t val);

    uint16_t AddConstant(TaggedValue val);

    // Disassembly for verification and debugging
    std::string Disassemble() const;

    // JIT Tiering & Invocation Profiling
    uint32_t InvocationCount() const noexcept { return invocation_count_; }
    void IncrementInvocationCount() const noexcept { ++invocation_count_; }
    void ResetInvocationCount() const noexcept { invocation_count_ = 0; }

    const ExecutableMemory* GetTier1Code() const noexcept { return tier1_code_.get(); }
    void SetTier1Code(std::shared_ptr<ExecutableMemory> code) const noexcept { tier1_code_ = std::move(code); }

    const ExecutableMemory* GetTier2Code() const noexcept { return tier2_code_.get(); }
    void SetTier2Code(std::shared_ptr<ExecutableMemory> code) const noexcept { tier2_code_ = std::move(code); }

private:
    std::vector<uint8_t> bytes_;
    std::vector<TaggedValue> constants_;
    uint32_t register_count_{0};
    uint32_t parameter_count_{0};
    mutable uint32_t invocation_count_{0};
    mutable std::shared_ptr<ExecutableMemory> tier1_code_{nullptr};
    mutable std::shared_ptr<ExecutableMemory> tier2_code_{nullptr};
};

} // namespace aether

#endif // AETHER_BYTECODE_H
