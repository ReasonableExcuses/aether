# Decision 010: Per-Object Memory Diet

## Context
Under `AETHER_AGENT_PLAN.md` T1.7, per-object memory footprint in AetherVM previously suffered from severe memory bloat:
- `JSObject` embedded a 24-byte `HeapObject` header, multiple raw `std::vector` and `std::unordered_map` instances for properties, accessors, deleted properties, and attribute overrides, plus multiple boolean status flags. A default object `{a: 1, b: 2}` occupied $> 1.2$ KB across 7 auxiliary heap allocations.
- `JSArray` embedded a full `std::vector<TaggedValue, ManagedAllocator<TaggedValue>>`, bloating instance size.
- `JSFunction` embedded constructor prototype values, name strings, and instance field vectors.

The Phase 1 memory diet targets strict upper bounds:
- `{a: 1, b: 2}` $\le 64$ B (+ shared shape, zero out-of-line storage);
- `[x]` $\le 64$ B + elements;
- Closure $\le 64$ B.
- Zero `std::map` / `std::unordered_map` in young/old generation for default objects.

## Decision
1. **Header Bitfield Packing (`HeapObject::gc_metadata_`)**:
   - `kExtensibleBit = 1 << 9` (Initial state: 1)
   - `kSealedBit     = 1 << 10` (Initial state: 0)
   - `kFrozenBit     = 1 << 11` (Initial state: 0)
   - Eliminates 3 boolean member variables from `JSObject` with 0 bytes overhead.

2. **JSObject Diet (`sizeof(JSObject) == 56` bytes)**:
   - Layout:
     - 24-byte `HeapObject` base
     - 8-byte `JSObject* prototype_`
     - 16-byte `TaggedValue in_object_slots_[kDefaultInObjectProperties]` (`kDefaultInObjectProperties = 2`)
     - 8-byte `std::unique_ptr<ObjectExtraData> extra_` (lazily allocated)
   - `ObjectExtraData` encapsulates out-of-line slots, accessors, deleted property set, and attribute overrides. For default objects with $\le 2$ properties, `extra_ == nullptr`, requiring zero auxiliary allocations.

3. **JSArray Diet (`sizeof(JSArray) == 64` bytes)**:
   - Layout:
     - 56-byte `JSObject` base
     - 8-byte `std::unique_ptr<ArrayStorage> array_storage_`
   - `ArrayStorage` lazily holds backing element storage vector and length/capacity counters. Empty arrays require 64 bytes total. Capacity tracking via `detail::ReserveStorage` charges against isolate memory budget cleanly.

4. **JSFunction Diet (`sizeof(JSFunction) == 64` bytes)**:
   - Layout:
     - 56-byte `JSObject` base
     - 8-byte `std::unique_ptr<FunctionData> function_data_`
   - `FunctionData` holds function name, constructor prototype, instance fields, and lexical scope bindings lazily. Standard closures with default prototype state remain strictly 64 bytes.

5. **In-Object Transition & Budget Integration**:
   - `Shape::LookupProperty` resolves slot index and in-object flag (`is_in_object`).
   - `JSObject::SetNamedProperty` stores first 2 properties directly in `in_object_slots_`.
   - Adding a 3rd property spills into `extra_->out_of_line_slots`.
   - `MemoryBudgetState` distinguishes unlimited headroom (`SetHeapBudget(0)`) from zero headroom limit (`BudgetLimit limit(0)`), ensuring strict budget limit enforcement across property insertions and re-additions.

## Verification
- Compile-time static assertions:
  - `static_assert(sizeof(HeapObject) == 24);`
  - `static_assert(sizeof(JSObject) == 56);`
  - `static_assert(sizeof(JSArray) == 64);`
  - `static_assert(sizeof(JSFunction) == 64);`
- Test suite `tests/test_object_sizes.cpp` tests in-object storage, out-of-line spill at 3 properties, deletion/re-addition, array storage, and function diet.
- All 31 CTest suites pass locally with 100% success rate.
