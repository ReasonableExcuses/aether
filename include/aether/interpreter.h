#ifndef AETHER_INTERPRETER_H
#define AETHER_INTERPRETER_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/heap_object.h"
#include <vector>

namespace aether {


struct ExceptionHandler {
    const uint8_t* target_pc{nullptr};
    LexicalEnvironment* env{nullptr};
};

/**
 * @brief ExecutionFrame representing an activation record on the virtual stack.
 */
class ExecutionFrame {
public:
    ExecutionFrame(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args, TaggedValue this_val = TaggedValue::Undefined(), LexicalEnvironment* env = nullptr)
        : bytecode_(bytecode),
          this_value_(this_val),
          environment_(env),
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

    std::vector<TaggedValue>& GetRegisters() noexcept { return registers_; }
    const std::vector<TaggedValue>& GetRegisters() const noexcept { return registers_; }

    TaggedValue GetThis() const noexcept { return this_value_; }
    void SetThis(TaggedValue val) noexcept { this_value_ = val; }

    LexicalEnvironment* GetEnvironment() const noexcept { return environment_; }
    void SetEnvironment(LexicalEnvironment* env) noexcept { environment_ = env; }

    void PushExceptionHandler(const uint8_t* target, LexicalEnvironment* env) {
        handlers_.push_back({target, env});
    }

    bool HasExceptionHandler() const noexcept {
        return !handlers_.empty();
    }

    ExceptionHandler PopExceptionHandler() {
        assert(!handlers_.empty());
        ExceptionHandler h = handlers_.back();
        handlers_.pop_back();
        return h;
    }

    const BytecodeArray& GetBytecode() const noexcept { return bytecode_; }

private:
    const BytecodeArray& bytecode_;
    TaggedValue this_value_{TaggedValue::Undefined()};
    LexicalEnvironment* environment_{nullptr};
    std::vector<TaggedValue> registers_;
    std::vector<ExceptionHandler> handlers_;
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
    TaggedValue Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args = {}, TaggedValue this_val = TaggedValue::Undefined(), LexicalEnvironment* env = nullptr);

    TypeFeedbackVector& GetFeedbackVector() noexcept { return feedback_; }

    Shape* GetRootShape() {
        if (!root_shape_) {
            root_shape_ = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());
        }
        return root_shape_.get();
    }

    bool HasPendingException() const noexcept { return has_pending_exception_; }
    TaggedValue PendingException() const noexcept { return pending_exception_; }
    void ClearPendingException() noexcept {
        has_pending_exception_ = false;
        pending_exception_ = TaggedValue::Undefined();
    }

    static Interpreter* Current() noexcept { return s_current_interpreter_; }
    static void SetCurrent(Interpreter* interp) noexcept { s_current_interpreter_ = interp; }
    LexicalEnvironment* GetLastEnvironment() const noexcept { return last_environment_; }

private:
    static inline thread_local Interpreter* s_current_interpreter_{nullptr};
    friend struct InterpreterScope;
    TaggedValue acc_{TaggedValue::Undefined()};
    TypeFeedbackVector feedback_;
    uint32_t next_shape_id_{1};
    std::unique_ptr<Shape> root_shape_;
    bool has_pending_exception_{false};
    TaggedValue pending_exception_{TaggedValue::Undefined()};
    LexicalEnvironment* last_environment_{nullptr};
};

} // namespace aether

#endif // AETHER_INTERPRETER_H
