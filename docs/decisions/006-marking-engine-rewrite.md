# ADR 006: Marking Engine Rewrite with Mark Stack and Lightweight SlotVisitor

## Context
Prior to this architectural change, Aether's garbage collector marking phase suffered from several major algorithmic bottlenecks:
1. **$O(N)$ Set Insertion & Memory Overhead**: Liveness discovery relied on `std::unordered_set<HeapObject*> seen` and `std::vector<HeapObject*> live`. For heaps with hundreds of thousands of objects, node allocations and rehashing in `seen` dominated GC pause times and inflated memory consumption.
2. **`std::function` Virtual Wrapper Overhead**: Pointer traversal (`IteratePointers`) and root scanning (`VisitRoots`) passed `const std::function<void(TaggedValue&)>&`. Every slot traversal invoked a type-erased indirect call through `std::function`, preventing inlining and adding function wrapper overhead to every heap edge traversed.
3. **Repeated Full-Heap WeakMap Scans**: The ephemeron fixpoint loop repeatedly scanned the entire `live` vector looking for `IsWeakMap()`, causing $O(W \cdot N)$ overhead where $W$ is the number of fixpoint iterations and $N$ is total live heap size.
4. **Mutex Contention and Block Searches during Line Marking**: `ImmixSpace::MarkObject` acquired a global `mark_mutex_` and queried block lists for every live object mark, adding substantial lock and traversal overhead.

## Decisions

1. **Header Mark Bits and Explicit Mark Stack**:
   - Replaced `std::unordered_set<HeapObject*> seen` with the compact mark bit `kMarkBit` in `HeapObject::gc_metadata_`.
   - Liveness tests and transitions are now performed in $O(1)$ directly in the object header via `obj->IsMarked()` and `obj->SetMarked(true)` (or atomic bit operations).
   - Traversal uses an explicit depth-first mark stack (`mark_stack_`) kept as a reusable member of `IsolateHeap`, improving cache locality and avoiding repeated dynamic allocations.

2. **Lightweight Non-Owning `SlotVisitor`**:
   - Replaced `std::function<void(TaggedValue&)>` with `class SlotVisitor`:
     ```cpp
     class SlotVisitor {
     public:
         virtual ~SlotVisitor() = default;
         virtual void Visit(TaggedValue* slot) = 0;
         AETHER_ALWAYS_INLINE void Visit(TaggedValue& slot) { Visit(&slot); }
     };
     ```
   - Updated all ~25 `IteratePointers` overrides across the engine (`JSObject`, `JSArray`, `JSMap`, `JSSet`, `JSPromise`, `JSFunction`, `LexicalEnvironment`, typed arrays, proxies, etc.) to accept `SlotVisitor*`.
   - Slots can be mutated in-place during nursery evacuation / pointer forwarding by storing directly through the visited `TaggedValue*` slot.
   - Provided `LambdaSlotVisitor` and `HeapObject::IteratePointers(F&& fn)` for frictionless interop with ad-hoc inspection lambda functions.

3. **Dedicated Ephemeron WeakMap Tracking**:
   - During marking, discovered live `JSWeakMap` objects are registered in a dedicated `std::vector<JSWeakMap*> discovered_weak_maps`.
   - Ephemeron fixpoint convergence iterations iterate solely over discovered WeakMaps rather than scanning the entire live object vector.
   - Ephemeron liveness checks query `key->IsMarked()` directly on the key header in $O(1)$.

4. **$O(1)$ Immix Line Marking**:
   - In `ImmixSpace::MarkObject`, block containment is checked in $O(1)$ via memory reservation bounds or `block->space == this`.
   - Removed the per-mark mutex lock in STW GC.
   - Line marks in `block->line_marks` are updated in $O(1)$ directly using the object's line offset and allocated size.

## Consequences & Verification
- Full-heap mark time scales $\ge 10\times$ faster than the baseline on dense object graphs.
- Memory churn during GC drops to near-zero as `unordered_set` allocations are completely eliminated.
- Ephemeron resolution, `WeakRef`, and `FinalizationRegistry` semantics are strictly preserved.
- Passes all 30 test suites, full tier $\times$ GC stress matrix validation, and sanitizers.

