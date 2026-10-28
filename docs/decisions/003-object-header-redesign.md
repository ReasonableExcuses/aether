# Decision 003: Compact 24-Byte Object Header Layout

## Context
Under `AETHER_AGENT_PLAN.md` T1.1, the previous object header in `HeapObject` needed modernization to support upcoming Immix generational garbage collection, non-moving nursery age promotion, conservative stack pinning, and object-based remembered sets.

## Decision
1. **Header Structure (`include/aether/heap_object.h`)**:
   - Total `sizeof(HeapObject)` is strictly 24 bytes (3 64-bit words):
     - `uint16_t type_` (HeapObjectType: Shape through JSBigInt)
     - `uint16_t gc_metadata_` (bitfield for GC flags and generational age)
     - `uint32_t flags_` (Reserved / object properties)
     - `Shape* shape_` (8 bytes)
     - `HeapObject* forwarding_address_` (8 bytes)
2. **GC Metadata Bit Allocation**:
   - `kMarkBit = 1 << 0` (Mark bit for Immix / GC liveness)
   - `kForwardedBit = 1 << 1` (Indicates object relocated / forwarded)
   - `kOldGenBit = 1 << 2` (Generational status: 0 = Young/Nursery, 1 = Tenured/Old)
   - `kRememberedBit = 1 << 3` (Indicates inclusion in remembered set for old-to-young references)
   - `kPinnedBit = 1 << 4` (Conservative stack scan pin flag, prevents movement/reclamation)
   - `kAgeShift = 5`, `kAgeMask = 0x01E0` (4 bits: tenure age counter 0-15 with saturation at 15)
3. **Accessors**:
   - Added `IsRemembered()`, `SetRemembered(bool)`
   - Added `IsPinned()`, `SetPinned(bool)`
   - `GetAge()`, `IncrementAge()`, `ResetAge()`
4. **Verification**:
   - Unit test `TestObjectHeaderLayout()` in `tests/test_flags.cpp` verifies `sizeof(HeapObject) == 24`, bit operations, flag independence, and age counter saturation.
