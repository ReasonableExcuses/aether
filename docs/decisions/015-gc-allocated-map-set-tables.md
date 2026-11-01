# Decision 015: JSMap and JSSet Tables Migration to GC-Allocated FixedArray

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), collection objects `JSMap` and `JSSet` previously stored their entries in out-of-band heap vectors:
- `JSMap`: `MemoryCharge entries_storage_` (16 bytes) and `std::vector<std::pair<TaggedValue, TaggedValue>> entries_` (24 bytes)
- `JSSet`: `MemoryCharge values_storage_` (16 bytes) and `std::vector<TaggedValue> values_` (24 bytes)

This out-of-band representation had several drawbacks:
1. **Non-Trivial Destructors**: Both `JSMap` and `JSSet` had non-trivial destructors requiring out-of-band `free` calls rather than uniform Immix line sweeping.
2. **Excessive Object Header Footprint**: Both object headers were 96 bytes (`JSObject` base 56 B + `MemoryCharge` 16 B + `std::vector` 24 B).
3. **Out-of-Heap Storage**: Collection entries were allocated via `malloc` / `operator new` rather than within the Immix GC nursery/old generation blocks.
4. **Transient Allocation on Iteration**: Iterators (`Map.prototype.entries()`, `keys()`, `values()`, and `Set.prototype.values()`) created and queried full vector copies.

## Decision
1. **Direct GC-Managed Storage**:
   - Replaced `MemoryCharge entries_storage_` and `std::vector<...>` in `JSMap` with `FixedArray* table_{nullptr};`.
   - Replaced `MemoryCharge values_storage_` and `std::vector<...>` in `JSSet` with `FixedArray* table_{nullptr};`.
   - Reduced `sizeof(JSMap)` from 96 bytes to **64 bytes** (56-byte `JSObject` base + 8-byte `table_` pointer).
   - Reduced `sizeof(JSSet)` from 96 bytes to **64 bytes** (56-byte `JSObject` base + 8-byte `table_` pointer).
   - Both `JSMap` and `JSSet` achieved trivial destructors (`~JSMap() override = default;`, `~JSSet() override = default;`), satisfying Decision D4.

2. **Contiguous Key-Value & Value Layout**:
   - `JSMap` encodes key-value pairs contiguously in `FixedArray* table_` of length $2 \times N$:
     - Slot $2i + 0$: `key`
     - Slot $2i + 1$: `value`
     - Number of entries: `Size() == table_ ? (table_->Length() / 2) : 0`.
   - `JSSet` encodes unique values contiguously in `FixedArray* table_` of length $N$:
     - Slot $i$: `value`
     - Number of elements: `Size() == table_ ? table_->Length() : 0`.
   - Table growth employs geometric capacity doubling with safe intermediate pointer rooting via `HandleScope`.
   - Deletion performs in-place compaction preserving insertion order, and clears trailing slots to `TaggedValue::Empty()`.

3. **Zero-Allocation Iteration**:
   - Added `map->Table()` and `set->Table()` accessors enabling iterator steps in `src/runtime.cpp` to read directly from `FixedArray` slots without constructing transient vectors.

4. **Generational Write Barriers & Tracing**:
   - Generational write barriers on `JSMap` / `JSSet` record `this` pointing to `table_` when allocating a young table for a tenured collection.
   - Element writes automatically trigger `FixedArray::Set` generational write barriers when writing young values into a tenured table.
   - Tracing in `IteratePointers` visits `table_` as a single GC pointer slot, delegating element scanning to `FixedArray::IteratePointers`.

## Verification
- Verified compile-time layout: `sizeof(JSMap) == 64`, `sizeof(JSSet) == 64`.
- Added runtime test `TestMapSetMemoryDiet` in `tests/test_object_sizes.cpp`.
- 100% pass rate across all 31 CTest suites.
- 100% pass rate across all 9 matrix configurations in `tools/matrix.py`.
