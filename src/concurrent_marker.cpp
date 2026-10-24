#include "aether/concurrent_marker.h"
#include "aether/heap.h"
#include "aether/gc.h"
#include <iostream>

namespace aether {

static thread_local ConcurrentMarker* s_current_concurrent_marker_{nullptr};
static std::atomic<bool> s_concurrent_marker_enabled_{false};

ConcurrentMarker* ConcurrentMarker::Current() noexcept {
    return s_current_concurrent_marker_;
}

void ConcurrentMarker::SetCurrent(ConcurrentMarker* marker) noexcept {
    s_current_concurrent_marker_ = marker;
}

bool ConcurrentMarker::IsEnabled() noexcept {
    return s_concurrent_marker_enabled_.load(std::memory_order_relaxed);
}

void ConcurrentMarker::SetEnabled(bool enabled) noexcept {
    s_concurrent_marker_enabled_.store(enabled, std::memory_order_relaxed);
}

ConcurrentMarker::ConcurrentMarker(IsolateHeap& heap)
    : heap_(heap) {
    if (!s_current_concurrent_marker_) {
        s_current_concurrent_marker_ = this;
    }
}

ConcurrentMarker::~ConcurrentMarker() {
    Stop();
    if (s_current_concurrent_marker_ == this) {
        s_current_concurrent_marker_ = nullptr;
    }
}

void ConcurrentMarker::Start(const std::vector<HeapObject*>& roots) {
    heap_.AssertOwnerThread();
    if (!heap_.GCAllowed()) throw std::logic_error("Marking inside NoGCScope");
    Stop();
    roots_.reserve(roots.size());
    for (auto* root : roots) if (root) roots_.push_back(TaggedValue::FromHeapObject(root));
    try {
        for (auto& root : roots_) {
            heap_.RegisterExtraRoot(&root);
            ++registered_roots_;
        }
        Trace();
    } catch (...) {
        Stop();
        throw;
    }
    state_.store(ConcurrentMarkerState::Marking, std::memory_order_release);
}

void ConcurrentMarker::Stop() {
    for (size_t i = 0; i < registered_roots_; ++i) heap_.UnregisterExtraRoot(&roots_[i]);
    registered_roots_ = 0;
    roots_.clear();
    state_.store(ConcurrentMarkerState::Idle, std::memory_order_release);
}

void ConcurrentMarker::NotifyWrite(HeapObject*, TaggedValue) noexcept {
    // No graph access here. Finalization always retraces current roots.
}

void ConcurrentMarker::WaitForCompletion() {
    heap_.AssertOwnerThread();
    if (IsActive()) Trace();
}

void ConcurrentMarker::FinalizeMarking() {
    heap_.AssertOwnerThread();
    if (IsActive()) {
        marked_count_.store(heap_.MarkFromRoots({}, true), std::memory_order_relaxed);
    }
    state_.store(ConcurrentMarkerState::Finished, std::memory_order_release);
}

size_t ConcurrentMarker::WorklistSize() const noexcept {
    return 0;
}

void ConcurrentMarker::Trace() {
    marked_count_.store(heap_.MarkFromRoots({}), std::memory_order_relaxed);
}

} // namespace aether
