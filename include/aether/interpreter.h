#ifndef AETHER_INTERPRETER_H
#define AETHER_INTERPRETER_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/heap_object.h"
#include <vector>

namespace aether {

/**
 * @brief Feedback slot record for Inline Caching (IC).
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
            if (val.AsHeapObject()->IsString()) {
                observed_type |= static_cast<uint8_t>(ObservedType::String);
            } else {
                observed_type |= static_cast<uint8_t>(ObservedType::Object);
            }
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

private:
    std::vector<FeedbackSlot> slots_;
};

/**
 * @brief ExecutionFrame representing an activation record on the virtual stack.
 */
class ExecutionFrame {
public:
    ExecutionFrame(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args)
        : bytecode_(bytecode),
          registers_(bytecode.RegisterCount(), TaggedValue::Undefined()) {
        // Copy incoming arguments into registers if specified
        for (size_t i = 0; i < args.size() && i < registers_.size(); ++i) {
            registers_[i] = args[i];
        }
    }

    AETHER_ALWAYS_INLINE TaggedValue GetRegister(uint8_t reg) const noexcept {
        assert(reg < registers_.size());
        return registers_[reg];
    }

    AETHER_ALWAYS_INLINE void SetRegister(uint8_t reg, TaggedValue val) noexcept {
        assert(reg < registers_.size());
        registers_[reg] = val;
    }

    const BytecodeArray& GetBytecode() const noexcept { return bytecode_; }

private:
    const BytecodeArray& bytecode_;
    std::vector<TaggedValue> registers_;
};

/**
 * @brief Tier 0 Bytecode Interpreter with Accumulator architecture and Inline Caches.
 */
class Interpreter {
public:
    Interpreter() = default;

    /**
     * @brief Executes a bytecode array and returns the resulting TaggedValue.
     */
    TaggedValue Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args = {});

    TypeFeedbackVector& GetFeedbackVector() noexcept { return feedback_; }

    Shape* GetRootShape() {
        if (!root_shape_) {
            root_shape_ = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());
        }
        return root_shape_.get();
    }

private:
    TaggedValue acc_{TaggedValue::Undefined()};
    TypeFeedbackVector feedback_;
    uint32_t next_shape_id_{1};
    std::unique_ptr<Shape> root_shape_;
};

} // namespace aether

#endif // AETHER_INTERPRETER_H
