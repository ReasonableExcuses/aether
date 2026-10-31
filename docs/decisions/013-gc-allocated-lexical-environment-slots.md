# Decision 013: LexicalEnvironment Slots Migration to GC-Allocated FixedArray

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), lexical scope environments (`LexicalEnvironment`) for closures and captured variables previously stored their slots in:
- `MemoryCharge slots_storage_` (16 bytes)
- `std::vector<TaggedValue> slots_` (24 bytes)

This representation had several major drawbacks:
1. **Non-trivial Destructor**: `LexicalEnvironment` had a non-trivial destructor due to `std::vector` and `MemoryCharge`, requiring out-of-band heap deallocation rather than uniform Immix line reclamation.
2. **Excessive Object Header Footprint**: The object header was 72 bytes (`HeapObject` base 24 B + `outer_` 8 B + `slots_storage_` 16 B + `slots_` 24 B).
3. **Out-of-Heap Storage**: Variable slots were allocated in standard C++ heap memory via `malloc` / `operator new` rather than within the Immix GC nursery/old generation.

## Decision
1. **Direct GC-Managed Storage**:
   - Replaced `MemoryCharge slots_storage_` and `std::vector<TaggedValue> slots_` with a single raw pointer `FixedArray* slots_{nullptr};`.
   - Reduced `sizeof(LexicalEnvironment)` from 72 bytes to **40 bytes** (24-byte `HeapObject` base + 8-byte `outer_` pointer + 8-byte `slots_` pointer).
   - Zero auxiliary allocation for empty scopes (`slot_count == 0` leaves `slots_ == nullptr`).
   - `LexicalEnvironment` is now **trivially destructible** (`~LexicalEnvironment() override = default;`), satisfying Decision D4.

2. **Accessors and Write Barriers**:
   - Updated `GetSlot(index)` and `Get(depth, index)` to read directly from `slots_->elements()[index]`.
   - Updated `SetSlot(index, val)` and `Set(depth, index, val)` to write into `slots_->elements()[index]` and record generational write barriers (`RecordWriteBarrier(curr->slots_, val)`) on the underlying `FixedArray` when writing heap object pointers into an old-generation environment.
   - Updated `IteratePointers` to visit `outer_` and `slots_` as first-class GC pointer slots.

3. **Compile-Time Size & Triviality Invariants**:
   - Added `static_assert(sizeof(LexicalEnvironment) == 40)` and `static_assert(std::is_trivially_destructible_v<LexicalEnvironment>)` in `tests/test_object_sizes.cpp`.

## Verification
- Verified compile-time layout: `sizeof(LexicalEnvironment) == 40`.
- Verified trivial destructibility: `std::is_trivially_destructible_v<LexicalEnvironment> == true`.
- All closures, generator environments, async/await frames, and exception handler scopes verified across test suites.
- 100% pass rate maintained across all 31 CTest suites and 9 matrix configurations.

