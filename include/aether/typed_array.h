#ifndef AETHER_TYPED_ARRAY_H
#define AETHER_TYPED_ARRAY_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string_view>
#include <vector>

namespace aether {

/**
 * @brief Managed ArrayBuffer representing a fixed-length raw binary data buffer.
 */
class JSArrayBuffer : public JSObject {
public:
    explicit JSArrayBuffer(size_t byte_length, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSArrayBuffer, shape),
          byte_length_(byte_length),
          is_detached_(false),
          owns_data_(true),
          is_shared_(false),
          max_byte_length_(byte_length),
          is_growable_(false) {
        if (byte_length_ > 0) {
            data_ = new uint8_t[byte_length_]();
        } else {
            data_ = nullptr;
        }
    }

    JSArrayBuffer(size_t byte_length, size_t max_byte_length, bool is_shared, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSArrayBuffer, shape),
          byte_length_(byte_length),
          is_detached_(false),
          owns_data_(true),
          is_shared_(is_shared),
          max_byte_length_(std::max(byte_length, max_byte_length)),
          is_growable_(max_byte_length > 0 && max_byte_length >= byte_length) {
        size_t alloc_size = is_growable_ ? max_byte_length_ : byte_length_;
        if (alloc_size > 0) {
            data_ = new uint8_t[alloc_size]();
        } else {
            data_ = nullptr;
        }
    }

    JSArrayBuffer(uint8_t* external_data, size_t byte_length, bool owns = false, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSArrayBuffer, shape),
          data_(external_data),
          byte_length_(byte_length),
          is_detached_(false),
          owns_data_(owns),
          is_shared_(false),
          max_byte_length_(byte_length),
          is_growable_(false) {}

    JSArrayBuffer(JSArrayBuffer&& other) noexcept
        : JSObject(std::move(other)),
          data_(other.data_),
          byte_length_(other.byte_length_),
          is_detached_(other.is_detached_),
          owns_data_(other.owns_data_),
          is_shared_(other.is_shared_),
          max_byte_length_(other.max_byte_length_),
          is_growable_(other.is_growable_) {
        other.data_ = nullptr;
        other.byte_length_ = 0;
        other.owns_data_ = false;
        other.is_shared_ = false;
        other.max_byte_length_ = 0;
        other.is_growable_ = false;
    }

    ~JSArrayBuffer() override {
        if (owns_data_ && data_ != nullptr) {
            delete[] data_;
            data_ = nullptr;
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSArrayBuffer(std::move(*this));
    }

    size_t ObjectSize() const override { return sizeof(JSArrayBuffer); }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
    }

    uint8_t* Data() const noexcept { return is_detached_ ? nullptr : data_; }
    size_t ByteLength() const noexcept { return is_detached_ ? 0 : byte_length_; }
    bool IsDetached() const noexcept { return is_detached_; }
    bool IsShared() const noexcept { return is_shared_; }
    void SetShared(bool shared) noexcept { is_shared_ = shared; }
    bool IsGrowable() const noexcept { return is_growable_; }
    size_t MaxByteLength() const noexcept { return is_growable_ ? max_byte_length_ : byte_length_; }

    bool Grow(size_t new_byte_length) noexcept {
        if (!is_growable_ || is_detached_) return false;
        if (new_byte_length < byte_length_ || new_byte_length > max_byte_length_) return false;
        if (data_ && new_byte_length > byte_length_) {
            std::memset(data_ + byte_length_, 0, new_byte_length - byte_length_);
        }
        byte_length_ = new_byte_length;
        return true;
    }

    void Detach() noexcept {
        if (is_shared_) return; // SharedArrayBuffer cannot be detached per ECMAScript specification
        if (!is_detached_) {
            is_detached_ = true;
            if (owns_data_ && data_ != nullptr) {
                delete[] data_;
                data_ = nullptr;
            }
            byte_length_ = 0;
        }
    }

    JSArrayBuffer* Slice(int64_t begin, int64_t end, Shape* shape = nullptr) const {
        if (is_detached_) return new JSArrayBuffer(0, shape);

        int64_t len = static_cast<int64_t>(byte_length_);
        int64_t relative_begin = begin < 0 ? std::max<int64_t>(len + begin, 0) : std::min<int64_t>(begin, len);
        int64_t relative_end = end < 0 ? std::max<int64_t>(len + end, 0) : std::min<int64_t>(end, len);
        int64_t new_len = std::max<int64_t>(relative_end - relative_begin, 0);

        JSArrayBuffer* copy = nullptr;
        if (is_shared_) {
            copy = new JSArrayBuffer(static_cast<size_t>(new_len), static_cast<size_t>(new_len), true, shape);
        } else {
            copy = new JSArrayBuffer(static_cast<size_t>(new_len), shape);
        }
        if (new_len > 0 && data_ != nullptr) {
            std::memcpy(copy->Data(), data_ + relative_begin, static_cast<size_t>(new_len));
        }
        return copy;
    }

private:
    uint8_t* data_{nullptr};
    size_t byte_length_{0};
    bool is_detached_{false};
    bool owns_data_{true};
    bool is_shared_{false};
    size_t max_byte_length_{0};
    bool is_growable_{false};
};

/**
 * @brief Element types for JavaScript TypedArrays.
 */
enum class TypedArrayKind : uint8_t {
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,
    BigInt64,
    BigUint64
};

inline size_t TypedArrayElementSize(TypedArrayKind kind) noexcept {
    switch (kind) {
        case TypedArrayKind::Int8:
        case TypedArrayKind::Uint8:
        case TypedArrayKind::Uint8Clamped:
            return 1;
        case TypedArrayKind::Int16:
        case TypedArrayKind::Uint16:
            return 2;
        case TypedArrayKind::Int32:
        case TypedArrayKind::Uint32:
        case TypedArrayKind::Float32:
            return 4;
        case TypedArrayKind::Float64:
        case TypedArrayKind::BigInt64:
        case TypedArrayKind::BigUint64:
            return 8;
    }
    return 1;
}

inline std::string_view TypedArrayKindName(TypedArrayKind kind) noexcept {
    switch (kind) {
        case TypedArrayKind::Int8:         return "Int8Array";
        case TypedArrayKind::Uint8:        return "Uint8Array";
        case TypedArrayKind::Uint8Clamped: return "Uint8ClampedArray";
        case TypedArrayKind::Int16:        return "Int16Array";
        case TypedArrayKind::Uint16:       return "Uint16Array";
        case TypedArrayKind::Int32:        return "Int32Array";
        case TypedArrayKind::Uint32:       return "Uint32Array";
        case TypedArrayKind::Float32:      return "Float32Array";
        case TypedArrayKind::Float64:      return "Float64Array";
        case TypedArrayKind::BigInt64:     return "BigInt64Array";
        case TypedArrayKind::BigUint64:    return "BigUint64Array";
    }
    return "TypedArray";
}

/**
 * @brief Managed TypedArray providing an array-like typed view of an underlying ArrayBuffer.
 */
class JSTypedArray : public JSObject {
public:
    JSTypedArray(TypedArrayKind kind, JSArrayBuffer* buffer, size_t byte_offset, size_t length, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSTypedArray, shape),
          buffer_(buffer),
          byte_offset_(byte_offset),
          byte_length_(length * TypedArrayElementSize(kind)),
          length_(length),
          kind_(kind) {}

    JSTypedArray(JSTypedArray&& other) noexcept
        : JSObject(std::move(other)),
          buffer_(other.buffer_),
          byte_offset_(other.byte_offset_),
          byte_length_(other.byte_length_),
          length_(other.length_),
          kind_(other.kind_) {
        other.buffer_ = nullptr;
        other.length_ = 0;
        other.byte_length_ = 0;
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSTypedArray(std::move(*this));
    }

    size_t ObjectSize() const override { return sizeof(JSTypedArray); }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (buffer_ != nullptr) {
            TaggedValue buf_val = TaggedValue::FromHeapObject(buffer_);
            visitor(buf_val);
            if (buf_val.IsHeapObject()) {
                buffer_ = buf_val.As<JSArrayBuffer>();
            }
        }
    }

    JSArrayBuffer* Buffer() const noexcept { return buffer_; }
    size_t ByteOffset() const noexcept { return byte_offset_; }
    size_t ByteLength() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? byte_length_ : 0; }
    size_t Length() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? length_ : 0; }
    TypedArrayKind Kind() const noexcept { return kind_; }
    size_t BytesPerElement() const noexcept { return TypedArrayElementSize(kind_); }

    uint8_t* Data() const noexcept {
        if (!buffer_ || buffer_->IsDetached()) return nullptr;
        uint8_t* raw = buffer_->Data();
        return raw ? raw + byte_offset_ : nullptr;
    }

    TaggedValue GetElement(size_t index) const {
        if (index >= length_ || !buffer_ || buffer_->IsDetached()) {
            return TaggedValue::Undefined();
        }
        uint8_t* ptr = Data();
        if (!ptr) return TaggedValue::Undefined();

        switch (kind_) {
            case TypedArrayKind::Int8: {
                int8_t v = *reinterpret_cast<const int8_t*>(ptr + index);
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Uint8:
            case TypedArrayKind::Uint8Clamped: {
                uint8_t v = *(ptr + index);
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Int16: {
                int16_t v = *reinterpret_cast<const int16_t*>(ptr + index * 2);
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Uint16: {
                uint16_t v = *reinterpret_cast<const uint16_t*>(ptr + index * 2);
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Int32: {
                int32_t v = *reinterpret_cast<const int32_t*>(ptr + index * 4);
                return TaggedValue::FromSmi(v);
            }
            case TypedArrayKind::Uint32: {
                uint32_t v = *reinterpret_cast<const uint32_t*>(ptr + index * 4);
                if (v <= 0x3FFFFFFF) {
                    return TaggedValue::FromSmi(static_cast<int32_t>(v));
                }
                return TaggedValue::FromDouble(static_cast<double>(v));
            }
            case TypedArrayKind::Float32: {
                float v = *reinterpret_cast<const float*>(ptr + index * 4);
                return TaggedValue::FromDouble(static_cast<double>(v));
            }
            case TypedArrayKind::Float64: {
                double v = *reinterpret_cast<const double*>(ptr + index * 8);
                return TaggedValue::FromDouble(v);
            }
            default:
                return TaggedValue::Undefined();
        }
    }

    void SetElement(size_t index, TaggedValue value) {
        if (index >= length_ || !buffer_ || buffer_->IsDetached()) {
            return;
        }
        uint8_t* ptr = Data();
        if (!ptr) return;

        double num = 0.0;
        if (value.IsSmi()) num = static_cast<double>(value.AsSmi());
        else if (value.IsDouble()) num = value.AsDouble();
        else if (value.IsBoolean()) num = value.AsBoolean() ? 1.0 : 0.0;
        else if (value.IsUndefined() || value.IsNull()) num = 0.0;

        switch (kind_) {
            case TypedArrayKind::Int8: {
                int32_t i = static_cast<int32_t>(num);
                *reinterpret_cast<int8_t*>(ptr + index) = static_cast<int8_t>(i);
                break;
            }
            case TypedArrayKind::Uint8: {
                uint32_t u = static_cast<uint32_t>(num);
                *(ptr + index) = static_cast<uint8_t>(u);
                break;
            }
            case TypedArrayKind::Uint8Clamped: {
                if (std::isnan(num) || num <= 0.0) {
                    *(ptr + index) = 0;
                } else if (num >= 255.0) {
                    *(ptr + index) = 255;
                } else {
                    double f = std::floor(num);
                    double diff = num - f;
                    if (diff > 0.5) {
                        *(ptr + index) = static_cast<uint8_t>(f + 1);
                    } else if (diff < 0.5) {
                        *(ptr + index) = static_cast<uint8_t>(f);
                    } else {
                        // Round half to even
                        uint8_t i = static_cast<uint8_t>(f);
                        *(ptr + index) = (i % 2 == 0) ? i : static_cast<uint8_t>(i + 1);
                    }
                }
                break;
            }
            case TypedArrayKind::Int16: {
                int32_t i = static_cast<int32_t>(num);
                *reinterpret_cast<int16_t*>(ptr + index * 2) = static_cast<int16_t>(i);
                break;
            }
            case TypedArrayKind::Uint16: {
                uint32_t u = static_cast<uint32_t>(num);
                *reinterpret_cast<uint16_t*>(ptr + index * 2) = static_cast<uint16_t>(u);
                break;
            }
            case TypedArrayKind::Int32: {
                *reinterpret_cast<int32_t*>(ptr + index * 4) = static_cast<int32_t>(num);
                break;
            }
            case TypedArrayKind::Uint32: {
                *reinterpret_cast<uint32_t*>(ptr + index * 4) = static_cast<uint32_t>(num);
                break;
            }
            case TypedArrayKind::Float32: {
                *reinterpret_cast<float*>(ptr + index * 4) = static_cast<float>(num);
                break;
            }
            case TypedArrayKind::Float64: {
                *reinterpret_cast<double*>(ptr + index * 8) = num;
                break;
            }
            default:
                break;
        }
    }

    JSTypedArray* Subarray(int64_t begin, int64_t end, Shape* shape = nullptr) const {
        if (!buffer_ || buffer_->IsDetached()) {
            return new JSTypedArray(kind_, buffer_, 0, 0, shape);
        }
        int64_t len = static_cast<int64_t>(length_);
        int64_t rel_begin = begin < 0 ? std::max<int64_t>(len + begin, 0) : std::min<int64_t>(begin, len);
        int64_t rel_end = end < 0 ? std::max<int64_t>(len + end, 0) : std::min<int64_t>(end, len);
        size_t new_len = static_cast<size_t>(std::max<int64_t>(rel_end - rel_begin, 0));
        size_t new_byte_offset = byte_offset_ + (rel_begin * TypedArrayElementSize(kind_));

        return new JSTypedArray(kind_, buffer_, new_byte_offset, new_len, shape);
    }

private:
    JSArrayBuffer* buffer_{nullptr};
    size_t byte_offset_{0};
    size_t byte_length_{0};
    size_t length_{0};
    TypedArrayKind kind_{TypedArrayKind::Uint8};
};

/**
 * @brief Managed DataView providing low-level, endian-aware read/write operations on an ArrayBuffer.
 */
class JSDataView : public JSObject {
public:
    JSDataView(JSArrayBuffer* buffer, size_t byte_offset, size_t byte_length, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSDataView, shape),
          buffer_(buffer),
          byte_offset_(byte_offset),
          byte_length_(byte_length) {}

    JSDataView(JSDataView&& other) noexcept
        : JSObject(std::move(other)),
          buffer_(other.buffer_),
          byte_offset_(other.byte_offset_),
          byte_length_(other.byte_length_) {
        other.buffer_ = nullptr;
        other.byte_length_ = 0;
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSDataView(std::move(*this));
    }

    size_t ObjectSize() const override { return sizeof(JSDataView); }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (buffer_ != nullptr) {
            TaggedValue buf_val = TaggedValue::FromHeapObject(buffer_);
            visitor(buf_val);
            if (buf_val.IsHeapObject()) {
                buffer_ = buf_val.As<JSArrayBuffer>();
            }
        }
    }

    JSArrayBuffer* Buffer() const noexcept { return buffer_; }
    size_t ByteOffset() const noexcept { return byte_offset_; }
    size_t ByteLength() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? byte_length_ : 0; }

    uint8_t* Data() const noexcept {
        if (!buffer_ || buffer_->IsDetached()) return nullptr;
        uint8_t* raw = buffer_->Data();
        return raw ? raw + byte_offset_ : nullptr;
    }

    bool CheckBounds(size_t offset, size_t size) const noexcept {
        return buffer_ && !buffer_->IsDetached() && (offset + size <= byte_length_);
    }

    // Int8 / Uint8 Accessors
    int8_t GetInt8(size_t byte_offset) const {
        if (!CheckBounds(byte_offset, 1)) return 0;
        return *reinterpret_cast<const int8_t*>(Data() + byte_offset);
    }
    void SetInt8(size_t byte_offset, int8_t value) {
        if (!CheckBounds(byte_offset, 1)) return;
        *reinterpret_cast<int8_t*>(Data() + byte_offset) = value;
    }

    uint8_t GetUint8(size_t byte_offset) const {
        if (!CheckBounds(byte_offset, 1)) return 0;
        return *(Data() + byte_offset);
    }
    void SetUint8(size_t byte_offset, uint8_t value) {
        if (!CheckBounds(byte_offset, 1)) return;
        *(Data() + byte_offset) = value;
    }

    // Int16 / Uint16 Accessors
    int16_t GetInt16(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 2)) return 0;
        uint16_t raw = Read16(Data() + byte_offset, little_endian);
        return static_cast<int16_t>(raw);
    }
    void SetInt16(size_t byte_offset, int16_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 2)) return;
        Write16(Data() + byte_offset, static_cast<uint16_t>(value), little_endian);
    }

    uint16_t GetUint16(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 2)) return 0;
        return Read16(Data() + byte_offset, little_endian);
    }
    void SetUint16(size_t byte_offset, uint16_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 2)) return;
        Write16(Data() + byte_offset, value, little_endian);
    }

    // Int32 / Uint32 Accessors
    int32_t GetInt32(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 4)) return 0;
        uint32_t raw = Read32(Data() + byte_offset, little_endian);
        return static_cast<int32_t>(raw);
    }
    void SetInt32(size_t byte_offset, int32_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 4)) return;
        Write32(Data() + byte_offset, static_cast<uint32_t>(value), little_endian);
    }

    uint32_t GetUint32(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 4)) return 0;
        return Read32(Data() + byte_offset, little_endian);
    }
    void SetUint32(size_t byte_offset, uint32_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 4)) return;
        Write32(Data() + byte_offset, value, little_endian);
    }

    // Float32 / Float64 Accessors
    float GetFloat32(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 4)) return 0.0f;
        uint32_t raw = Read32(Data() + byte_offset, little_endian);
        float result;
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }
    void SetFloat32(size_t byte_offset, float value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 4)) return;
        uint32_t raw;
        std::memcpy(&raw, &value, sizeof(raw));
        Write32(Data() + byte_offset, raw, little_endian);
    }

    double GetFloat64(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 8)) return 0.0;
        uint64_t raw = Read64(Data() + byte_offset, little_endian);
        double result;
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }
    void SetFloat64(size_t byte_offset, double value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 8)) return;
        uint64_t raw;
        std::memcpy(&raw, &value, sizeof(raw));
        Write64(Data() + byte_offset, raw, little_endian);
    }

private:
    static uint16_t Read16(const uint8_t* p, bool le) noexcept {
        if (le) return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    }
    static void Write16(uint8_t* p, uint16_t v, bool le) noexcept {
        if (le) { p[0] = static_cast<uint8_t>(v & 0xFF); p[1] = static_cast<uint8_t>(v >> 8); }
        else    { p[0] = static_cast<uint8_t>(v >> 8);   p[1] = static_cast<uint8_t>(v & 0xFF); }
    }

    static uint32_t Read32(const uint8_t* p, bool le) noexcept {
        if (le) {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }
    static void Write32(uint8_t* p, uint32_t v, bool le) noexcept {
        if (le) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        } else {
            p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[3] = static_cast<uint8_t>(v & 0xFF);
        }
    }

    static uint64_t Read64(const uint8_t* p, bool le) noexcept {
        if (le) {
            uint64_t lo = Read32(p, true);
            uint64_t hi = Read32(p + 4, true);
            return lo | (hi << 32);
        }
        uint64_t hi = Read32(p, false);
        uint64_t lo = Read32(p + 4, false);
        return (hi << 32) | lo;
    }
    static void Write64(uint8_t* p, uint64_t v, bool le) noexcept {
        if (le) {
            Write32(p, static_cast<uint32_t>(v & 0xFFFFFFFF), true);
            Write32(p + 4, static_cast<uint32_t>(v >> 32), true);
        } else {
            Write32(p, static_cast<uint32_t>(v >> 32), false);
            Write32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFF), false);
        }
    }

    JSArrayBuffer* buffer_{nullptr};
    size_t byte_offset_{0};
    size_t byte_length_{0};
};

} // namespace aether

#endif // AETHER_TYPED_ARRAY_H
