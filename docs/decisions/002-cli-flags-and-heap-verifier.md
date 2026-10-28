# Decision 002: Canonical CLI Flags and Heap Verification Subsystem

## Context
Third-party review audit identified the need for canonical CLI and runtime flags for GC stress testing, heap limits, verification, and JIT tier control (`AETHER_AGENT_PLAN.md` T0.4 and T0.5).

## Decision
1. **Options Singleton (`include/aether/options.h`, `src/options.cpp`)**:
   - Implemented canonical flag parser `Options::Parse(std::vector<std::string>& args)` supporting:
     - `--gc-stress[=N]` (triggers GC on every allocation or every N allocations)
     - `--gc-verify` (runs full heap verifier before and after each GC cycle)
     - `--trace-gc` (logs pause and memory statistics for minor and major collections)
     - `--gc-at-safepoints` (forces GC at loop backedges and execution safepoints)
     - `--max-heap=MB` (enforces hard heap budget with OutOfMemoryError)
     - `--interpreter-only` (bypasses baseline and optimizing JITs)
     - `--always-baseline` (compiles to baseline JIT on first call)
     - `--always-optimize` (compiles to optimizing JIT on first call)
     - `--tier-threshold=N` (overrides tiering thresholds)
     - `--stack-size=KB` (sets native stack recursion limit)
   - Non-flag arguments are forwarded to script execution and exposed via `process.argv`.
   - Default values preserve existing runtime defaults (0 = no override).

2. **Heap Verifier (`include/aether/heap_verifier.h`, `src/heap_verifier.cpp`)**:
   - `HeapVerifier::VerifyHeap(heap, phase)`:
     - Iterates all heap roots (HandleScopes, extra roots, realms).
     - Checks memory boundaries and protects against low address corruption (< 0x10000).
     - Detects poisoned memory cells (`0xDD` swept poison, `0xCD` unallocated poison).
     - Validates `HeapObjectType` tags (`Shape` through `JSBigInt`).
     - Validates `JSObject` shape pointers.
     - Enforces generational write barrier invariants: old-to-young pointers must have their corresponding card marked in `CardTable`.
   - Integrated into `MinorGC` and `MajorGC` when `--gc-verify` is active.

3. **Verification**:
   - Validated via `tests/test_flags.cpp` covering option parsing, runtime stress, interpreter bypass, heap verifier pass on healthy heap, poison detection, and missing card barrier detection.
   - All 30 test suites pass cleanly under `ctest`.
