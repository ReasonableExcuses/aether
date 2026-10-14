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
    Stop();

    {
        std::lock_guard<std::mutex> lock(worklist_mutex_);
        worklist_.clear();
    }
    {
        std::lock_guard<std::mutex> b_lock(barrier_mutex_);
        write_barrier_buffer_.clear();
        dirty_sources_.clear();
    }

    stop_requested_.store(false, std::memory_order_release);
    marked_count_.store(0, std::memory_order_release);

    // Initial mark phase: atomical mark all roots and push to worklist
    {
        std::lock_guard<std::mutex> lock(worklist_mutex_);
        for (HeapObject* root : roots) {
            if (root && root->AtomicSetMarked()) {
                worklist_.push_back(root);
                marked_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    state_.store(ConcurrentMarkerState::Marking, std::memory_order_release);
    worker_ = std::thread(&ConcurrentMarker::WorkerLoop, this);
}

void ConcurrentMarker::Stop() {
    if (state_.load(std::memory_order_acquire) == ConcurrentMarkerState::Idle) {
        return;
    }

    state_.store(ConcurrentMarkerState::Stopping, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    cv_work_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    state_.store(ConcurrentMarkerState::Idle, std::memory_order_release);
}

void ConcurrentMarker::NotifyWrite(HeapObject* source, TaggedValue value) {
    if (!IsActive()) {
        return;
    }

    if (value.IsHeapObject()) {
        HeapObject* target = value.AsHeapObject();
        if (target && target->AtomicSetMarked()) {
            marked_count_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(barrier_mutex_);
                write_barrier_buffer_.push_back(target);
            }
            cv_work_.notify_one();
        }
    }

    if (source) {
        std::lock_guard<std::mutex> lock(barrier_mutex_);
        dirty_sources_.push_back(source);
    }
}

void ConcurrentMarker::WaitForCompletion() {
    std::unique_lock<std::mutex> lock(worklist_mutex_);
    cv_idle_.wait(lock, [this]() {
        bool barrier_empty = true;
        {
            std::lock_guard<std::mutex> b_lock(barrier_mutex_);
            barrier_empty = write_barrier_buffer_.empty();
        }
        return worklist_.empty() && barrier_empty && !worker_active_.load(std::memory_order_relaxed);
    });
}

void ConcurrentMarker::FinalizeMarking() {
    Stop();

    // Drain remaining write barrier buffer and re-trace mutated sources during STW remark
    std::vector<HeapObject*> remaining_barrier;
    std::vector<HeapObject*> dirty_sources;
    {
        std::lock_guard<std::mutex> b_lock(barrier_mutex_);
        remaining_barrier = std::move(write_barrier_buffer_);
        dirty_sources = std::move(dirty_sources_);
        write_barrier_buffer_.clear();
        dirty_sources_.clear();
    }

    for (HeapObject* obj : remaining_barrier) {
        TraceObject(obj);
    }

    for (HeapObject* src : dirty_sources) {
        if (src) {
            TraceObject(src);
        }
    }

    // Drain any remaining objects in worklist
    std::deque<HeapObject*> remaining_work;
    {
        std::lock_guard<std::mutex> lock(worklist_mutex_);
        remaining_work = std::move(worklist_);
        worklist_.clear();
    }

    while (!remaining_work.empty()) {
        HeapObject* obj = remaining_work.front();
        remaining_work.pop_front();
        TraceObject(obj);
    }

    state_.store(ConcurrentMarkerState::Finished, std::memory_order_release);
}

size_t ConcurrentMarker::WorklistSize() const noexcept {
    std::lock_guard<std::mutex> lock(worklist_mutex_);
    return worklist_.size();
}

void ConcurrentMarker::TraceObject(HeapObject* obj) {
    if (!obj) return;

    if (heap_.GetOldGen().Contains(obj)) {
        heap_.GetOldGen().MarkObject(obj);
    }

    obj->IteratePointers([this](TaggedValue& slot) {
        if (slot.IsHeapObject()) {
            HeapObject* child = slot.AsHeapObject();
            if (child && child->AtomicSetMarked()) {
                marked_count_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(worklist_mutex_);
                    worklist_.push_back(child);
                }
                cv_work_.notify_one();
            }
        }
    });
}

void ConcurrentMarker::WorkerLoop() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        HeapObject* curr = nullptr;
        {
            std::unique_lock<std::mutex> lock(worklist_mutex_);

            if (worklist_.empty()) {
                // Check write barrier buffer
                std::vector<HeapObject*> from_barrier;
                {
                    std::lock_guard<std::mutex> b_lock(barrier_mutex_);
                    if (!write_barrier_buffer_.empty()) {
                        from_barrier = std::move(write_barrier_buffer_);
                        write_barrier_buffer_.clear();
                    }
                }
                for (auto* b_obj : from_barrier) {
                    worklist_.push_back(b_obj);
                }
            }

            if (worklist_.empty()) {
                worker_active_.store(false, std::memory_order_release);
                cv_idle_.notify_all();
                cv_work_.wait(lock, [this]() {
                    return stop_requested_.load(std::memory_order_relaxed) || !worklist_.empty();
                });

                if (stop_requested_.load(std::memory_order_relaxed) && worklist_.empty()) {
                    break;
                }
                if (worklist_.empty()) {
                    continue;
                }
            }

            worker_active_.store(true, std::memory_order_release);
            curr = worklist_.front();
            worklist_.pop_front();
        }

        if (curr) {
            TraceObject(curr);
        }
    }

    worker_active_.store(false, std::memory_order_release);
}

} // namespace aether
