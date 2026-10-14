#ifndef AETHER_CONCURRENT_MARKER_H
#define AETHER_CONCURRENT_MARKER_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>

namespace aether {

class IsolateHeap;

enum class ConcurrentMarkerState {
    Idle,
    Marking,
    Stopping,
    Finished
};

/**
 * @brief Concurrent background Immix GC marking thread.
 *
 * Runs concurrent mark passes parallel to mutator execution, drastically reducing
 * Stop-The-World (STW) pauses during major garbage collection cycles.
 */
class ConcurrentMarker {
public:
    explicit ConcurrentMarker(IsolateHeap& heap);
    ~ConcurrentMarker();

    static ConcurrentMarker* Current() noexcept;
    static void SetCurrent(ConcurrentMarker* marker) noexcept;
    static bool IsEnabled() noexcept;
    static void SetEnabled(bool enabled) noexcept;

    // Start background concurrent marking thread with given initial root set
    void Start(const std::vector<HeapObject*>& roots);

    // Stop and join background worker
    void Stop();

    // Write barrier hook called by mutator on pointer mutations during concurrent marking
    void NotifyWrite(HeapObject* source, TaggedValue value);

    // Drain remaining write barrier buffers and finalize marking during brief STW pause
    void FinalizeMarking();

    // Wait for the background worker to drain the initial worklist
    void WaitForCompletion();

    bool IsActive() const noexcept {
        return state_.load(std::memory_order_acquire) == ConcurrentMarkerState::Marking;
    }
    ConcurrentMarkerState GetState() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    size_t MarkedObjectCount() const noexcept {
        return marked_count_.load(std::memory_order_relaxed);
    }
    size_t WorklistSize() const noexcept;

private:
    void WorkerLoop();
    void TraceObject(HeapObject* obj);

    IsolateHeap& heap_;
    std::atomic<ConcurrentMarkerState> state_{ConcurrentMarkerState::Idle};
    std::thread worker_;

    mutable std::mutex worklist_mutex_;
    std::condition_variable cv_work_;
    std::condition_variable cv_idle_;
    std::deque<HeapObject*> worklist_;

    mutable std::mutex barrier_mutex_;
    std::vector<HeapObject*> write_barrier_buffer_;
    std::vector<HeapObject*> dirty_sources_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> marked_count_{0};
    std::atomic<bool> worker_active_{false};
};

} // namespace aether

#endif // AETHER_CONCURRENT_MARKER_H
