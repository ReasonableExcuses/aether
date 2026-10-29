#include "aether/event_loop.h"
#include "aether/runtime.h"
#include <thread>
#include <algorithm>
#include <list>

namespace aether {

struct MicrotaskQueue::Entry {
    std::function<void()> task;
    std::vector<TaggedValue> roots;
    std::atomic<bool> cancelled{false};
    std::list<std::shared_ptr<Entry>>::iterator position;
    bool ready{false}; // protected by State::mutex
    bool linked{false};
};

struct MicrotaskQueue::State {
    mutable std::mutex mutex;
    std::list<std::shared_ptr<Entry>> pending;
    std::list<std::shared_ptr<Entry>> ready;
};

class MicrotaskQueue::RootAnchor final : public JSObject {
public:
    explicit RootAnchor(std::shared_ptr<State> state) : JSObject(nullptr), state_(std::move(state)) {}
    RootAnchor(RootAnchor&&) noexcept = default;
    size_t ObjectSize() const override { return sizeof(RootAnchor); }
    HeapObject* RelocateTo(void* dst) override { return new (dst) RootAnchor(std::move(*this)); }
    void IteratePointers(SlotVisitor* visitor) override {
        JSObject::IteratePointers(visitor);
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (auto* entries : {&state_->pending, &state_->ready}) {
            for (auto& entry : *entries) {
                if (!entry->cancelled.load(std::memory_order_acquire)) {
                    for (auto& root : entry->roots) visitor->Visit(&root);
                }
            }
        }
    }
private:
    std::shared_ptr<State> state_;
};

MicrotaskQueue::~MicrotaskQueue() {
    Clear();
    if (heap_) heap_->UnregisterExtraRoot(&anchor_);
}

void MicrotaskQueue::InitializeRoots(Heap& heap) {
    heap.AssertOwnerThread();
    if (std::this_thread::get_id() != owner_thread_) throw std::logic_error("Microtask queue owner mismatch");
    if (heap_) {
        if (heap_ != &heap) throw std::logic_error("Microtask queue cannot change heaps");
        return;
    }
    NoGCScope guard(heap);
    auto state = std::make_shared<State>();
    auto* anchor = heap.AllocateStable<RootAnchor>(state);
    anchor_ = TaggedValue::FromHeapObject(anchor);
    heap.RegisterExtraRoot(&anchor_);
    state_ = std::move(state);
    heap_ = &heap;
}

void MicrotaskQueue::EnsureRoots() {
    if (std::this_thread::get_id() == owner_thread_ && !heap_) {
        InitializeRoots(Heap::Current() ? *Heap::Current() : Heap::RuntimeFallbackHeap());
    }
    if (!state_) throw std::logic_error("Initialize microtask roots before worker publication");
}

std::shared_ptr<MicrotaskQueue::Entry> MicrotaskQueue::AddTask(
    std::function<void()> task, std::vector<TaggedValue> roots, bool ready) {
    EnsureRoots();
    auto entry = std::make_shared<Entry>();
    entry->task = std::move(task);
    entry->roots = std::move(roots);
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& entries = ready ? state_->ready : state_->pending;
    entry->position = entries.insert(entries.end(), entry);
    entry->ready = ready;
    entry->linked = true;
    return entry;
}

void MicrotaskQueue::Enqueue(std::function<void()> task, std::vector<TaggedValue> roots) {
    AddTask(std::move(task), std::move(roots), true);
}

MicrotaskQueue::PendingTask MicrotaskQueue::Prepare(std::function<void()> task, std::vector<TaggedValue> roots) {
    if (std::this_thread::get_id() != owner_thread_) throw std::logic_error("Prepare requires the queue owner");
    auto entry = AddTask(std::move(task), std::move(roots), false);
    return PendingTask(entry, state_);
}

void MicrotaskQueue::PendingTask::Publish() const noexcept {
    auto state = state_.lock();
    auto entry = entry_.lock();
    if (!state || !entry) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!entry->cancelled.load(std::memory_order_acquire) && entry->linked && !entry->ready) {
        // Splicing the preallocated list node is allocation-free and preserves
        // FIFO by publication order, rather than wait-registration order.
        state->ready.splice(state->ready.end(), state->pending, entry->position);
        entry->ready = true;
    }
}

void MicrotaskQueue::PendingTask::Cancel() const noexcept {
    if (auto entry = entry_.lock()) entry->cancelled.store(true, std::memory_order_release);
}

void MicrotaskQueue::Clear() {
    if (!state_) return;
    while (true) {
        std::shared_ptr<Entry> discarded;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto& entries = !state_->ready.empty() ? state_->ready : state_->pending;
            if (entries.empty()) break;
            discarded = std::move(entries.front());
            entries.pop_front();
            discarded->linked = false;
            discarded->cancelled.store(true, std::memory_order_release);
        }
        // Destroy closures outside the queue lock, with no cleanup allocation.
    }
}

size_t MicrotaskQueue::Size() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return std::count_if(state_->ready.begin(), state_->ready.end(), [](const auto& entry) {
        return !entry->cancelled.load(std::memory_order_acquire);
    });
}

bool MicrotaskQueue::HasPendingWork() const noexcept {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto* entries : {&state_->pending, &state_->ready}) {
        if (std::any_of(entries->begin(), entries->end(), [](const auto& entry) {
            return !entry->cancelled.load(std::memory_order_acquire);
        })) return true;
    }
    return false;
}

void MicrotaskQueue::PerformMicrotaskCheckpoint() {
    if (!state_) return;
    heap_->AssertOwnerThread();
    // Prune cancelled, unpublished tasks once per checkpoint. Their closures
    // must be destroyed on the owner, not by a worker calling Cancel().
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        auto it = state_->pending.begin();
        while (it != state_->pending.end()) {
            if (!(*it)->cancelled.load(std::memory_order_acquire)) { ++it; continue; }
            auto discarded = std::move(*it);
            it = state_->pending.erase(it);
            discarded->linked = false;
            lock.unlock();
            discarded.reset();
            lock.lock();
            // A destructor can reenter or a worker can publish while unlocked.
            it = state_->pending.begin();
        }
    }
    while (true) {
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->ready.empty()) break;
            entry = std::move(state_->ready.front());
            state_->ready.pop_front();
            entry->linked = false;
        }
        if (entry->cancelled.load(std::memory_order_acquire)) continue;
        // Native callbacks and their opaque temporaries remain protected through
        // JS reentry. No collection occurs between dequeue and this guard.
        NoGCScope native_scope(*heap_);
        try { entry->task(); }
        catch (const std::length_error& error) { RaiseResourceError(error.what()); }
        catch (const std::bad_alloc&) { RaiseResourceError("Microtask allocation failed"); }
    }
}

EventLoop::EventLoop()
    : start_time_(std::chrono::steady_clock::now()) {}

static std::shared_ptr<NativeRootSet> RootCallback(TaggedValue callback, const std::vector<TaggedValue>& args) {
    auto& heap = Heap::Current() ? *Heap::Current() : Heap::RuntimeFallbackHeap();
    auto roots = std::make_shared<NativeRootSet>(heap);
    roots->Add(callback);
    for (auto value : args) roots->Add(value);
    return roots;
}

uint32_t EventLoop::SetTimeout(TaggedValue callback, int64_t delay_ms, std::vector<TaggedValue> args) {
    NoGCScope guard;
    if (delay_ms < 0) delay_ms = 0;
    delay_ms = std::min<int64_t>(delay_ms, INT32_MAX);
    auto entry = std::make_shared<TimerEntry>();
    entry->roots = RootCallback(callback, args);
    entry->id = next_timer_id_++;
    entry->due_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    entry->interval = std::chrono::milliseconds(0);
    entry->callback = callback;
    entry->args = std::move(args);
    entry->is_interval = false;
    entry->cancelled = false;

    timers_by_id_[entry->id] = entry;
    try { timer_queue_.push(entry); }
    catch (...) { timers_by_id_.erase(entry->id); throw; }
    return entry->id;
}

void EventLoop::ClearTimeout(uint32_t id) {
    auto it = timers_by_id_.find(id);
    if (it != timers_by_id_.end()) {
        it->second->cancelled = true;
        it->second->roots.reset();
        it->second->callback = TaggedValue::Undefined();
        it->second->args.clear();
        timers_by_id_.erase(it);
    }
}

uint32_t EventLoop::SetInterval(TaggedValue callback, int64_t interval_ms, std::vector<TaggedValue> args) {
    NoGCScope guard;
    if (interval_ms <= 0) interval_ms = 1;
    interval_ms = std::min<int64_t>(interval_ms, INT32_MAX);
    auto entry = std::make_shared<TimerEntry>();
    entry->roots = RootCallback(callback, args);
    entry->id = next_timer_id_++;
    entry->interval = std::chrono::milliseconds(interval_ms);
    entry->due_time = std::chrono::steady_clock::now() + entry->interval;
    entry->callback = callback;
    entry->args = std::move(args);
    entry->is_interval = true;
    entry->cancelled = false;

    timers_by_id_[entry->id] = entry;
    try { timer_queue_.push(entry); }
    catch (...) { timers_by_id_.erase(entry->id); throw; }
    return entry->id;
}

void EventLoop::ClearInterval(uint32_t id) {
    ClearTimeout(id);
}

uint32_t EventLoop::SetImmediate(TaggedValue callback, std::vector<TaggedValue> args) {
    NoGCScope guard;
    auto entry = std::make_shared<ImmediateEntry>();
    entry->roots = RootCallback(callback, args);
    entry->id = next_immediate_id_++;
    entry->callback = callback;
    entry->args = std::move(args);
    entry->cancelled = false;

    immediates_by_id_[entry->id] = entry;
    try { immediate_queue_.push_back(entry); }
    catch (...) { immediates_by_id_.erase(entry->id); throw; }
    return entry->id;
}

void EventLoop::ClearImmediate(uint32_t id) {
    auto it = immediates_by_id_.find(id);
    if (it != immediates_by_id_.end()) {
        it->second->cancelled = true;
        it->second->roots.reset();
        it->second->callback = TaggedValue::Undefined();
        it->second->args.clear();
        immediates_by_id_.erase(it);
    }
}

double EventLoop::PerformanceNow() const noexcept {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_time_).count();
}

bool EventLoop::HasPendingWork() const noexcept {
    return !timers_by_id_.empty() || !immediates_by_id_.empty();
}

bool EventLoop::Step(Agent& agent) {
    bool did_work = false;
    agent.GetMicrotasks().PerformMicrotaskCheckpoint();

    // 1. Process Immediates phase (Node.js check phase)
    if (!immediate_queue_.empty()) {
        size_t count = immediate_queue_.size();
        for (size_t i = 0; i < count && !immediate_queue_.empty(); ++i) {
            auto imm = immediate_queue_.front();
            immediate_queue_.pop_front();
            if (!imm->cancelled) {
                immediates_by_id_.erase(imm->id);
                did_work = true;
                {
                    NoGCScope guard(agent.GetHeap());
                    CallFunction(imm->callback, TaggedValue::Undefined(), imm->args);
                }
                imm->roots.reset();
                agent.GetMicrotasks().PerformMicrotaskCheckpoint();
            }
        }
    }

    // 2. Check and fire expired timers
    auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<TimerEntry>> to_fire;

    while (!timer_queue_.empty()) {
        auto top = timer_queue_.top();
        if (top->cancelled) {
            timer_queue_.pop();
            continue;
        }
        if (top->due_time <= now) {
            to_fire.push_back(top);
            timer_queue_.pop();
        } else {
            break;
        }
    }

    for (const auto& timer : to_fire) {
        if (timer->cancelled) continue;
        did_work = true;

        if (!timer->is_interval) {
            timers_by_id_.erase(timer->id);
        }

        {
            NoGCScope guard(agent.GetHeap());
            // An interval may cancel itself. Retain the arguments until return.
            auto args = timer->args;
            CallFunction(timer->callback, TaggedValue::Undefined(), args);
        }
        if (!timer->is_interval) timer->roots.reset();
        agent.GetMicrotasks().PerformMicrotaskCheckpoint();

        if (timer->is_interval && !timer->cancelled) {
            timer->due_time = std::chrono::steady_clock::now() + timer->interval;
            try { timer_queue_.push(timer); }
            catch (...) { ClearInterval(timer->id); throw; }
        }
    }

    // 3. If there is work remaining in future timers, sleep shortly to avoid busy spinning
    if (!did_work && !timers_by_id_.empty()) {
        while (!timer_queue_.empty() && timer_queue_.top()->cancelled) {
            timer_queue_.pop();
        }
        if (!timer_queue_.empty()) {
            auto next_due = timer_queue_.top()->due_time;
            auto curr = std::chrono::steady_clock::now();
            if (next_due > curr) {
                auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_due - curr);
                auto sleep_duration = std::min(wait_ms, std::chrono::milliseconds(5));
                if (sleep_duration.count() > 0) {
                    std::this_thread::sleep_for(sleep_duration);
                }
            }
        }
    }

    agent.GetMicrotasks().PerformMicrotaskCheckpoint();
    if (!did_work && !HasPendingWork() && agent.GetMicrotasks().HasPendingWork()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return did_work || HasPendingWork() || agent.GetMicrotasks().HasPendingWork();
}

void EventLoop::Run(Agent& agent, size_t max_ticks) {
    stopped_ = false;
    size_t ticks = 0;
    while ((HasPendingWork() || agent.GetMicrotasks().HasPendingWork()) && !stopped_ && ticks < max_ticks) {
        Step(agent);
        ticks++;
    }
}

} // namespace aether
