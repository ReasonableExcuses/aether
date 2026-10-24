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
#include <limits>
#include <stdexcept>

namespace aether {

/**
 * @brief Managed ArrayBuffer representing a fixed-length raw binary data buffer.
 */
class JSArrayBuffer : public JSObject {
public:
    explicit JSArrayBuffer(size_t byte_length, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSArrayBuffer, shape),
          storage_(CheckedBufferLength(byte_length), true),
          data_(nullptr),
          byte_length_(CheckedBufferLength(byte_length)),
          is_detached_(false),
          owns_data_(true),
          is_shared_(false),
          max_byte_length_(CheckedBufferLength(byte_length)),
          is_growable_(false) {
        if (byte_length_ > 0) {
            data_ = new uint8_t[byte_length_]();
        } else {
            data_ = nullptr;
        }
    }

    JSArrayBuffer(size_t byte_length, size_t max_byte_length, bool is_shared, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSArrayBuffer, shape),
          storage_(CheckedBufferAllocationSize(byte_length, max_byte_length), true),
          data_(nullptr),
          byte_length_(CheckedBufferLength(byte_length)),
          is_detached_(false),
          owns_data_(true),
          is_shared_(is_shared),
          max_byte_length_(CheckedMaximumLength(byte_length, max_byte_length)),
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
          storage_(CheckedExternalBuffer(external_data, byte_length), true),
          data_(external_data),
          byte_length_(CheckedBufferLength(byte_length)),
          is_detached_(false),
          owns_data_(owns),
          is_shared_(false),
          max_byte_length_(CheckedBufferLength(byte_length)),
          is_growable_(false) {}

    JSArrayBuffer(JSArrayBuffer&& other) noexcept
        : JSObject(std::move(other)),
          storage_(std::move(other.storage_)),
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
    size_t MaxByteLength() const noexcept { return is_detached_ ? 0 : (is_growable_ ? max_byte_length_ : byte_length_); }

    bool Grow(size_t new_byte_length) noexcept {
        if (!is_growable_ || is_detached_) return false;
        if (new_byte_length < byte_length_ || new_byte_length > max_byte_length_) return false;
        if (data_ && new_byte_length > byte_length_) {
            std::memset(data_ + byte_length_, 0, new_byte_length - byte_length_);
        }
        byte_length_ = new_byte_length;
        return true;
    }

    void Detach() {
        if (is_shared_) return; // SharedArrayBuffer cannot be detached per ECMAScript specification
        if (!is_detached_) {
            storage_.Resize(0);
            is_detached_ = true;
            if (owns_data_ && data_ != nullptr) {
                delete[] data_;
            }
            data_ = nullptr;
            byte_length_ = 0;
        }
    }

    JSArrayBuffer* Slice(int64_t begin, int64_t end, Shape* shape = nullptr) const;

private:
    static size_t CheckedBufferLength(size_t byte_length) {
        if (byte_length > kMaxArrayBufferSize) {
            throw std::length_error("ArrayBuffer exceeds the maximum byte length");
        }
        return byte_length;
    }

    static size_t CheckedMaximumLength(size_t byte_length, size_t max_byte_length) {
        const size_t checked_byte_length = CheckedBufferLength(byte_length);
        if (max_byte_length < checked_byte_length) {
            throw std::length_error("ArrayBuffer maxByteLength is smaller than byteLength");
        }
        return CheckedBufferLength(max_byte_length);
    }

    static size_t CheckedBufferAllocationSize(size_t byte_length, size_t max_byte_length) {
        const size_t checked_byte_length = CheckedBufferLength(byte_length);
        const size_t checked_max = CheckedMaximumLength(checked_byte_length, max_byte_length);
        return (max_byte_length > 0 && checked_max >= checked_byte_length)
            ? checked_max : checked_byte_length;
    }

    static size_t CheckedExternalBuffer(uint8_t* external_data, size_t byte_length) {
        if (byte_length != 0 && external_data == nullptr) {
            throw std::length_error("external ArrayBuffer storage is null");
        }
        return CheckedBufferLength(byte_length);
    }

    static int64_t NormalizeIndex(int64_t index, int64_t length) noexcept {
        if (index < 0) {
            if (index < -length) return 0;
            return length + index;
        }
        return std::min(index, length);
    }

    MemoryCharge storage_;
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
          byte_offset_(CheckedTypedArrayOffset(kind, buffer, byte_offset, length)),
          byte_length_(CheckedStorageBytes(length, TypedArrayElementSize(kind),
                                           "TypedArray length overflow")),
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
    size_t ByteOffset() const noexcept { return buffer_ && !buffer_->IsDetached() ? byte_offset_ : 0; }
    size_t ByteLength() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? byte_length_ : 0; }
    size_t Length() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? length_ : 0; }
    TypedArrayKind Kind() const noexcept { return kind_; }
    size_t BytesPerElement() const noexcept { return TypedArrayElementSize(kind_); }

    uint8_t* Data() const noexcept {
        if (!buffer_ || buffer_->IsDetached()) return nullptr;
        uint8_t* raw = buffer_->Data();
        if (!raw || byte_offset_ > buffer_->ByteLength() ||
            byte_length_ > buffer_->ByteLength() - byte_offset_) {
            return nullptr;
        }
        return raw + byte_offset_;
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
                int16_t v;
                std::memcpy(&v, ptr + index * 2, sizeof(v));
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Uint16: {
                uint16_t v;
                std::memcpy(&v, ptr + index * 2, sizeof(v));
                return TaggedValue::FromSmi(static_cast<int32_t>(v));
            }
            case TypedArrayKind::Int32: {
                int32_t v;
                std::memcpy(&v, ptr + index * 4, sizeof(v));
                return TaggedValue::FromSmi(v);
            }
            case TypedArrayKind::Uint32: {
                uint32_t v;
                std::memcpy(&v, ptr + index * 4, sizeof(v));
                if (v <= 0x3FFFFFFF) {
                    return TaggedValue::FromSmi(static_cast<int32_t>(v));
                }
                return TaggedValue::FromDouble(static_cast<double>(v));
            }
            case TypedArrayKind::Float32: {
                float v;
                std::memcpy(&v, ptr + index * 4, sizeof(v));
                return TaggedValue::FromDouble(static_cast<double>(v));
            }
            case TypedArrayKind::Float64: {
                double v;
                std::memcpy(&v, ptr + index * 8, sizeof(v));
                return TaggedValue::FromDouble(v);
            }
            case TypedArrayKind::BigInt64: {
                int64_t v;
                std::memcpy(&v, ptr + index * 8, sizeof(v));
                return TaggedValue::FromHeapObject(JSBigInt::Create(v));
            }
            case TypedArrayKind::BigUint64: {
                uint64_t v;
                std::memcpy(&v, ptr + index * 8, sizeof(v));
                return TaggedValue::FromHeapObject(JSBigInt::Create(v));
            }
        }
        return TaggedValue::Undefined();
    }

    static int64_t TypedArrayToBigInt64(TaggedValue value) {
        if (value.IsHeapObject()) {
            HeapObject* h = value.AsHeapObject();
            if (h->IsBigInt()) {
                return static_cast<JSBigInt*>(h)->ToInt64();
            }
            if (h->IsString()) {
                JSBigInt bi(static_cast<JSString*>(h)->View());
                return bi.ToInt64();
            }
        }
        if (value.IsBoolean()) {
            return value.AsBoolean() ? 1LL : 0LL;
        }
        if (value.IsSmi()) {
            return static_cast<int64_t>(value.AsSmi());
        }
        if (value.IsDouble()) {
            double d = value.AsDouble();
            if (!std::isnan(d) && !std::isinf(d)) {
                return static_cast<int64_t>(std::trunc(d));
            }
        }
        return 0;
    }

    static uint64_t TypedArrayToBigUint64(TaggedValue value) {
        if (value.IsHeapObject()) {
            HeapObject* h = value.AsHeapObject();
            if (h->IsBigInt()) {
                return static_cast<JSBigInt*>(h)->ToUint64();
            }
            if (h->IsString()) {
                JSBigInt bi(static_cast<JSString*>(h)->View());
                return bi.ToUint64();
            }
        }
        if (value.IsBoolean()) {
            return value.AsBoolean() ? 1ULL : 0ULL;
        }
        if (value.IsSmi()) {
            return static_cast<uint64_t>(static_cast<int64_t>(value.AsSmi()));
        }
        if (value.IsDouble()) {
            double d = value.AsDouble();
            if (!std::isnan(d) && !std::isinf(d)) {
                return static_cast<uint64_t>(static_cast<int64_t>(std::trunc(d)));
            }
        }
        return 0;
    }

    // ECMAScript ToNumber conversion for typed array assignment
    static double TypedArrayToNumber(TaggedValue value) noexcept {
        if (value.IsSmi()) return static_cast<double>(value.AsSmi());
        if (value.IsDouble()) return value.AsDouble();
        if (value.IsBoolean()) return value.AsBoolean() ? 1.0 : 0.0;
        if (value.IsUndefined()) return std::numeric_limits<double>::quiet_NaN();
        if (value.IsNull()) return 0.0;
        if (value.IsHeapObject() && value.AsHeapObject()->IsString()) {
            auto sv = static_cast<JSString*>(value.AsHeapObject())->View();
            if (sv.empty()) return 0.0;
            try { return std::stod(std::string(sv)); } catch (...) {}
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // ECMAScript ToInt32 (wrapping modulo 2^32 then interpret as signed)
    static int32_t ToInt32(double num) noexcept {
        if (std::isnan(num) || std::isinf(num) || num == 0.0) return 0;
        // Use fmod to get modulo 2^32, then convert
        double d = std::fmod(std::trunc(num), 4294967296.0);
        if (d < 0) d += 4294967296.0;
        if (d >= 2147483648.0) d -= 4294967296.0;
        return static_cast<int32_t>(d);
    }

    // ECMAScript ToUint32 (wrapping modulo 2^32)
    static uint32_t ToUint32(double num) noexcept {
        if (std::isnan(num) || std::isinf(num) || num == 0.0) return 0;
        double d = std::fmod(std::trunc(num), 4294967296.0);
        if (d < 0) d += 4294967296.0;
        return static_cast<uint32_t>(d);
    }

    // ECMAScript ToUint16 (wrapping modulo 2^16)
    static uint16_t ToUint16(double num) noexcept {
        return static_cast<uint16_t>(ToUint32(num) & 0xFFFF);
    }

    void SetElement(size_t index, TaggedValue value) {
        if (index >= length_ || !buffer_ || buffer_->IsDetached()) {
            return;
        }
        uint8_t* ptr = Data();
        if (!ptr) return;

        double num = TypedArrayToNumber(value);

        switch (kind_) {
            case TypedArrayKind::Int8: {
                int8_t v = static_cast<int8_t>(ToInt32(num) & 0xFF);
                *(ptr + index) = static_cast<uint8_t>(v);
                break;
            }
            case TypedArrayKind::Uint8: {
                uint8_t v = static_cast<uint8_t>(ToUint32(num) & 0xFF);
                *(ptr + index) = v;
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
                int16_t v = static_cast<int16_t>(ToInt32(num) & 0xFFFF);
                std::memcpy(ptr + index * 2, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::Uint16: {
                uint16_t v = ToUint16(num);
                std::memcpy(ptr + index * 2, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::Int32: {
                int32_t v = ToInt32(num);
                std::memcpy(ptr + index * 4, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::Uint32: {
                uint32_t v = ToUint32(num);
                std::memcpy(ptr + index * 4, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::Float32: {
                float v = static_cast<float>(num);
                std::memcpy(ptr + index * 4, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::Float64: {
                std::memcpy(ptr + index * 8, &num, sizeof(num));
                break;
            }
            case TypedArrayKind::BigInt64: {
                int64_t v = TypedArrayToBigInt64(value);
                std::memcpy(ptr + index * 8, &v, sizeof(v));
                break;
            }
            case TypedArrayKind::BigUint64: {
                uint64_t v = TypedArrayToBigUint64(value);
                std::memcpy(ptr + index * 8, &v, sizeof(v));
                break;
            }
        }
    }

    JSTypedArray* Subarray(int64_t begin, int64_t end, Shape* shape = nullptr) const;

private:
    static size_t CheckedTypedArrayOffset(TypedArrayKind kind,
                                          JSArrayBuffer* buffer,
                                          size_t byte_offset,
                                          size_t length) {
        if (buffer == nullptr) {
            throw std::length_error("TypedArray requires an ArrayBuffer");
        }
        if (kind > TypedArrayKind::BigUint64) throw std::length_error("invalid TypedArray kind");
        const size_t element_size = TypedArrayElementSize(kind);
        const size_t byte_length = CheckedStorageBytes(length, element_size,
                                                        "TypedArray length overflow");
        if (buffer->IsDetached()) throw std::length_error("TypedArray buffer is detached");
        if (byte_offset % element_size != 0 ||
            byte_offset > buffer->ByteLength() ||
            byte_length > buffer->ByteLength() - byte_offset) {
            throw std::length_error("TypedArray view is outside its ArrayBuffer");
        }
        return byte_offset;
    }

    static int64_t NormalizeIndex(int64_t index, int64_t length) noexcept {
        if (index < 0) {
            if (index < -length) return 0;
            return length + index;
        }
        return std::min(index, length);
    }

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
          byte_offset_(CheckedDataViewOffset(buffer, byte_offset, byte_length)),
          byte_length_(CheckedDataViewLength(buffer, byte_offset, byte_length)) {}

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
    size_t ByteOffset() const noexcept { return buffer_ && !buffer_->IsDetached() ? byte_offset_ : 0; }
    size_t ByteLength() const noexcept { return (buffer_ && !buffer_->IsDetached()) ? byte_length_ : 0; }

    uint8_t* Data() const noexcept {
        if (!buffer_ || buffer_->IsDetached()) return nullptr;
        uint8_t* raw = buffer_->Data();
        if (!raw || byte_offset_ > buffer_->ByteLength() ||
            byte_length_ > buffer_->ByteLength() - byte_offset_) {
            return nullptr;
        }
        return raw + byte_offset_;
    }

    bool CheckBounds(size_t offset, size_t size) const noexcept {
        return buffer_ && !buffer_->IsDetached() &&
               byte_offset_ <= buffer_->ByteLength() &&
               byte_length_ <= buffer_->ByteLength() - byte_offset_ &&
               offset <= byte_length_ && size <= byte_length_ - offset;
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

    int64_t GetBigInt64(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 8)) return 0;
        uint64_t raw = Read64(Data() + byte_offset, little_endian);
        return static_cast<int64_t>(raw);
    }
    void SetBigInt64(size_t byte_offset, int64_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 8)) return;
        Write64(Data() + byte_offset, static_cast<uint64_t>(value), little_endian);
    }

    uint64_t GetBigUint64(size_t byte_offset, bool little_endian = false) const {
        if (!CheckBounds(byte_offset, 8)) return 0;
        return Read64(Data() + byte_offset, little_endian);
    }
    void SetBigUint64(size_t byte_offset, uint64_t value, bool little_endian = false) {
        if (!CheckBounds(byte_offset, 8)) return;
        Write64(Data() + byte_offset, value, little_endian);
    }

private:
    static size_t CheckedDataViewOffset(JSArrayBuffer* buffer,
                                        size_t byte_offset,
                                        size_t byte_length) {
        if (buffer == nullptr || buffer->IsDetached() ||
            byte_offset > buffer->ByteLength() ||
            byte_length > buffer->ByteLength() - byte_offset) {
            throw std::length_error("DataView is outside its ArrayBuffer");
        }
        return byte_offset;
    }

    static size_t CheckedDataViewLength(JSArrayBuffer* buffer,
                                        size_t byte_offset,
                                        size_t byte_length) {
        // Validate through the same subtraction-based range check.  Keeping
        // this separate makes both constructor initializers overflow-safe.
        (void)CheckedDataViewOffset(buffer, byte_offset, byte_length);
        return byte_length;
    }

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
