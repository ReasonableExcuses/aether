#include "aether/gc.h"
#include "aether/options.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace aether {

ImmixBlock::ImmixBlock() {
    std::memset(line_marks, 0, sizeof(line_marks));
    std::memset(object_starts, 0, sizeof(object_starts));
    line_allocated.reset();
    for (size_t l = 0; l < kHeaderLines; ++l) {
        line_allocated.set(l);
    }
    state = BlockState::Free;
    live_lines_count = 0;
    flags = 0;
    cursor_offset = static_cast<uint32_t>(kHeaderSize);
    limit_offset = static_cast<uint32_t>(kHeaderSize);
    next = nullptr;
    prev = nullptr;
    space = nullptr;
    std::memset(_padding, 0, sizeof(_padding));
}

void* ImmixBlock::Allocate(size_t bytes) {
    if (bytes == 0 || bytes > kPayloadSize) return nullptr;
    bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

    // Fast path: bump allocation within current hole
    if (cursor_offset + bytes <= limit_offset) {
        state = BlockState::Recyclable;
        uint8_t* mem = reinterpret_cast<uint8_t*>(this) + cursor_offset;
        cursor_offset += static_cast<uint32_t>(bytes);
        size_t start_line = LineFor(mem);
        size_t end_line = LineFor(mem + bytes - 1);
        for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
            line_allocated.set(l);
        }
        return mem;
    }

    // Slow path: scan forward for next available hole
    size_t search_line = LineFor(reinterpret_cast<uint8_t*>(this) + limit_offset);
    if (search_line < kHeaderLines) search_line = kHeaderLines;

    while (search_line < kLinesPerBlock) {
        while (search_line < kLinesPerBlock && line_allocated.test(search_line)) {
            ++search_line;
        }
        if (search_line >= kLinesPerBlock) break;

        size_t hole_start = search_line;
        while (search_line < kLinesPerBlock && !line_allocated.test(search_line)) {
            ++search_line;
        }
        size_t hole_end = search_line;
        size_t hole_bytes = (hole_end - hole_start) * kImmixLineSize;

        if (hole_bytes >= bytes) {
            state = BlockState::Recyclable;
            cursor_offset = static_cast<uint32_t>(hole_start * kImmixLineSize);
            limit_offset = static_cast<uint32_t>(hole_end * kImmixLineSize);

            uint8_t* mem = reinterpret_cast<uint8_t*>(this) + cursor_offset;
            cursor_offset += static_cast<uint32_t>(bytes);
            size_t start_line = LineFor(mem);
            size_t end_line = LineFor(mem + bytes - 1);
            for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
                line_allocated.set(l);
            }
            return mem;
        }
    }

    state = BlockState::Full;
    return nullptr;
}

void ImmixBlock::Sweep() noexcept {
    live_lines_count = 0;
    for (size_t i = kHeaderLines; i < kLinesPerBlock; ++i) {
        if (line_allocated.test(i)) {
            if (line_marks[i]) {
                ++live_lines_count;
            } else {
                std::memset(AddressForLine(i), kOldGenPoisonByte, kImmixLineSize);
                line_allocated.reset(i);
                object_starts[i * 2] = 0;
                object_starts[i * 2 + 1] = 0;
            }
        }
    }

    std::memset(line_marks, 0, sizeof(line_marks));
    flags &= ~kBlockFlagNeedsSweep;

    if (live_lines_count == 0) {
        state = BlockState::Free;
        cursor_offset = static_cast<uint32_t>(kHeaderSize);
        limit_offset = static_cast<uint32_t>(kImmixBlockSize);
    } else if (live_lines_count == kPayloadLines) {
        state = BlockState::Full;
        cursor_offset = static_cast<uint32_t>(kImmixBlockSize);
        limit_offset = static_cast<uint32_t>(kImmixBlockSize);
    } else {
        state = BlockState::Recyclable;
        cursor_offset = static_cast<uint32_t>(kHeaderSize);
        limit_offset = static_cast<uint32_t>(kHeaderSize);
    }
}

ImmixSpace::~ImmixSpace() {
    DestroyAll();
    for (auto* block : all_blocks_) {
        uintptr_t address = reinterpret_cast<uintptr_t>(block);
        uintptr_t start = reinterpret_cast<uintptr_t>(reservation_start_);
        if (reservation_start_ && address >= start && address - start < max_size_) {
            block->~ImmixBlock();
        }
    }
    for (auto* block : standalone_blocks_) {
        block->~ImmixBlock();
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        free(block);
#endif
    }
    all_blocks_.clear();
    standalone_blocks_.clear();
}

void ImmixSpace::SetReservation(uint8_t* reservation_start, size_t max_size) {
    reservation_start_ = reservation_start;
    max_size_ = max_size;
    uintptr_t raw = reinterpret_cast<uintptr_t>(reservation_start);
    reservation_padding_ = (kImmixBlockSize - (raw % kImmixBlockSize)) % kImmixBlockSize;
}

bool ImmixSpace::AllocateBlock() {
    if (Options::Get().max_heap_mb > 0) {
        size_t opt_limit = Options::Get().max_heap_mb * 1024 * 1024;
        if ((all_blocks_.size() + 1) * kImmixBlockSize > opt_limit) return false;
    }

    all_blocks_.reserve(all_blocks_.size() + 1);
    if (reservation_start_) {
        if (reservation_padding_ > max_size_) return false;
        size_t available_blocks = (max_size_ - reservation_padding_) / kImmixBlockSize;
        if (all_blocks_.size() >= available_blocks) return false;

        size_t offset = reservation_padding_ + all_blocks_.size() * kImmixBlockSize;
        uint8_t* block_addr = reservation_start_ + offset;

#ifdef _WIN32
        if (!VirtualAlloc(block_addr, kImmixBlockSize, MEM_COMMIT, PAGE_READWRITE)) return false;
#else
        if (mprotect(block_addr, kImmixBlockSize, PROT_READ | PROT_WRITE) != 0) return false;
#endif
        auto* block = new (block_addr) ImmixBlock();
        block->space = this;
        block->state = BlockState::Recyclable;
        all_blocks_.push_back(block);
        return true;
    }

    void* mem = nullptr;
#ifdef _WIN32
    mem = VirtualAlloc(nullptr, kImmixBlockSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    if (posix_memalign(&mem, kImmixBlockSize, kImmixBlockSize) != 0) mem = nullptr;
#endif
    if (!mem) return false;

    auto* block = new (mem) ImmixBlock();
    block->space = this;
    block->state = BlockState::Recyclable;
    all_blocks_.push_back(block);
    standalone_blocks_.push_back(block);
    return true;
}

void* ImmixSpace::AllocateLarge(size_t bytes, bool charged) {
    size_t header_size = LargeObjectHeader::HeaderSize();
    size_t total = header_size + bytes;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    size_t page = info.dwPageSize;
#else
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    total = (total + page - 1) / page * page;

    if (Options::Get().max_heap_mb > 0) {
        size_t opt_limit = Options::Get().max_heap_mb * 1024 * 1024;
        size_t current_committed = all_blocks_.size() * kImmixBlockSize;
        for (auto* l = large_objects_head_; l; l = l->next) current_committed += l->size;
        if (current_committed + total > opt_limit) return nullptr;
    }

#ifdef _WIN32
    void* mem = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* mem = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) mem = nullptr;
#endif
    if (!mem) return nullptr;

    auto* header = new (mem) LargeObjectHeader();
    header->size = total;
    header->constructed = false;
    header->charged = charged;
    header->marked = false;

    header->next = large_objects_head_;
    header->prev = nullptr;
    if (large_objects_head_) {
        large_objects_head_->prev = header;
    }
    large_objects_head_ = header;
    ++large_object_count_;

    return header->Object();
}

void ImmixSpace::FreeLarge(LargeObjectHeader* header) noexcept {
    if (!header) return;
    if (header->prev) header->prev->next = header->next;
    else large_objects_head_ = header->next;
    if (header->next) header->next->prev = header->prev;
    --large_object_count_;

    size_t total = header->size;
    (void)total;
#ifdef _WIN32
    VirtualFree(header, 0, MEM_RELEASE);
#else
    munmap(header, total);
#endif
}

bool ImmixSpace::IsLargeObject(const void* ptr) const noexcept {
    if (!large_objects_head_ || !ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    for (auto* curr = large_objects_head_; curr; curr = curr->next) {
        uintptr_t start = reinterpret_cast<uintptr_t>(curr->Object());
        if (addr >= start && addr < reinterpret_cast<uintptr_t>(curr) + curr->size) {
            return true;
        }
    }
    return false;
}

bool ImmixSpace::ContainsBlock(const ImmixBlock* block) const noexcept {
    if (!block) return false;
    if (reservation_start_) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(block);
        uintptr_t start = reinterpret_cast<uintptr_t>(reservation_start_) + reservation_padding_;
        size_t total_span = all_blocks_.size() * kImmixBlockSize;
        if (addr >= start && addr < start + total_span) {
            return (addr - start) % kImmixBlockSize == 0;
        }
    }
    return block->space == this;
}

ImmixBlock* ImmixSpace::BlockForPointer(const void* ptr) const noexcept {
    if (!ptr) return nullptr;
    if (reservation_start_) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t start = reinterpret_cast<uintptr_t>(reservation_start_) + reservation_padding_;
        size_t total_span = all_blocks_.size() * kImmixBlockSize;
        if (addr >= start && addr < start + total_span) {
            return ImmixBlock::BlockFor(ptr);
        }
        return nullptr;
    }
    ImmixBlock* block = ImmixBlock::BlockFor(ptr);
    for (const auto* s : standalone_blocks_) {
        if (s == block) return block;
    }
    return nullptr;
}

void* ImmixSpace::Allocate(size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<size_t>::max() - kImmixLineSize) return nullptr;
    bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

    if (bytes > kLargeObjectThreshold) {
        void* mem = AllocateLarge(bytes);
        last_allocation_ptr_ = mem;
        last_allocation_bytes_ = bytes;
        return mem;
    }

    if (all_blocks_.empty()) {
        if (!AllocateBlock()) return nullptr;
        active_block_index_ = 0;
    }

    if (active_block_index_ < all_blocks_.size()) {
        auto* active = all_blocks_[active_block_index_];
        if (active->flags & ImmixBlock::kBlockFlagNeedsSweep) SweepBlock(active);
        if (active->state == BlockState::Recyclable || active->state == BlockState::Free) {
            void* mem = active->Allocate(bytes);
            if (mem) {
                last_allocation_ptr_ = mem;
                last_allocation_bytes_ = bytes;
                return mem;
            }
        }
    }

    for (size_t i = 0; i < all_blocks_.size(); ++i) {
        if (i == active_block_index_) continue;
        auto* block = all_blocks_[i];
        if (block->flags & ImmixBlock::kBlockFlagNeedsSweep) SweepBlock(block);
        if (block->state == BlockState::Recyclable || block->state == BlockState::Free) {
            void* mem = block->Allocate(bytes);
            if (mem) {
                active_block_index_ = i;
                last_allocation_ptr_ = mem;
                last_allocation_bytes_ = bytes;
                return mem;
            }
        }
    }

    if (!AllocateBlock()) return nullptr;
    active_block_index_ = all_blocks_.size() - 1;
    void* mem = all_blocks_[active_block_index_]->Allocate(bytes);
    if (!mem) return nullptr;
    last_allocation_ptr_ = mem;
    last_allocation_bytes_ = bytes;
    return mem;
}

void ImmixSpace::RegisterObject(HeapObject* obj, bool charged, bool old) {
    if (!obj) throw std::logic_error("Registering null object");
    if (IsLargeObject(obj)) {
        LargeObjectHeader* header = LargeObjectHeader::FromObject(obj);
        header->constructed = true;
        header->charged = charged;
        if (obj->GetAllocatedSize() == 0) {
            obj->SetAllocatedSize(static_cast<uint32_t>(header->size - LargeObjectHeader::HeaderSize()));
        }
        obj->SetCharged(charged);
        obj->SetOld(old);
        if (obj == last_allocation_ptr_) {
            last_allocation_ptr_ = nullptr;
            last_allocation_bytes_ = 0;
        }
        return;
    }

    ImmixBlock* block = ImmixBlock::BlockFor(obj);
    if (!ContainsBlock(block)) {
        throw std::logic_error("Unowned Immix object");
    }

    block->state = BlockState::Recyclable;
    block->SetObjectStart(obj);
    if (obj->GetAllocatedSize() == 0) {
        if (obj == last_allocation_ptr_) {
            obj->SetAllocatedSize(static_cast<uint32_t>(last_allocation_bytes_));
        } else {
            obj->SetAllocatedSize(static_cast<uint32_t>(obj->ObjectSize()));
        }
    }
    obj->SetCharged(charged);
    obj->SetOld(old);

    size_t bytes = obj->GetAllocatedSize();
    if (bytes == 0) bytes = sizeof(HeapObject);
    size_t start_line = block->LineFor(obj);
    size_t end_line = block->LineFor(reinterpret_cast<const uint8_t*>(obj) + bytes - 1);
    for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
        block->line_allocated.set(l);
    }

    if (obj == last_allocation_ptr_) {
        last_allocation_ptr_ = nullptr;
        last_allocation_bytes_ = 0;
    }
}

void ImmixSpace::DiscardAllocation(void* ptr) noexcept {
    if (!ptr) return;
    if (IsLargeObject(ptr)) {
        FreeLarge(LargeObjectHeader::FromObject(static_cast<HeapObject*>(ptr)));
        if (ptr == last_allocation_ptr_) {
            last_allocation_ptr_ = nullptr;
            last_allocation_bytes_ = 0;
        }
        return;
    }
    ImmixBlock* block = ImmixBlock::BlockFor(ptr);
    if (ContainsBlock(block)) {
        block->ClearObjectStart(ptr);
        if (ptr == last_allocation_ptr_) {
            uint8_t* cursor_addr = reinterpret_cast<uint8_t*>(block) + block->cursor_offset;
            if (static_cast<uint8_t*>(ptr) + last_allocation_bytes_ == cursor_addr) {
                block->cursor_offset -= static_cast<uint32_t>(last_allocation_bytes_);
            }
            last_allocation_ptr_ = nullptr;
            last_allocation_bytes_ = 0;
        }
    }
}

void ImmixSpace::SweepBlock(ImmixBlock* block) {
    if (!block || block->state == BlockState::Free) return;
    if (block->flags & ImmixBlock::kBlockFlagNeedsSweep) {
        block->flags &= ~ImmixBlock::kBlockFlagNeedsSweep;
        block->Sweep();
    }
}

void ImmixSpace::EnsureAllSwept() {
    std::lock_guard<std::mutex> lock(mark_mutex_);
    for (auto* block : all_blocks_) {
        if (block && (block->flags & ImmixBlock::kBlockFlagNeedsSweep)) {
            SweepBlock(block);
        }
    }
}

void ImmixSpace::SweepLargeObjects() {
    std::lock_guard<std::mutex> lock(mark_mutex_);
    LargeObjectHeader* header = large_objects_head_;
    while (header) {
        LargeObjectHeader* next = header->next;
        HeapObject* obj = header->Object();
        if (header->constructed && !obj->IsMarked() && !header->marked) {
            obj->~HeapObject();
            if (header->charged && budget_) {
                budget_->ReleaseManaged(header->size - LargeObjectHeader::HeaderSize());
            }
            FreeLarge(header);
        }
        header = next;
    }
}

void ImmixSpace::PrepareLazySweep() {
    std::lock_guard<std::mutex> lock(mark_mutex_);
    for (auto* block : all_blocks_) {
        if (!block || block->state == BlockState::Free) continue;

        block->ForEachObject([&](HeapObject* obj) {
            if (!obj->IsMarked()) {
                size_t bytes = obj->GetAllocatedSize();
                obj->~HeapObject();
                if (obj->IsCharged() && budget_) {
                    budget_->ReleaseManaged(bytes);
                }
                block->ClearObjectStart(obj);
            } else {
                size_t bytes = obj->GetAllocatedSize();
                if (bytes == 0) bytes = sizeof(HeapObject);
                size_t start_line = block->LineFor(obj);
                size_t end_line = block->LineFor(reinterpret_cast<const uint8_t*>(obj) + bytes - 1);
                for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
                    block->line_marks[l] = 1;
                }
            }
        });

        bool has_live = false;
        for (size_t i = kHeaderLines; i < kLinesPerBlock; ++i) {
            if (block->line_marks[i]) {
                has_live = true;
                break;
            }
        }
        if (!has_live) {
            block->Sweep();
        } else {
            block->flags |= ImmixBlock::kBlockFlagNeedsSweep;
        }
    }
    ShrinkLocked();
    ResetActiveBlock();
}

void ImmixSpace::Shrink() {
    std::lock_guard<std::mutex> lock(mark_mutex_);
    ShrinkLocked();
}

void ImmixSpace::ShrinkLocked() {
    size_t free_kept = 0;
    constexpr size_t kMaxCachedFreeBlocks = 1;

    if (!reservation_start_) {
        std::vector<ImmixBlock*> kept_standalone;
        kept_standalone.reserve(standalone_blocks_.size());
        for (auto* block : standalone_blocks_) {
            if (block->state == BlockState::Free) {
                if (free_kept < kMaxCachedFreeBlocks) {
                    ++free_kept;
                    kept_standalone.push_back(block);
                } else {
                    if (last_allocation_ptr_ && block->ContainsPayload(last_allocation_ptr_)) {
                        last_allocation_ptr_ = nullptr;
                        last_allocation_bytes_ = 0;
                    }
                    block->~ImmixBlock();
#ifdef _WIN32
                    VirtualFree(block, 0, MEM_RELEASE);
#else
                    free(block);
#endif
                }
            } else {
                kept_standalone.push_back(block);
            }
        }
        standalone_blocks_ = std::move(kept_standalone);

        std::vector<ImmixBlock*> kept_all;
        kept_all.reserve(standalone_blocks_.size());
        for (auto* block : standalone_blocks_) {
            kept_all.push_back(block);
        }
        all_blocks_ = std::move(kept_all);
    } else {
        while (!all_blocks_.empty() && all_blocks_.back()->state == BlockState::Free) {
            if (free_kept < kMaxCachedFreeBlocks) {
                ++free_kept;
                break;
            }
            ImmixBlock* block = all_blocks_.back();
            all_blocks_.pop_back();
            if (last_allocation_ptr_ && block->ContainsPayload(last_allocation_ptr_)) {
                last_allocation_ptr_ = nullptr;
                last_allocation_bytes_ = 0;
            }
            block->~ImmixBlock();
#ifdef _WIN32
            VirtualFree(block, kImmixBlockSize, MEM_DECOMMIT);
#else
            madvise(block, kImmixBlockSize, MADV_DONTNEED);
#endif
        }
    }

    ResetActiveBlock();
}

void ImmixSpace::Sweep() {
    SweepLargeObjects();
    PrepareLazySweep();
    EnsureAllSwept();
    Shrink();
}

void ImmixSpace::DestroyAll() noexcept {
    EnsureAllSwept();
    for (auto* block : all_blocks_) {
        if (block->state == BlockState::Free) continue;
        block->ForEachObject([&](HeapObject* obj) {
            size_t bytes = obj->GetAllocatedSize();
            obj->~HeapObject();
            if (obj->IsCharged() && budget_) budget_->ReleaseManaged(bytes);
            block->ClearObjectStart(obj);
        });
        block->state = BlockState::Free;
    }
    while (large_objects_head_) {
        auto* curr = large_objects_head_;
        large_objects_head_ = curr->next;
        if (curr->constructed) {
            curr->Object()->~HeapObject();
        }
        if (curr->charged && budget_) {
            budget_->ReleaseManaged(curr->size - LargeObjectHeader::HeaderSize());
        }
        FreeLarge(curr);
    }
}

std::vector<HeapObject*> ImmixSpace::Objects() const {
    const_cast<ImmixSpace*>(this)->EnsureAllSwept();
    std::vector<HeapObject*> result;
    for (const auto* block : all_blocks_) {
        if (block->state == BlockState::Free) continue;
        const_cast<ImmixBlock*>(block)->ForEachObject([&](HeapObject* obj) {
            result.push_back(obj);
        });
    }
    for (auto* lobj = large_objects_head_; lobj; lobj = lobj->next) {
        if (lobj->constructed) {
            result.push_back(lobj->Object());
        }
    }
    return result;
}

size_t ImmixSpace::AllocationSize(const void* obj) const noexcept {
    if (!obj) return 0;
    if (IsLargeObject(obj)) {
        return LargeObjectHeader::FromObject(static_cast<const HeapObject*>(obj))->size - LargeObjectHeader::HeaderSize();
    }
    return static_cast<const HeapObject*>(obj)->GetAllocatedSize();
}

void ImmixSpace::ClearMarks() noexcept {
    EnsureAllSwept();
    for (auto* block : all_blocks_) {
        if (block->state == BlockState::Free) continue;
        std::memset(block->line_marks, 0, sizeof(block->line_marks));
        block->ForEachObject([](HeapObject* obj) {
            obj->SetMarked(false);
            obj->SetPinned(false);
        });
    }
    for (auto* lobj = large_objects_head_; lobj; lobj = lobj->next) {
        lobj->Object()->SetMarked(false);
        lobj->Object()->SetPinned(false);
        lobj->marked = false;
    }
}

bool ImmixSpace::Contains(const void* ptr) const noexcept {
    if (!ptr) return false;
    ImmixBlock* block = BlockForPointer(ptr);
    if (block) return block->ContainsPayload(ptr);
    return IsLargeObject(ptr);
}

void ImmixSpace::MarkObject(HeapObject* obj, size_t allocation_size) {
    if (!obj) return;
    size_t size = allocation_size ? allocation_size : obj->GetAllocatedSize();
    if (large_objects_head_ && (size > kLargeObjectThreshold || obj->GetAllocatedSize() > kLargeObjectThreshold)) {
        if (IsLargeObject(obj)) {
            LargeObjectHeader* header = LargeObjectHeader::FromObject(obj);
            header->marked = true;
            return;
        }
    }

    ImmixBlock* block = BlockForPointer(obj);
    if (!block) return;

    if (size == 0) size = sizeof(HeapObject);
    size_t start_line = block->LineFor(obj);
    size_t end_line = block->LineFor(reinterpret_cast<const uint8_t*>(obj) + size - 1);
    for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
        block->line_marks[l] = 1;
    }
}

bool ImmixSpace::IsValidObjectStart(const void* ptr) const noexcept {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if ((addr & (kObjectAlignment - 1)) != 0) return false;

    if (Contains(ptr)) {
        if (IsLargeObject(ptr)) {
            for (auto* curr = large_objects_head_; curr; curr = curr->next) {
                if (curr->Object() == ptr) return true;
            }
            return false;
        }
        ImmixBlock* block = ImmixBlock::BlockFor(ptr);
        if (block && block->ContainsPayload(ptr)) {
            return block->IsObjectStart(ptr);
        }
    }
    return false;
}

std::vector<ImmixBlock*> ImmixSpace::SelectEvacuationCandidates() {
    std::vector<ImmixBlock*> candidates;
    size_t active_block_count = 0;
    for (auto* block : all_blocks_) {
        if (block && block->state != BlockState::Free) ++active_block_count;
    }
    if (active_block_count <= 1) return candidates;

    for (auto* block : all_blocks_) {
        if (!block || block->state == BlockState::Free) continue;
        size_t live_bytes = 0, live_objects = 0;
        block->ForEachObject([&](HeapObject* obj) {
            if (obj && obj->IsMarked()) {
                live_bytes += obj->GetAllocatedSize() ? obj->GetAllocatedSize() : obj->ObjectSize();
                ++live_objects;
            }
        });
        if (live_objects > 0 && live_bytes <= (kPayloadSize / 2)) {
            block->flags |= ImmixBlock::kBlockFlagEvacuate;
            candidates.push_back(block);
        }
    }
    return candidates;
}

void* ImmixSpace::AllocateForEvacuation(size_t bytes) {
    if (bytes == 0 || bytes > kPayloadSize) return nullptr;
    bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

    // 1. Try non-candidate recyclable blocks first
    for (auto* block : all_blocks_) {
        if (block->state == BlockState::Recyclable && !(block->flags & ImmixBlock::kBlockFlagEvacuate)) {
            void* mem = block->Allocate(bytes);
            if (mem) {
                last_allocation_ptr_ = mem;
                last_allocation_bytes_ = bytes;
                return mem;
            }
        }
    }

    // 2. Try non-candidate free blocks
    for (auto* block : all_blocks_) {
        if (block->state == BlockState::Free && !(block->flags & ImmixBlock::kBlockFlagEvacuate)) {
            block->state = BlockState::Recyclable;
            void* mem = block->Allocate(bytes);
            if (mem) {
                last_allocation_ptr_ = mem;
                last_allocation_bytes_ = bytes;
                return mem;
            }
        }
    }

    // 3. Allocate fresh block from reservation if space remains
    try {
        AllocateBlock();
        auto* new_block = all_blocks_.back();
        void* mem = new_block->Allocate(bytes);
        if (mem) {
            last_allocation_ptr_ = mem;
            last_allocation_bytes_ = bytes;
            return mem;
        }
    } catch (...) {
        return nullptr;
    }
    return nullptr;
}

void ImmixSpace::ResetActiveBlock() noexcept {
    for (size_t i = 0; i < all_blocks_.size(); ++i) {
        if (all_blocks_[i]->state == BlockState::Recyclable ||
            all_blocks_[i]->state == BlockState::Free ||
            (all_blocks_[i]->flags & ImmixBlock::kBlockFlagNeedsSweep)) {
            active_block_index_ = i;
            return;
        }
    }
    active_block_index_ = 0;
}

size_t ImmixSpace::ActiveCommittedBytes() const noexcept {
    size_t count = 0;
    for (const auto* b : all_blocks_) if (b && b->state != BlockState::Free) count += kImmixBlockSize;
    for (const auto* lobj = large_objects_head_; lobj; lobj = lobj->next) count += lobj->size;
    return count;
}

size_t ImmixSpace::TotalCommittedBytes() const noexcept {
    size_t count = all_blocks_.size() * kImmixBlockSize;
    for (const auto* lobj = large_objects_head_; lobj; lobj = lobj->next) count += lobj->size;
    return count;
}

size_t ImmixSpace::FreeBlockCount() const noexcept {
    size_t count = 0;
    for (const auto* b : all_blocks_) if (b && b->state == BlockState::Free) ++count;
    return count;
}

bool ImmixSpace::HasUnsweptBlocks() const noexcept {
    for (const auto* b : all_blocks_) {
        if (b && (b->flags & ImmixBlock::kBlockFlagNeedsSweep)) return true;
    }
    return false;
}

} // namespace aether
