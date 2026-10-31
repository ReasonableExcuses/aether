# Decision 012: JSArray Elements Migration to GC-Allocated FixedArray

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), `JSArray` elements previously lived in an auxiliary C++ structure `ArrayStorage` containing `std::vector<TaggedValue> elements` and an explicit `MemoryCharge elements_storage`.

This previous representation introduced three fundamental drawbacks:
1. **Non-trivial Destructor**: `JSArray` required `~JSArray() override = default;` to invoke `std::unique_ptr<ArrayStorage>` destruction, which freed out-of-band memory rather than allowing the Immix mark-sweep/evacuation engine to reclaim lines uniformly.
2. **Auxiliary Allocation Overhead**: Even an empty array (`[]` / `new Array()`) required either deferred allocation or heap churn on initial creation.
3. **Out-of-Heap Storage**: Array element slots were not directly traversable as first-class GC heap objects.

## Decision
1. **Direct GC-Managed Storage**:
   - Replaced `std::unique_ptr<ArrayStorage> array_storage_` with `FixedArray* elements_{nullptr};`.
   - `JSArray` destructor is now completely trivial (`~JSArray() override = default;`), freeing zero auxiliary memory out-of-band.
   - Preserved `sizeof(JSArray) == 64` bytes (56-byte `JSObject` base + 8-byte pointer).
   - Empty arrays (`JSArray(&shape, 0)`) have `elements_ == nullptr`, requiring zero auxiliary allocation.

2. **Array Method Implementation**:
   - Implemented high-performance methods directly on `JSArray`: `SetElement`, `SetLength`, `Push`, `Pop`, `Shift`, `Unshift`, `Reverse`, `Splice`, `ReserveElements`, `ReplaceElements`.
   - Element operations preserve amortized $2\times$ growth geometry using `FixedArray` allocation.
   - Write barriers (`RecordWriteBarrier`) automatically fire when writing `HeapObject` pointers into an old-generation array's `FixedArray`.

3. **Owner-Isolate Allocation & Root Protection**:
   - Added `IsolateHeap::HeapForObject(const void* ptr)` using thread-local active heap tracking (`s_active_heaps`) and $O(1)$ Immix space bounds checking (`Contains(ptr)`).
   - Added allocation facades `AllocateRuntimeFixedArrayFor(owner, ...)` and `AllocateRuntimeByteArrayFor(owner, ...)` that guarantee array resizes allocate on the owner isolate's heap even during cross-isolate operations.
   - Guaranteed GC safety during resizes: `AllocateRuntimeFixedArrayFor` roots `owner` (including existing `FixedArray* elements_`) in a temporary `HandleScope`, ensuring existing array elements are marked and protected from collection when `ReserveManaged` triggers `MajorGC()` under tight memory budgets.

4. **Traversal & Object Model**:
   - `JSArray::IteratePointers` visits `elements_` as a direct GC `TaggedValue` slot.
   - When GC visits `elements_`, `FixedArray::IteratePointers` traverses and marks all element slots.
   - Updated `JSObject::GetNamedProperty`, `HasNamedProperty`, `HasOwnProperty`, and `GetOwnPropertyNames` to query `HasElement` and `GetElement`.
   - Updated `Interpreter` indexed element operations and all `Array.prototype` methods in `src/runtime.cpp` to use direct `JSArray` methods.

## Verification
- `sizeof(JSArray)` validated at 64 bytes via `static_assert`.
- Empty arrays confirmed to have `ElementsArray() == nullptr` in `tests/test_object_sizes.cpp`.
- Array growth, contraction, push/pop/shift/unshift/splice/reverse, and memory budget limit handling verified in `tests/test_hardening_limits.cpp`.
- All 31 test suites pass with 100% pass rate under CTest.
