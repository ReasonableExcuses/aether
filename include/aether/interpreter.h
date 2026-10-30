#ifndef AETHER_INTERPRETER_H
#define AETHER_INTERPRETER_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/heap_object.h"
#include <vector>

namespace aether {

class HandleScope;
class IsolateHeap;

struct ExceptionHandler {
    const uint8_t* target_pc{nullptr};
    LexicalEnvironment* env{nullptr};
    size_t with_depth{0};
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
          registers_(bytecode.RegisterCount(), TaggedValue::Undefined()),
          args_(args) {
        // Copy incoming arguments into registers if specified
        for (size_t i = 0; i < args.size() && i < registers_.size(); ++i) {
            registers_[i] = args[i];
        }
        for (const auto& lex_name : bytecode.GetLexicalNames()) {
            auto it = bytecode.GetRegisterScope().find(lex_name);
            if (it != bytecode.GetRegisterScope().end() && it->second < registers_.size()) {
                registers_[it->second] = TaggedValue::Empty();
            }
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

    void PushExceptionHandler(const uint8_t* target, LexicalEnvironment* env, size_t with_depth = 0);
    bool HasExceptionHandler() const noexcept {
        return !handlers_.empty();
    }
    ExceptionHandler PopExceptionHandler();

    std::vector<ExceptionHandler>& Handlers() noexcept { return handlers_; }
    const std::vector<ExceptionHandler>& Handlers() const noexcept { return handlers_; }

    const BytecodeArray& GetBytecode() const noexcept { return bytecode_; }
    const std::vector<TaggedValue>& Args() const noexcept { return args_; }

    JSGeneratorObject* GetGenerator() const noexcept { return generator_; }
    void SetGenerator(JSGeneratorObject* gen) noexcept { generator_ = gen; }

    ExecutionFrame* GetPrevFrame() const noexcept { return prev_frame_; }
    void SetPrevFrame(ExecutionFrame* prev) noexcept { prev_frame_ = prev; }

private:
    const BytecodeArray& bytecode_;
    TaggedValue this_value_{TaggedValue::Undefined()};
    LexicalEnvironment* environment_{nullptr};
    std::vector<TaggedValue> registers_;
    std::vector<ExceptionHandler> handlers_;
    const std::vector<TaggedValue>& args_;
    JSGeneratorObject* generator_{nullptr};
    ExecutionFrame* prev_frame_{nullptr};
};

/**
 * @brief Tier 0 Bytecode Interpreter with Accumulator architecture and Inline Caches.
 */
class Interpreter {
public:
    Interpreter() = default;
    ~Interpreter();
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    void UnregisterRoots() noexcept;

    // Call at Agent startup, with its isolate current, before imposing a small
    // budget. Execute also initializes lazily for standalone interpreters.
    void InitializeResourceExceptions();
    void SetResourceException(TaggedValue exception);
    void ReportResourceError(std::string_view message = "Memory budget exceeded") noexcept;

    /**
     * @brief Executes a bytecode array and returns the resulting TaggedValue.
     */
    TaggedValue Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args = {}, TaggedValue this_val = TaggedValue::Undefined(), LexicalEnvironment* env = nullptr);

    /**
     * @brief Resumes a suspended JSGeneratorObject with the given sent value.
     */
    TaggedValue ResumeGenerator(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection = false, bool* out_is_await = nullptr);

    /**
     * @brief Drives an async coroutine step upon microtask resolution or initial call.
     */
    void RunAsyncStep(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection = false);

    /**
     * @brief Drives an async generator coroutine step upon microtask resolution or next() call.
     */
    void RunAsyncGeneratorStep(JSGeneratorObject* gen);
    void ExecuteAsyncGeneratorRequest(JSGeneratorObject* gen, TaggedValue resume_val = TaggedValue(), bool is_rejection = false);

    TypeFeedbackVector& GetFeedbackVector() noexcept { return feedback_; }

    Shape* GetRootShape() {
        if (!root_shape_) {
            root_shape_ = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());
        }
        return root_shape_.get();
    }

    bool HasPendingException() const noexcept { return has_pending_exception_; }
    TaggedValue PendingException() const noexcept { return pending_exception_; }
    void SetPendingException(TaggedValue exc) noexcept {
        has_pending_exception_ = true;
        pending_exception_ = exc;
    }
    void ClearPendingException() noexcept {
        has_pending_exception_ = false;
        pending_exception_ = TaggedValue::Undefined();
    }

    TaggedValue ToPrimitive(TaggedValue input, const char* hint = "default");
    TaggedValue ToStringOp(TaggedValue val);
    TaggedValue ToNumeric(TaggedValue val);
    TaggedValue ToPropertyKey(TaggedValue val);

    enum class RelationalResult {
        True,
        False,
        Undefined
    };
    RelationalResult AbstractRelationalComparison(TaggedValue x, TaggedValue y, bool left_first);

    bool IsStrictlyEqual(TaggedValue x, TaggedValue y);
    bool IsLooselyEqual(TaggedValue x, TaggedValue y);

    static Interpreter* Current() noexcept { return s_current_interpreter_; }
    static void SetCurrent(Interpreter* interp) noexcept { s_current_interpreter_ = interp; }
    LexicalEnvironment* GetLastEnvironment() const noexcept {
        return last_environment_root_.IsHeapObject() ? last_environment_root_.As<LexicalEnvironment>() : nullptr;
    }
    TaggedValue GetNewTarget() const noexcept { return current_new_target_; }
    void SetNewTarget(TaggedValue target) noexcept { current_new_target_ = target; }
    bool IsStrictMode() const noexcept { return is_strict_mode_; }
    void SetStrictMode(bool s) noexcept { is_strict_mode_ = s; }
    const std::vector<JSObject*>& GetWithStack() const noexcept { return with_stack_; }
    void PushWith(JSObject* obj) { with_stack_.push_back(obj); }
    void PopWith() { if (!with_stack_.empty()) with_stack_.pop_back(); }
    void PopWithToDepth(size_t depth) { if (depth < with_stack_.size()) with_stack_.resize(depth); }
    ExecutionFrame* GetCurrentFrame() const noexcept { return current_frame_; }
    void IterateRoots(SlotVisitor* visitor);

private:
    static inline thread_local Interpreter* s_current_interpreter_{nullptr};
    friend struct InterpreterScope;
    std::vector<JSObject*> with_stack_;
    TaggedValue acc_{TaggedValue::Undefined()};
    TaggedValue current_new_target_{TaggedValue::Undefined()};
    TypeFeedbackVector feedback_;
    uint32_t next_shape_id_{1};
    std::unique_ptr<Shape> root_shape_;
    bool has_pending_exception_{false};
    TaggedValue pending_exception_{TaggedValue::Undefined()};
    TaggedValue last_environment_root_{TaggedValue::Undefined()};
    bool is_strict_mode_{false};
    JSObject* last_lookup_base_{nullptr};
    std::string last_lookup_name_{};
    int last_lookup_reg_{-1};
    void ClearLastLookup() noexcept {
        last_lookup_base_ = nullptr;
        last_lookup_name_.clear();
        last_lookup_reg_ = -1;
    }
    uint32_t call_depth_{0};
    static constexpr uint32_t kMaxCallDepth = 256;
    IsolateHeap* root_heap_{nullptr};
    TaggedValue resource_exception_{TaggedValue::Undefined()};
    TaggedValue stack_exception_{TaggedValue::Undefined()};
    TaggedValue last_result_{TaggedValue::Undefined()};
    HandleScope* instruction_roots_{nullptr};
    void EnsureRoots();
    void SnapshotState(HandleScope& roots, const ExecutionFrame* frame = nullptr);
    TaggedValue PublishResult(TaggedValue result);
    TaggedValue ExecuteImpl(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args, TaggedValue this_val, LexicalEnvironment* env);
    TaggedValue ResumeGeneratorImpl(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection, bool* out_is_await);
    ExecutionFrame* top_level_frame_{nullptr};
    ExecutionFrame* current_frame_{nullptr};
    TaggedValue RunExecutionLoop(ExecutionFrame& frame, const uint8_t* start_pc, bool* out_yielded = nullptr, bool* out_is_await = nullptr);
};

inline void ExecutionFrame::PushExceptionHandler(const uint8_t* target, LexicalEnvironment* env, size_t with_depth) {
    if (with_depth == 0 && Interpreter::Current()) {
        with_depth = Interpreter::Current()->GetWithStack().size();
    }
    handlers_.push_back({target, env, with_depth});
}

inline ExceptionHandler ExecutionFrame::PopExceptionHandler() {
    assert(!handlers_.empty());
    ExceptionHandler h = handlers_.back();
    handlers_.pop_back();
    if (Interpreter::Current() && h.with_depth <= Interpreter::Current()->GetWithStack().size()) {
        Interpreter::Current()->PopWithToDepth(h.with_depth);
    }
    return h;
}

} // namespace aether

#endif // AETHER_INTERPRETER_H
