#ifndef AETHER_MEMORY_BUDGET_H
#define AETHER_MEMORY_BUDGET_H

#include "aether/common.h"

#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace aether {

inline constexpr size_t kDefaultArrayBufferExternalBudget =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

/**
 * Shared accounting state owned by an IsolateHeap.
 *
 * The state deliberately outlives the heap when a MemoryCharge is kept by host
 * code.  It contains no raw pointer back to the isolate, so releasing such a
 * charge after isolate teardown remains valid.
 */
struct MemoryBudgetState {
    explicit MemoryBudgetState(
        size_t total_limit = kDefaultHeapBudget,
        size_t array_buffer_limit = kDefaultArrayBufferExternalBudget) noexcept
        : total_limit_(total_limit),
          has_total_limit_(total_limit != 0),
          array_buffer_limit_(array_buffer_limit) {}

    bool TryCharge(size_t bytes, bool array_buffer) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytes > std::numeric_limits<size_t>::max() - charged_bytes_) {
            return false;
        }
        if (has_total_limit_ && charged_bytes_ + bytes > total_limit_) {
            return false;
        }
        if (array_buffer) {
            if (bytes > std::numeric_limits<size_t>::max() - array_buffer_bytes_) {
                return false;
            }
            if (array_buffer_limit_ != 0 &&
                array_buffer_bytes_ + bytes > array_buffer_limit_) {
                return false;
            }
        }
        charged_bytes_ += bytes;
        if (array_buffer) array_buffer_bytes_ += bytes;
        return true;
    }

    bool TryReserveManaged(size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytes > std::numeric_limits<size_t>::max() - managed_bytes_) {
            return false;
        }
        if (bytes > std::numeric_limits<size_t>::max() - charged_bytes_) {
            return false;
        }
        const size_t total_after = charged_bytes_ + bytes;
        if (has_total_limit_ && total_after > total_limit_) return false;
        managed_bytes_ += bytes;
        charged_bytes_ += bytes;
        return true;
    }

    void ReleaseCharge(size_t bytes, bool array_buffer) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        charged_bytes_ = bytes > charged_bytes_ ? 0 : charged_bytes_ - bytes;
        if (array_buffer) {
            array_buffer_bytes_ = bytes > array_buffer_bytes_
                ? 0 : array_buffer_bytes_ - bytes;
        }
    }

    void ReleaseManaged(size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        managed_bytes_ = bytes > managed_bytes_ ? 0 : managed_bytes_ - bytes;
        charged_bytes_ = bytes > charged_bytes_ ? 0 : charged_bytes_ - bytes;
    }

    void SetTotalLimit(size_t limit, bool has_limit = true) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        total_limit_ = limit;
        has_total_limit_ = has_limit;
    }

    void ClearTotalLimit() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        total_limit_ = 0;
        has_total_limit_ = false;
    }

    bool HasTotalLimit() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return has_total_limit_;
    }

    size_t TotalLimit() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return has_total_limit_ ? total_limit_ : 0;
    }

    size_t ChargedBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return charged_bytes_;
    }

    size_t ManagedBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return managed_bytes_;
    }

    size_t ArrayBufferBytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return array_buffer_bytes_;
    }

private:
    mutable std::mutex mutex_;
    size_t total_limit_;
    bool has_total_limit_{true};
    size_t array_buffer_limit_;
    size_t charged_bytes_{0};
    size_t managed_bytes_{0};
    size_t array_buffer_bytes_{0};
};

// Implemented by heap.cpp to avoid making this header depend on heap.h.
std::shared_ptr<MemoryBudgetState> CurrentMemoryBudgetState() noexcept;

/**
 * Move-only external/resource charge.
 *
 * A charge never invokes GC.  Resize either completes atomically or throws
 * std::length_error, allowing callers to collect/retry at a safe boundary.
 * When no isolate is current, a private state is used; aggregate per-isolate
 * accounting begins once the charge is created while an isolate is active.
 */
class MemoryCharge {
private:
    struct Holding {
        Holding(std::shared_ptr<MemoryBudgetState> state, bool array_buffer)
            : state(std::move(state)), array_buffer(array_buffer) {}

        ~Holding() {
            if (state && bytes != 0) state->ReleaseCharge(bytes, array_buffer);
        }

        std::shared_ptr<MemoryBudgetState> state;
        size_t bytes{0};
        bool array_buffer{false};
    };

public:
    explicit MemoryCharge(size_t bytes = 0, bool array_buffer = false) {
        auto state = CurrentMemoryBudgetState();
        if (!state) state = std::make_shared<MemoryBudgetState>();
        holding_ = std::make_unique<Holding>(std::move(state), array_buffer);
        if (bytes != 0 && !holding_->state->TryCharge(bytes, array_buffer)) {
            holding_.reset();
            throw std::length_error("Aether memory budget exceeded");
        }
        holding_->bytes = bytes;
    }

    MemoryCharge(const MemoryCharge&) = delete;
    MemoryCharge& operator=(const MemoryCharge&) = delete;
    MemoryCharge(MemoryCharge&&) noexcept = default;
    MemoryCharge& operator=(MemoryCharge&&) noexcept = default;

    void Resize(size_t bytes) {
        if (!holding_) {
            auto state = CurrentMemoryBudgetState();
            if (!state) state = std::make_shared<MemoryBudgetState>();
            holding_ = std::make_unique<Holding>(std::move(state), false);
        }

        const size_t old_bytes = holding_->bytes;
        if (bytes > old_bytes) {
            const size_t delta = bytes - old_bytes;
            if (!holding_->state->TryCharge(delta, holding_->array_buffer)) {
                throw std::length_error("Aether memory budget exceeded");
            }
        } else if (old_bytes > bytes) {
            holding_->state->ReleaseCharge(old_bytes - bytes, holding_->array_buffer);
        }
        holding_->bytes = bytes;
    }

    size_t Bytes() const noexcept {
        return holding_ ? holding_->bytes : 0;
    }

    ~MemoryCharge() = default;

private:
    std::unique_ptr<Holding> holding_;
};

} // namespace aether

#endif // AETHER_MEMORY_BUDGET_H
