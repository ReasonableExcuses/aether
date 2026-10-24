#ifndef AETHER_CONCURRENT_MARKER_H
#define AETHER_CONCURRENT_MARKER_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
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
 * Compatibility facade for synchronous, owning-thread STW marking.
 * Background traversal is intentionally gated off: atomic mark bits do not
 * make mutable vectors, maps or object fields safe to read concurrently.
 * Start and FinalizeMarking trace synchronously; mutations between them are
 * seen by a fresh trace. No worker ever reads the object graph.
 */
class ConcurrentMarker {
public:
    explicit ConcurrentMarker(IsolateHeap& heap);
    ~ConcurrentMarker();

    static ConcurrentMarker* Current() noexcept;
    static void SetCurrent(ConcurrentMarker* marker) noexcept;
    static bool IsEnabled() noexcept;
    static void SetEnabled(bool enabled) noexcept;

    static constexpr bool SupportsConcurrentTraversal() noexcept { return false; }

    // Snapshot root slots (kept relocatable/rooted until Stop) and mark STW.
    void Start(const std::vector<HeapObject*>& roots);

    // Release explicit root slots. Does not sweep.
    void Stop();

    void NotifyWrite(HeapObject* source, TaggedValue value) noexcept;

    // Fresh STW trace, retaining explicit root slots until Stop.
    void FinalizeMarking();

    // Synchronous compatibility checkpoint.
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
    void Trace();

    IsolateHeap& heap_;
    std::atomic<ConcurrentMarkerState> state_{ConcurrentMarkerState::Idle};
    std::vector<TaggedValue> roots_;
    size_t registered_roots_{0};
    std::atomic<size_t> marked_count_{0};
};

} // namespace aether

#endif // AETHER_CONCURRENT_MARKER_H
