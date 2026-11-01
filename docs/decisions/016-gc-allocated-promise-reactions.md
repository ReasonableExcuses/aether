# Decision 016: JSPromise Reactions Migration to GC-Allocated FixedArray

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), `JSPromise` previously maintained its queue of reaction callbacks in an out-of-band `std::vector<PromiseReaction> reactions_` guarded by a `MemoryCharge reactions_storage_`:
- `MemoryCharge reactions_storage_` (16 bytes)
- `std::vector<PromiseReaction> reactions_` (24 bytes)

This out-of-band representation suffered from several architectural drawbacks:
1. **Non-Trivial Destructors**: `JSPromise` required a custom destructor to deallocate the reactions vector and release memory charge, preventing uniform, high-throughput line-sweep reclamation in Immix.
2. **Excessive Object Header Footprint**: `sizeof(JSPromise)` was 112 bytes (`JSObject` base 56 B + `PromiseState state_` 1 B + 7 B padding + `TaggedValue result_` 8 B + `MemoryCharge reactions_storage_` 16 B + `std::vector` 24 B).
3. **Out-of-Heap Storage**: Reaction triplets (`on_fulfilled`, `on_rejected`, `downstream_promise`) were stored in process heap memory via `malloc`/`operator new` rather than within GC nursery/old generation blocks.
4. **Suboptimal Tracing**: GC slot scanning in `IteratePointers` manually walked the vector elements rather than leveraging uniform `FixedArray` slot visitation.

## Decision
1. **Direct GC-Managed Reaction Storage**:
   - Replaced `MemoryCharge reactions_storage_` and `std::vector<PromiseReaction> reactions_` with `FixedArray* reactions_{nullptr};`.
   - Layout reordered and packed: `JSObject` base (56 B) + `TaggedValue result_` (8 B) + `FixedArray* reactions_` (8 B) + `PromiseState state_` (1 B) + 7 B trailing padding = **80 bytes** (reduced from 112 bytes, saving 32 bytes per promise object, a 28.5% reduction in header footprint).
   - Achieved trivial destructor (`~JSPromise() override = default;`), satisfying Decision D4.

2. **Contiguous Triplet FixedArray Encoding**:
   - Reactions are stored contiguously in groups of three slots in `FixedArray* reactions_`:
     - Slot $3i + 0$: `on_fulfilled` (`TaggedValue`)
     - Slot $3i + 1$: `on_rejected` (`TaggedValue`)
     - Slot $3i + 2$: `downstream_promise` (`TaggedValue::FromHeapObject(downstream)` or `TaggedValue::Null()`)
   - Number of pending reactions: `ReactionCount() == reactions_ ? (reactions_->Length() / 3) : 0`.
   - Fast inspection queries: `HasReactions()`, `ReactionCount()`, `ReactionsArray()`.
   - Immediate settlement detachment: On `Fulfill` or `Reject`, reactions are queued to `MicrotaskQueue` and `reactions_ = nullptr;` immediately detaches the list, permitting prompt minor GC reclamation.

3. **Generational Write Barriers & Rooting**:
   - Array resizing uses `AllocateRuntimeFixedArrayFor` with geometric doubling and safe rooting via `HandleScope`.
   - Inter-generational write barrier: If `JSPromise` is tenured (old) and `reactions_` is young, `RecordWriteBarrier(this, reactions_)` is emitted.
   - Slot writes automatically call `FixedArray::Set`, which handles write barriers for old `reactions_` to young values/downstream promises.
   - Tracing simplified: `IteratePointers` visits `reactions_` as a single GC pointer slot, delegating element scanning to `FixedArray::IteratePointers`.

## Verification
- Verified compile-time layout: `sizeof(JSPromise) == 80`.
- Added runtime test `TestPromiseMemoryDiet` in `tests/test_object_sizes.cpp`.
- 100% pass rate across all 31 CTest suites.
- 100% pass rate across all 9 matrix configurations in `tools/matrix.py`.
