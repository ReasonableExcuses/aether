# Decision 001: Portable CMakeLists.txt and CTest Integration

- **Date:** 2026-10-28
- **Context:** `CMakeLists.txt` contained hardcoded MSVC flags (`/Zi`, `/DEBUG`), duplicated 32 individual executable targets, did not enable CTest, did not define sanitization or debug default configurations, and broke on non-MSVC environments (GCC/Clang on Linux).
- **Options Considered:**
  1. Maintain separate build scripts for Linux and Windows.
  2. Unify `CMakeLists.txt` with conditional compiler flags, dynamic test enumeration via `file(GLOB ...)`, `Threads::Threads` linkage, and `enable_testing()`.
- **Decision:** Adopt Option 2. Wrap all MSVC-specific options in `if(MSVC)`. Default `CMAKE_BUILD_TYPE=RelWithDebInfo`. Provide options `AETHER_SANITIZE`, `AETHER_TSAN`, `AETHER_GC_STRESS_DEFAULT`. Automate test targets and register with `add_test` for `ctest`.
- **Consequences:** Builds seamlessly across Windows MSVC and Linux/macOS Clang/GCC with automated `ctest` execution.
