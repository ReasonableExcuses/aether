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

/**
 * @brief Managed Host Native Function callable from ECMAScript.
 */
class JSNativeFunction : public HeapObject {
public:
    JSNativeFunction(std::string_view name, NativeCallback callback, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::JSNativeFunction, shape),
          name_(name),
          callback_(callback) {}
    JSNativeFunction(JSNativeFunction&& other) noexcept = default;

    const std::string& Name() const noexcept { return name_; }
    NativeCallback Callback() const noexcept { return callback_; }

    size_t ObjectSize() const override { return sizeof(JSNativeFunction); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override { (void)visitor; }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSNativeFunction(std::move(*this));
    }

private:
    std::string name_;
    NativeCallback callback_;
};

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

    JSObject* GetGlobalObject() noexcept { return global_object_; }
    TaggedValue GetGlobalProperty(std::string_view name);
    void SetGlobalProperty(std::string_view name, TaggedValue value);

    Shape* GetRootShape() noexcept { return root_shape_.get(); }

private:
    void InitializeIntrinsics(Heap& heap);

    std::unique_ptr<Shape> root_shape_;
    uint32_t next_shape_id_{1};
    JSObject* global_object_{nullptr};
    JSObject* math_object_{nullptr};
    JSObject* console_object_{nullptr};
};

/**
 * @brief ECMAScript Agent encapsulating Isolate execution context and JIT runtime.
 */
class Agent {
public:
    Agent();
    ~Agent() = default;

    /**
     * @brief Evaluates an ECMAScript script string end-to-end through the AetherVM engine.
     */
    TaggedValue EvaluateScript(std::string_view source);

    Realm& GetRealm() noexcept { return *realm_; }
    Heap& GetHeap() noexcept { return *heap_; }
    Interpreter& GetInterpreter() noexcept { return interpreter_; }
    MicrotaskQueue& GetMicrotasks() noexcept { return microtasks_; }

private:
    std::unique_ptr<Heap> heap_;
    std::unique_ptr<Realm> realm_;
    Interpreter interpreter_;
    MicrotaskQueue microtasks_;
};

} // namespace aether

#endif // AETHER_RUNTIME_H
