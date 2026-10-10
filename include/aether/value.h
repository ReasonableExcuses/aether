#ifndef AETHER_VALUE_H
#define AETHER_VALUE_H

#include "aether/common.h"
#include <cstring>
#include <cmath>

namespace aether {

class HeapObject;
class JSObject;
class Shape;

/**
 * @brief 64-bit TaggedValue implementing IEEE-754 NaN-Boxing.
 *
 * Bitwise Architecture:
 * - Doubles: Standard IEEE-754 representation (NaN values canonicalized to 0x7FF8'0000'0000'0000).
 * - Non-Doubles: Encoded inside the Quiet NaN payload space [0x7FF9 .. 0x7FFF]:
 *   - Smi (Int32)    : 0x7FF9'0000'0000'0000 | (uint32_t)value
 *   - HeapObject*    : 0x7FFA'0000'0000'0000 | (uint48_t)pointer
 *   - Boolean        : 0x7FFB'0000'0000'0000 | (0 or 1)
 *   - Undefined      : 0x7FFC'0000'0000'0000
 *   - Null           : 0x7FFD'0000'0000'0000
 *   - Symbol         : 0x7FFE'0000'0000'0000 | (uint48_t)symbol_ptr
 *   - Empty Sentinel : 0x7FFF'0000'0000'0000 (Internal uninitialized / deleted marker)
 */
class TaggedValue {
public:
    static constexpr uint64_t kCanonicalNaN = 0x7FF8'0000'0000'0000ULL;
    static constexpr uint64_t kTagMask     = 0xFFFF'0000'0000'0000ULL;
    static constexpr uint64_t kPayloadMask = 0x0000'FFFF'FFFF'FFFFULL;

    static constexpr uint64_t kTagBase       = 0x7FF9'0000'0000'0000ULL;
    static constexpr uint64_t kTagRange      = 0x0007'0000'0000'0000ULL;

    static constexpr uint64_t kTagSmi        = 0x7FF9'0000'0000'0000ULL;
    static constexpr uint64_t kTagHeapObject = 0x7FFA'0000'0000'0000ULL;
    static constexpr uint64_t kTagBoolean    = 0x7FFB'0000'0000'0000ULL;
    static constexpr uint64_t kTagUndefined  = 0x7FFC'0000'0000'0000ULL;
    static constexpr uint64_t kTagNull       = 0x7FFD'0000'0000'0000ULL;
    static constexpr uint64_t kTagSymbol     = 0x7FFE'0000'0000'0000ULL;
    static constexpr uint64_t kTagEmpty      = 0x7FFF'0000'0000'0000ULL;

    // Constructors
    constexpr TaggedValue() noexcept : raw_(kTagEmpty) {}
    explicit constexpr TaggedValue(uint64_t raw) noexcept : raw_(raw) {}

    // Factory methods
    AETHER_ALWAYS_INLINE static TaggedValue FromDouble(double val) noexcept {
        if (AETHER_UNLIKELY(std::isnan(val))) {
            return TaggedValue(kCanonicalNaN);
        }
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(bits));
        return TaggedValue(bits);
    }

    AETHER_ALWAYS_INLINE static TaggedValue FromSmi(int32_t val) noexcept {
        return TaggedValue(kTagSmi | static_cast<uint32_t>(val));
    }

    AETHER_ALWAYS_INLINE static TaggedValue FromHeapObject(const HeapObject* ptr) noexcept {
        uint64_t addr = reinterpret_cast<uintptr_t>(ptr);
        assert((addr & kTagMask) == 0 && "Pointer must fit in 48-bit address space");
        return TaggedValue(kTagHeapObject | (addr & kPayloadMask));
    }

    AETHER_ALWAYS_INLINE static TaggedValue FromBoolean(bool val) noexcept {
        return TaggedValue(kTagBoolean | (val ? 1ULL : 0ULL));
    }

    AETHER_ALWAYS_INLINE static TaggedValue Undefined() noexcept {
        return TaggedValue(kTagUndefined);
    }

    AETHER_ALWAYS_INLINE static TaggedValue Null() noexcept {
        return TaggedValue(kTagNull);
    }

    AETHER_ALWAYS_INLINE static TaggedValue True() noexcept {
        return TaggedValue(kTagBoolean | 1ULL);
    }

    AETHER_ALWAYS_INLINE static TaggedValue False() noexcept {
        return TaggedValue(kTagBoolean | 0ULL);
    }

    AETHER_ALWAYS_INLINE static TaggedValue Empty() noexcept {
        return TaggedValue(kTagEmpty);
    }

    // Type queries
    AETHER_ALWAYS_INLINE bool IsDouble() const noexcept {
        // Any value not within [kTagBase, kTagBase + kTagRange) is an IEEE-754 double
        return (raw_ - kTagBase) >= kTagRange;
    }

    AETHER_ALWAYS_INLINE bool IsNumber() const noexcept {
        return IsDouble() || IsSmi();
    }

    AETHER_ALWAYS_INLINE bool IsSmi() const noexcept {
        return (raw_ & kTagMask) == kTagSmi;
    }

    AETHER_ALWAYS_INLINE bool IsHeapObject() const noexcept {
        return (raw_ & kTagMask) == kTagHeapObject;
    }

    AETHER_ALWAYS_INLINE bool IsBoolean() const noexcept {
        return (raw_ & kTagMask) == kTagBoolean;
    }

    AETHER_ALWAYS_INLINE bool IsUndefined() const noexcept {
        return raw_ == kTagUndefined;
    }

    AETHER_ALWAYS_INLINE bool IsNull() const noexcept {
        return raw_ == kTagNull;
    }

    AETHER_ALWAYS_INLINE bool IsNullOrUndefined() const noexcept {
        return IsNull() || IsUndefined();
    }

    AETHER_ALWAYS_INLINE bool IsEmpty() const noexcept {
        return raw_ == kTagEmpty;
    }

    // Value extractors
    AETHER_ALWAYS_INLINE double AsDouble() const noexcept {
        assert(IsDouble() && "Value is not a double");
        double val;
        std::memcpy(&val, &raw_, sizeof(val));
        return val;
    }

    AETHER_ALWAYS_INLINE int32_t AsSmi() const noexcept {
        assert(IsSmi() && "Value is not a Smi");
        return static_cast<int32_t>(raw_);
    }

    AETHER_ALWAYS_INLINE double AsNumber() const noexcept {
        assert(IsNumber() && "Value is not a number");
        return IsSmi() ? static_cast<double>(AsSmi()) : AsDouble();
    }

    AETHER_ALWAYS_INLINE HeapObject* AsHeapObject() const noexcept {
        assert(IsHeapObject() && "Value is not a HeapObject");
        return reinterpret_cast<HeapObject*>(static_cast<uintptr_t>(raw_ & kPayloadMask));
    }

    template <typename T>
    AETHER_ALWAYS_INLINE T* As() const noexcept {
        return reinterpret_cast<T*>(AsHeapObject());
    }

    AETHER_ALWAYS_INLINE bool AsBoolean() const noexcept {
        assert(IsBoolean() && "Value is not a boolean");
        return (raw_ & 1ULL) != 0;
    }

    AETHER_ALWAYS_INLINE uint64_t RawBits() const noexcept {
        return raw_;
    }

    // ECMAScript ToBoolean conversion
    bool ToBoolean() const noexcept;

    // Diagnostic stringification
    std::string ToString() const;

    // Comparison operators
    constexpr bool operator==(const TaggedValue& other) const noexcept {
        return raw_ == other.raw_;
    }

    constexpr bool operator!=(const TaggedValue& other) const noexcept {
        return raw_ != other.raw_;
    }

private:
    uint64_t raw_;
};

static_assert(sizeof(TaggedValue) == 8, "TaggedValue must be exactly 8 bytes (64 bits)");
static_assert(std::is_trivially_copyable_v<TaggedValue>, "TaggedValue must be trivially copyable");

} // namespace aether

#endif // AETHER_VALUE_H
