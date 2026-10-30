# AETHER_AGENT_PLAN.md

Execution plan for an autonomous coding agent working on **ReasonableExcuses/aether**
(C++23 JS/Wasm engine, ~60K LOC). Read the whole file once, then work task by task.

Order of work: **Phase 0 → 1 → 2 → 3 (GC)**, then **4 → 5 (interpreter, bytecode)**,
**6 (missing functionality)**, **7 (JIT)**, **8 (docs honesty)**. Phases 4 and 6a (correctness bugs)
may be interleaved with GC work when a GC task is blocked.

---

## 0. Operating rules (non-negotiable)

1. **Evidence over claims.** Never write "passes" / "faster" / "fixed" without a command and its output.
   Every task below has an *Acceptance* line. Meet it, then paste the evidence into the PR/commit message.
2. **Never weaken tests.** Do not delete/skip tests, loosen assertions, or edit Test262 baselines to raise a number.
   Baselines may only change through the generated-baseline tool (T0.3) and only in the direction of more passes.
3. **One task = one small PR** (target < 600 changed lines). Add a regression test for every bug fixed.
4. **GC changes must pass the stress matrix** (see T0.4/T0.5): all test binaries and the bench set under
   `--gc-stress`, `--gc-verify`, and each forced tier. No exceptions.
5. **Do not trust `README.md` / `progress.md`.** Several claims are false (see §2). Trust code + measurements.
6. **Record decisions** in `docs/decisions/NNN-title.md` (context, options, choice). Record blockers in
   `docs/agent_notes.md` and move to the next unblocked task.
7. Keep Windows/MSVC working (the author's primary platform) **and** make Linux/Clang/GCC first-class.

### Environment facts (measured)
- Plain `g++ 13 -std=c++23` compiles all 24 `src/*.cpp` files on Linux. A hand-linked CLI runs.
  `test_milestone3` and `test_hardening_gc` pass on Linux. **CMake itself is MSVC-only** (see T0.1).
- The repo has **no `.git` history in the zip, no `.github/` directory, no CI**.
- Only 46 Test262 files are in `test/test262/`. The ~52K-test corpus the docs cite (`test/official-test262`) is **not in the repo**.
- Baseline numbers below were taken at **g++ -O0, 1 core**. Treat them as relative; **re-baseline at -O2 in T0.6**.

---

## 1. Architecture summary (verified from source)

| Area | Reality |
|---|---|
| Values | 64-bit NaN-boxed `TaggedValue`, 48-bit raw pointers (`value.h`). **No pointer compression exists.** |
| Heap objects | Polymorphic C++ objects (`virtual ~HeapObject`, `RelocateTo`, `IteratePointers(std::function)`). 127 uses of `std::vector/string/map/function/deque` in `heap_object.h`. Object *payloads live in malloc*, not in the GC heap. |
| Nursery | `NurserySpace` 16 MB semi-spaces, bump alloc. Young objects tracked in `std::map<HeapObject*,size_t> young_` (`heap.h`). |
| Old gen | `ImmixSpace`: every object recorded in `std::map<void*,Allocation> allocations_`; 32 KB `ImmixBlock`s with 128 B lines (`gc.h/gc.cpp`). |
| Allocation entry | `AllocateRuntime<T>` → `AllocateStable` → **`AllocateOld`**, under `NoGCScope` (`heap.h`). Interpreter uses it at 132 sites, runtime at 378. |
| GC trigger | Only when `HeapPressure() >= 75%` of a **512 MB** budget (`interpreter.cpp` ~L1224, `common.h`). Always a full STW `MajorGC`. |
| Roots | `HandleScope` (std::deque slots) + `extra_roots_`; interpreter snapshots state into handles at each safepoint check. |
| "Concurrent" marker | **Facade.** `concurrent_marker.h` states "No worker ever reads the object graph"; `NotifyWrite` is a no-op; `FinalizeConcurrentGC` = STW `MajorGC`. |
| Card table | Allocated, marked by the barrier, **never read**. `MinorGC` calls `Trace(old_gen_.Objects())` = traces the *entire old gen*. |
| Interpreter | Single `switch` inside `try` inside `while` (`interpreter.cpp` L1222+). **JS→JS calls recurse in C++.** Hard cap `kMaxCallDepth = 256` and 256 KB native-stack budget (`NativeStackGuard`). |
| Tier 1 JIT | `BaselineJIT::CanCompile` accepts 61/121 opcodes: constants, arithmetic, compares, branches, `Return`. **No property access, calls, closures, exceptions, globals.** |
| Tier 2 JIT | `OptimizingJIT::CanCompile` accepts ~41 opcodes, **exactly one `Return`, ≤ 1 loop, Smi-typed**. Everything else is rejected. |
| Net effect | Real JS programs run in the **interpreter**. "3-tier pipeline" = numeric micro-kernels only. |
| Strings | `JSString` wraps UTF-8 `std::string` (`"😀".length === 4`, `charCodeAt(0) === 240`). |
| Regex | `bounded_regex.cpp`: **no** lookahead, lookbehind, backreferences, named groups, `u` flag, property escapes. |
| Wasm | MVP interpreter (`wasm.cpp` 3.4K lines). Out of scope for this plan. |

---

## 2. Claims vs reality (fix the docs in Phase 8; do not copy these claims anywhere)

| Claim (README / progress.md) | Finding |
|---|---|
| 32-bit pointer compression, `r14` heap base | Not implemented. `r14` appears only in `push/pop` in `assembler.cpp`. |
| Concurrent marking with SATB barrier | Facade; synchronous STW trace only. Barrier calls a no-op. |
| Card-table remembered set for minor GC | Never consulted; minor GC traces all old objects. |
| Cheney scavenger | Map-based relocation with `std::map` forwarding, not Cheney. |
| Immix line recycling | Each object occupies **whole 128 B lines** (`ImmixBlock::Allocate` rounds to lines). `MarkObject` linearly scans all blocks under a mutex. |
| Generational GC in use | Runtime allocations go straight to old gen; the nursery is effectively unused by JS execution. |
| Multi-tier JIT speedups | See table above; only trivial numeric functions are JIT-eligible. |
| "Build passing" badge | Static shields.io image; no CI exists. |
| Builds on GCC/Clang | `CMakeLists.txt` adds `/Zi` and `/DEBUG` unconditionally → CMake build fails off MSVC. |
| 52,280 Test262 tests, 42.5% | Corpus not in repo; number not reproducible from the zip. |
| Test counts (23/24/29 binaries, 223/231 probes) | Inconsistent across docs. `CMakeLists.txt` defines 32 executables incl. `aether`. |

---

## 3. Measured baselines (g++ 13, -O0, 1 core — re-measure at -O2)

Probe scripts live in this plan's appendix (A.1). `process.argv[2]` is the size parameter.

| Probe | N | Wall | Peak RSS |
|---|---|---|---|
| `gc1.js` churn (3 small objects/iter, almost all garbage) | 50K | 1.9 s | **202 MB** |
| | 100K | 3.3 s | 386 MB |
| | 200K | 6.6 s | 755 MB |
| | 400K | 13.5 s | 938 MB (first GC finally runs) |
| `gc2.js` live set `{a,b:[i]}` | 40K | 0.9 s | 112 MB (≈ **1.2 KB per small object**) |
| | 300K | 6.7 s | 733 MB |
| `str.js` `s += "ab"` | 10K / 20K / 40K / 80K | 0.9 / 3.3 / 12.7 / 46 s | 137→499 MB (**quadratic**, hits cap) |
| `gc3.js` binary trees | D=12 | 0.55 s | 41 MB |
| recursion depth | 20 ok, **25 fails** (`RangeError`) at -O0; hard cap 256 at any opt level | | |
| `loop.js` 3M iterations | | 10 s | 17 MB (idle RSS 17 MB) |

Conclusions: memory per object is ~15–25× too high; GC never runs until ~384 MB; string append is O(n²);
JS recursion depth is unusable.

---

## 4. Design decisions (adopt unless a measurement proves them wrong)

**D1 — Go non-moving first.** The 500+ `AllocateRuntime` sites hold raw `HeapObject*` across allocations; a
moving collector is unsafe until all of them are move-safe. Replace the (effectively dead) copying nursery
with a **non-moving, sticky-mark-bit generational Immix**. Evacuation comes later, only for unpinned objects
(Phase 2/3).

**D2 — Conservative native-stack scanning.** Keep the heap precise, but scan native C++ stacks and saved
registers conservatively, treating hits as *pinned* roots. An O(1) "is this an object start?" check
(per-block object-start bitmap, T1.2) makes this cheap and safe. This removes the fragile `NoGCScope` /
`HandleScope` / `HandleScope::Clear()`-per-opcode protocol and allows GC at any allocation.

**D3 — Object remembered set, not address cards.** Object payloads live outside the GC heap, so address-based
cards do not cover them. Use a per-object "remembered" bit plus a vector of remembered objects.
Remove `CardTable` once nothing uses it.

**D4 — Move payloads into the GC heap (Phase 2).** Properties, array elements, strings and environments
become GC-allocated, trivially relocatable arrays. This removes destructors from hot objects and is the
prerequisite for evacuation, parallel marking and any real concurrency.

**D5 — Honest naming.** Rename the facade `ConcurrentMarker` → `MarkCoordinator` (or keep name but document)
until real concurrent marking lands (Phase 3).

---

## PHASE 0 — Foundation (blocks everything; ~2 weeks)

### [x] T0.1 Portable build
- **Files:** `CMakeLists.txt`.
- Wrap `/Zi` and `/DEBUG` in `if(MSVC)`. Default `CMAKE_BUILD_TYPE=RelWithDebInfo` if unset.
- Add `option(AETHER_SANITIZE "ASan+UBSan" OFF)`, `AETHER_TSAN`, `AETHER_GC_STRESS_DEFAULT`.
- Replace the 32 hand-written `add_executable` blocks with a loop over `tests/*.cpp`; register each with `add_test`.
- Link `Threads::Threads`.
- **Acceptance:** `cmake -B build && cmake --build build -j && ctest --test-dir build` is green on Ubuntu 24.04 (g++ 13 **and** clang 17+) and on Windows MSVC.

### [x] T0.2 CI
- Add `.github/workflows/ci.yml`: matrix {ubuntu-gcc, ubuntu-clang, windows-msvc, macos-clang(allow-fail)} × {Release, ASan+UBSan}; plus a TSan job.
- Replace the static badge in `README.md` with the real workflow badge.
- **Acceptance:** CI green on `main`; badge reflects it.

### [x] T0.3 Real Test262 pipeline
- Add `tools/fetch_test262.sh` that clones tc39/test262 at a **pinned commit** into `test/official-test262` (git-ignored). Record the commit in `tools/test262.lock`.
- Extend `tools/run_test262.py`: `--json out.json`, `--features`, `--tier {interp,baseline,opt}`, `--gc-stress N`, `--shard i/n`, `--timeout`, per-directory summary.
- Add `tools/baselines/test262.json` (list of passing test paths). Add `tools/check_baseline.py`: **fail CI if any previously-passing test now fails**; print newly-passing count.
- Generate the Test262 section of `progress.md` from the JSON (no hand-edited numbers).
- **Acceptance:** one command reproduces the pass count; CI nightly job runs it and uploads the JSON.

### [x] T0.4 CLI + runtime flags (`src/main.cpp` only supports `-v -h -e` today)
Implement, in a shared `Options` struct readable by heap/interpreter/JIT:
`--gc-stress[=N]` (collect every N allocations, default 1), `--gc-verify` (run heap verifier before/after every GC),
`--trace-gc`, `--gc-at-safepoints`, `--max-heap=MB`, `--interpreter-only`, `--always-baseline`, `--always-optimize`,
`--tier-threshold=N`, `--stack-size=KB`. Also pass script arguments to `process.argv` (verify `process.argv[2]`).
- **Acceptance:** each flag has a test in `tests/test_flags.cpp` proving it changes behavior.

### [x] T0.5 Heap verifier (debug/`--gc-verify`)
- New `src/heap_verifier.cpp`. Checks before and after each GC: every `TaggedValue` slot in every live object, handle, extra root and JIT constant cell points to a valid object start; mark bits consistent; no poison bytes (`0xDD`/`0xCD`) reachable; remembered-set completeness (after minor GC: full trace finds no unmarked young object reachable from old); budget accounting equals sum of live sizes.
- **Acceptance:** verifier runs clean over all existing test binaries; a deliberately injected missing-barrier bug is caught by a new test.

### [x] T0.6 Benchmarks
- Create `bench/` with the probes in Appendix A.1 plus: binary-trees, splay-like, Map/Set churn, string builder, array push/pop, closure creation, `JSON` round trip, `fib(30)`, `nbody`.
- `bench/run.py` outputs JSON (wall, peak RSS, GC count, total/max pause). Commit **-O2 baselines** to `bench/baselines.json`.
- **Acceptance:** `python bench/run.py --check` compares against baselines and prints a table.

### [x] T0.7 Tier-parity + GC-stress matrix
- Script `tools/matrix.sh` runs all test binaries and Test262 under: `{default, --interpreter-only, --always-baseline, --always-optimize} × {no stress, --gc-stress=1000, --gc-stress=1 --gc-verify}`.
- Any cell that differs from the default cell is a bug; file it and fix it in the owning subsystem.
- **Acceptance:** matrix is green or every divergence is listed in `docs/known_divergences.md` with an owner.

---

## PHASE 1 — GC Stage A: remove algorithmic disasters, keep the object model (3–5 weeks)

Goal: stop paying O(heap) per minor GC, O(blocks) per mark, ~1.2 KB per small object, and 384 MB before first GC.
Implements D1, D2, D3. **Order matters.**

### [x] T1.1 Object header redesign
- **Files:** `heap_object.h`, `gc.h`.
- Give every `HeapObject` a compact header word: `mark`, `old/young (sticky)`, `remembered`, `pinned`, `age`, `type`. Keep `ObjectSize()` but cache the size where cheap.
- **Acceptance:** `sizeof(HeapObject)` documented and tested; all existing tests green.

### [x] T1.2 Immix allocator rewrite (`gc.h/gc.cpp`)
1. Allocate blocks from the 4 GB reservation **32 KB-aligned**, header at block start; `BlockFor(ptr)` = mask; `Contains` becomes O(1) (range check + block-state check).
2. Per-block **object-start bitmap** (1 bit per 8 B) for conservative-root validation and linear heap walks.
3. **Bump-allocate within holes at 8-byte granularity.** Do *not* round objects to 128 B lines. Mark every line an object spans. Add the standard "overflow allocator" for objects > 1 line.
4. Block lists: free / recyclable / full, with per-block live-line counts for hole selection.
5. **Large-object space** (> ~8 KB): own mapping/list, never moved, intrusive header. Stop calling `::operator new` through the map.
6. **Delete** `ImmixSpace::allocations_` (`std::map`) and `IsolateHeap::young_` (`std::map`).
- **Acceptance:** `gc2.js 300000` peak RSS ≤ 40% of baseline; allocation micro-bench ≥ 5× faster; `test_milestone3`, `test_hardening_gc` green; verifier clean.

### [x] T1.3 Marking engine
- Replace `std::unordered_set<HeapObject*> seen` + `live` vector with header mark bits and an explicit mark stack.
- Replace `std::function<void(TaggedValue&)>` in `IteratePointers` with a lightweight non-owning visitor (`SlotVisitor` with a virtual `Visit(TaggedValue*)`, or `function_ref`). Update all overrides (`grep -n IteratePointers`).
- Keep the ephemeron fixpoint, `WeakRef`, `FinalizationRegistry` semantics (tests: `test_milestone21`, `test_hardening_gc`).
- Delete `ImmixSpace::MarkObject`'s block scan; line marks computed O(1) from the block header.
- **Acceptance:** full-heap mark time on a 200K-object heap ≥ 10× faster than baseline (use `--trace-gc`).

### [x] T1.4 Conservative stack scanning (D2)
- Capture registers (`setjmp`/`getcontext`/`RtlCaptureContext`) and scan `[sp, stack_base)` of the owning thread; each word that points at a valid object start (bitmap) pins that object.
- Make `AllocateRuntime` collectable at allocation (remove the "never collects" restriction). Remove per-opcode `op_roots.Clear()` and `SnapshotState` from the safepoint check once stress runs clean.
- Keep `HandleScope` working for embedders; mark it "optional" in docs.
- **Acceptance:** `--gc-stress=1 --gc-verify` passes all test binaries; `HandleScope::Clear` removed from the dispatch loop; measured interpreter overhead ↓.

### [x] T1.5 Sticky generational minor GC (replace `MinorGC` and the copying nursery)
- Allocate everything in Immix. "Young" = allocated since the last collection (sticky mark bits: survivors keep their mark).
- Minor GC marks from roots + remembered set, only newly allocated objects; sweeps only blocks that received allocation.
- **Write barrier:** `WriteBarrier(source, value)` records `source` into the remembered set when `source` is old and `value` is young. **Audit every mutation path** (property set, array element set, `Map/Set` insert, closures/environments, `JSFunction` fields, TypedArray no, promise reactions, generators, proxies, module bindings). Today there are only ~22 barrier call sites.
- Add `--gc-verify-barrier`: after each minor GC, run a full trace and assert no old→unmarked-young edge exists.
- Remove `NurserySpace`, `young_`, `RelocateTo`-based scavenging, and `CardTable` (D3) once green. Keep poisoning (`0xDD`/`0xCD`) under a debug flag.
- **Acceptance:** `gc1.js 400000` peak RSS ≤ 15% of baseline; minor GC time scales with *live young* data, not heap size (benchmark at 1×, 4×, 16× old-gen size; pause growth < 1.5×).

### [x] T1.6 Triggering and pacing
- Replace "75% of 512 MB" with allocation-volume triggers: minor GC after `young_budget` bytes (start 8 MB, adaptive on survival rate); major GC when old gen grows 1.5–2× over post-last-major live size (min 32 MB).
- Safepoint poll = decrement an allocation debt counter at backward jumps/calls; no per-instruction `HeapPressure()` call.
- Keep `MemoryBudget` as a hard limit only (OOM), not as a trigger.
- **Acceptance:** `gc1.js` runs in bounded RSS (< 64 MB) at any N; `--trace-gc` shows regular minor GCs.

### T1.7 Per-object memory diet
- Investigate `PropertyStorage` (`heap_object.h` ~L150) and `Shape` (`shape.h`): ≈1.2 KB per small object means per-object hash tables / vectors / `MemoryCharge` members.
- Store named properties as inline slots described by `Shape`; spill to a single out-of-line array; dictionary mode only after many deletes/adds. Keep property **insertion order** (see T6.2).
- Target sizes: `{a:1,b:2}` ≤ 64 B (+ shared shape); `[x]` ≤ 64 B + elements; closure ≤ 64 B.
- **Acceptance:** `gc2.js 300000` ≤ 120 MB peak RSS; sizes asserted in `tests/test_object_sizes.cpp`.

---

## PHASE 2 — GC Stage B: payloads into the GC heap (4–6 weeks)

Goal: trivially relocatable objects, no per-object destructors, real evacuation. Implements D4.

### T2.1 GC-allocated array types
- Introduce `FixedArray<TaggedValue>`, `ByteArray`, `PropertyArray`, `ElementsArray` as heap objects (some exist: `HeapObjectType::FixedArray`; check `heap_object.h` before adding).
- Migrate in order of volume: `JSObject` property slots → `JSArray` elements → `LexicalEnvironment` slots → `JSFunction` captured data → `JSMap/JSSet` tables → promise reaction lists.
- Each migration PR removes the matching `std::vector`/`std::unordered_map` and `MemoryCharge` for that field.
- **Acceptance:** per type, `sizeof` and RSS benches improve; destructor of migrated type becomes trivial.

### T2.2 Strings (also fixes UTF-16 conformance; coordinate with T6.1)
- New string representation: flat **Latin-1 or UTF-16**, hash cached, interning table for property keys, **ropes** for concatenation (flatten lazily), sliced strings for `substring`.
- `s += "ab"` loop must be O(n) total.
- **Acceptance:** `str.js 80000` < 0.3 s (-O2) and RSS < 64 MB; `"😀".length === 2`, `"😀".charCodeAt(0) === 0xD83D`, lone surrogates preserved.

### T2.3 Opportunistic Immix defragmentation
- During major marking, compute per-block live-line histograms; select sparsely used blocks as evacuation candidates; evacuate **unpinned** objects (those not hit by conservative stack scan), install forwarding pointers, fix references, update the remembered set.
- Objects with non-trivial C++ state are excluded until migrated (T2.1).
- **Acceptance:** fragmentation (committed bytes / live bytes) after a churn benchmark ≤ 1.3×; verifier clean under `--gc-stress`.

### T2.4 Lazy sweeping and heap shrinking
- Sweep blocks on demand during allocation; return fully free blocks to the OS (`madvise(MADV_DONTNEED)` / `VirtualFree(MEM_DECOMMIT)`).
- Honor `--max-heap`; throw a catchable `RangeError`-style OOM (or terminate) deterministically.
- **Acceptance:** RSS returns to < 2× idle after a large workload is released.

### T2.5 Dead metadata collection
- Weak shape transitions, IC cache clearing on GC (`FeedbackSlot::cached_shape` is a raw `Shape*`), code and bytecode flushing for cold functions, wasm registries (existing hooks: `WasmRegistryPruneDead`, `WasmRegistryRelocate`).
- **Acceptance:** a test that creates/discards 1M distinct shapes does not grow RSS unboundedly.

---

## PHASE 3 — GC Stage C: parallel and concurrent (4–6 weeks)

Only start when T2.1 covers JSObject/JSArray/JSString/Environment (otherwise worker threads would read mutable `std::vector`s).

### T3.1 Parallel STW marking
- N worker threads, per-thread mark stacks with work stealing, atomic mark-bit CAS. Same for parallel sweeping.
- **Acceptance:** major GC time on a 1M-object heap scales ≥ 2.5× with 4 threads; TSan clean.

### T3.2 Parallel minor GC
- Parallelize remembered-set + root scanning and young marking.

### T3.3 Real concurrent marking
- Choose **SATB** (deletion barrier: record the *overwritten* value) and implement it exactly: barrier on every reference overwrite, allocate-black during marking, bounded STW initial/remark pauses, concurrent mark thread(s).
- Current description in `progress.md` ("record mutated *source object*, retrace at remark") is incremental-update, not SATB. Do not mix them.
- Add `--gc-verify-satb` checker. Concurrent worker may only read objects whose payloads are in GC heap or protected by the object's own lock.
- Remove the facade or make it the real implementation; update docs (D5).
- **Acceptance:** TSan clean; pause p99 < 5 ms on a 500 MB heap (bench `bench/pause.js`); stress matrix green.

### T3.4 Pacing and tuning
- Mutator-utilization-based pacing, heap growth heuristics, idle-time/memory-reducer GC, `--max-heap`, near-limit callback in the embedder API.

### T3.5 (stretch, only if T3.3 pauses are still too long)
- Concurrent evacuation with load barriers. Needs its own design doc first.

---

## PHASE 4 — Interpreter core (3–5 weeks; may overlap Phase 2)

### T4.1 Non-recursive JS→JS calls
- Today every JS call re-enters `Interpreter::Execute` in C++, capped at 256 frames / 256 KB (`interpreter.h` L190, `interpreter.cpp` L26–48).
- Introduce a contiguous `FrameStack` (registers + frame metadata) scanned **precisely** as a GC root. `Call/Construct/Return/Throw` push/pop frames inside the dispatch loop; only native→JS re-entry recurses.
- Depth limit by **bytes of frame stack** (default ≈ 10K+ typical frames), configurable by `--stack-size`; throw `RangeError` correctly and keep the engine usable afterwards.
- **Acceptance:** recursion depth 10,000 works (`function r(n){return n?1+r(n-1):0}`); `fib(30)` runs; tests for exception unwinding across frames; `--gc-stress` clean.

### T4.2 Generators/async on heap frames
- Verify how generators/async currently save state (investigate; the engine does not use threads or ucontext). Move to "suspend = copy register window into a GC array; resume = copy back" or run the generator on the frame stack in a detached segment.
- **Acceptance:** `[...gen()]` and `yield*` work (currently `[...g()]` prints `[object Object]`); async-order tests pass; deep async recursion works.

### T4.3 Dispatch loop quality
- Move `try/catch` out of the per-instruction loop (exceptions handled via handler tables → unwind to nearest handler); keep `AETHER_MUSTTAIL` threaded dispatch for Clang, `switch` + computed goto for GCC, `switch` for MSVC.
- **Acceptance:** `loop.js`, `fib(30)` ≥ 2× faster at -O2 than the T0.6 baseline.

---

## PHASE 5 — Bytecode / ISA (4–6 weeks)

Current ISA: 121 opcodes in `bytecode.h`. Register operands are `u8` (≤ 256 registers), constants `u16`, feedback slots `u16`.
Interpreter implements all 121. Tier 1: 61. Tier 2: ~41.

### T5.1 ISA infrastructure
- **Disassembler** (`aether --dump-bytecode`), **bytecode verifier** (operand ranges, jump targets, register bounds, handler table consistency), **source-position table** (for stack traces, error messages, future debugger).
- **Wide / ExtraWide prefixes** so functions with > 256 registers or > 65,535 constants compile instead of failing.
- Peephole pass (`Star r; Ldar r` elision, constant folding of `LdaSmi+Add`), constant-pool dedup.
- **Acceptance:** verifier runs on every function emitted by every test; fuzzer `tests/fuzz_emitter.cpp` (random AST → emit → verify) finds no verifier failures in 1M iterations.

### T5.2 Coverage audit
- Generate `docs/isa_coverage.md` from the AST (`ast.h`) × emitter × interpreter × Tier1 × Tier2 matrix. For each AST construct, say whether it is a dedicated opcode, a runtime call, or a desugaring.
- Anything lowered by calling named runtime helpers or by desugaring in `bytecode_emitter.cpp` goes on a list.

### T5.3 Candidate opcodes (verify each against the emitter before adding; add only what removes a desugaring or enables correctness/perf)
- **Scope/TDZ:** `PushBlockContext`/`PopContext`/`CreateBlockContext`, `ThrowIfHole <reg|slot>` (TDZ is **not enforced today**: `try{ x; let x=1 }catch(e){}` does not throw), `ThrowConstAssign`.
- **Classes/objects:** `DefineOwnProperty`, `DefineKeyedOwn` (field definition without setters), `DefineMethod` (with home object), `SetHomeObject`, `GetSuperProperty/SetSuperProperty`, `LdaNewTarget`, `DefinePrivateField/Method`, private brand check, static-block entry.
- **Literals:** `CreateObjectLiteral` / `CreateArrayLiteral` from boilerplate constants, `GetTemplateObject`, `CreateMappedArguments` / `CreateUnmappedArguments` (sloppy mapped arguments are not implemented: `function f(a){arguments[0]=9;return a}` returns 1).
- **Iteration:** `GetIterator` (with feedback), `IteratorNext`, `IteratorClose`, `GetAsyncIterator`; use them for `for-of`, destructuring, spread, `yield*`, `for await`.
- **Control:** `SwitchOnSmi` jump table, `JumpLoop` (budget + OSR hook + safepoint poll), `ReThrow`, `ThrowReferenceError/TypeError <const>`.
- **Calls:** `CallProperty`/`CallUndefinedReceiver` with feedback, `ConstructWithSpread`, `TailCall` only if required by spec tests (it is not in ES2023).
- **Generators/async:** `SuspendGenerator`, `ResumeGenerator`, `GetGeneratorState` (aligns with T4.2).
- **Lookup:** `LdaLookupSlot`/`StaLookupSlot` for sloppy `with`/direct `eval`.
- **Acceptance per opcode:** emitter + interpreter + disassembler + verifier + unit test + Test262 delta noted in the PR.

### T5.4 Make Tier 1 cover **every** opcode
- Goal: *eligibility ≠ rarity.* Template JIT emits inline fast paths for Smi/number arithmetic, IC hits for `LdaNamed/StaNamed`, and **calls into shared C++ stubs for everything else** (property slow paths, calls, closures, exceptions). No opcode may make `CanCompile` return false.
- ABI: introduce a platform ABI layer in `assembler.h` (Win64 vs SysV). Today `assembler.cpp` hard-codes `push rdi/…` prologues; confirm SysV correctness on Linux before enabling.
- **Acceptance:** `--always-baseline` passes the entire test matrix and Test262 with **zero** interpreter fallbacks (counter exposed via `--trace-tier`). `fib(30)` ≥ 3× faster than interpreter.

### T5.5 Tier 2 generalization (after T5.4)
- Arbitrary CFGs, multiple returns, nested loops, phis, property loads with shape guards, calls, **deoptimization** with real frame reconstruction, OSR entry at `JumpLoop`.
- Until deopt exists, Tier 2 must stay opt-in (`--enable-tier2`); today it is limited to straight-line / single-loop Smi functions.
- **Acceptance:** deopt-fuzz mode (`--deopt-every=N`) passes the matrix.

---

## PHASE 6 — Missing functionality

### 6a. Correctness bugs found by probing (do these first; each is small)

| ID | Probe | Observed | Expected |
|---|---|---|---|
| T6.1 | `"😀".length`, `.charCodeAt(0)`, `"é".length` | 4, 240, 2 | 2, 55357, 1 (UTF-16 semantics; see T2.2) |
| T6.2 | `JSON.stringify({a:[1,{b:2}],c:"x"})` | `{"c":"x","a":…}` | `{"a":[…],"c":"x"}`. Own-property order must be: integer keys ascending, then strings in insertion order, then symbols. Fix in property storage (T1.7). |
| T6.3 | `a: for(…){ for(;;){ continue a } }` | SyntaxError | labeled statements, `break/continue label` |
| T6.4 | `try{ x; let x=1 }catch(e){…}` | no throw | `ReferenceError` (TDZ) |
| T6.5 | `function f(a){arguments[0]=9;return a} f(1)` | 1 | 9 (sloppy mapped arguments) |
| T6.6 | `[...(function*(){ yield* [1,2]; return 3 })()]` | `[object Object]` | `[1,2]` |
| T6.7 | `for (const [k,v] of new Map([[1,2]])) console.log(k,v)` | prints nothing | `1 2` |
| T6.8 | `Symbol("x").description` | throws TypeError | `"x"` |
| T6.9 | `"a".localeCompare("b")`, `"ä".normalize("NFD")` | "callee is not a function" | implement (ICU-free table or minimal Unicode data; see T6.12) |
| T6.10 | `class A{ static z; static { A.z=3 } }` | SyntaxError | class static blocks |
| T6.11 | `/(?<y>\d+)/.exec("1").groups`, `/a(?=b)/`, `/(a)\1/`, `/\u{1F600}/u` | unsupported / TypeError | see T6.20 |
| T6.12 | `new Error().stack` | `undefined` | non-standard but expected by tooling; provide a string with function names + positions (needs source-position table, T5.1) |
| T6.13 | recursion depth | ≤ 256 | see T4.1 |

For each: add a failing test first, then fix. Run the full matrix.

### 6b. Missing built-ins and features (probe output: `undefined`)
Prioritize by Test262 `features:` frontmatter (use `tools/run_test262.py --features` from T0.3) and by tests gained per day of work.

1. **Array:** `toSorted`, `toReversed`, `toSpliced`, `with`, `findLast*` (verify), `Array.fromAsync`, `flat/flatMap` edge cases.
2. **Object / Map:** `Object.groupBy`, `Map.groupBy`, `Object.hasOwn` (verify).
3. **Promise:** `Promise.withResolvers`, `Promise.try` (verify against the pinned Test262).
4. **Set methods:** `union`, `intersection`, `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf`, `isDisjointFrom`.
5. **Iterator helpers:** global `Iterator`, `Iterator.prototype.map/filter/take/drop/flatMap/reduce/toArray/forEach/some/every/find`, `Iterator.from`.
6. **ArrayBuffer:** resizable (`maxByteLength`, `resize`), `transfer`, `transferToFixedLength`, `detached`; fix TypedArray length-tracking views.
7. **Classes (≈ 5,000 Test262 tests combined):** static blocks, accessor/computed/static edge cases, `super` edge cases, subclassing built-ins, class element ordering, name inference; decorators last.
8. **RegExp (T6.20):** replace `bounded_regex.cpp` with a UTF-16-aware backtracking VM supporting lookahead, lookbehind, backreferences, named groups, `u`/`v`/`s`/`d`/`y` flags, Unicode property escapes (generated tables), case folding; **keep** the step counter and recursion/ReDoS limits and the `test_hardening_regex` guarantees. Wire `Symbol.match/replace/split/matchAll`.
9. **Date:** parsing formats, local time zone handling, `toLocale*`, edge arithmetic (≈ 400 failing tests).
10. **Annex B / sloppy mode:** function-in-block hoisting, `__proto__`, `escape/unescape`, HTML comments, legacy octal.
11. **Modules:** `import.meta`, import attributes, top-level-await edge cases, dynamic-import error paths.
12. **Temporal:** only after everything above; large surface.
13. **Host (non-ES) APIs:** `TextEncoder/TextDecoder`, `URL/URLSearchParams`, `structuredClone`, `fetch` (optional), `queueMicrotask` exists. Put in a separate `host/` library, not in the ES core.
14. **Unicode data:** generate tables from the UCD in `tools/gen_unicode.py`; used by regex, `normalize`, `toUpperCase/toLowerCase`, identifier lexing, `localeCompare`.

### 6c. Already working (probed OK — do **not** rework)
Class fields, private fields and methods, accessors, `super.method()`, `new.target`, tagged templates, `switch`/fallthrough,
BigInt basics, optional chaining, spread calls, destructuring in `for-of` over arrays, `Proxy` get, direct `eval`/`Function`,
`with`, microtask/macrotask ordering, `Intl.NumberFormat`, basic `Date`/`JSON`, typed array `map`.

**Per-feature workflow:** failing Test262 tests → minimal repro in `tests/` → emitter/runtime fix → matrix → baseline update.

---

## PHASE 7 — JIT & platform (only after Phases 4–5)

- **T7.1 OSR** (interpreter→Tier 1→Tier 2 at `JumpLoop`).
- **T7.2 Deopt bailouts** with frame reconstruction (prerequisite for broader Tier 2 speculation).
- **T7.3 Escape analysis / scalar replacement** (after objects have stable shapes and IC data).
- **T7.4 ARM64 backend** (`assembler.h` split into x64/arm64; shared `MacroAssembler` interface).
- **T7.5 Executable memory:** replace `PROT_READ|WRITE|EXEC` with W^X (map RW, write, then flip to RX; macOS `MAP_JIT`). Add code-cache GC (free JIT code of dead functions).
- **T7.6 Head-to-head benchmarks** vs QuickJS, Boa, Node `--jitless`, then V8/JSC. Publish results only from the CI bench job.

---

## PHASE 8 — Documentation honesty (after Phases 0–3 changes land; update continuously)

- Rewrite `README.md`/`progress.md`: remove or correct every row in §2; auto-generate test/bench tables from CI artifacts.
- Mark status per subsystem as **Prototype / Usable / Production** with evidence links.
- Document: object model, GC (stages, barriers, root protocol, pinning), tiering rules, flags, how to run the matrix.
- Delete stale statements (pointer compression, SATB, "sub-megabyte", "Cheney"), or implement them.
- Reconcile `aether_vm_architecture_specification.md` with the code.

---

## 5. First ten PRs (do these in order)

1. T0.1 portable CMake + ctest.
2. T0.2 CI (Linux gcc/clang, Windows).
3. T0.4 flags: `--gc-stress`, `--gc-verify`, `--trace-gc`, `--interpreter-only`, argv passing.
4. T0.5 heap verifier (initial: roots + object graph validity).
5. T0.6 bench suite with -O2 baselines.
6. T0.3 Test262 fetch + baseline gate.
7. T0.7 matrix script; fix top divergences.
8. T1.2 Immix allocator rewrite (aligned blocks, bitmaps, no `std::map`).
9. T1.3 marking engine (mark stack, header bits, visitor).
10. T1.4 conservative stack scanning, then T1.5 sticky minor GC.

## 6. Definition of done for any GC PR

- `ctest` green on Linux gcc + clang + Windows.
- `tools/matrix.sh` green (tiers × `--gc-stress` × `--gc-verify`).
- ASan+UBSan green; TSan green if any threading is touched.
- `bench/run.py --check` shows no regression > 5%, and the PR states the improvement it claims.
- Docs/decision record updated.

---

## Appendix A — Reproduction scripts

### A.1 Probes (save under `bench/`)

```js
// gc1.js — churn
var N = +process.argv[2] || 100000; var t = Date.now(); var keep = null;
for (var i = 0; i < N; i++) { var o = {a:i, b:[i,i+1], c:{d:i}}; if (i % 1000 == 0) keep = o; }
console.log("churn N=" + N, (Date.now() - t) + "ms");
```
```js
// gc2.js — live set
var N = +process.argv[2] || 20000; var t = Date.now(); var live = [];
for (var i = 0; i < N; i++) live.push({a:i, b:[i]});
console.log("live N=" + N, (Date.now() - t) + "ms", live.length);
```
```js
// gc3.js — binary trees
function Node(l, r) { this.l = l; this.r = r }
function make(d) { return d == 0 ? new Node(null, null) : new Node(make(d-1), make(d-1)) }
function check(n) { return n.l ? 1 + check(n.l) + check(n.r) : 1 }
var D = +process.argv[2] || 12; var t = Date.now(); var long = make(D); var s = 0;
for (var i = 0; i < 20; i++) s += check(make(D - 4));
console.log("bintrees D=" + D, (Date.now() - t) + "ms", s, check(long));
```
```js
// str.js — string append (must become O(n))
var n = +process.argv[2] || 20000; var t = Date.now(); var s = "";
for (var i = 0; i < n; i++) s += "ab";
console.log(s.length, (Date.now() - t) + "ms");
```
```js
// depth.js
function r(n) { return n ? 1 + r(n - 1) : 0 }
try { console.log(r(+process.argv[2] || 10000)) } catch (e) { console.log("ERR", e.message) }
```

### A.2 Minimal Linux build used for the findings (until T0.1 lands)

```bash
mkdir -p build/obj
for f in src/*.cpp; do g++ -std=c++23 -O2 -Iinclude -c $f -o build/obj/$(basename $f .cpp).o; done
g++ build/obj/*.o -o build/aether -pthread
./build/aether -e "console.log('ok')"
# tests: link each tests/X.cpp against all objects except main.o
```

### A.3 Feature probe one-liners (each must print the expected value after its task)

```bash
aether -e 'console.log("😀".length)'                                      # 2
aether -e 'console.log(JSON.stringify({a:1,c:2}))'                        # {"a":1,"c":2}
aether -e 'a: for(var i=0;i<3;i++){ for(;;){ continue a } } console.log(i)' # 3
aether -e 'try{ x; let x=1 }catch(e){ console.log(e.constructor.name) }'  # ReferenceError
aether -e 'function f(a){arguments[0]=9;return a} console.log(f(1))'     # 9
aether -e 'function* g(){ yield* [1,2]; return 3 } console.log([...g()].join())' # 1,2
aether -e 'for (const [k,v] of new Map([[1,2]])) console.log(k,v)'        # 1 2
aether -e 'console.log(Symbol("x").description)'                          # x
aether -e 'class A{ static z; static { A.z=3 } } console.log(A.z)'        # 3
aether -e 'console.log(/a(?=b)/.test("ab"), /(a)\1/.test("aa"))'          # true true
aether -e 'console.log("2020-01".match(/(?<y>\d+)-(?<m>\d+)/).groups.y)'  # 2020
aether -e 'console.log(typeof Object.groupBy, typeof Promise.withResolvers, typeof [].toSorted)' # function function function
```

---

## Appendix B — Risks

| Risk | Mitigation |
|---|---|
| Missed GC root after removing `NoGCScope` protocol | D2 conservative stack scan + verifier + `--gc-stress=1` in CI |
| Missed write barrier (old→young) | `--gc-verify-barrier` full-trace cross-check on every minor GC |
| Native code reading object payload while concurrent marker runs | Gate concurrency on T2.1; object lock or snapshot for non-migrated types |
| Performance regression hidden by -O0 numbers | Only -O2 baselines in `bench/baselines.json` count |
| Windows-only JIT/ABI assumptions | ABI layer (T5.4), CI on both platforms |
| Scope creep into Temporal/decorators/Wasm extensions | Out of scope until Phases 0–6a finish |
| Docs drifting from code | Generated tables + Phase 8 |
