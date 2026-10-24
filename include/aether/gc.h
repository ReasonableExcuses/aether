#ifndef AETHER_GC_H
#define AETHER_GC_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/memory_budget.h"
#include <map>
#include <vector>
#include <bitset>
#include <memory>
#include <functional>
#include <cassert>
#include <mutex>

namespace aether {

// GC Architectural Constants
inline constexpr size_t kCardShift     = 9; // 512 bytes per card
inline constexpr size_t kCardSize      = 1ULL << kCardShift;
inline constexpr uint8_t kCleanCard    = 0x00;
inline constexpr uint8_t kDirtyCard    = 0x01;

inline constexpr size_t kDefaultNurserySemiSpaceSize = 16 * 1024 * 1024; // 16 MB Semi-space
inline constexpr size_t kImmixBlockSize               = 32 * 1024;        // 32 KB Block
inline constexpr size_t kImmixLineSize                = 128;              // 128 Byte Line
inline constexpr size_t kLinesPerBlock                = kImmixBlockSize / kImmixLineSize; // 256 lines
inline constexpr uint8_t kTenuringAgeThreshold        = 2;                // Tenures after 2 scavenges

/**
 * @brief 8MB Card Table tracking generational pointer stores across the 4GB Heap.
 */
class CardTable {
public:
    CardTable(uintptr_t heap_base, size_t heap_size)
        : heap_base_(heap_base),
          card_count_(heap_size >> kCardShift),
          cards_(card_count_, kCleanCard) {}

    AETHER_ALWAYS_INLINE void MarkCard(const void* ptr) noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        if (AETHER_UNLIKELY(addr < heap_base_)) return;
        size_t idx = (addr - heap_base_) >> kCardShift;
        if (AETHER_LIKELY(idx < card_count_)) {
            cards_[idx] = kDirtyCard;
        }
    }

    AETHER_ALWAYS_INLINE bool IsDirty(size_t index) const noexcept {
        if (index >= card_count_) return false;
        return cards_[index] == kDirtyCard;
    }

    AETHER_ALWAYS_INLINE void ClearCard(size_t index) noexcept {
        if (index < card_count_) {
            cards_[index] = kCleanCard;
        }
    }

    void Clear() noexcept {
        std::fill(cards_.begin(), cards_.end(), kCleanCard);
    }

    size_t CardCount() const noexcept { return card_count_; }
    uintptr_t AddressForCard(size_t idx) const noexcept {
        return heap_base_ + (idx << kCardShift);
    }

private:
    uintptr_t heap_base_;
    size_t card_count_;
    std::vector<uint8_t> cards_;
};

/**
 * @brief Semi-space Nursery with ultra-fast bump-pointer allocation and Cheney evacuation.
 */
class NurserySpace {
public:
    explicit NurserySpace(size_t semi_space_size = kDefaultNurserySemiSpaceSize);
    ~NurserySpace();
    NurserySpace(const NurserySpace&) = delete;
    NurserySpace& operator=(const NurserySpace&) = delete;

    void* Allocate(size_t bytes) noexcept;

    bool Contains(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        return addr >= from_space_start_ && addr < from_space_limit_;
    }

    // Flip after the collector has destroyed dead from-space objects.  The
    // live bump is passed explicitly so survivors are not discarded.
    void Flip(uintptr_t live_bump) noexcept;
    void Flip() noexcept;

    uintptr_t FromSpaceStart() const noexcept { return from_space_start_; }
    uintptr_t FromSpaceLimit() const noexcept { return from_space_limit_; }
    uintptr_t ToSpaceStart() const noexcept { return to_space_start_; }
    uintptr_t ToSpaceLimit() const noexcept { return to_space_limit_; }
    bool ContainsToSpace(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        return addr >= to_space_start_ && addr < to_space_limit_;
    }

    size_t BytesAllocated() const noexcept { return bump_ptr_ - from_space_start_; }
    size_t Capacity() const noexcept { return semi_space_size_; }
    bool HasReservation() const noexcept { return has_reservation_; }
    uint8_t* OldGenStart() const noexcept { return buffer_ + (semi_space_size_ * 2); }

    void ResetBumpPointer() noexcept {
        bump_ptr_ = from_space_start_;
    }

private:
    size_t semi_space_size_;
    uint8_t* buffer_{nullptr};
    bool has_reservation_{false};

    uintptr_t from_space_start_{0};
    uintptr_t from_space_limit_{0};
    uintptr_t bump_ptr_{0};

    uintptr_t to_space_start_{0};
    uintptr_t to_space_limit_{0};
};

/**
 * @brief Immix 32KB Block Header and Line Metadata.
 */
struct ImmixBlock {
    std::bitset<kLinesPerBlock> line_marks;
    std::bitset<kLinesPerBlock> line_allocated;
    size_t current_line{0};
    alignas(kObjectAlignment) uint8_t data[kImmixBlockSize];

    ImmixBlock() {
        line_marks.reset();
        line_allocated.reset();
    }

    void* Allocate(size_t bytes);
    void Sweep() noexcept;
    bool Contains(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t start = reinterpret_cast<uintptr_t>(data);
        return addr >= start && addr < start + kImmixBlockSize;
    }
};

/**
 * @brief Immix Line-and-Block Region Allocator for Old Generation Space.
 */
class ImmixSpace {
public:
    ImmixSpace() = default;
    ~ImmixSpace();

    void SetReservation(uint8_t* reservation_start, size_t max_size) {
        reservation_start_ = reservation_start;
        max_size_ = max_size;
    }

    void* Allocate(size_t bytes);
    void Sweep();
    // Allocate returns raw storage. Placement-new users must register only
    // after successful construction to enable destruction at sweep/shutdown.
    void RegisterObject(HeapObject* obj, bool charged = false);
    void DiscardAllocation(void* ptr) noexcept;
    void DestroyAll() noexcept;
    void SetBudget(std::shared_ptr<MemoryBudgetState> budget) { budget_ = std::move(budget); }
    std::vector<HeapObject*> Objects() const;
    size_t AllocationSize(const void* obj) const noexcept;
    void ClearMarks() noexcept;

    bool Contains(const void* ptr) const noexcept;
    void MarkObject(HeapObject* obj, size_t allocation_size = 0);

    size_t BlockCount() const noexcept { return blocks_.size(); }
    std::mutex& GetMarkMutex() const noexcept { return mark_mutex_; }

private:
    bool AllocateBlock();

    uint8_t* reservation_start_{nullptr};
    size_t max_size_{0};
    struct Allocation {
        size_t bytes;
        bool large;
        bool constructed{false};
        bool charged{false};
    };
    std::map<void*, Allocation> allocations_;
    std::shared_ptr<MemoryBudgetState> budget_;
    std::vector<ImmixBlock*> blocks_;
    size_t active_block_index_{0};
    mutable std::mutex mark_mutex_;
};

} // namespace aether

#endif // AETHER_GC_H
