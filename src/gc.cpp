#include "aether/gc.h"
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

NurserySpace::NurserySpace(size_t semi_space_size)
    : semi_space_size_(semi_space_size) {
    if (semi_space_size == 0 || semi_space_size > kHeapReservationSize / 2 ||
        semi_space_size % kObjectAlignment != 0) {
        throw std::length_error("Invalid nursery size");
    }
    size_t total_bytes = semi_space_size * 2;
#ifdef _WIN32
    buffer_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kHeapReservationSize, MEM_RESERVE, PAGE_NOACCESS));
    if (buffer_) {
        has_reservation_ = true;
        if (!VirtualAlloc(buffer_, total_bytes, MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(buffer_, 0, MEM_RELEASE);
            throw std::bad_alloc();
        }
    } else {
        buffer_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, total_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
#else
    buffer_ = static_cast<uint8_t*>(mmap(nullptr, kHeapReservationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (buffer_ != MAP_FAILED) {
        has_reservation_ = true;
        if (mprotect(buffer_, total_bytes, PROT_READ | PROT_WRITE) != 0) {
            munmap(buffer_, kHeapReservationSize);
            throw std::bad_alloc();
        }
    } else {
        buffer_ = static_cast<uint8_t*>(mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (buffer_ == MAP_FAILED) buffer_ = nullptr;
    }
#endif
    if (!buffer_) throw std::bad_alloc();

    from_space_start_ = reinterpret_cast<uintptr_t>(buffer_);
    from_space_limit_ = from_space_start_ + semi_space_size;
    bump_ptr_         = from_space_start_;

    to_space_start_   = from_space_limit_;
    to_space_limit_   = to_space_start_ + semi_space_size;
}

NurserySpace::~NurserySpace() {
    if (buffer_) {
#ifdef _WIN32
        VirtualFree(buffer_, 0, MEM_RELEASE);
#else
        munmap(buffer_, has_reservation_ ? kHeapReservationSize : semi_space_size_ * 2);
#endif
        buffer_ = nullptr;
    }
}

void* NurserySpace::Allocate(size_t bytes) noexcept {
    if (bytes == 0 || bytes > std::numeric_limits<size_t>::max() - (kObjectAlignment - 1)) return nullptr;
    bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);
    if (AETHER_LIKELY(bytes <= from_space_limit_ - bump_ptr_)) {
        void* mem = reinterpret_cast<void*>(bump_ptr_);
        bump_ptr_ += bytes;
        return mem;
    }
    return nullptr;
}

void NurserySpace::Flip() noexcept {
    Flip(to_space_start_);
}

void NurserySpace::Flip(uintptr_t live_bump) noexcept {
    assert(live_bump >= to_space_start_ && live_bump <= to_space_limit_);
    // Poison the old from-space (about to become to-space) to catch use-after-free.
    // Any stale pointer reading from evacuated memory will hit 0xDD bytes,
    // causing immediate crashes instead of silent data corruption.
    size_t used_bytes = bump_ptr_ - from_space_start_;
    if (used_bytes > 0) {
        std::memset(reinterpret_cast<void*>(from_space_start_), kNurseryPoisonByte, used_bytes);
    }

    std::swap(from_space_start_, to_space_start_);
    std::swap(from_space_limit_, to_space_limit_);
    bump_ptr_ = live_bump;
}

void* ImmixBlock::Allocate(size_t bytes) {
    if (bytes == 0 || bytes > kImmixBlockSize) return nullptr;
    size_t lines_needed = (bytes + kImmixLineSize - 1) / kImmixLineSize;
    if (lines_needed > kLinesPerBlock) return nullptr;

    for (size_t i = current_line; i + lines_needed <= kLinesPerBlock; ++i) {
        bool free = true;
        for (size_t j = 0; j < lines_needed; ++j) {
            if (line_allocated.test(i + j)) {
                free = false;
                i += j;
                break;
            }
        }
        if (free) {
            for (size_t j = 0; j < lines_needed; ++j) {
                line_allocated.set(i + j);
            }
            current_line = i + lines_needed;
            return data + (i * kImmixLineSize);
        }
    }
    return nullptr;
}

void ImmixBlock::Sweep() noexcept {
    // Poison freed lines: any line that was allocated but NOT marked alive is dead.
    // Fill with 0xCD so stale old-gen pointers trigger visible corruption.
    for (size_t i = 0; i < kLinesPerBlock; ++i) {
        if (line_allocated.test(i) && !line_marks.test(i)) {
            std::memset(data + (i * kImmixLineSize), kOldGenPoisonByte, kImmixLineSize);
        }
    }
    line_allocated = line_marks;
    line_marks.reset();
    current_line = 0;
}

ImmixSpace::~ImmixSpace() {
    DestroyAll();
    for (auto* block : blocks_) {
        uintptr_t address = reinterpret_cast<uintptr_t>(block);
        uintptr_t start = reinterpret_cast<uintptr_t>(reservation_start_);
        if (reservation_start_ && address >= start && address - start < max_size_) {
            block->~ImmixBlock();
        } else {
            delete block;
        }
    }
}

bool ImmixSpace::AllocateBlock() {
    blocks_.reserve(blocks_.size() + 1);
    if (reservation_start_) {
        // Each commitment must start on an OS page boundary (mprotect requires
        // this; sizeof(ImmixBlock) includes non-page-aligned metadata).
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const size_t page = info.dwPageSize;
#else
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
        const size_t stride = (sizeof(ImmixBlock) + page - 1) / page * page;
        const uintptr_t raw = reinterpret_cast<uintptr_t>(reservation_start_);
        const size_t padding = (page - raw % page) % page;
        if (padding <= max_size_ && blocks_.size() < (max_size_ - padding) / stride) {
            const size_t offset = padding + blocks_.size() * stride;
            uint8_t* block_addr = reservation_start_ + offset;
#ifdef _WIN32
            if (!VirtualAlloc(block_addr, stride, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc();
#else
            if (mprotect(block_addr, stride, PROT_READ | PROT_WRITE) != 0) throw std::bad_alloc();
#endif
            auto* block = new (block_addr) ImmixBlock();
            blocks_.push_back(block);
            return true;
        }
        throw std::bad_alloc();
    }
    blocks_.push_back(new ImmixBlock());
    return true;
}

void* ImmixSpace::Allocate(size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<size_t>::max() - kImmixLineSize) throw std::bad_alloc();
    // Allocate bookkeeping first. Installing the extracted node after taking
    // storage cannot fail and strand untracked occupied lines on native OOM.
    auto node = allocations_.extract(allocations_.emplace(nullptr, Allocation{bytes, bytes > kImmixBlockSize}).first);
    auto record = [&](void* mem) {
        node.key() = mem;
        allocations_.insert(std::move(node));
        return mem;
    };
    if (bytes > kImmixBlockSize) {
        return record(::operator new(bytes));
    }
    if (blocks_.empty()) {
        AllocateBlock();
        active_block_index_ = 0;
    }

    void* mem = blocks_[active_block_index_]->Allocate(bytes);
    if (mem) return record(mem);

    // Search recyclable blocks for hole-filling
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (i == active_block_index_) continue;
        mem = blocks_[i]->Allocate(bytes);
        if (mem) {
            active_block_index_ = i;
            return record(mem);
        }
    }

    // Allocate fresh block
    AllocateBlock();
    active_block_index_ = blocks_.size() - 1;
    mem = blocks_[active_block_index_]->Allocate(bytes);
    return record(mem);
}

void ImmixSpace::Sweep() {
    for (auto it = allocations_.begin(); it != allocations_.end(); ) {
        auto* obj = static_cast<HeapObject*>(it->first);
        const Allocation allocation = it->second;
        // Raw placement-new storage is owned by its caller until registered.
        // It is retained by line marks if the caller has rooted it.
        bool live = allocation.constructed && obj->IsMarked();
        if (!allocation.constructed && !allocation.large) {
            for (auto* block : blocks_) {
                if (block->Contains(obj)) {
                    size_t line = (reinterpret_cast<uintptr_t>(obj) - reinterpret_cast<uintptr_t>(block->data)) / kImmixLineSize;
                    live = block->line_marks.test(line);
                    break;
                }
            }
        }
        if (live) {
            if (allocation.constructed) MarkObject(obj, allocation.bytes);
            ++it;
            continue;
        }
        if (allocation.constructed) obj->~HeapObject();
        if (allocation.charged && budget_) budget_->ReleaseManaged(allocation.bytes);
        if (allocation.large) ::operator delete(obj);
        it = allocations_.erase(it);
    }
    for (auto& block : blocks_) {
        block->Sweep();
    }
}

bool ImmixSpace::Contains(const void* ptr) const noexcept {
    if (allocations_.find(const_cast<void*>(ptr)) != allocations_.end()) return true;
    for (const auto& block : blocks_) {
        if (block->Contains(ptr)) return true;
    }
    return false;
}

void ImmixSpace::MarkObject(HeapObject* obj, size_t allocation_size) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t size = allocation_size ? allocation_size : AllocationSize(obj);
    if (size == 0) return;

    std::lock_guard<std::mutex> lock(mark_mutex_);
    for (auto& block : blocks_) {
        if (block->Contains(obj)) {
            uintptr_t block_start = reinterpret_cast<uintptr_t>(block->data);
            size_t start_line = (addr - block_start) / kImmixLineSize;
            size_t end_line = (addr + size - 1 - block_start) / kImmixLineSize;
            for (size_t line = start_line; line <= end_line && line < kLinesPerBlock; ++line) {
                block->line_marks.set(line);
            }
            break;
        }
    }
}

void ImmixSpace::RegisterObject(HeapObject* obj, bool charged) {
    auto it = allocations_.find(obj);
    if (it == allocations_.end()) throw std::logic_error("Unowned Immix object");
    it->second.constructed = true;
    it->second.charged = charged;
}

void ImmixSpace::DiscardAllocation(void* ptr) noexcept {
    auto it = allocations_.find(ptr);
    if (it == allocations_.end()) return;
    if (it->second.large) ::operator delete(ptr);
    else {
        for (auto* block : blocks_) if (block->Contains(ptr)) {
            size_t first = (reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(block->data)) / kImmixLineSize;
            size_t count = (it->second.bytes + kImmixLineSize - 1) / kImmixLineSize;
            for (size_t i = first; i < first + count; ++i) {
                block->line_allocated.reset(i);
                block->line_marks.reset(i);
            }
            block->current_line = 0;
            break;
        }
    }
    allocations_.erase(it);
}

void ImmixSpace::DestroyAll() noexcept {
    for (auto& [ptr, allocation] : allocations_) {
        if (allocation.constructed) static_cast<HeapObject*>(ptr)->~HeapObject();
        if (allocation.charged && budget_) budget_->ReleaseManaged(allocation.bytes);
        if (allocation.large) ::operator delete(ptr);
    }
    allocations_.clear();
}

std::vector<HeapObject*> ImmixSpace::Objects() const {
    std::vector<HeapObject*> result;
    result.reserve(allocations_.size());
    for (auto& [ptr, allocation] : allocations_) {
        if (allocation.constructed) result.push_back(static_cast<HeapObject*>(ptr));
    }
    return result;
}

size_t ImmixSpace::AllocationSize(const void* obj) const noexcept {
    auto it = allocations_.find(const_cast<void*>(obj));
    return it == allocations_.end() ? 0 : it->second.bytes;
}

void ImmixSpace::ClearMarks() noexcept {
    for (auto* block : blocks_) block->line_marks.reset();
    for (auto& [ptr, allocation] : allocations_) {
        if (allocation.constructed) static_cast<HeapObject*>(ptr)->SetMarked(false);
    }
}

} // namespace aether
