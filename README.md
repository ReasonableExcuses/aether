# Aether

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B)](https://en.wikipedia.org/wiki/C%2B%2B23)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()

Aether is an experimental ECMAScript and WebAssembly runtime implemented from scratch in modern C++ (ISO C++23). It is designed as an embeddable, modular virtual machine with a multi-tier JIT compilation pipeline, an Immix-based generational garbage collector, and native concurrency primitives.

> **Status**: Aether is an active research and development project. While it implements a substantial subset of ECMAScript and the WebAssembly MVP specification, it is not yet intended for production deployments. For detailed milestone tracking, architecture deep-dives, and verification status, see [progress.md](progress.md).

---

## Key Architectural Components

### Value Representation (NaN-Boxing)
All runtime values are represented as 64-bit quiet NaN-boxed `TaggedValue` instances. Primitive types (doubles, 32-bit signed integers, booleans, null, undefined, sentinels) and 48-bit heap object pointers fit directly inside CPU registers, eliminating pointer dereferencing for scalar operations.

### Multi-Tier Execution Pipeline
Aether employs an incremental execution pipeline that transitions code from quick startup to optimized execution:
- **Tier 0 — Bytecode Interpreter**: An accumulator-based bytecode interpreter designed for low startup latency and minimal memory overhead.
- **Tier 1 — Baseline Template JIT**: A fast, single-pass machine-code generator for x86-64 that expands bytecode into native machine code with inline caches.
- **Tier 2 — Sea-of-Nodes Optimizing JIT**: A speculative optimizing compiler based on an SSA intermediate representation combining value, effect, and control dependencies. Includes Global Value Numbering (GVN), constant folding, dead-code elimination, and type specialization.
- **Background Compiler**: A dedicated worker thread pool (`BackgroundCompiler`) that compiles hot functions asynchronously without stalling mutator threads.

### Generational & Immix Memory Management
- **Nursery Generation**: Bump-pointer allocation within a semi-space young generation, reclaimed via a Cheney copying scavenger (`MinorGC`).
- **Old Generation**: An Immix mark-region space that allocates into recyclable 32 KiB blocks and 128-byte lines, eliminating full-heap compaction pauses (`MajorGC`).
- **Concurrent Marking**: A dedicated background marking thread (`ConcurrentMarker`) that traces the heap concurrently with mutator execution, guarded by a Snapshot-At-The-Beginning (SATB) write barrier.

### Object Model & Inline Caching
- **Hidden Classes (Shapes)**: Objects track property layout via directed acyclic shape transition trees with immutable property descriptors.
- **Inline Caches (ICs)**: Property accesses use monomorphic, polymorphic, and megamorphic ICs with inline feedback vectors to accelerate dynamic lookups.

### WebAssembly 1.0 MVP
- Stack machine interpreter supporting standard Wasm binary decoding (Sections 0–11).
- Linear memory (`WebAssembly.Memory`), tables (`WebAssembly.Table`, `call_indirect`), and mutable/immutable globals (`WebAssembly.Global`).
- Full instruction-level validation (operand-stack typing, structured control flow, and branch depth verification).
- Lossless 64-bit integer (`i64`) interoperability with ECMAScript `BigInt`.

### Concurrency & Atomics
- **SharedArrayBuffer**: Multi-agent shared memory backing for typed arrays.
- **Atomics**: Complete implementation of standard `Atomics` operations, supporting 32-bit integers as well as 64-bit `BigInt64Array` and `BigUint64Array`.
- **Futex Synchronization**: Thread blocking and wake-up semantics via `Atomics.wait` and `Atomics.notify`.
- **Asynchronous Waits**: Non-blocking `Atomics.waitAsync` with Promise-based asynchronous resolution and agent lifecycle cleanup.

---

## Language & Specification Support

Aether implements core parts of the ECMAScript specification:
- **Syntax & Language Features**: ES2023+ syntax, lexical closures, classes, destructuring assignment, template literals, optional chaining (`?.`), nullish coalescing (`??`), and logical assignment operators (`&&=`, `||=`, `??=`).
- **Async Programming**: Native Promises (Promises/A+ compliant), `async`/`await`, async generators (`async function*`), and `for await...of` iteration.
- **Iterators & Generators**: Iteration protocols, `Symbol.iterator`, `Symbol.asyncIterator`, and generator coroutines (`function*` / `yield`).
- **Meta-Programming**: Complete `Proxy` (all 13 traps) and `Reflect` APIs.
- **Binary Data**: `ArrayBuffer`, `DataView`, and typed array views (`Int8Array` through `Float64Array`, `BigInt64Array`, `BigUint64Array`).
- **Collections & Intrinsics**: `Map`, `Set`, `WeakMap`, `WeakSet`, `WeakRef`, and `FinalizationRegistry`.
- **Modules**: Both CommonJS (`require` / `module.exports`) and ECMAScript Modules (`import` / `export`, dynamic `import()`, top-level `await`).
- **Internationalization**: Core ECMA-402 `Intl` APIs (`NumberFormat`, `DateTimeFormat`, `Collator`).
- **Conformance**: Tested against curated subsets of the official TC39 Test262 suite.

---

## Building from Source

### Prerequisites
- **C++ Compiler**: A modern C++20/C++23 compliant compiler:
  - MSVC 2022 (version 19.38+)
  - GCC 13+ or Clang 17+
- **CMake**: 3.20 or newer
- **Architecture**: x86-64 (AVX2 supported; ARM64 port in development)

### Build Instructions

```bash
# Clone the repository
git clone https://github.com/ReasonableExcuses/aether.git
cd aether

# Configure and build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Linux or macOS with multiple cores:
```bash
cmake --build build -j$(nproc)
```

The build produces:
- `build/Release/aether` (or `aether.exe` on Windows): The standalone CLI executable.
- `build/Release/aether_core.lib` (or `.a`): The static engine library.
- Test executables under `build/Release/`.

---

## Usage

### Command-Line Interface

```bash
# Run a JavaScript file
./build/Release/aether script.js

# Evaluate code directly
./build/Release/aether -e "console.log('Hello from Aether!');"

# Display engine version
./build/Release/aether -v
```

---

## Testing

Aether includes a test suite covering each subsystem and regression scenarios:

```bash
# Run the architecture regression and memory safety suite
./build/Release/test_issue_regressions

# Run subsystem test suites
./build/Release/test_milestone19_wasm
./build/Release/test_milestone20_parallel
./build/Release/test_milestone21_advanced_intrinsics
./build/Release/test_milestone11_typed_arrays

# Run Test262 test harness
./build/Release/test_test262_harness
```

---

## Project Structure

```
.
├── CMakeLists.txt                # Build configuration
├── LICENSE                       # Apache 2.0 license
├── README.md                     # Project overview and architecture summary
├── progress.md                   # Detailed milestone tracking, verification & roadmap
├── include/aether/               # Public headers
│   ├── advanced_intrinsics.h     # Weak collections, SharedArrayBuffer, Atomics
│   ├── background_compiler.h     # Background Sea-of-Nodes worker pool
│   ├── baseline_jit.h            # Tier 1 Baseline Template JIT
│   ├── bytecode.h                # Bytecode ISA and container
│   ├── concurrent_marker.h       # Concurrent Immix GC marker
│   ├── event_loop.h              # Async event loop and timers
│   ├── gc.h                      # Cheney Scavenger and Immix GC
│   ├── heap.h                    # IsolateHeap and HandleScope
│   ├── heap_object.h             # Heap object models (JSObject, JSFunction, etc.)
│   ├── interpreter.h             # Tier 0 Bytecode interpreter
│   ├── optimizing_jit.h          # Tier 2 Sea-of-Nodes optimizing JIT
│   ├── runtime.h                 # Agent, Realm, and runtime intrinsics
│   ├── typed_array.h             # ArrayBuffer, DataView, and TypedArray views
│   ├── value.h                   # 64-bit NaN-boxed TaggedValue
│   └── wasm.h                    # WebAssembly MVP engine and validator
├── src/                          # Engine implementation
├── test/test262/                 # Test262 conformance test suite assets
├── tests/                        # Subsystem and regression test suites
└── tools/                        # Developer utilities and test scripts
```

---

## License

Aether is open-source software licensed under the [Apache License, Version 2.0](LICENSE).
