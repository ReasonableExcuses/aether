# Decision 017: GC-Managed Strings, UTF-16 Conformance, Ropes, and Interning

## Context
Under `AETHER_AGENT_PLAN.md` Phase 2 (T2.2: Strings; Decision D4) and Phase 6 (T6.1: UTF-16 Conformance), `JSString` previously relied on out-of-band `std::string data_` and `MemoryCharge external_storage_`:
- `sizeof(JSString)` was 80 bytes.
- Non-trivial destructor (`~JSString()`) was required to destroy `data_` and release external memory charges.
- String allocations bypassed the GC managed heap, fragmenting the system allocator and preventing Immix line-sweep reclamation.
- String concatenation was eager and quadratic ($O(n^2)$), causing `str.js 80000` to take 13.5 seconds.
- Strings used UTF-8 byte indexing rather than ECMA-262 UTF-16 code unit semantics:
  - `"😀".length` returned 4 (bytes) instead of 2 (UTF-16 code units).
  - `"😀".charCodeAt(0)` returned 240 instead of 55357 (0xD83D).
  - `"é".length` returned 2 instead of 1.
  - Slicing multi-byte characters split UTF-8 sequences and corrupted code points.
- Property access and identifier references repeatedly allocated duplicate string objects in constant pools during bytecode emission.

## Decision

1. **Header Diet & Trivial Destructor**:
   - Flattened `JSString` header to **56 bytes** (saving 24 bytes per string header, a 30% reduction):
     - Base `HeapObject`: 24 bytes
     - `length_`: 4 bytes (length in 16-bit code units)
     - `hash_`: 4 bytes (cached FNV-1a hash)
     - `kind_`: 1 byte (`FlatLatin1`, `FlatUtf16`, `Cons`, `Sliced`)
     - `str_flags_`: 1 byte (`kFlagIsLatin1 = 1`)
     - `depth_`: 1 byte (rope depth)
     - `reserved_`: 1 byte
     - `slice_offset_`: 4 bytes
     - Union (`payload_` / `left_` / `parent_`): 8 bytes
     - `right_`: 8 bytes
   - Achieved trivial destructor (`~JSString() override = default;`), eliminating deallocation overhead during GC sweeps and satisfying Decision D4.

2. **GC-Allocated ByteArrays for Flat Latin-1 and UTF-16**:
   - `FlatLatin1`: 8-bit characters (`uint8_t[]`) stored in GC-managed `ByteArray*` for strings with code points $\le 255$.
   - `FlatUtf16`: 16-bit code units (`uint16_t[]`) stored in GC-managed `ByteArray*` for strings containing non-Latin-1 code points.
   - Provides zero-copy `std::string_view View()` for Latin-1 strings.

3. **ECMA-262 UTF-16 Code Unit Conformance (T6.1)**:
   - All string indexing operations (`length`, `charCodeAt`, `charAt`, bracket indexing `s[i]`, `slice`, `substring`) operate strictly on 16-bit UTF-16 code units.
   - Surrogate pairs (e.g., `"😀"`) have `length === 2`, `charCodeAt(0) === 0xD83D` (55357), `charCodeAt(1) === 0xDE00` (56832).
   - Lone surrogates (e.g. `\uD83D`) are strictly preserved in WTF-8 transcoding.

4. **Cons Strings (Ropes) for $O(1)$ Concatenation**:
   - Binary string addition (`+`, `+=`, `String.prototype.concat`) allocates a lightweight `Cons` node linking `left_` and `right_` without eagerly copying buffers.
   - Rope depth is tracked via `depth_` and capped at `kMaxConsDepth = 32`. When depth reaches 32 or when continuous memory is required, lazy flattening is performed.
   - Flattening (`FlattenToLatin1`, `FlattenToUtf16`, `EnsureFlat`) is completely non-recursive using an explicit iteration stack, preventing C++ stack overflow on deep trees.
   - Concatenation of small strings ($\le 14$ characters) flattens eagerly to avoid rope overhead.

5. **Sliced Strings for $O(1)$ Substrings**:
   - `String.prototype.slice` and `String.prototype.substring` create `Sliced` strings pointing to the parent buffer with `slice_offset_`.
   - Nested slices fold offsets (`root_offset += parent->slice_offset_`), keeping slice depth strictly $\le 1$.
   - Cons parent strings are flattened before slicing, ensuring that slice lookups are always direct array accesses.
   - Small slices ($\le 8$ characters) allocate compact flat strings to avoid holding large parent buffers alive.

6. **Property Key Interning & Constant Deduplication**:
   - `BytecodeEmitter::InternString` interns property and identifier names within each compilation unit, reusing `JSString*` objects.
   - `BytecodeArray::AddConstant` deduplicates string constants via `s1->Equals(s2)`, consolidating identical property keys into single constant pool slots.

## Verification
- Verified compile-time layout: `sizeof(JSString) == 56`.
- Benchmark `bench/str.js 80000` dropped from **13,542 ms** to **134 ms** (101× speedup), surpassing the $< 0.3$s target.
- Added comprehensive unit tests in `tests/test_object_sizes.cpp` and `tests/test_issue_regressions.cpp`.
- 100% test pass rate across all 31 CTest suites.
- 100% test pass rate across all 9 matrix configurations in `tools/matrix.py`.

