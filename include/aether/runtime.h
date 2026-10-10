#ifndef AETHER_RUNTIME_H
#define AETHER_RUNTIME_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/heap.h"
#include "aether/interpreter.h"
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

namespace aether {

using NativeCallback = TaggedValue (*)(const std::vector<TaggedValue>& args);
using NativeMethodCallback = TaggedValue (*)(TaggedValue this_val, const std::vector<TaggedValue>& args);

/**
 * @brief Managed Host Native Function callable from ECMAScript.
 */
class JSNativeFunction : public JSObject {
public:
    using CallbackType = std::function<TaggedValue(const std::vector<TaggedValue>&)>;
    using MethodCallbackType = std::function<TaggedValue(TaggedValue, const std::vector<TaggedValue>&)>;

    JSNativeFunction(std::string_view name, CallbackType callback, Shape* shape = nullptr);
    JSNativeFunction(std::string_view name, MethodCallbackType method_callback, Shape* shape = nullptr);

    JSNativeFunction(JSNativeFunction&& other) noexcept = default;

    const std::string& Name() const noexcept { return name_; }
    const CallbackType& Callback() const noexcept { return callback_; }
    const MethodCallbackType& MethodCallback() const noexcept { return method_callback_; }
    bool IsMethodCallback() const noexcept { return static_cast<bool>(method_callback_); }

    TaggedValue Invoke(TaggedValue this_val, const std::vector<TaggedValue>& args) const {
        if (method_callback_) {
            return method_callback_(this_val, args);
        }
        if (callback_) {
            return callback_(args);
        }
        return TaggedValue::Undefined();
    }

    size_t ObjectSize() const override { return sizeof(JSNativeFunction); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSNativeFunction(std::move(*this));
    }

private:
    std::string name_;
    CallbackType callback_{nullptr};
    MethodCallbackType method_callback_{nullptr};
};

TaggedValue CallFunction(TaggedValue callee, TaggedValue this_val, const std::vector<TaggedValue>& args);

/**
 * @brief ECMA-262 Microtask Queue processing asynchronous Promise reactions.
 */
class MicrotaskQueue {
public:
    MicrotaskQueue() = default;

    void Enqueue(std::function<void()> task) {
        queue_.push_back(std::move(task));
    }

    void PerformMicrotaskCheckpoint() {
        while (!queue_.empty()) {
            auto task = std::move(queue_.front());
            queue_.pop_front();
            task();
        }
    }

    size_t Size() const noexcept { return queue_.size(); }
    bool Empty() const noexcept { return queue_.empty(); }

private:
    std::deque<std::function<void()>> queue_;
};

/**
 * @brief Execution Realm containing intrinsic objects and global lexical environment.
 */
class Realm {
public:
    explicit Realm(Heap& heap);

    static Realm* Current() noexcept { return s_current_realm_; }
    static void SetCurrent(Realm* realm) noexcept { s_current_realm_ = realm; }

    JSObject* GetGlobalObject() noexcept { return global_object_; }
    TaggedValue GetGlobalProperty(std::string_view name);
    void SetGlobalProperty(std::string_view name, TaggedValue value);

    Shape* GetRootShape() noexcept { return root_shape_.get(); }

    JSObject* GetArrayPrototype() const noexcept { return array_prototype_; }
    JSObject* GetStringPrototype() const noexcept { return string_prototype_; }
    JSObject* GetMapPrototype() const noexcept { return map_prototype_; }
    JSObject* GetSetPrototype() const noexcept { return set_prototype_; }
    JSObject* GetPromisePrototype() const noexcept { return promise_prototype_; }
    JSObject* GetRegExpPrototype() const noexcept { return regexp_prototype_; }
    JSObject* GetJSONObject() const noexcept { return json_object_; }

private:
    void InitializeIntrinsics(Heap& heap);

    static inline thread_local Realm* s_current_realm_{nullptr};

    std::unique_ptr<Shape> root_shape_;
    uint32_t next_shape_id_{1};
    JSObject* global_object_{nullptr};
    JSObject* math_object_{nullptr};
    JSObject* console_object_{nullptr};
    JSObject* json_object_{nullptr};
    JSObject* array_prototype_{nullptr};
    JSObject* string_prototype_{nullptr};
    JSObject* map_prototype_{nullptr};
    JSObject* set_prototype_{nullptr};
    JSObject* promise_prototype_{nullptr};
    JSObject* regexp_prototype_{nullptr};
};

inline JSNativeFunction::JSNativeFunction(std::string_view name, CallbackType callback, Shape* shape)
    : JSObject(HeapObjectType::JSNativeFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(name),
      callback_(std::move(callback)),
      method_callback_(nullptr) {}

inline JSNativeFunction::JSNativeFunction(std::string_view name, MethodCallbackType method_callback, Shape* shape)
    : JSObject(HeapObjectType::JSNativeFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(name),
      callback_(nullptr),
      method_callback_(std::move(method_callback)) {}

class EventLoop;

/**
 * @brief Invokes a JavaScript or native function with the given receiver and argument list.
 */
TaggedValue CallFunction(TaggedValue callee, TaggedValue this_val, const std::vector<TaggedValue>& args);

/**
 * @brief ECMAScript Agent encapsulating Isolate execution context and JIT runtime.
 */
class Agent {
public:
    Agent();
    ~Agent();

    /**
     * @brief Evaluates an ECMAScript script string end-to-end through the AetherVM engine.
     */
    TaggedValue EvaluateScript(std::string_view source);

    /**
     * @brief Evaluates a script and runs the event loop until all tasks, timers, and microtasks complete.
     */
    TaggedValue EvaluateScriptWithEventLoop(std::string_view source);

    /**
     * @brief Runs the asynchronous event loop until all pending timers, intervals, and microtasks drain.
     */
    void RunEventLoop();

    Realm& GetRealm() noexcept { return *realm_; }
    Heap& GetHeap() noexcept { return *heap_; }
    Interpreter& GetInterpreter() noexcept { return interpreter_; }
    MicrotaskQueue& GetMicrotasks() noexcept { return microtasks_; }
    EventLoop& GetEventLoop() noexcept { return *event_loop_; }

private:
    std::unique_ptr<Heap> heap_;
    std::unique_ptr<Realm> realm_;
    Interpreter interpreter_;
    MicrotaskQueue microtasks_;
    std::unique_ptr<EventLoop> event_loop_;
};

} // namespace aether

#endif // AETHER_RUNTIME_H
