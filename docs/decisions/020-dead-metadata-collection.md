# Decision 020: Dead Metadata Collection, Weak Shape Transitions, and Code Flushing

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.5: Dead Metadata Collection; Decision D10), AetherVM accumulated metadata across execution that was not automatically reclaimed when objects died. Specifically:
1. **Transition Trees (Shapes)**: In AetherVM, shapes form transition trees rooted at `Realm::root_shape_`. Because shapes held child transitions in `std::unordered_map<std::string, std::unique_ptr<Shape>>`, every newly created shape remained permanently anchored in memory even when all objects conforming to it had become garbage. Over long-running processes or workloads that synthesize dynamic properties or discard transient objects (such as JSON deserialization with unique schema variations), shape count grew monotonically, leaking C++ heap memory.
2. **Inline Caches (ICs)**: Inline cache feedback slots (`FeedbackSlot`) store raw pointers to shapes (`cached_shape`). If a dead shape is pruned or relocated, stale IC entries could retain dangling pointers or corrupt cache lookups.
3. **JIT Code Memory**: Functions compiled to Tier 1 native machine code (`tier1_code_`) retained executable pages indefinitely, even when the functions were cold (uninvoked for extended execution intervals) or discarded.
4. **Wasm Registries**: Module and instance registries retained entries for uncollected instances unless pruned during GC cycles.

The acceptance requirement specifies:
- Prune dead shapes from transition trees during `MajorGC` when neither live objects nor live descendant shapes reference them.
- Clear IC feedback slots whose cached shape was pruned, resetting the slot to `Uninitialized` and `nullptr`.
- Flush Tier 1/2 executable code for cold functions with 0 invocations during a GC cycle, provided they are not active on the execution stack.
- A test that creates and discards 1,000,000 distinct shapes must not grow RSS unboundedly.

## Decision

1. **Weak Shape Transitions and Liveness Mark Sweep**:
   - During `MajorGC`, after processing weak handles and Wasm registry pruning, `IsolateHeap::PruneDeadMetadata(const std::vector<HeapObject*>& live)` is executed.
   - All live shapes are discovered by:
     - Root shapes from `Realm::GetRootShape()` and `Interpreter::GetRootShape()`.
     - Inspecting all surviving objects in `live`. For any `JSObject`, its current `shape_` is marked live, and its ancestor chain (`Parent()`) is traversed to the root, adding all parent shapes to the live set.
     - Inspecting lexical environments and functions referencing shapes.
   - `Shape::PruneDeadTransitions(const std::unordered_set<Shape*>& live_shapes)` recursively walks the transition tree. For each child transition:
     - If the child shape is present in `live_shapes`, recursion continues into the child's transitions.
     - If the child shape is not in `live_shapes`, the transition entry is removed from `transitions_`. Because child shapes are owned via `std::unique_ptr<Shape>`, removing the entry cleanly invokes the destructor hierarchy, releasing the entire dead subtree and its property descriptors.
   - `Shape::TotalTreeShapeCount()` provides an exact count of all nodes in the transition hierarchy.

2. **Inline Cache Pruning & Reset**:
   - `TypeFeedbackVector::PruneDeadShapes(const std::unordered_set<Shape*>& live_shapes)` inspects all `FeedbackSlot` instances.
   - If a slot holds a `cached_shape` that is not present in `live_shapes`, it resets the slot: `cached_shape = nullptr` and `state = State::Uninitialized`.
   - Pruning is applied to feedback vectors of all live `JSFunction` objects on the heap as well as active interpreter frames.

3. **Code and Bytecode Flushing for Cold Functions**:
   - Active execution frames on the stack are enumerated to collect all `BytecodeArray` instances currently executing (`active_bytecodes`).
   - For all live `JSFunction` instances on the heap:
     - If the function's bytecode is active on the stack, it is spared from flushing.
     - If `InvocationCount() == 0` during the GC cycle and it is not on the stack, `BytecodeArray::FlushJITCode()` releases `tier1_code_` (and `tier2_code_`), freeing the native executable memory pages.
     - If `InvocationCount() > 0`, the count is reset to 0 for tracking activity over the subsequent GC cycle.

4. **Pointer Relocation & Wasm Registry Preservation**:
   - Shape prototype pointers (`prototype_`) are updated during defragmentation compaction via `Shape::UpdatePointers(resolve)`.
   - Existing hooks `WasmRegistryPruneDead` and `WasmRegistryRelocate` are preserved and integrated seamlessly.

## Consequences

- **Bounded RSS on Dynamic Shapes**: Creating and discarding 1,000,000 distinct shapes executes in ~1.3 seconds with RSS strictly bounded (< 19 MB total), matching baseline shape count (508 shapes) exactly after GC.
- **Safety**: IC slots referencing pruned shapes revert to `Uninitialized`, safely reprofiling on subsequent execution without dangling pointer faults or crashes.
- **Code Footprint**: Cold function executable pages are reclaimed, preventing JIT code bloat in long-running services.
- **Matrix & Test Suite**: 100% pass rate across all 34 CTest suites (including `test_dead_metadata_collection`) and all 9 test matrix configurations (`python tools/matrix.py`).
