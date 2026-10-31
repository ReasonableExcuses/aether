# Decision 011: GC-Allocated Array Types (FixedArray & ByteArray)

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.1: GC-allocated array types; Decision D4), object payloads across AetherVM previously lived in the C++ process heap (`malloc`/`new`) wrapped inside `std::vector` and tracked via auxiliary `MemoryCharge` instances.

Storing payloads outside the GC heap had major architectural costs:
1. Garbage collection could not directly evacuate or compact payload memory.
2. Every vector resize performed out-of-band heap allocations and required explicit `MemoryCharge` accounting.
3. Object destruction required running non-trivial C++ destructors to free internal heap buffers.

To achieve trivially relocatable objects with zero per-object destructors and real evacuation (D4), payloads must be first-class GC-managed heap objects.

## Decision
1. **Heap Object Types**:
   - Added `HeapObjectType::ByteArray = 0x19` to `HeapObjectType` enum.
   - Verified in `HeapVerifier` bounds check (`0x01` through `0x19`).
   - Added `IsFixedArray()` and `IsByteArray()` predicates to `HeapObject`.

2. **FixedArray Layout (`sizeof(FixedArray) == 32` bytes)**:
   - Header:
     - 24-byte `HeapObject` base (`type_ = HeapObjectType::FixedArray`, shape = `nullptr`)
     - 4-byte `uint32_t length_`
     - 4-byte `uint32_t capacity_`
   - Total header size: exactly 32 bytes (`kHeaderSize = 32`), aligned to 8 bytes (`kObjectAlignment`).
   - Payload: trailing contiguous `TaggedValue` array starting immediately at offset 32.
   - Allocation size: `(kHeaderSize + capacity * sizeof(TaggedValue) + kObjectAlignment - 1) & ~(kObjectAlignment - 1)`.
   - Traversal: `IteratePointers` scans all elements up to `capacity_`.
   - Relocation: trivially relocatable via `std::memcpy`.

3. **ByteArray Layout (`sizeof(ByteArray) == 32` bytes)**:
   - Header:
     - 24-byte `HeapObject` base (`type_ = HeapObjectType::ByteArray`, shape = `nullptr`)
     - 4-byte `uint32_t length_`
     - 4-byte `uint32_t reserved_`
   - Total header size: exactly 32 bytes (`kHeaderSize = 32`), aligned to 8 bytes.
   - Payload: trailing contiguous `uint8_t` byte array starting immediately at offset 32.
   - Traversal: `IteratePointers` is a no-op (raw byte storage contains no GC pointers).
   - Relocation: trivially relocatable via `std::memcpy`.

4. **Type Aliases**:
   - `using PropertyArray = FixedArray;`
   - `using ElementsArray = FixedArray;`

5. **Variable-Sized GC Allocation Machinery**:
   - Implemented `IsolateHeap::AllocateVariableImpl<T>(bool old, size_t bytes, InitFn&& init_fn)` supporting dynamic payload allocations in Immix lines or large object space.
   - Refactored `IsolateHeap::AllocateImpl` to forward through `AllocateVariableImpl`.
   - Implemented `AllocateFixedArray(uint32_t length, uint32_t capacity, TaggedValue init, bool old)` and `AllocateByteArray(uint32_t length, bool old)`.
   - Added global runtime facades `AllocateRuntimeFixedArray` and `AllocateRuntimeByteArray`.

6. **JSObject Out-Of-Line Slots Migration**:
   - Replaced `std::vector<TaggedValue> out_of_line_slots` and `MemoryCharge slots_storage` in `ObjectExtraData` with `FixedArray* out_of_line_slots{nullptr}`.
   - Updated `JSObject::PrepareFastSlot` to grow out-of-line storage as a GC-allocated `FixedArray`, copying existing slots and issuing write barriers as required.
   - Updated `JSObject::IteratePointers` to traverse `out_of_line_slots` as a `TaggedValue` heap object root.

## Verification
- Compile-time static assertions:
  - `static_assert(sizeof(FixedArray) == 32);`
  - `static_assert(sizeof(ByteArray) == 32);`
- Test suite `tests/test_object_sizes.cpp` verifies `FixedArray` and `ByteArray` allocation, indexing, capacity, bounds, and `JSObject` out-of-line spill.
- All 31 CTest test suites pass locally with 100% success rate.
