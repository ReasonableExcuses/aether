#ifndef AETHER_COMMON_H
#define AETHER_COMMON_H

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <type_traits>
#include <concepts>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include <iostream>
#include <format>
#include <limits>
#include <stdexcept>

namespace aether {

inline size_t CheckedStorageBytes(size_t count, size_t element_size,
                                  const char* what = "storage size overflow") {
    if (element_size != 0 && count > std::numeric_limits<size_t>::max() / element_size) {
        throw std::length_error(what);
    }
    return count * element_size;
}

// Dedicated architectural limits and alignment constants
inline constexpr size_t kPointerSize = sizeof(void*);
inline constexpr size_t kCompressedPointerSize = sizeof(uint32_t);
inline constexpr size_t kObjectAlignment = 8;
inline constexpr size_t kHeapReservationSize = 4ULL * 1024ULL * 1024ULL * 1024ULL; // 4GB Heap Window

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
#define AETHER_LIKELY(x) __builtin_expect(!!(x), 1)
#define AETHER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define AETHER_ALWAYS_INLINE [[gnu::always_inline]] inline
#define AETHER_NOINLINE __attribute__((noinline))
#define AETHER_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address, no_sanitize("thread", "undefined")))
#elif defined(_MSC_VER)
#define AETHER_LIKELY(x) (x)
#define AETHER_UNLIKELY(x) (x)
#define AETHER_ALWAYS_INLINE __forceinline
#define AETHER_NOINLINE __declspec(noinline)
#if defined(__SANITIZE_ADDRESS__)
#define AETHER_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
#define AETHER_NO_SANITIZE_ADDRESS
#endif
#else
#define AETHER_LIKELY(x) (x)
#define AETHER_UNLIKELY(x) (x)
#define AETHER_ALWAYS_INLINE inline
#define AETHER_NOINLINE
#define AETHER_NO_SANITIZE_ADDRESS
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_AddressOfReturnAddress)
#endif

AETHER_ALWAYS_INLINE void* GetCurrentStackPointer() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_frame_address(0);
#elif defined(_MSC_VER)
    return _AddressOfReturnAddress();
#else
    volatile uintptr_t marker = 0;
    return const_cast<uintptr_t*>(&marker);
#endif
}

// Compiler tail-call optimization attribute for direct-threaded dispatch
#if defined(__clang__)
#define AETHER_MUSTTAIL [[clang::musttail]]
#else
#define AETHER_MUSTTAIL
#endif

// ── Enterprise Safety Limits ──────────────────────────────────────────────────
// These constants define hard upper bounds enforced throughout the VM to prevent
// denial-of-service, out-of-memory crashes, and runaway resource consumption.

/// Maximum byte length for a single JSString (256 MB).
inline constexpr size_t kMaxStringLength          = 256ULL * 1024ULL * 1024ULL;

inline size_t CheckedStringLength(size_t length) {
    if (length > kMaxStringLength) {
        throw std::length_error("String exceeds the maximum length");
    }
    return length;
}

/// Maximum element count for a JSArray (2^28 ≈ 268M elements).
inline constexpr size_t kMaxArrayLength            = 1ULL << 28;

/// Maximum number of named properties on a single JSObject (2^20 ≈ 1M).
inline constexpr size_t kMaxObjectProperties       = 1ULL << 20;

/// Maximum byte length for a single ArrayBuffer (1 GB).
inline constexpr size_t kMaxArrayBufferSize        = 1ULL * 1024ULL * 1024ULL * 1024ULL;

/// Default per-isolate heap budget before MajorGC pressure triggers (512 MB).
inline constexpr size_t kDefaultHeapBudget         = 512ULL * 1024ULL * 1024ULL;

/// GC pressure threshold as a percentage (75%) — triggers collection when exceeded.
inline constexpr size_t kGCPressureThresholdPct    = 75;

/// Maximum prototype chain depth before cycle/runaway detection (256 hops).
inline constexpr size_t kMaxPrototypeChainDepth    = 256;

/// Maximum regex execution steps before aborting (10 million).
inline constexpr size_t kMaxRegexSteps             = 10'000'000;

/// Poison byte pattern written to freed nursery from-space for UAF detection.
inline constexpr uint8_t kNurseryPoisonByte        = 0xDD;

/// Poison byte pattern for freed old-gen Immix lines.
inline constexpr uint8_t kOldGenPoisonByte         = 0xCD;

// Forward declarations
class HeapObject;
class JSObject;
class Shape;
class ExecutionFrame;
class BytecodeArray;

} // namespace aether

#endif // AETHER_COMMON_H
