# ADR 007: Conservative Stack Scanning (Decision D2)

## Context
Prior to this architectural change, Aether relied exclusively on an explicit handle protocol (`HandleScope` and `Handle<T>`) to track live object references. This design introduced several severe drawbacks:
1. **Fragile Scope Invariants**: Any allocation inside native builtins, intrinsics, or bytecode opcode handlers required every intermediate `HeapObject*` or `TaggedValue` to be meticulously wrapped in a `HandleScope` or protected with `NoGCScope`. Forgetting a single handle led to silent heap corruption or use-after-free bugs when garbage collection triggered.
2. **"Never Collects" Allocation Restrictions**: Runtime helper allocations (`AllocateRuntime`) had to forbid garbage collection during allocation because native expressions could hold unpublished raw pointers in registers or C++ stack frames.
3. **Dispatch Loop Per-Opcode Overhead**: To prevent unbounded handle accumulation from temporary results in opcodes, the bytecode execution loop was forced to run `if (!op_roots.Slots().empty()) op_roots.Clear();` on *every single instruction dispatch*, in addition to taking `SnapshotState(safepoint_roots, &frame)` at every safepoint check.

## Decisions

1. **Conservative Native Stack and Register Scanning (D2)**:
   - When garbage collection triggers during VM execution, the collector captures architectural CPU registers via `setjmp` (`jmp_buf`) and on Windows x64 via `RtlCaptureContext`.
   - The owning thread's stack range `[sp, stack_base)` is scanned word-by-word.
   - For every aligned word, `IsolateHeap::IsValidObjectStart` checks whether the value points to a genuine object start:
     - In Immix space: uses the 512-byte per-block `object_start_bitmap` in $O(1)$ (and exact header comparison for large objects).
     - In Young generation: uses binary search over sequentially bump-allocated young objects.
     - For `TaggedValue` representations with `kTagHeapObject`, validates the 48-bit pointer payload against the object start bitmaps.
   - Any valid object found on the stack is pinned (`obj->SetPinned(true)`) and marked as a root, ensuring it is never swept or moved.

2. **Collectable Runtime Allocations**:
   - Removed the "never collects" restriction on `AllocateRuntime` and `AllocateImpl`.
   - If old gen memory is exhausted, `AllocateImpl` can now safely invoke `MajorGC()` to recycle lines and blocks before retrying allocation. Any raw pointers held in calling C++ stack frames or registers remain safe and pinned.

3. **Elimination of Per-Opcode Root Clearing & Safepoint Snapshotting**:
   - Active interpreter execution frames are now tracked directly via intrusive `ExecutionFrame::prev_frame_` links and visited during `IsolateHeap::VisitRoots` via `Interpreter::IterateRoots`.
   - Removed `if (!op_roots.Slots().empty()) op_roots.Clear();` from the inner dispatch loop in `Interpreter::RunExecutionLoop`.
   - Removed `SnapshotState` and temporary `HandleScope safepoint_roots` from the safepoint check in `RunExecutionLoop`, reducing safepoint polling overhead to a direct `heap.CollectAtSafepoint()`.
   - Simplified `PublishResult` to avoid accumulating redundant handles on `instruction_roots_`.

4. **Preserved Embedder HandleScope API**:
   - `HandleScope` remains fully supported for backward compatibility and explicit embedder scoping, but is now documented as optional since stack-allocated raw pointers and `TaggedValue` instances are automatically preserved.

## Consequences & Verification
- Eliminates per-opcode handle clearing and safepoint frame snapshotting, measurably reducing interpreter dispatch overhead.
- Enables safe garbage collection at any allocation boundary, including under `--gc-stress=1`.
- Passes all 30 test suites cleanly.
- Passes all 9 configurations of the Tier $\times$ GC stress matrix (`python tools/matrix.py`).
- All 8 benchmarks in `bench/run.py` pass across Interpreter, Baseline JIT, and Optimizing JIT tiers.
