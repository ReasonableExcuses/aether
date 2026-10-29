#ifndef AETHER_ADVANCED_INTRINSICS_H
#define AETHER_ADVANCED_INTRINSICS_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/typed_array.h"
#include "aether/runtime.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <string_view>
#include <memory>
#include <algorithm>
#include <cmath>

namespace aether {

/**
 * @brief ECMAScript WeakMap collection with ephemeron/weak key referencing.
 */
class JSWeakMap : public JSObject {
public:
    using EntryStorage = std::unordered_map<HeapObject*, TaggedValue,
        std::hash<HeapObject*>, std::equal_to<HeapObject*>,
        detail::StorageAllocator<std::pair<HeapObject* const, TaggedValue>>>;
    explicit JSWeakMap(Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSWeakMap, shape) {}

    JSWeakMap(JSWeakMap&& other) noexcept
        : JSObject(std::move(other)),
          storage_charge_(std::move(other.storage_charge_)),
          entries_(std::move(other.entries_)) {}

    size_t Size() const noexcept { return entries_->size(); }

    void Set(HeapObject* key, TaggedValue val) {
        if (!entries_->contains(key) && entries_->size() >= kMaxObjectProperties) {
            throw std::length_error("WeakMap size limit exceeded");
        }
        (*entries_)[key] = val;
    }

    TaggedValue Get(HeapObject* key) const {
        auto it = entries_->find(key);
        if (it != entries_->end()) {
            return it->second;
        }
        return TaggedValue::Undefined();
    }

    bool Has(HeapObject* key) const {
        return entries_->find(key) != entries_->end();
    }

    bool Delete(HeapObject* key) {
        return entries_->erase(key) > 0;
    }

    void Clear() {
        entries_->clear();
    }

    const EntryStorage& Entries() const noexcept {
        return *entries_;
    }

    size_t ObjectSize() const override {
        return sizeof(JSWeakMap);
    }

    void IteratePointers(SlotVisitor* visitor) override {
        JSObject::IteratePointers(visitor);
        // Both halves of an ephemeron are handled by the collector's fixed
        // point pass: a value is strong only once its key is known to be live.
    }

    template <typename AlivePredicate>
    void TraceEphemerons(AlivePredicate&& is_alive, SlotVisitor* visitor) {
        for (auto& [key, value] : *entries_) if (is_alive(key)) visitor->Visit(&value);
    }

    template <typename AlivePredicate>
    void TraceEphemerons(AlivePredicate&& is_alive, const std::function<void(TaggedValue&)>& visitor) {
        for (auto& [key, value] : *entries_) if (is_alive(key)) visitor(value);
    }

    void ResolveWeakKeys(const std::function<HeapObject*(HeapObject*)>& resolve) {
        for (auto it = entries_->begin(); it != entries_->end(); ) {
            auto current = it++;
            HeapObject* key = resolve(current->first);
            if (!key) entries_->erase(current);
            else if (key != current->first) {
                auto node = entries_->extract(current);
                node.key() = key;
                entries_->insert(std::move(node));
            }
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSWeakMap(std::move(*this));
    }

    void PruneDeadKeys(const std::function<bool(HeapObject*)>& is_alive) {
        for (auto it = entries_->begin(); it != entries_->end(); ) {
            if (!is_alive(it->first)) {
                it = entries_->erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    // Indirection makes nursery relocation allocation-free even on MSVC,
    // whose unordered_map move constructor allocates an empty sentinel.
    MemoryCharge storage_charge_{sizeof(EntryStorage)};
    std::unique_ptr<EntryStorage> entries_{std::make_unique<EntryStorage>()};
};

/**
 * @brief ECMAScript WeakSet collection with weak value referencing.
 */
class JSWeakSet : public JSObject {
public:
    using EntryStorage = std::unordered_set<HeapObject*, std::hash<HeapObject*>,
        std::equal_to<HeapObject*>, detail::StorageAllocator<HeapObject*>>;
    explicit JSWeakSet(Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSWeakSet, shape) {}

    JSWeakSet(JSWeakSet&& other) noexcept
        : JSObject(std::move(other)),
          storage_charge_(std::move(other.storage_charge_)),
          entries_(std::move(other.entries_)) {}

    size_t Size() const noexcept { return entries_->size(); }

    void Add(HeapObject* val) {
        if (!entries_->contains(val) && entries_->size() >= kMaxObjectProperties) {
            throw std::length_error("WeakSet size limit exceeded");
        }
        entries_->insert(val);
    }

    bool Has(HeapObject* val) const {
        return entries_->find(val) != entries_->end();
    }

    bool Delete(HeapObject* val) {
        return entries_->erase(val) > 0;
    }

    void Clear() {
        entries_->clear();
    }

    const EntryStorage& Entries() const noexcept {
        return *entries_;
    }

    size_t ObjectSize() const override {
        return sizeof(JSWeakSet);
    }

    void IteratePointers(SlotVisitor* visitor) override {
        JSObject::IteratePointers(visitor);
        // Elements are held weakly; not visited.
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSWeakSet(std::move(*this));
    }

    void PruneDeadEntries(const std::function<bool(HeapObject*)>& is_alive) {
        for (auto it = entries_->begin(); it != entries_->end(); ) {
            if (!is_alive(*it)) {
                it = entries_->erase(it);
            } else {
                ++it;
            }
        }
    }

    void ResolveWeakEntries(const std::function<HeapObject*(HeapObject*)>& resolve) {
        for (auto it = entries_->begin(); it != entries_->end(); ) {
            auto current = it++;
            HeapObject* value = resolve(*current);
            if (!value) entries_->erase(current);
            else if (value != *current) {
                auto node = entries_->extract(current);
                node.value() = value;
                entries_->insert(std::move(node));
            }
        }
    }

private:
    MemoryCharge storage_charge_{sizeof(EntryStorage)};
    std::unique_ptr<EntryStorage> entries_{std::make_unique<EntryStorage>()};
};

/**
 * @brief ECMAScript WeakRef object maintaining a non-owning target reference.
 */
class JSWeakRef : public JSObject {
public:
    explicit JSWeakRef(HeapObject* target, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSWeakRef, shape),
          target_(target) {}

    JSWeakRef(JSWeakRef&& other) noexcept
        : JSObject(std::move(other)),
          target_(other.target_) {
        other.target_ = nullptr;
    }

    TaggedValue Deref() const noexcept {
        return target_ ? TaggedValue::FromHeapObject(target_) : TaggedValue::Undefined();
    }

    HeapObject* Target() const noexcept { return target_; }
    void ClearTarget() noexcept { target_ = nullptr; }
    void SetTarget(HeapObject* target) noexcept { target_ = target; }

    size_t ObjectSize() const override { return sizeof(JSWeakRef); }

    void IteratePointers(SlotVisitor* visitor) override {
        JSObject::IteratePointers(visitor);
        // Target is held weakly; not visited.
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSWeakRef(std::move(*this));
    }

private:
    HeapObject* target_{nullptr};
};

/**
 * @brief ECMAScript FinalizationRegistry tracking object lifecycles with cleanup callbacks.
 */
class JSFinalizationRegistry : public JSObject {
public:
    struct Registration {
        HeapObject* target{nullptr};
        TaggedValue held_value{TaggedValue::Undefined()};
        HeapObject* unregister_token{nullptr};
    };

    explicit JSFinalizationRegistry(TaggedValue cleanup_callback, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSFinalizationRegistry, shape),
          cleanup_callback_(cleanup_callback) {}

    JSFinalizationRegistry(JSFinalizationRegistry&& other) noexcept
        : JSObject(std::move(other)),
          cleanup_callback_(other.cleanup_callback_),
          registrations_charge_(std::move(other.registrations_charge_)),
          registrations_(std::move(other.registrations_)) {
        other.cleanup_callback_ = TaggedValue::Undefined();
    }

    void Register(HeapObject* target, TaggedValue held_value, HeapObject* token = nullptr) {
        detail::ReserveStorage(registrations_, registrations_charge_, registrations_.size() + 1,
                               kMaxObjectProperties);
        registrations_.push_back({target, held_value, token});
    }

    bool Unregister(HeapObject* token) {
        if (!token) return false;
        size_t initial = registrations_.size();
        std::erase_if(registrations_, [token](const Registration& r) {
            return r.unregister_token == token;
        });
        return registrations_.size() < initial;
    }

    TaggedValue CleanupCallback() const noexcept { return cleanup_callback_; }
    const std::vector<Registration>& Registrations() const noexcept { return registrations_; }
    size_t RegistrationCount() const noexcept { return registrations_.size(); }

    // GC queues cleanup only; it never invokes JavaScript while sweeping.
    void ProcessWeakTargets(const std::function<HeapObject*(HeapObject*)>& resolve,
                            const std::function<void(TaggedValue, TaggedValue)>& enqueue) {
        for (auto it = registrations_.begin(); it != registrations_.end(); ) {
            HeapObject* target = resolve(it->target);
            if (!target) {
                enqueue(cleanup_callback_, it->held_value);
                it = registrations_.erase(it);
            } else {
                it->target = target;
                it->unregister_token = resolve(it->unregister_token);
                ++it;
            }
        }
    }

    size_t CleanupForTarget(HeapObject* target, const std::function<void(TaggedValue, TaggedValue)>& invoke_fn = nullptr) {
        size_t cleaned = 0;
        for (auto it = registrations_.begin(); it != registrations_.end(); ) {
            if (it->target == target) {
                if (invoke_fn && !cleanup_callback_.IsEmpty()) {
                    invoke_fn(cleanup_callback_, it->held_value);
                }
                it = registrations_.erase(it);
                cleaned++;
            } else {
                ++it;
            }
        }
        return cleaned;
    }

    size_t ObjectSize() const override {
        return sizeof(JSFinalizationRegistry);
    }

    void IteratePointers(SlotVisitor* visitor) override {
        JSObject::IteratePointers(visitor);
        if (!cleanup_callback_.IsEmpty()) visitor->Visit(&cleanup_callback_);
        for (auto& r : registrations_) {
            if (!r.held_value.IsEmpty()) visitor->Visit(&r.held_value);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSFinalizationRegistry(std::move(*this));
    }

private:
    TaggedValue cleanup_callback_{TaggedValue::Undefined()};
    MemoryCharge registrations_charge_;
    std::vector<Registration> registrations_;
};

/**
 * @brief Thread-safe Futex wait/notify synchronization manager for Atomics.
 */
class FutexManager {
public:
    static FutexManager& Instance() {
        static FutexManager s_instance;
        return s_instance;
    }

    struct Waiter {
        void* addr{nullptr};
        std::condition_variable cv;
        bool notified{false};
    };

    std::string Wait32(int32_t* ptr, int32_t expected, double timeout_ms) {
        auto waiter = std::make_shared<Waiter>();
        waiter->addr = static_cast<void*>(ptr);

        std::unique_lock<std::mutex> lock(mutex_);
        std::atomic_ref<int32_t> ref(*ptr);
        if (ref.load(std::memory_order_seq_cst) != expected) {
            return "not-equal";
        }

        if (timeout_ms == 0.0) {
            return "timed-out";
        }

        waiters_.push_back(waiter);

        bool timed_out = false;
        if (std::isinf(timeout_ms) || timeout_ms < 0.0) {
            waiter->cv.wait(lock, [&] { return waiter->notified; });
        } else {
            auto dur = std::chrono::duration<double, std::milli>(timeout_ms);
            auto cv_status = waiter->cv.wait_for(lock, std::chrono::duration_cast<std::chrono::milliseconds>(dur), [&] {
                return waiter->notified;
            });
            if (!cv_status && !waiter->notified) {
                timed_out = true;
            }
        }

        auto it = std::find(waiters_.begin(), waiters_.end(), waiter);
        if (it != waiters_.end()) {
            waiters_.erase(it);
        }

        if (timed_out) {
            return "timed-out";
        }
        return "ok";
    }

    std::string Wait64(int64_t* ptr, int64_t expected, double timeout_ms) {
        auto waiter = std::make_shared<Waiter>();
        waiter->addr = static_cast<void*>(ptr);

        std::unique_lock<std::mutex> lock(mutex_);
        std::atomic_ref<int64_t> ref(*ptr);
        if (ref.load(std::memory_order_seq_cst) != expected) {
            return "not-equal";
        }

        if (timeout_ms == 0.0) {
            return "timed-out";
        }

        waiters_.push_back(waiter);

        bool timed_out = false;
        if (std::isinf(timeout_ms) || timeout_ms < 0.0) {
            waiter->cv.wait(lock, [&] { return waiter->notified; });
        } else {
            auto dur = std::chrono::duration<double, std::milli>(timeout_ms);
            auto cv_status = waiter->cv.wait_for(lock, std::chrono::duration_cast<std::chrono::milliseconds>(dur), [&] {
                return waiter->notified;
            });
            if (!cv_status && !waiter->notified) {
                timed_out = true;
            }
        }

        auto it = std::find(waiters_.begin(), waiters_.end(), waiter);
        if (it != waiters_.end()) {
            waiters_.erase(it);
        }

        if (timed_out) {
            return "timed-out";
        }
        return "ok";
    }

    uint32_t Notify(void* addr, uint32_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t awakened = 0;
        for (auto& w : waiters_) {
            if (w->addr == addr && !w->notified) {
                w->notified = true;
                w->cv.notify_one();
                awakened++;
                if (count != UINT32_MAX && awakened >= count) {
                    break;
                }
            }
        }
        return awakened;
    }

    size_t WaiterCount(void* addr = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!addr) return waiters_.size();
        size_t c = 0;
        for (auto& w : waiters_) {
            if (w->addr == addr) c++;
        }
        return c;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& w : waiters_) {
            w->notified = true;
            w->cv.notify_all();
        }
        waiters_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<Waiter>> waiters_;
};

class Agent;
class JSPromise;

/**
 * @brief Managed asynchronous wait coordinator for Atomics.waitAsync.
 * Prevents dangling threads, roots promises in realms, and routes completions to the owning agent.
 */
class AsyncWaitManager {
public:
    static AsyncWaitManager& Instance();

    struct WaitTask {
        Agent* agent{nullptr};
        std::atomic<bool> cancelled{false};
        std::thread worker;
    };

    void SpawnWait(int32_t* ptr, int32_t expected, double timeout, JSPromise* promise, Agent* agent, Realm* realm);
    void SpawnWait64(int64_t* ptr, int64_t expected, double timeout, JSPromise* promise, Agent* agent, Realm* realm);
    void CancelAndJoinForAgent(Agent* agent);
    void Shutdown();

private:
    std::mutex mtx_;
    std::vector<std::shared_ptr<WaitTask>> tasks_;
};

/**
 * @brief Initializes all Advanced Intrinsics (WeakMap, WeakSet, SharedArrayBuffer, Atomics, WeakRef, FinalizationRegistry) on the given Realm.
 */
void InitializeAdvancedIntrinsics(Realm& realm);

} // namespace aether

#endif // AETHER_ADVANCED_INTRINSICS_H
