# Decision 014: JSFunction Captured Data Migration to GC-Allocated FixedArray

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), `JSFunction` previously stored dynamically captured scopes and class instance field descriptors in out-of-band heap vectors inside `FunctionData`:
- `MemoryCharge with_storage` and `std::vector<JSObject*> with_stack` (captured `with` scope chains when closures are instantiated within `with` blocks)
- `MemoryCharge fields_storage` and `std::vector<InstanceField> instance_fields` (captured instance fields and private method descriptors on class constructor functions)
- `MemoryCharge name_storage` and `std::string name` within each `InstanceField`

This out-of-band representation had significant disadvantages:
1. **Out-of-Heap Storage & Manual Tracking**: Captured scopes and fields required separate C++ allocator invocations and explicit manual `MemoryCharge` bookkeeping.
2. **Double GC Scanning Overhead**: `JSFunction::IteratePointers` was forced to iterate raw vectors and manually visit each nested object and value.
3. **Transient Overhead on Invocations**: Passing or reading `with_stack` required copying or allocating `std::vector<JSObject*>`.

## Decision
1. **GC-Allocated Storage via FixedArray**:
   - Replaced `MemoryCharge with_storage` and `std::vector<JSObject*> with_stack` with `FixedArray* with_stack{nullptr};`.
   - Replaced `MemoryCharge fields_storage` and `std::vector<InstanceField> instance_fields` with `FixedArray* instance_fields{nullptr};`.
   - Removed `MemoryCharge name_storage` from `InstanceField`.
   - In `FunctionData`, memory is saved and dynamic array allocations now reside directly on the GC-managed Immix heap.

2. **Field Descriptor Encoding**:
   - Stored instance fields in triplets within `instance_fields` (`FixedArray` length = $3 \times N$):
     - Slot $3i + 0$: Field name as GC-managed `JSString*` (`TaggedValue::FromHeapObject(...)`)
     - Slot $3i + 1$: Initial value (`TaggedValue`)
     - Slot $3i + 2$: Method flag (`TaggedValue::FromBoolean(is_method)`)
   - `AddInstanceField()` resizes the `FixedArray` with geometric doubling and safely roots intermediate strings and values across allocations.

3. **Optimized Accessors & Zero-Allocation Fast Paths**:
   - Added `GetWithStackArray()` allowing the interpreter to iterate captured `with` frames without creating transient `std::vector` instances.
   - Added `HasInstanceFields()` enabling runtime and interpreter call paths to bypass field iteration entirely for standard functions.
   - Preserved `InstanceFields()` and `GetWithStack()` for backward compatibility.

4. **Generational Write Barriers & Tracing**:
   - When storing `with_stack` or `instance_fields` into an old-generation `JSFunction`, generational write barriers record `this` pointing to the `FixedArray`.
   - When modifying elements inside the arrays, `FixedArray::Set` automatically records generational write barriers on the `FixedArray` if it is tenured and elements are young.
   - Tracing in `JSFunction::IteratePointers` uniformly visits `with_stack` and `instance_fields` as standard heap object slots; element tracing is delegated to `FixedArray::IteratePointers`.

## Verification
- Verified compile-time layout: `sizeof(JSFunction) == 64`.
- Verified runtime behavior in `tests/test_object_sizes.cpp` (`TestFunctionMemoryDiet`).
- 100% pass rate across all 31 CTest suites.
- 100% pass rate across all 9 configurations in full matrix validation (`tools/matrix.py`).
