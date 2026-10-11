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
    TestInstanceOf,         // [Op, reg: u8]           acc = (acc instanceof Reg[reg])

    // Control Flow
    Jump,               // [Op, offset: i32]           pc += offset
    JumpIfTrue,         // [Op, offset: i32]           if (ToBoolean(acc)) pc += offset
    JumpIfFalse,        // [Op, offset: i32]           if (!ToBoolean(acc)) pc += offset
    JumpIfNotNullish,   // [Op, offset: i32]           if (!acc.IsNull() && !acc.IsUndefined()) pc += offset
    JumpIfNullish,      // [Op, offset: i32]           if (acc.IsNull() || acc.IsUndefined()) pc += offset

    // Object & Property Access (Inline Cache enabled)
    LdaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  acc = obj.name
    StaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  obj.name = acc
    LdaKeyed,           // [Op, obj: u8, key: u8, slot: u16]        acc = obj[key]
    StaKeyed,           // [Op, obj: u8, key: u8, slot: u16]        obj[key] = acc

    // Unary Operations
    LogicalNot,             // [Op]                    acc = !ToBoolean(acc)
    Negate,                 // [Op]                    acc = -acc
    BitwiseNot,             // [Op]                    acc = ~acc
    TypeOf,                 // [Op]                    acc = typeof acc

    // Function Calls & Object Instantiation
    Call,                   // [Op, callee: u8, args: u8, count: u8, slot: u16]
    CallMethod,             // [Op, callee: u8, this: u8, args: u8, count: u8, slot: u16]
    CreateObject,           // [Op]                    acc = new JSObject()
    CreateArray,            // [Op, start_reg: u8, count: u16] acc = new JSArray(...)
    Construct,              // [Op, ctor: u8, args: u8, count: u8, slot: u16]
    LdaThis,                // [Op]                    acc = this
    ArrayPush,              // [Op, arr_reg: u8, val_reg: u8]  arr.push(val)
    ArraySpread,            // [Op, target_reg: u8, src_reg: u8] arr.push(...src)
    ObjectSpread,           // [Op, target_reg: u8, src_reg: u8] Object.assign(target, src)
    CreateRestArray,        // [Op, dest_reg: u8, start_idx: u16] dest = args.slice(start_idx)

    // Closures & Lexical Scopes
    CreateClosure,          // [Op, const_idx: u16]    acc = new JSFunction(const[idx], env)
    CreateFunctionContext,  // [Op, slot_count: u16]   env = new LexicalEnvironment(env, slot_count)
    LdaContextSlot,         // [Op, depth: u8, slot: u16] acc = env.Get(depth, slot)
    StaContextSlot,         // [Op, depth: u8, slot: u16] env.Set(depth, slot, acc)

    // Exception Handling
    PushExceptionHandler,   // [Op, offset: i32]       push catch handler
    PopExceptionHandler,    // [Op]                    pop catch handler
    Throw,                  // [Op]                    throw acc

    // Execution & Control
    Yield,              // [Op]                        suspend and yield acc
    Return,             // [Op]                        return acc
    Halt,               // [Op]                        halt execution
    Count               // Total opcode count marker
};

const char* OpcodeToString(Opcode op);

class Shape;

/**
 * @brief Feedback slot record for Inline Caching (IC) and Type Profiling.
 */
struct FeedbackSlot {
    enum class State : uint8_t {
        Uninitialized = 0,
        Monomorphic   = 1,
        Polymorphic   = 2,
        Megamorphic   = 3
    };

    enum class ObservedType : uint8_t {
        None   = 0,
        Smi    = 1 << 0,
        Number = 1 << 1,
        String = 1 << 2,
        Object = 1 << 3,
        Any    = 0xFF
    };

    State state{State::Uninitialized};
    uint8_t observed_type{static_cast<uint8_t>(ObservedType::None)};
    Shape* cached_shape{nullptr};
    uint32_t cached_slot{0};
    bool cached_in_object{true};

    void RecordType(const TaggedValue& val) {
        if (val.IsSmi()) {
            observed_type |= static_cast<uint8_t>(ObservedType::Smi);
        } else if (val.IsDouble()) {
            observed_type |= static_cast<uint8_t>(ObservedType::Number);
        } else if (val.IsHeapObject()) {
            observed_type |= static_cast<uint8_t>(ObservedType::Object);
        }
    }

    bool IsMonomorphicSmi() const noexcept {
        return observed_type == static_cast<uint8_t>(ObservedType::Smi);
    }
};

/**
 * @brief Vector of Type and Shape feedback slots for Tier 0 profiling and ICs.
 */
class TypeFeedbackVector {
public:
    explicit TypeFeedbackVector(size_t slot_count = 64)
        : slots_(slot_count) {}

    FeedbackSlot& GetSlot(size_t idx) {
        if (idx >= slots_.size()) {
            slots_.resize(idx + 1);
        }
        return slots_[idx];
    }

    const FeedbackSlot& GetSlot(size_t idx) const {
        assert(idx < slots_.size());
        return slots_[idx];
    }

    size_t Size() const noexcept { return slots_.size(); }

private:
    std::vector<FeedbackSlot> slots_;
};

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

    TypeFeedbackVector& GetFeedbackVector() const noexcept { return feedback_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<TaggedValue> constants_;
    uint32_t register_count_{0};
    uint32_t parameter_count_{0};
    mutable uint32_t invocation_count_{0};
    mutable std::shared_ptr<ExecutableMemory> tier1_code_{nullptr};
    mutable std::shared_ptr<ExecutableMemory> tier2_code_{nullptr};
    mutable TypeFeedbackVector feedback_;
};

} // namespace aether

#endif // AETHER_BYTECODE_H
