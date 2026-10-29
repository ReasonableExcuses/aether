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
inline constexpr size_t kHeaderLines                  = 8;                // 8 Lines = 1024 Bytes
inline constexpr size_t kHeaderSize                   = kHeaderLines * kImmixLineSize; // 1024 Bytes
inline constexpr size_t kPayloadLines                 = kLinesPerBlock - kHeaderLines; // 248 lines
inline constexpr size_t kPayloadSize                  = kPayloadLines * kImmixLineSize; // 31744 Bytes
inline constexpr size_t kObjectStartBitmapSize        = kImmixBlockSize / (8 * 8); // 512 Bytes (4096 bits)
inline constexpr size_t kLargeObjectThreshold         = 8 * 1024;         // 8 KB
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

    AETHER_ALWAYS_INLINE bool IsCardMarked(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        if (addr < heap_base_) return false;
        size_t idx = (addr - heap_base_) >> kCardShift;
        return IsDirty(idx);
    }

    AETHER_ALWAYS_INLINE void ClearCard(size_t index) noexcept {
        if (index < card_count_) {
            cards_[index] = kCleanCard;
        }
    }

    AETHER_ALWAYS_INLINE void ClearCard(const void* ptr) noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        if (addr < heap_base_) return;
        size_t idx = (addr - heap_base_) >> kCardShift;
        ClearCard(idx);
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

enum class BlockState : uint8_t {
    Free = 0,
    Recyclable = 1,
    Full = 2
};

struct LargeObjectHeader {
    LargeObjectHeader* next{nullptr};
    LargeObjectHeader* prev{nullptr};
    size_t size{0}; // total allocated size including header
    bool marked{false};
    bool constructed{false};
    bool charged{false};

    HeapObject* Object() noexcept {
        return reinterpret_cast<HeapObject*>(
            reinterpret_cast<uint8_t*>(this) + HeaderSize());
    }

    const HeapObject* Object() const noexcept {
        return reinterpret_cast<const HeapObject*>(
            reinterpret_cast<const uint8_t*>(this) + HeaderSize());
    }

    static const LargeObjectHeader* FromObject(const HeapObject* obj) noexcept {
        return reinterpret_cast<const LargeObjectHeader*>(
            reinterpret_cast<const uint8_t*>(obj) - HeaderSize());
    }

    static LargeObjectHeader* FromObject(HeapObject* obj) noexcept {
        return reinterpret_cast<LargeObjectHeader*>(
            reinterpret_cast<uint8_t*>(obj) - HeaderSize());
    }

    static constexpr size_t HeaderSize() noexcept {
        return (sizeof(LargeObjectHeader) + kObjectAlignment - 1) & ~(kObjectAlignment - 1);
    }
};

/**
 * @brief Immix 32KB Block Header and Line Metadata.
 */
struct alignas(64) ImmixBlock {
    // 1024-byte Header (Lines 0..7)
    uint8_t line_marks[kLinesPerBlock];               // 256 bytes (offset 0)
    uint8_t object_starts[kObjectStartBitmapSize];     // 512 bytes (offset 256)
    std::bitset<kLinesPerBlock> line_allocated;       // 32 bytes  (offset 768)
    ImmixBlock* next{nullptr};                        // 8 bytes   (offset 800)
    ImmixBlock* prev{nullptr};                        // 8 bytes   (offset 808)
    class ImmixSpace* space{nullptr};                 // 8 bytes   (offset 816)
    uint32_t cursor_offset{static_cast<uint32_t>(kHeaderSize)}; // 4 bytes (offset 824)
    uint32_t limit_offset{static_cast<uint32_t>(kHeaderSize)};  // 4 bytes (offset 828)
    uint16_t flags{0};                                // 2 bytes   (offset 832)
    BlockState state{BlockState::Free};               // 1 byte    (offset 834)
    uint8_t live_lines_count{0};                      // 1 byte    (offset 835)
    uint8_t _padding[kHeaderSize - 836];              // 188 bytes (offset 836..1023)

    // 31,744-byte Payload (Lines 8..255)
    alignas(kObjectAlignment) uint8_t payload[kPayloadSize];

    ImmixBlock();

    static ImmixBlock* BlockFor(const void* ptr) noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        return reinterpret_cast<ImmixBlock*>(addr & ~(kImmixBlockSize - 1));
    }

    bool Contains(const void* ptr) const noexcept {
        return BlockFor(ptr) == this && state != BlockState::Free;
    }

    bool ContainsPayload(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t start = reinterpret_cast<uintptr_t>(payload);
        return addr >= start && addr < start + kPayloadSize;
    }

    size_t LineFor(const void* ptr) const noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base = reinterpret_cast<uintptr_t>(this);
        return (addr - base) / kImmixLineSize;
    }

    uint8_t* AddressForLine(size_t line) const noexcept {
        return const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(this)) + line * kImmixLineSize;
    }

    void SetObjectStart(const void* ptr) noexcept {
        size_t bit_index = (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(this)) >> 3;
        object_starts[bit_index >> 3] |= static_cast<uint8_t>(1 << (bit_index & 7));
    }

    void ClearObjectStart(const void* ptr) noexcept {
        size_t bit_index = (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(this)) >> 3;
        object_starts[bit_index >> 3] &= static_cast<uint8_t>(~(1 << (bit_index & 7)));
    }

    bool IsObjectStart(const void* ptr) const noexcept {
        size_t bit_index = (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(this)) >> 3;
        return (object_starts[bit_index >> 3] & (1 << (bit_index & 7))) != 0;
    }

    void* Allocate(size_t bytes);
    void Sweep() noexcept;

    template <typename F>
    void ForEachObject(F&& cb) {
        for (size_t b = (kHeaderSize >> 6); b < kObjectStartBitmapSize; ++b) {
            uint8_t byte = object_starts[b];
            if (!byte) continue;
            for (size_t bit = 0; bit < 8; ++bit) {
                if (byte & (1 << bit)) {
                    size_t offset = (b * 8 + bit) * 8;
                    HeapObject* obj = reinterpret_cast<HeapObject*>(
                        reinterpret_cast<uint8_t*>(this) + offset);
                    cb(obj);
                }
            }
        }
    }
};

static_assert(sizeof(ImmixBlock) == kImmixBlockSize, "ImmixBlock must be exactly 32 KB");
static_assert(offsetof(ImmixBlock, payload) == kHeaderSize, "Payload must start at offset 1024 (Line 8)");

/**
 * @brief Immix Line-and-Block Region Allocator for Old Generation Space.
 */
class ImmixSpace {
public:
    ImmixSpace() = default;
    ~ImmixSpace();

    void SetReservation(uint8_t* reservation_start, size_t max_size);

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

    bool IsValidObjectStart(const void* ptr) const noexcept;
    size_t BlockCount() const noexcept { return all_blocks_.size(); }
    std::mutex& GetMarkMutex() const noexcept { return mark_mutex_; }

private:
    bool AllocateBlock();
    void* AllocateLarge(size_t bytes, bool charged = false);
    void FreeLarge(LargeObjectHeader* header) noexcept;
    bool IsLargeObject(const void* ptr) const noexcept;
    bool ContainsBlock(const ImmixBlock* block) const noexcept;

    uint8_t* reservation_start_{nullptr};
    size_t max_size_{0};
    size_t reservation_padding_{0};

    std::shared_ptr<MemoryBudgetState> budget_;
    std::vector<ImmixBlock*> all_blocks_;
    std::vector<ImmixBlock*> standalone_blocks_;
    size_t active_block_index_{0};

    LargeObjectHeader* large_objects_head_{nullptr};
    size_t large_object_count_{0};

    void* last_allocation_ptr_{nullptr};
    size_t last_allocation_bytes_{0};

    mutable std::mutex mark_mutex_;
};

} // namespace aether

#endif // AETHER_GC_H
