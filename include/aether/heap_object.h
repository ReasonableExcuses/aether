#ifndef AETHER_HEAP_OBJECT_H
#define AETHER_HEAP_OBJECT_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace aether {

enum class HeapObjectType : uint16_t {
    Shape       = 0x01,
    String      = 0x02,
    Symbol      = 0x03,
    JSObject    = 0x04,
    JSArray     = 0x05,
    JSFunction        = 0x06,
    FixedArray        = 0x07,
    JSNativeFunction  = 0x08,
};

/**
 * @brief Base class for all managed garbage-collected heap objects.
 *
 * Header Layout (8 bytes):
 * - Shape* shape_ (32-bit compressed pointer in production heap)
 * - uint16_t type_ (HeapObjectType)
 * - uint16_t gc_metadata_ (mark state, age, pinned bit)
 */
class HeapObject {
public:
    HeapObject(HeapObjectType type, Shape* shape)
        : shape_(shape), type_(type), gc_metadata_(0) {}
    virtual ~HeapObject() = default;

    AETHER_ALWAYS_INLINE Shape* GetShape() const noexcept { return shape_; }
    AETHER_ALWAYS_INLINE void SetShape(Shape* shape) noexcept { shape_ = shape; }

    AETHER_ALWAYS_INLINE HeapObjectType GetType() const noexcept { return type_; }
    AETHER_ALWAYS_INLINE bool IsJSObject() const noexcept { return type_ == HeapObjectType::JSObject; }
    AETHER_ALWAYS_INLINE bool IsString() const noexcept { return type_ == HeapObjectType::String; }
    AETHER_ALWAYS_INLINE bool IsArray() const noexcept { return type_ == HeapObjectType::JSArray; }
    AETHER_ALWAYS_INLINE bool IsFunction() const noexcept { return type_ == HeapObjectType::JSFunction || type_ == HeapObjectType::JSNativeFunction; }
    AETHER_ALWAYS_INLINE bool IsNativeFunction() const noexcept { return type_ == HeapObjectType::JSNativeFunction; }

    static constexpr uint16_t kMarkBit      = 1 << 0;
    static constexpr uint16_t kForwardedBit = 1 << 1;
    static constexpr uint16_t kOldGenBit    = 1 << 2;
    static constexpr uint16_t kAgeShift     = 4;
    static constexpr uint16_t kAgeMask      = 0x00F0;

    AETHER_ALWAYS_INLINE bool IsMarked() const noexcept { return (gc_metadata_ & kMarkBit) != 0; }
    AETHER_ALWAYS_INLINE void SetMarked(bool m) noexcept {
        if (m) gc_metadata_ |= kMarkBit;
        else   gc_metadata_ &= ~kMarkBit;
    }

    AETHER_ALWAYS_INLINE bool IsForwarded() const noexcept { return (gc_metadata_ & kForwardedBit) != 0; }
    AETHER_ALWAYS_INLINE HeapObject* GetForwardedAddress() const noexcept {
        assert(IsForwarded());
        return reinterpret_cast<HeapObject*>(shape_);
    }
    AETHER_ALWAYS_INLINE void SetForwardedAddress(HeapObject* target) noexcept {
        gc_metadata_ |= kForwardedBit;
        shape_ = reinterpret_cast<Shape*>(target);
    }

    AETHER_ALWAYS_INLINE bool IsOld() const noexcept { return (gc_metadata_ & kOldGenBit) != 0; }
    AETHER_ALWAYS_INLINE bool IsYoung() const noexcept { return !IsOld(); }
    AETHER_ALWAYS_INLINE void SetOld(bool old_gen) noexcept {
        if (old_gen) gc_metadata_ |= kOldGenBit;
        else         gc_metadata_ &= ~kOldGenBit;
    }

    AETHER_ALWAYS_INLINE uint8_t GetAge() const noexcept {
        return static_cast<uint8_t>((gc_metadata_ & kAgeMask) >> kAgeShift);
    }
    AETHER_ALWAYS_INLINE void IncrementAge() noexcept {
        uint16_t age = (gc_metadata_ & kAgeMask) >> kAgeShift;
        if (age < 15) {
            age++;
            gc_metadata_ = (gc_metadata_ & ~kAgeMask) | (age << kAgeShift);
        }
    }

    virtual size_t ObjectSize() const { return sizeof(*this); }
    virtual void IteratePointers(const std::function<void(TaggedValue&)>& visitor) { (void)visitor; }

private:
    Shape* shape_{nullptr};
    HeapObjectType type_;
    uint16_t gc_metadata_{0};
};

/**
 * @brief Managed JavaScript String object on the heap.
 */
class JSString : public HeapObject {
public:
    explicit JSString(std::string_view str, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::String, shape), data_(str) {
        ComputeHash();
    }

    const std::string& Value() const noexcept { return data_; }
    std::string_view View() const noexcept { return data_; }
    size_t Length() const noexcept { return data_.length(); }
    uint32_t Hash() const noexcept { return hash_; }

    size_t ObjectSize() const override { return sizeof(JSString); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override { (void)visitor; }

private:
    void ComputeHash() noexcept {
        // FNV-1a 32-bit hash
        uint32_t h = 2166136261u;
        for (char c : data_) {
            h ^= static_cast<uint8_t>(c);
            h *= 16777619u;
        }
        hash_ = h;
    }

    std::string data_;
    uint32_t hash_{0};
};

/**
 * @brief Standard ECMAScript Object with hidden class transitions and property storage.
 */
class JSObject : public HeapObject {
public:
    explicit JSObject(Shape* shape)
        : HeapObject(HeapObjectType::JSObject, shape) {
        for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
            in_object_slots_[i] = TaggedValue::Empty();
        }
    }

    // Fast-path direct slot access using cached Inline Cache offset
    AETHER_ALWAYS_INLINE TaggedValue GetFastSlot(uint32_t slot_index, bool in_object) const {
        if (AETHER_LIKELY(in_object)) {
            assert(slot_index < Shape::kDefaultInObjectProperties);
            return in_object_slots_[slot_index];
        }
        assert(slot_index - Shape::kDefaultInObjectProperties < out_of_line_slots_.size());
        return out_of_line_slots_[slot_index - Shape::kDefaultInObjectProperties];
    }

    AETHER_ALWAYS_INLINE void SetFastSlot(uint32_t slot_index, bool in_object, TaggedValue value) {
        if (AETHER_LIKELY(in_object)) {
            assert(slot_index < Shape::kDefaultInObjectProperties);
            in_object_slots_[slot_index] = value;
            return;
        }
        size_t ool_idx = slot_index - Shape::kDefaultInObjectProperties;
        if (ool_idx >= out_of_line_slots_.size()) {
            out_of_line_slots_.resize(ool_idx + 1, TaggedValue::Empty());
        }
        out_of_line_slots_[ool_idx] = value;
    }

    // Full property lookup and mutation with shape transitions
    TaggedValue GetNamedProperty(std::string_view key) const;
    void SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id);

    size_t ObjectSize() const override { return sizeof(JSObject); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
            if (!in_object_slots_[i].IsEmpty()) {
                visitor(in_object_slots_[i]);
            }
        }
        for (auto& slot : out_of_line_slots_) {
            if (!slot.IsEmpty()) {
                visitor(slot);
            }
        }
    }

private:
    TaggedValue in_object_slots_[Shape::kDefaultInObjectProperties];
    std::vector<TaggedValue> out_of_line_slots_;
};

/**
 * @brief Managed JavaScript Function object holding compiled bytecode.
 */
class JSFunction : public HeapObject {
public:
    JSFunction(std::string_view name, const BytecodeArray* bytecode, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::JSFunction, shape),
          name_(name),
          bytecode_(bytecode) {}

    const std::string& Name() const noexcept { return name_; }
    const BytecodeArray* GetBytecode() const noexcept { return bytecode_; }
    void SetBytecode(const BytecodeArray* bytecode) noexcept { bytecode_ = bytecode; }

    size_t ObjectSize() const override { return sizeof(JSFunction); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override { (void)visitor; }

private:
    std::string name_;
    const BytecodeArray* bytecode_{nullptr};
};

} // namespace aether

#endif // AETHER_HEAP_OBJECT_H
