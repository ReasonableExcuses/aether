# Decision 019: Lazy Sweeping and Heap Shrinking

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.4: Lazy Sweeping and Heap Shrinking; Decision D8), the Immix collector reclaimed unused lines synchronously during GC pause times. Sweeping all blocks during STW pause times introduced allocation-proportional pause latencies. Additionally, while the allocator acquired 32KB blocks from the OS or reservation, free blocks resulting from object death or defragmentation compaction were not decommitted or released back to the host operating system. Consequently, the Resident Set Size (RSS) remained inflated after large transient memory spikes, violating the requirement that RSS returns to $< 2\times$ idle after workload release.

Furthermore, long-running processes required an enforceable maximum heap budget (`--max-heap=MB`) where memory exhaustion deterministically throws a catchable `RangeError`/OOM exception rather than terminating the process unpredictably.

## Decision

1. **On-Demand Lazy Sweeping**:
   - `PrepareLazySweep()` is introduced during `MajorGC`. It runs the destructors of all dead objects immediately (releasing native resources and charges), resets object starts for reclaimed allocations, and marks surviving recyclable blocks with `kBlockFlagNeedsSweep`.
   - Blocks with no live lines (`!has_live`) are swept immediately into `BlockState::Free`.
   - Blocks marked with `kBlockFlagNeedsSweep` defer line-scanning and poisoning until an allocation actively needs memory from that block (`SweepBlock(block)` in `ImmixSpace::Allocate()`).
   - `EnsureAllSwept()` guarantees all blocks are fully swept before operations requiring a complete object enumeration (such as `ClearMarks()`, `Objects()`, `DestroyAll()`, or verifier passes).

2. **Heap Shrinking and OS Release**:
   - `ShrinkLocked()` / `Shrink()` is invoked after `PrepareLazySweep()` and explicit heap shrink requests (`IsolateHeap::Shrink()`).
   - Retains at most 1 free block (`kMaxCachedFreeBlocks = 1`) in reserve to avoid OS allocator churn.
   - For standalone blocks, excess free blocks are released to the OS via `VirtualFree(block, 0, MEM_RELEASE)` (Windows) or `free(block)` (POSIX).
   - For reserved blocks (`reservation_start_`), excess free blocks at the tail of the block vector are decommitted to the OS via `VirtualFree(block, kImmixBlockSize, MEM_DECOMMIT)` (Windows) or `madvise(block, kImmixBlockSize, MADV_DONTNEED)` (POSIX).

3. **Deterministic Max-Heap Enforcement**:
   - `--max-heap=MB` CLI option sets `Options::Get().max_heap_mb`.
   - `ImmixSpace::AllocateBlock()` and `AllocateLarge()` verify that the total committed block and large object memory does not exceed `max_heap_mb * 1024 * 1024`.
   - When exceeded, allocation fails cleanly, allowing `IsolateHeap::AllocateVariableImpl` to throw a catchable `std::bad_alloc` which maps directly to a JavaScript `RangeError` ("Maximum heap budget exceeded").

4. **Interior Pointer and String View Hardening**:
   - `JSObject::SetNamedProperty` buffers the `key` into a local stack-owned `std::string` before invoking `PrepareFastSlot()`. This prevents interior string views pointing to bytecode constants from dangling if out-of-line property growth triggers an evacuation defragmentation pass.

## Consequences

- **RSS Post-Release**: On transient workloads (e.g. allocating 4 MB of objects and releasing them), committed memory drops from 4.2 MB back to 32 KB, and process RSS returns to idle ($\le 1.05\times$ idle, well below the $< 2.0\times$ threshold).
- **Lazy Sweep Throughput**: Fast-path allocations only sweep blocks on demand, reducing pause times on large heaps.
- **Matrix & Test Suite**: 100% pass rate across all 33 CTest suites (including `test_lazy_sweeping_and_shrinking`) and all 9 test matrix configurations (`python tools/matrix.py`).
