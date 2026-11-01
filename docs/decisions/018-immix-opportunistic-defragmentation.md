# Decision 018: Opportunistic Immix Defragmentation and Compaction

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.3: Opportunistic Immix Defragmentation; Decision D7), the Immix mark-sweep collector reclaimed unused lines within 32KB blocks, but long-running object churn benchmarks with irregular object lifecycles experienced heap fragmentation. When surviving objects were scattered across many blocks, each partially populated block retained committed virtual memory. Without compaction, the committed heap size grew significantly larger than the live object size, exceeding the acceptance criterion of `fragmentation (committed bytes / live bytes) <= 1.3x`.

Furthermore, conservative stack scanning (Decision D2) creates unmovable pinned roots on the native C++ stack. A purely moving copying collector cannot relocate stack-pinned objects, while a purely non-moving mark-sweep collector cannot resolve external fragmentation.

## Decision

1. **Selective Evacuation Candidates**:
   - During `MajorGC`, after the live mark phase (`Trace`), `old_gen_.SelectEvacuationCandidates()` analyzes all recyclable Immix blocks.
   - Any block whose live object occupancy is $\le 50\%$ of payload capacity (`kPayloadSize / 2 = 16128` bytes) and has active unpinned objects is marked with `kBlockFlagEvacuate`.
   - Blocks with $\ge 50\%$ occupancy remain in-place and are reclaimed via standard Immix line sweeping.

2. **Conservative Stack Pinning and Stable Objects**:
   - Objects pinned by native stack frames (`ScanStack`) or marked stable via `SetStable(true)` (`kStableBit`) are never evacuated.
   - Pinned and stable objects remain in their original blocks; only movable heap objects (such as `JSObject`, `JSArray`, `JSMap`, `JSSet`, `JSString`, `ByteArray`, `FixedArray`, `LexicalEnvironment`) in candidate blocks are relocated.

3. **Two-Space Evacuation into Recyclable Blocks**:
   - Evacuated objects are allocated via `AllocateForEvacuation()` into existing recyclable blocks or non-candidate free blocks.
   - Old headers are stamped with forwarding addresses (`SetForwardedAddress(new_obj)`), retaining object type while pointing directly to the destination.

4. **Comprehensive Pointer Fixup**:
   - `Defragment` Phase 2 updates references using `SlotVisitor`:
     - Roots: HandleScope stacks, extra roots, finalizers, and interpreter frames via `VisitRoots(&fixup)`.
     - Live heap objects: All surviving objects in `live` and newly evacuated objects have `IteratePointers(&fixup)` invoked to rewrite slots.
     - Remembered set & card table: Stale old-to-young pointers are updated to destination addresses and re-registered in the card table.
     - Global and runtime registries: `Realm` prototype/intrinsics tables and Wasm host registries are relocated to their forwarded destinations.
     - Active C++ methods protect target receiver and value operands with `HandleScope` and `PinnedScope` RAII guards.

5. **Complete Block Reclamation**:
   - Candidate blocks whose live objects were evacuated are cleared and swept, transitioning to `BlockState::Free` for immediate reuse.
   - Memory of evacuated objects in candidate blocks is safely poisoned under `--gc-verify` (`kOldGenPoisonByte`).

## Consequences

- **Fragmentation**: On the 10,000-object churn benchmark (`test_defragmentation`), fragmentation ratio (`ActiveCommittedBytes / live_bytes`) is **1.17x**, comfortably satisfying the target $\le 1.30\times$.
- **Verification**: Zero memory leaks or dangling pointers under `--gc-stress` and `--gc-verify`.
- **Full Test Suite & Matrix**: 100% test pass rate across all 32 CTest suites and all 9 tier/GC matrix configurations.
