#include "aether/gc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace aether {

NurserySpace::NurserySpace(size_t semi_space_size)
    : semi_space_size_(semi_space_size) {
    size_t total_bytes = semi_space_size * 2;
#ifdef _WIN32
    buffer_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kHeapReservationSize, MEM_RESERVE, PAGE_NOACCESS));
    if (buffer_) {
        has_reservation_ = true;
        VirtualAlloc(buffer_, total_bytes, MEM_COMMIT, PAGE_READWRITE);
    } else {
        buffer_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, total_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
#else
    buffer_ = static_cast<uint8_t*>(mmap(nullptr, kHeapReservationSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (buffer_ != MAP_FAILED) {
        has_reservation_ = true;
        mprotect(buffer_, total_bytes, PROT_READ | PROT_WRITE);
    } else {
        buffer_ = static_cast<uint8_t*>(mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    }
#endif
    assert(buffer_ != nullptr && "Failed to map virtual memory for Nursery");

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
    bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);
    uintptr_t next = bump_ptr_ + bytes;
    if (AETHER_LIKELY(next <= from_space_limit_)) {
        void* mem = reinterpret_cast<void*>(bump_ptr_);
        bump_ptr_ = next;
        return mem;
    }
    return nullptr;
}

void NurserySpace::Flip() noexcept {
    std::swap(from_space_start_, to_space_start_);
    std::swap(from_space_limit_, to_space_limit_);
    bump_ptr_ = from_space_start_;
}

void* ImmixBlock::Allocate(size_t bytes) {
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
    line_allocated = line_marks;
    line_marks.reset();
    current_line = 0;
}

ImmixSpace::~ImmixSpace() {
    for (auto* block : blocks_) {
        if (using_reservation_) {
            block->~ImmixBlock();
        } else {
            delete block;
        }
    }
}

void ImmixSpace::AllocateBlock() {
    if (reservation_start_) {
        size_t offset = blocks_.size() * sizeof(ImmixBlock);
        if (offset + sizeof(ImmixBlock) <= max_size_) {
            uint8_t* block_addr = reservation_start_ + offset;
#ifdef _WIN32
            VirtualAlloc(block_addr, sizeof(ImmixBlock), MEM_COMMIT, PAGE_READWRITE);
#else
            mprotect(block_addr, sizeof(ImmixBlock), PROT_READ | PROT_WRITE);
#endif
            auto* block = new (block_addr) ImmixBlock();
            using_reservation_ = true;
            blocks_.push_back(block);
            return;
        }
    }
    blocks_.push_back(new ImmixBlock());
}

void* ImmixSpace::Allocate(size_t bytes) {
    if (blocks_.empty()) {
        AllocateBlock();
        active_block_index_ = 0;
    }

    void* mem = blocks_[active_block_index_]->Allocate(bytes);
    if (mem) return mem;

    // Search recyclable blocks for hole-filling
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (i == active_block_index_) continue;
        mem = blocks_[i]->Allocate(bytes);
        if (mem) {
            active_block_index_ = i;
            return mem;
        }
    }

    // Allocate fresh block
    AllocateBlock();
    active_block_index_ = blocks_.size() - 1;
    return blocks_[active_block_index_]->Allocate(bytes);
}

void ImmixSpace::Sweep() {
    for (auto& block : blocks_) {
        block->Sweep();
    }
}

bool ImmixSpace::Contains(const void* ptr) const noexcept {
    for (const auto& block : blocks_) {
        if (block->Contains(ptr)) return true;
    }
    return false;
}

void ImmixSpace::MarkObject(HeapObject* obj) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t size = obj->ObjectSize();

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

} // namespace aether
