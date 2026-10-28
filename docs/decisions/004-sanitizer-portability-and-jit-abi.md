# ADR 004: Sanitizer Portability, JIT ABI Alignment, and Downcast Invariants

## Context
When running the full continuous integration matrix across Ubuntu (GCC 13, Clang 18), Windows (MSVC 19), and macOS (Apple Clang) under diverse configurations (Release, AddressSanitizer + UndefinedBehaviorSanitizer, ThreadSanitizer, and Full Matrix Validation), several critical architectural divergences and undefined behavior bugs emerged:

1. **JIT Calling Convention Mismatch**:
   - The Microsoft x64 ABI passes first parameters in `RCX` and `RDX`.
   - The System V AMD64 ABI (Linux and macOS) passes first parameters in `RDI` and `RSI`.
   - Hardcoding `RCX`/`RDX` caused immediate crashes when JIT machine code called runtime helper stubs (`Aether_ToBoolean`) on Linux/macOS.

2. **Undefined Behavior in Type Downcasts (UBSan)**:
   - `HeapObject::IsFunction()` checks `type_ == JSFunction || type_ == JSNativeFunction`.
   - In multiple interpreter call-sites (`LdaConstant`, `LdaPrivate`, `StaPrivate`), code inspected `obj->IsFunction()` and executed `static_cast<JSFunction*>(obj)`.
   - Because `JSNativeFunction` inherits from `JSObject` directly rather than `JSFunction`, downcasting a `JSNativeFunction` to `JSFunction*` is illegal undefined behavior caught by Clang/GCC `-fsanitize=undefined`.

3. **Sanitizer Stack Frame Bloat**:
   - AddressSanitizer inserts redzones, shadow memory tracking, and canaries around every function frame, enlarging stack usage by 3x-5x.
   - Recursive micro-benchmarks (`test_hardening_execution.cpp:TestNativeAndDirectRecursion`) overflowed thread stacks before hitting native stack guards or during `RangeError` allocation in `AllocateRuntime<JSError>`.

4. **Multi-Threaded JIT Idle Notification Race (TSan)**:
   - In `BackgroundCompiler::WorkerLoop()`, worker threads decremented `active_workers_` outside `queue_mutex_` before acquiring the lock to notify `cv_idle_`.
   - Under ThreadSanitizer, this allowed `WaitForPendingTasks()` to miss notifications or evaluate state inconsistently, resulting in test timeouts.

5. **Test Object Lifetime Leaks in Placement-New Scenarios**:
   - Milestone verification tests (`test_milestone3`, `test_milestone7_closures`) allocated objects into Immix space or raw heap memory via placement `new` without registering them with `ImmixSpace::RegisterObject` or issuing `delete`.
   - LeakSanitizer (LSan) correctly flagged the uncollected property storage allocations on process termination.

## Decisions

1. **System V AMD64 Register Mapping**:
   - Introduced canonical register definitions `kJITArg0Reg` and `kJITArg1Reg` in `include/aether/assembler.h`.
   - On Windows: `kJITArg0Reg = RCX`, `kJITArg1Reg = RDX`.
   - On Linux/macOS: `kJITArg0Reg = RDI`, `kJITArg1Reg = RSI`.
   - Updated `BaselineJIT` and `OptimizingJIT` to pass trampoline arguments strictly through these platform-independent register aliases.

2. **Enforce `IsUserFunction()` before `JSFunction*` Casts**:
   - Replaced ambiguous `IsFunction()` checks with `IsUserFunction()` prior to all downcasts to `JSFunction*`.
   - Handled `IsNativeFunction()` explicitly via `JSNativeFunction::Invoke` across bytecode dispatchers.
   - Provided static fallback root shape for bytecode emitter class definition creation to avoid null dereferences when running outside a full `Realm`.

3. **Real OS Thread Stack Headroom and Guard Invariants**:
   - Linked `/STACK:8388608` (8 MB) on MSVC.
   - Decoupled `NativeStackGuard` into:
     - Explicit user CLI limit (`Options::Get().stack_size_kb > 0`) checking consumed budget from origin.
     - Real OS stack headroom check:
       - Windows: `GetCurrentThreadStackLimits(&low, &high)`.
       - Linux: `pthread_getattr_np` and `pthread_attr_getstack` with thread-local caching.
       - macOS: `pthread_get_stackaddr_np` and `pthread_get_stacksize_np` with thread-local caching.
     - Headroom threshold:
       - 1 MB remaining headroom under ASan/TSan on Linux/macOS; 256 KB under MSVC ASan on Windows.
       - 128 KB on Linux/macOS release; 64 KB on Windows release.
   - Eliminated false `RangeError: Maximum call stack size exceeded` in non-recursive generator coroutines (`ResumeGenerator`) and async/await steps while reliably catching infinite direct/native recursion before the OS guard page is reached.

4. **Synchronized Worker Completion**:
   - Enclosed `active_workers_` decrement and `cv_idle_.notify_all()` strictly inside `queue_mutex_` in `BackgroundCompiler`.

5. **Explicit Lifetime Management in Tests**:
   - Ensured all manual `new (old_mem) JSObject` calls in unit tests register with `heap.GetOldGen().RegisterObject(...)` so `DestroyAll()` invokes class destructors.

## Consequences & Verification
- All 30 CTest suites pass across all platforms.
- `ubuntu-gcc (Release)`, `ubuntu-clang (Release)`, `windows-msvc (Release)`, `macos-clang (Release)`, `ubuntu-tsan`, `windows-msvc-sanitize`, `ubuntu-clang-sanitize`, and `full-matrix-validation` are fully green.
