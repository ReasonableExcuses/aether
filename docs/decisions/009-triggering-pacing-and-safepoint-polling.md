# ADR 009: Allocation-Volume Triggers, Adaptive Budgeting, and Loop Safepoint Polling (Task T1.6)

## Context
Prior to this task, garbage collection in Aether suffered from several structural pacing and triggering deficiencies:
1. **Arbitrary Percentage Trigger on Fixed 512 MB Pool**: Major GC decisions checked `heap.HeapPressure() >= 75` against a hardcoded 512 MB budget pool, which was arbitrary and ineffective for isolates with dynamic, small, or custom memory requirements.
2. **Per-Instruction Dispatch Loop Overhead**: In `Interpreter::RunExecutionLoop`, `heap.HeapPressure() >= 75` was evaluated on *every single opcode dispatch* (millions of times per benchmark run). This per-instruction check performed integer/floating-point division and atomic accesses on hot dispatch paths, injecting substantial interpreter latency.
3. **No Allocation-Volume Pacing**: Allocation in young generation lacked volume pacing and adaptive budget scaling. Systems with short-lived allocation churn (such as `gc1.js`) or deeply nested iterations suffered from erratic collection cadences or high peak RSS.
4. **Lack of Loop/Call Safepoints**: Cooperative collection safepoints were missing at loop backward branches and function entry boundaries, meaning uncooperative loops could delay collections while allocations in native methods were unsafe without explicit polling.

## Decisions

### 1. Allocation-Volume Driven Minor GC
- Replaced the arbitrary 75% heap pressure check with allocation volume thresholds.
- Introduced `young_budget_` in `IsolateHeap`, defaulting to 8 MB (`kDefaultYoungBudget = 8 * 1024 * 1024`).
- Tracked cumulative young allocations via `young_bytes_allocated_`.
- When `young_bytes_allocated_ >= young_budget_`, Minor GC is triggered immediately (or deferred to the next safepoint if inside a `NoGCScope`).
- **Adaptive Young Budgeting**: Following each Minor GC, `young_budget_` dynamically adjusts based on the survivor ratio ($S = \text{surviving\_bytes} / \text{allocated\_before}$):
  - If $S > 0.60$ (high survival rate), `young_budget_` increases by $1.5\times$ (up to `max_young_budget_`, default 32 MB) to prevent scavenge churn on persistent workloads.
  - If $S < 0.20$ (high mortality/churn), `young_budget_` decreases by $0.8\times$ (down to `min_young_budget_`, default 1 MB) to aggressively collect dead allocations and keep peak RSS below 32–64 MB.

### 2. Major GC Growth Threshold and Budget Triggers
Major collections are governed strictly by the four criteria mandated in the engineering audit plan:
- **(a) Old-Gen Growth Threshold**: `old_gen_growth_threshold_` dynamically scales to $1.75\times$ post-last-major live size (`last_major_live_bytes_`), bounded with a minimum of 32 MB (`kMinMajorGCThreshold`). Major GC triggers when `old_bytes_allocated_ >= old_gen_growth_threshold_`.
- **(b) Promotion Failure**: If old generation allocation fails during tenuring or space exhaustion, Major GC is invoked immediately to sweep and reclaim dead old objects.
- **(c) Explicit GC Call**: Runtime `gc()` invokes `MajorGC()`.
- **(d) Memory Budget Pressure**: If an explicit budget is configured (`HeapBudget() > 0`) and total allocated bytes reach 75% of budget (`TotalAllocatedBytes() >= HeapBudget() * 75 / 100`), Major GC triggers.
- In addition, periodic Major GC is triggered after every 10 Minor GCs when under explicit budget limits (or every 50 Minor GCs in standard unconstrained runs) to sweep tenured garbage and maintain low memory footprints.

### 3. Safepoint Polling at Loop Backward Jumps and Function Entry
- **Eliminated Per-Instruction Polling**: Completely removed `HeapPressure()` and GC invocation checks from the main opcode dispatch loop (`while (pc < code_end)`).
- **Loop Backward Jumps**: Installed safepoint polling at all backward jumps (`offset < 0`) across `Opcode::Jump`, `Opcode::JumpIfTrue`, `Opcode::JumpIfFalse`, `Opcode::JumpIfNotNullish`, `Opcode::JumpIfNullish`, `Opcode::JumpIfUndefined`, `Opcode::JumpIfNotUndefined`, `Opcode::JumpIfNull`, `Opcode::JumpIfNotNull`.
  - The poll evaluates `AETHER_UNLIKELY(heap.GCAllowed() && (heap.IsSafepointRequested() || Options::Get().gc_at_safepoints))`.
  - On taken backward branches under safepoint requests, `heap.CollectAtSafepoint()` executes safely at a well-defined bytecode boundary.
- **Function Entry Safepoint**: Safepoint polling is installed in `Interpreter::Execute` *after* `HandleScope entry_roots` has created protective handles for `this_val`, `env`, `args`, and `bytecode.Constants()`, guaranteeing that all parameters and constants remain fully rooted during collection.
- **Deferred Safepoints**: When allocations occur inside `NoGCScope` (native runtime methods, string formatting, constructor initializations), the collection request is stored via `RequestSafepoint()`, which fires automatically at the next backward branch or function call.

### 4. Custom Nursery Budget & Memory Limit Isolation
- When custom nursery sizes are provided (e.g. `IsolateHeap(16 * 1024)` in stress fixtures), `min_young_budget_` and `max_young_budget_` clamp proportionately, ensuring high allocation pressure tests trigger dozens of minor collections as designed.
- When `SetHeapBudget(size_t budget)` is called, `young_budget_` and `old_gen_growth_threshold_` clamp proportionally to the custom limit; when reset to 0, defaults are restored cleanly.
- `MemoryBudget` remains strictly an OOM enforcement limit, throwing `std::length_error` upon hard exhaustion.

## Consequences & Verification
- **Test Suite**: 30 out of 30 test suites pass cleanly with 100% success rate in CTest.
- **Eliminated Dispatch Overhead**: Zero per-instruction pressure checks in the dispatch loop.
- **Bounded RSS**: Adaptive young gen budgeting contracts down to 1 MB for high-churn scripts (such as `gc1.js`), maintaining bounded RSS well below 64 MB.
- **Full Matrix Verification**: Verified across 9 matrix configurations in `tools/matrix.py`.
