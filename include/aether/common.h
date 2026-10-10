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

namespace aether {

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
#elif defined(_MSC_VER)
#define AETHER_LIKELY(x) (x)
#define AETHER_UNLIKELY(x) (x)
#define AETHER_ALWAYS_INLINE __forceinline
#else
#define AETHER_LIKELY(x) (x)
#define AETHER_UNLIKELY(x) (x)
#define AETHER_ALWAYS_INLINE inline
#endif

// Compiler tail-call optimization attribute for direct-threaded dispatch
#if defined(__clang__)
#define AETHER_MUSTTAIL [[clang::musttail]]
#else
#define AETHER_MUSTTAIL
#endif

// Forward declarations
class HeapObject;
class JSObject;
class Shape;
class ExecutionFrame;
class BytecodeArray;

} // namespace aether

#endif // AETHER_COMMON_H
