#ifndef AETHER_BYTECODE_H
#define AETHER_BYTECODE_H

#include "aether/common.h"
#include "aether/value.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

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
    LdaEmpty,           // [Op]                        acc = Empty

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
    Exp,                // [Op, reg: u8, slot: u16]    acc = acc ** Reg[reg]

    // Bitwise Operations
    BitwiseAnd,         // [Op, reg: u8]               acc = acc & Reg[reg]
    BitwiseOr,          // [Op, reg: u8]               acc = acc | Reg[reg]
    BitwiseXor,         // [Op, reg: u8]               acc = acc ^ Reg[reg]
    ShiftLeft,          // [Op, reg: u8]               acc = acc << Reg[reg]
    ShiftRight,         // [Op, reg: u8]               acc = acc >> Reg[reg]
    ShiftRightUnsigned, // [Op, reg: u8]               acc = acc >>> Reg[reg]

    // Comparisons (acc = (acc <op> Reg[reg]))
    TestEqual,              // [Op, reg: u8]           acc = (acc == Reg[reg])
    TestStrictEqual,        // [Op, reg: u8]           acc = (acc === Reg[reg])
    TestNotEqual,           // [Op, reg: u8]           acc = (acc != Reg[reg])
    TestStrictNotEqual,     // [Op, reg: u8]           acc = (acc !== Reg[reg])
    TestLessThan,           // [Op, reg: u8]           acc = (acc < Reg[reg])
    TestGreaterThan,        // [Op, reg: u8]           acc = (acc > Reg[reg])
    TestLessThanOrEqual,    // [Op, reg: u8]           acc = (acc <= Reg[reg])
    TestGreaterThanOrEqual, // [Op, reg: u8]           acc = (acc >= Reg[reg])
    TestInstanceOf,         // [Op, reg: u8]           acc = (acc instanceof Reg[reg])
    TestIn,                 // [Op, reg: u8]           acc = (Reg[reg] in acc)
    TestInWith,             // [Op, reg: u8]           acc = HasBindingWith(acc, Reg[reg])
    TestPrivateIn,          // [Op, name_idx: u16]     acc = (#name in acc)
    TestNull,               // [Op]                    acc = (acc === null)
    TestUndefined,          // [Op]                    acc = (acc === undefined)
    TestTypeOf,             // [Op, type_const: u16]   acc = (typeof acc === const[type_const])

    // Smi-Immediate Binary Arithmetic & Bitwise (acc = acc <op> imm)
    AddSmi,                 // [Op, imm: i32, slot: u16]   acc = acc + imm
    SubSmi,                 // [Op, imm: i32, slot: u16]   acc = acc - imm
    MulSmi,                 // [Op, imm: i32, slot: u16]   acc = acc * imm
    DivSmi,                 // [Op, imm: i32, slot: u16]   acc = acc / imm
    ModSmi,                 // [Op, imm: i32, slot: u16]   acc = acc % imm
    BitwiseAndSmi,          // [Op, imm: i32]              acc = acc & imm
    BitwiseOrSmi,           // [Op, imm: i32]              acc = acc | imm
    BitwiseXorSmi,          // [Op, imm: i32]              acc = acc ^ imm
    ShiftLeftSmi,           // [Op, imm: u8]               acc = acc << imm
    ShiftRightSmi,          // [Op, imm: u8]               acc = acc >> imm
    ShiftRightUnsignedSmi,  // [Op, imm: u8]               acc = acc >>> imm

    // Control Flow
    Jump,                   // [Op, offset: i32]           pc += offset
    JumpIfTrue,             // [Op, offset: i32]           if (ToBoolean(acc)) pc += offset
    JumpIfFalse,            // [Op, offset: i32]           if (!ToBoolean(acc)) pc += offset
    JumpIfNotNullish,       // [Op, offset: i32]           if (!acc.IsNull() && !acc.IsUndefined()) pc += offset
    JumpIfNullish,          // [Op, offset: i32]           if (acc.IsNull() || acc.IsUndefined()) pc += offset
    JumpIfUndefined,        // [Op, offset: i32]           if (acc.IsUndefined()) pc += offset
    JumpIfNotUndefined,     // [Op, offset: i32]           if (!acc.IsUndefined()) pc += offset
    JumpIfNull,             // [Op, offset: i32]           if (acc.IsNull()) pc += offset
    JumpIfNotNull,          // [Op, offset: i32]           if (!acc.IsNull()) pc += offset

    // Object & Property Access (Inline Cache enabled)
    LdaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  acc = obj.name
    StaNamed,           // [Op, obj: u8, name_idx: u16, slot: u16]  obj.name = acc
    LdaPrivate,         // [Op, obj: u8, name_idx: u16]              acc = obj.#name
    StaPrivate,         // [Op, obj: u8, name_idx: u16]              obj.#name = acc
    LdaKeyed,           // [Op, obj: u8, key: u8, slot: u16]        acc = obj[key]
    StaKeyed,           // [Op, obj: u8, key: u8, slot: u16]        obj[key] = acc
    DeleteNamed,        // [Op, obj: u8, name_idx: u16]             acc = delete obj.name
    DeleteKeyed,        // [Op, obj: u8, key: u8]                   acc = delete obj[key]
    DeleteGlobal,       // [Op, name_idx: u16]                      acc = delete global[name]
    DefineGetter,       // [Op, obj: u8, name_idx: u16]             define getter obj.name = acc
    DefineSetter,       // [Op, obj: u8, name_idx: u16]             define setter obj.name = acc
    DefineGetterKeyed,  // [Op, obj: u8, key: u8]                   define getter obj[key] = acc
    DefineSetterKeyed,  // [Op, obj: u8, key: u8]                   define setter obj[key] = acc

    // Unary Operations
    LogicalNot,             // [Op]                    acc = !ToBoolean(acc)
    Negate,                 // [Op]                    acc = -acc
    BitwiseNot,             // [Op]                    acc = ~acc
    TypeOf,                 // [Op]                    acc = typeof acc
    TypeOfGlobal,           // [Op, name_idx: u16]     acc = typeof global[name]
    LdaGlobal,              // [Op, name_idx: u16]     acc = global[name] (throws ReferenceError)
    StaGlobal,              // [Op, name_idx: u16]     global[name] = acc
    ToString,               // [Op]                    acc = ToString(acc)
    ToNumber,               // [Op]                    acc = ToNumber(acc)
    ToNumeric,              // [Op]                    acc = ToNumeric(acc)
    ToPropertyKey,          // [Op]                    acc = ToPropertyKey(acc)
    CheckObjectCoercible,   // [Op, reg: u8]           throws TypeError if reg is null or undefined
    PushWith,               // [Op, reg: u8]           push with_stack
    PopWith,                // [Op]                    pop with_stack
    Inc,                    // [Op]                    acc = Inc(acc)
    Dec,                    // [Op]                    acc = Dec(acc)

    // Function Calls & Object Instantiation
    Call,                   // [Op, callee: u8, args: u8, count: u8, slot: u16]
    CallDirectEval,         // [Op, callee: u8, args: u8, count: u8, slot: u16]
    CallMethod,             // [Op, callee: u8, this: u8, args: u8, count: u8, slot: u16]
    CallSuper,              // [Op, callee: u8, args: u8, count: u8, slot: u16]
    CallWithSpread,         // [Op, callee: u8, arg_array: u8, slot: u16]
    CallMethodWithSpread,   // [Op, callee: u8, this: u8, arg_array: u8, slot: u16]
    CreateObject,           // [Op]                    acc = new JSObject()
    CreateArray,            // [Op, start_reg: u8, count: u16] acc = new JSArray(...)
    Construct,              // [Op, ctor: u8, args: u8, count: u8, slot: u16]
    LdaThis,                // [Op]                    acc = this
    ArrayPush,              // [Op, arr_reg: u8, val_reg: u8]  arr.push(val)
    ArraySpread,            // [Op, target_reg: u8, src_reg: u8] arr.push(...src)
    ObjectSpread,           // [Op, target_reg: u8, src_reg: u8] Object.assign(target, src)
    CreateRestArray,        // [Op, dest_reg: u8, start_idx: u16] dest = args.slice(start_idx)
    CreateArguments,        // [Op, dest_reg: u8] dest = arguments object
    CreateEmptyArrayLiteral,// [Op]                    acc = new JSArray()
    CreateRegExpLiteral,    // [Op, pattern_idx: u16, flags_idx: u16] acc = new RegExp(...)

    // Closures & Lexical Scopes
    CreateClosure,          // [Op, const_idx: u16]    acc = new JSFunction(const[idx], env)
    CreateFunctionContext,  // [Op, slot_count: u16]   env = new LexicalEnvironment(env, slot_count)
    LdaContextSlot,         // [Op, depth: u8, slot: u16] acc = env.Get(depth, slot)
    StaContextSlot,         // [Op, depth: u8, slot: u16] env.Set(depth, slot, acc)
    LdaCurrentContextSlot,  // [Op, slot: u16]         acc = env.Get(0, slot)
    StaCurrentContextSlot,  // [Op, slot: u16]         env.Set(0, slot, acc)
    LdaImmutableContextSlot,// [Op, depth: u8, slot: u16] acc = env.Get(depth, slot)

    // Iteration & Property Enumeration
    ForInEnumerate,         // [Op, obj_reg: u8]       acc = EnumerateKeys(Reg[obj_reg])
    ForInNext,              // [Op, obj_reg: u8, enum_reg: u8, idx_reg: u8] acc = enum[idx]

    // Exception Handling
    PushExceptionHandler,   // [Op, offset: i32]       push catch handler
    PopExceptionHandler,    // [Op]                    pop catch handler
    Throw,                  // [Op]                    throw acc

    // Execution & Control
    Yield,              // [Op]                        suspend and yield acc
    Await,              // [Op]                        suspend async coroutine for await acc
    Debugger,           // [Op]                        debugger breakpoint hook
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

    TypeFeedbackVector(const TypeFeedbackVector& other) {
        std::lock_guard<std::mutex> lock(other.mutex_);
        slots_ = other.slots_;
    }

    TypeFeedbackVector& operator=(const TypeFeedbackVector& other) {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            slots_ = other.slots_;
        }
        return *this;
    }

    TypeFeedbackVector(TypeFeedbackVector&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        slots_ = std::move(other.slots_);
    }

    TypeFeedbackVector& operator=(TypeFeedbackVector&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            slots_ = std::move(other.slots_);
        }
        return *this;
    }

    std::unique_ptr<TypeFeedbackVector> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto snap = std::make_unique<TypeFeedbackVector>(slots_.size());
        snap->slots_ = slots_;
        return snap;
    }

    FeedbackSlot& GetSlot(size_t idx) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idx >= slots_.size()) {
            slots_.resize(idx + 1);
        }
        return slots_[idx];
    }

    const FeedbackSlot& GetSlot(size_t idx) const {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(idx < slots_.size());
        return slots_[idx];
    }

    size_t Size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<FeedbackSlot> slots_;
};

/**
 * @brief Represents a compiled bytecode routine along with constant pool and metadata.
 */
class BytecodeArray : public std::enable_shared_from_this<BytecodeArray> {
public:
    BytecodeArray(uint32_t register_count = 0, uint32_t parameter_count = 0)
        : register_count_(register_count), parameter_count_(parameter_count) {}

    BytecodeArray(BytecodeArray&& other) noexcept
        : bytes_(std::move(other.bytes_)),
          constants_(std::move(other.constants_)),
          register_count_(other.register_count_),
          parameter_count_(other.parameter_count_),
          length_(other.length_),
          is_strict_(other.is_strict_),
          invocation_count_(other.invocation_count_.load(std::memory_order_relaxed)),
          is_compiling_tier2_(other.is_compiling_tier2_.load(std::memory_order_relaxed)),
          feedback_(std::move(other.feedback_)),
          register_scope_(std::move(other.register_scope_)),
          context_slots_(std::move(other.context_slots_)) {
        std::lock_guard<std::mutex> lock(other.tier_mutex_);
        tier1_code_ = std::move(other.tier1_code_);
        tier2_code_ = std::move(other.tier2_code_);
    }

    BytecodeArray& operator=(BytecodeArray&& other) noexcept {
        if (this != &other) {
            bytes_ = std::move(other.bytes_);
            constants_ = std::move(other.constants_);
            register_count_ = other.register_count_;
            parameter_count_ = other.parameter_count_;
            length_ = other.length_;
            is_strict_ = other.is_strict_;
            invocation_count_.store(other.invocation_count_.load(std::memory_order_relaxed));
            is_compiling_tier2_.store(other.is_compiling_tier2_.load(std::memory_order_relaxed));
            feedback_ = std::move(other.feedback_);
            register_scope_ = std::move(other.register_scope_);
            context_slots_ = std::move(other.context_slots_);
            std::scoped_lock lock(tier_mutex_, other.tier_mutex_);
            tier1_code_ = std::move(other.tier1_code_);
            tier2_code_ = std::move(other.tier2_code_);
        }
        return *this;
    }

    std::unique_ptr<BytecodeArray> Clone() const {
        auto copy = std::make_unique<BytecodeArray>(register_count_, parameter_count_);
        copy->bytes_ = bytes_;
        copy->constants_ = constants_;
        copy->length_ = length_;
        copy->is_strict_ = is_strict_;
        copy->register_scope_ = register_scope_;
        copy->context_slots_ = context_slots_;
        copy->invocation_count_.store(invocation_count_.load(std::memory_order_relaxed));
        copy->is_compiling_tier2_.store(is_compiling_tier2_.load(std::memory_order_relaxed));
        auto snap = feedback_.Snapshot();
        if (snap) copy->feedback_ = *snap;
        std::lock_guard<std::mutex> lock(tier_mutex_);
        copy->tier1_code_ = tier1_code_;
        copy->tier2_code_ = tier2_code_;
        return copy;
    }

    std::shared_ptr<const BytecodeArray> GetShared() const {
        try {
            return shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            return std::shared_ptr<const BytecodeArray>(Clone().release());
        }
    }

    BytecodeArray(const BytecodeArray&) = delete;
    BytecodeArray& operator=(const BytecodeArray&) = delete;

    const std::vector<uint8_t>& Code() const noexcept { return bytes_; }
    const std::vector<TaggedValue>& Constants() const noexcept { return constants_; }
    uint32_t RegisterCount() const noexcept { return register_count_; }
    uint32_t ParameterCount() const noexcept { return parameter_count_; }
    uint32_t Length() const noexcept { return length_; }
    bool IsStrict() const noexcept { return is_strict_; }

    void SetRegisterCount(uint32_t count) noexcept { register_count_ = count; }
    void SetParameterCount(uint32_t count) noexcept { 
        parameter_count_ = count; 
        if (length_ == 0) length_ = count;
    }
    void SetLength(uint32_t len) noexcept { length_ = len; }
    void SetStrict(bool s) noexcept { is_strict_ = s; }

    const std::unordered_map<std::string, uint8_t>& GetRegisterScope() const noexcept { return register_scope_; }
    void SetRegisterScope(std::unordered_map<std::string, uint8_t> s) { register_scope_ = std::move(s); }

    const std::unordered_map<std::string, uint16_t>& GetContextSlots() const noexcept { return context_slots_; }
    void SetContextSlots(std::unordered_map<std::string, uint16_t> cs) { context_slots_ = std::move(cs); }

    // Emission primitives
    size_t CurrentOffset() const noexcept { return bytes_.size(); }

    void EmitByte(uint8_t b) { bytes_.push_back(b); }
    void EmitOpcode(Opcode op) { bytes_.push_back(static_cast<uint8_t>(op)); }
    void EmitU8(uint8_t u8) { bytes_.push_back(u8); }
    void EmitU16(uint16_t u16);
    void EmitI32(int32_t i32);

    void PatchI32(size_t offset, int32_t val);

    uint16_t AddConstant(TaggedValue val);

    void IterateConstants(const std::function<void(TaggedValue&)>& visitor) {
        for (auto& c : constants_) {
            if (c.IsHeapObject()) {
                visitor(c);
            }
        }
    }

    // Disassembly for verification and debugging
    std::string Disassemble() const;

    // JIT Tiering & Invocation Profiling
    uint32_t InvocationCount() const noexcept { return invocation_count_.load(std::memory_order_relaxed); }
    void IncrementInvocationCount() const noexcept { invocation_count_.fetch_add(1, std::memory_order_relaxed); }
    void ResetInvocationCount() const noexcept { invocation_count_.store(0, std::memory_order_relaxed); }

    const ExecutableMemory* GetTier1Code() const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        return tier1_code_.get();
    }
    std::shared_ptr<ExecutableMemory> GetTier1SharedCode() const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        return tier1_code_;
    }
    void SetTier1Code(std::shared_ptr<ExecutableMemory> code) const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        tier1_code_ = std::move(code);
    }

    const ExecutableMemory* GetTier2Code() const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        return tier2_code_.get();
    }
    std::shared_ptr<ExecutableMemory> GetTier2SharedCode() const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        return tier2_code_;
    }
    void SetTier2Code(std::shared_ptr<ExecutableMemory> code) const noexcept {
        std::lock_guard<std::mutex> lock(tier_mutex_);
        tier2_code_ = std::move(code);
        is_compiling_tier2_.store(false, std::memory_order_release);
    }

    bool TryBeginTier2Compilation() const noexcept {
        bool expected = false;
        return is_compiling_tier2_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void FinishTier2Compilation() const noexcept {
        is_compiling_tier2_.store(false, std::memory_order_release);
    }
    bool IsCompilingTier2() const noexcept {
        return is_compiling_tier2_.load(std::memory_order_acquire);
    }

    TypeFeedbackVector& GetFeedbackVector() const noexcept { return feedback_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<TaggedValue> constants_;
    uint32_t register_count_{0};
    uint32_t parameter_count_{0};
    uint32_t length_{0};
    bool is_strict_{false};
    mutable std::atomic<uint32_t> invocation_count_{0};
    mutable std::atomic<bool> is_compiling_tier2_{false};
    mutable std::mutex tier_mutex_;
    mutable std::shared_ptr<ExecutableMemory> tier1_code_{nullptr};
    mutable std::shared_ptr<ExecutableMemory> tier2_code_{nullptr};
    mutable TypeFeedbackVector feedback_;
    std::unordered_map<std::string, uint8_t> register_scope_;
    std::unordered_map<std::string, uint16_t> context_slots_;
};

} // namespace aether

#endif // AETHER_BYTECODE_H
