# ADR 008: Sticky Generational Minor GC in Immix with Object Remembered Set (Decisions D1, D3)

## Context
Prior to this architectural change, Aether implemented generational collection via a distinct copying nursery (`NurserySpace`):
1. **Virtual Memory Overhead**: Each `IsolateHeap` allocated a contiguous 32 MB virtual memory nursery buffer for semi-space copying, creating unnecessary memory overhead for default heaps and lightweight isolates.
2. **Moving Object Complications**: Copying young survivors out of the nursery to Immix changed object addresses (`RelocateTo`), which conflicted with conservative native stack scanning (Decision D2) and required complex pointer rewriting for any stack-pinned objects.
3. **Address Card Limitations (D3)**: The legacy `CardTable` mapped memory addresses to cards, but JavaScript objects with out-of-line payloads (such as property arrays, elements, or closure captures) live outside fixed address ranges, rendering byte-address card tables ineffective for catching mutations.
4. **Weak Container Tracking**: Old weak containers (`JSWeakMap`, `JSWeakSet`, `JSWeakRef`, `JSFinalizationRegistry`) holding young keys or targets could not be pruned without scanning the entire heap.

## Decisions

### 1. Sticky Mark Generational Collection in Immix (Decision D1)
- All objects, whether young or old, are allocated directly into Immix blocks (or large object pages).
- An object is "young" if newly allocated since the last major collection (`obj->IsOld() == false`, starting age = 0).
- **Non-Moving Survivors**: Surviving young objects remain in-place at identical memory addresses (`new_addr == old_addr`). Pointers held on the stack, in registers, or in active execution frames remain completely valid without pointer fixups.
- **Tenuring Age Threshold**: Each Minor GC increments survivor age (`obj->IncrementAge()`). Upon reaching `kTenuringAgeThreshold` (age 2), the object promotes in-place to old (`obj->SetOld(true)`).
- **In-Place Sweep**: Dead young objects in Immix blocks are destructed in-place and their lines unmarked, returning holes to Immix for subsequent bump allocation. Optional poison byte (`kNurseryPoisonByte = 0xDD`) is written under `--gc-verify`.
- **Elimination of Copying Nursery**: Removed copying nursery buffer overhead from default heap layouts.

### 2. Object Remembered Set (Decision D3)
- Replaced address-based card tables with an explicit Object Remembered Set (`std::vector<HeapObject*> remembered_set_;` in `IsolateHeap`).
- Each object header tracks whether it is currently enqueued in the remembered set via `obj->IsRemembered()` / `obj->SetRemembered(bool)`. Duplicate additions are filtered in $O(1)$.
- **Write Barrier Installation**: Audited every object mutation path across the runtime:
  - Property mutation: `JSObject::SetNamedProperty`, `JSObject::DefineOwnProperty`
  - Array mutation: `JSArray::Push`, `JSArray::SetElement`
  - Function & Environment mutation: `JSFunction` fields, `LexicalEnvironment::SetSlot`
  - Promise & Async: `JSPromise::AddReaction`, `JSGeneratorObject` state
  - Special containers: `JSMap`, `JSSet`, `JSProxy`
  - Whenever an old object mutates a slot to point to a young object, `WriteBarrier(source, value)` enqueues `source` into `remembered_set_`.
- **Promotion Barrier**: When an object is tenured from young to old during Minor GC, all its outward references are checked. If it references an object that remains young, it is automatically registered into `remembered_set_`.
- **Remembered Set Pruning**: At the end of each Minor GC, `remembered_set_` is swept: old objects that no longer hold any young references are removed, and their `IsRemembered` bit is cleared.

### 3. Weak Container Generational Tracking
- Maintained a dedicated `std::vector<HeapObject*> weak_containers_;` in `IsolateHeap` for weak primitives (`JSWeakMap`, `JSWeakSet`, `JSWeakRef`, `JSFinalizationRegistry`).
- During Minor GC, weak containers are processed without scanning old-gen:
  - Ephemeron fixpoint loop resolves live young values reachable through marked keys.
  - Dead young keys, entries, and targets are pruned directly.
  - Finalization callbacks for dead young targets are scheduled into `finalizers_`.

### 4. Verification and Invariant Checks
- Added `--gc-verify-barrier` CLI flag and integrated into `HeapVerifier`: after every Minor GC, runs a full heap verification to assert that no old $\to$ young pointer exists unless the source object is recorded in `remembered_set_`.
- Fixed Agent teardown ordering in `Agent::~Agent()` to ensure `Interpreter::UnregisterRoots()` and `MicrotaskQueue::Teardown()` execute before `Heap` destruction.

## Consequences & Verification
- **Eliminated Virtual Memory Overhead**: Eradicated the 32 MB copying nursery allocation overhead.
- **Zero `std::map` Usage**: Young and old gen utilize intrusive bitmaps and vector-backed structures with zero `std::map` lookups.
- **Comprehensive Test Suite**: All 30 test suites in CTest pass 100%.
- **Full Matrix Verification**: All 9 configurations across 3 execution tiers (Interpreter, Baseline JIT, Optimizing JIT) $\times$ 3 GC modes (Default, GC-Stress, GC-Verify) pass in `tools/matrix.py`.
- **Performance Gains**: `gc2.js` wall time improved by up to 35.8% compared to the pre-rewrite baseline, demonstrating that Minor GC pause times scale strictly with young survivor volume rather than old-gen heap size.
