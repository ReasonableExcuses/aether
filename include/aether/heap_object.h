#ifndef AETHER_HEAP_OBJECT_H
#define AETHER_HEAP_OBJECT_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <new>
#include <utility>
#include <atomic>

namespace aether {

class HeapObject;
void RecordWriteBarrier(HeapObject* source, TaggedValue value);

enum class HeapObjectType : uint16_t {
    Shape       = 0x01,
    String      = 0x02,
    Symbol      = 0x03,
    JSObject    = 0x04,
    JSArray     = 0x05,
    JSFunction        = 0x06,
    FixedArray        = 0x07,
    JSNativeFunction  = 0x08,
    LexicalEnvironment= 0x09,
    JSError           = 0x0A,
    JSMap             = 0x0B,
    JSSet             = 0x0C,
    JSPromise         = 0x0D,
    JSRegExp          = 0x0E,
    JSArrayBuffer     = 0x0F,
    JSTypedArray      = 0x10,
    JSDataView        = 0x11,
    JSGenerator       = 0x12,
    JSProxy           = 0x13,
    JSWeakMap         = 0x14,
    JSWeakSet         = 0x15,
    JSWeakRef         = 0x16,
    JSFinalizationRegistry = 0x17,
    JSBigInt          = 0x18,
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
    AETHER_ALWAYS_INLINE bool IsJSObject() const noexcept {
        return type_ == HeapObjectType::JSObject ||
               type_ == HeapObjectType::JSArray  ||
               type_ == HeapObjectType::JSFunction ||
               type_ == HeapObjectType::JSNativeFunction ||
               type_ == HeapObjectType::JSMap    ||
               type_ == HeapObjectType::JSSet    ||
               type_ == HeapObjectType::JSPromise ||
               type_ == HeapObjectType::JSRegExp ||
               type_ == HeapObjectType::JSError  ||
               type_ == HeapObjectType::JSArrayBuffer ||
               type_ == HeapObjectType::JSTypedArray ||
               type_ == HeapObjectType::JSDataView ||
               type_ == HeapObjectType::JSGenerator ||
               type_ == HeapObjectType::JSProxy  ||
               type_ == HeapObjectType::JSWeakMap ||
               type_ == HeapObjectType::JSWeakSet ||
               type_ == HeapObjectType::JSWeakRef ||
               type_ == HeapObjectType::JSFinalizationRegistry;
    }
    AETHER_ALWAYS_INLINE bool IsECMAObject() const noexcept {
        return IsJSObject() || IsFunction();
    }
    AETHER_ALWAYS_INLINE bool IsPlainObject() const noexcept { return type_ == HeapObjectType::JSObject; }
    AETHER_ALWAYS_INLINE bool IsString() const noexcept { return type_ == HeapObjectType::String; }
    AETHER_ALWAYS_INLINE bool IsArray() const noexcept { return type_ == HeapObjectType::JSArray; }
    AETHER_ALWAYS_INLINE bool IsFunction() const noexcept { return type_ == HeapObjectType::JSFunction || type_ == HeapObjectType::JSNativeFunction; }
    AETHER_ALWAYS_INLINE bool IsUserFunction() const noexcept { return type_ == HeapObjectType::JSFunction; }
    AETHER_ALWAYS_INLINE bool IsNativeFunction() const noexcept { return type_ == HeapObjectType::JSNativeFunction; }
    AETHER_ALWAYS_INLINE bool IsEnvironment() const noexcept { return type_ == HeapObjectType::LexicalEnvironment; }
    AETHER_ALWAYS_INLINE bool IsError() const noexcept { return type_ == HeapObjectType::JSError; }
    AETHER_ALWAYS_INLINE bool IsMap() const noexcept { return type_ == HeapObjectType::JSMap; }
    AETHER_ALWAYS_INLINE bool IsSet() const noexcept { return type_ == HeapObjectType::JSSet; }
    AETHER_ALWAYS_INLINE bool IsPromise() const noexcept { return type_ == HeapObjectType::JSPromise; }
    AETHER_ALWAYS_INLINE bool IsRegExp() const noexcept { return type_ == HeapObjectType::JSRegExp; }
    AETHER_ALWAYS_INLINE bool IsArrayBuffer() const noexcept { return type_ == HeapObjectType::JSArrayBuffer; }
    AETHER_ALWAYS_INLINE bool IsTypedArray() const noexcept { return type_ == HeapObjectType::JSTypedArray; }
    AETHER_ALWAYS_INLINE bool IsDataView() const noexcept { return type_ == HeapObjectType::JSDataView; }
    AETHER_ALWAYS_INLINE bool IsSymbol() const noexcept { return type_ == HeapObjectType::Symbol; }
    AETHER_ALWAYS_INLINE bool IsGeneratorObject() const noexcept { return type_ == HeapObjectType::JSGenerator; }
    AETHER_ALWAYS_INLINE bool IsProxy() const noexcept { return type_ == HeapObjectType::JSProxy; }
    AETHER_ALWAYS_INLINE bool IsWeakMap() const noexcept { return type_ == HeapObjectType::JSWeakMap; }
    AETHER_ALWAYS_INLINE bool IsWeakSet() const noexcept { return type_ == HeapObjectType::JSWeakSet; }
    AETHER_ALWAYS_INLINE bool IsWeakRef() const noexcept { return type_ == HeapObjectType::JSWeakRef; }
    AETHER_ALWAYS_INLINE bool IsFinalizationRegistry() const noexcept { return type_ == HeapObjectType::JSFinalizationRegistry; }
    AETHER_ALWAYS_INLINE bool IsBigInt() const noexcept { return type_ == HeapObjectType::JSBigInt; }

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

    AETHER_ALWAYS_INLINE bool AtomicSetMarked() noexcept {
        std::atomic_ref<uint16_t> ref(gc_metadata_);
        uint16_t old_val = ref.load(std::memory_order_relaxed);
        while (!(old_val & kMarkBit)) {
            if (ref.compare_exchange_weak(old_val, static_cast<uint16_t>(old_val | kMarkBit),
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
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

    HeapObject(HeapObject&& other) noexcept
        : shape_(other.shape_),
          type_(other.type_),
          gc_metadata_(other.gc_metadata_) {}

    virtual size_t ObjectSize() const { return sizeof(*this); }
    virtual void IteratePointers(const std::function<void(TaggedValue&)>& visitor) { (void)visitor; }
    virtual HeapObject* RelocateTo(void* dst) = 0;

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
    JSString(JSString&& other) noexcept = default;

    const std::string& Value() const noexcept { return data_; }
    std::string_view View() const noexcept { return data_; }
    size_t Length() const noexcept { return data_.length(); }
    uint32_t Hash() const noexcept { return hash_; }

    size_t ObjectSize() const override { return sizeof(JSString); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override { (void)visitor; }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSString(std::move(*this));
    }

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
 * @brief Managed JavaScript Symbol primitive object on the heap.
 */
class JSSymbol : public HeapObject {
public:
    explicit JSSymbol(std::string_view description, uint32_t id, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::Symbol, shape),
          description_(description),
          id_(id) {
        if (!description_.empty() && description_.rfind("@@", 0) == 0) {
            key_ = description_;
        } else {
            key_ = "@@Symbol(" + std::string(description_) + ")_" + std::to_string(id_);
        }
    }
    JSSymbol(JSSymbol&& other) noexcept = default;

    const std::string& Description() const noexcept { return description_; }
    const std::string& Key() const noexcept { return key_; }
    uint32_t Id() const noexcept { return id_; }

    size_t ObjectSize() const override { return sizeof(JSSymbol); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override { (void)visitor; }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSSymbol(std::move(*this));
    }

private:
    std::string description_;
    std::string key_;
    uint32_t id_{0};
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

    JSObject(JSObject&& other) noexcept
        : HeapObject(std::move(other)),
          is_frozen_(other.is_frozen_),
          is_sealed_(other.is_sealed_),
           prototype_(other.prototype_),
           out_of_line_slots_(std::move(other.out_of_line_slots_)),
           accessors_(std::move(other.accessors_)),
           own_attributes_(std::move(other.own_attributes_)),
           deleted_properties_(std::move(other.deleted_properties_)) {
        for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
            in_object_slots_[i] = other.in_object_slots_[i];
        }
    }

    virtual ~JSObject() = default;

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSObject(std::move(*this));
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
        } else {
            size_t ool_idx = slot_index - Shape::kDefaultInObjectProperties;
            if (ool_idx >= out_of_line_slots_.size()) {
                out_of_line_slots_.resize(ool_idx + 1, TaggedValue::Empty());
            }
            out_of_line_slots_[ool_idx] = value;
        }
        if (AETHER_UNLIKELY(IsOld() && value.IsHeapObject())) {
            RecordWriteBarrier(this, value);
        }
    }

    // Full property lookup and mutation with shape transitions
    TaggedValue GetNamedProperty(std::string_view key) const;
    void SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id);
    bool HasNamedProperty(std::string_view key) const;
    bool HasOwnProperty(std::string_view key) const;
    bool DeleteNamedProperty(std::string_view key);
    std::vector<std::string> GetOwnPropertyNames() const;
    PropertyAttributes GetOwnPropertyAttributes(std::string_view key) const;
    void SetOwnPropertyAttributes(std::string_view key, PropertyAttributes attributes);

    JSObject* GetPrototype() const noexcept { return prototype_; }
    void SetPrototype(JSObject* proto) noexcept {
        prototype_ = proto;
        if (AETHER_UNLIKELY(IsOld() && proto != nullptr)) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(proto));
        }
    }

    bool IsFrozen() const noexcept { return is_frozen_; }
    void Freeze() noexcept;
    bool IsSealed() const noexcept { return is_sealed_; }
    void Seal() noexcept;
    bool IsExtensible() const noexcept { return is_extensible_; }
    void PreventExtensions() noexcept { is_extensible_ = false; }

    bool IsDeletedProperty(std::string_view key) const {
        return deleted_properties_.count(std::string(key)) > 0;
    }
    void ClearDeletedProperty(std::string_view key) {
        deleted_properties_.erase(std::string(key));
    }
    void MarkDeletedProperty(std::string_view key) {
        deleted_properties_.insert(std::string(key));
    }
    const std::unordered_set<std::string>& GetDeletedProperties() const noexcept {
        return deleted_properties_;
    }

    struct AccessorDescriptor {
        TaggedValue getter{TaggedValue::Undefined()};
        TaggedValue setter{TaggedValue::Undefined()};
        PropertyAttributes attributes{PropertyAttributes::None};
    };

    void SetAccessor(std::string_view key, TaggedValue getter, TaggedValue setter,
                     PropertyAttributes attributes = PropertyAttributes::None) {
        accessors_[std::string(key)] = AccessorDescriptor{getter, setter, attributes};
        own_attributes_[std::string(key)] = attributes;
        if (AETHER_UNLIKELY(IsOld())) {
            if (getter.IsHeapObject()) RecordWriteBarrier(this, getter);
            if (setter.IsHeapObject()) RecordWriteBarrier(this, setter);
        }
    }
    const AccessorDescriptor* GetAccessor(std::string_view key) const {
        auto it = accessors_.find(std::string(key));
        if (it != accessors_.end()) return &it->second;
        return nullptr;
    }
    bool HasAccessor(std::string_view key) const {
        return accessors_.count(std::string(key)) > 0;
    }
    const std::unordered_map<std::string, AccessorDescriptor>& GetAccessors() const {
        return accessors_;
    }

    size_t ObjectSize() const override { return sizeof(JSObject); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        if (prototype_ != nullptr) {
            TaggedValue proto_val = TaggedValue::FromHeapObject(prototype_);
            visitor(proto_val);
            if (proto_val.IsHeapObject()) {
                prototype_ = proto_val.As<JSObject>();
            }
        }
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
        for (auto& [k, acc] : accessors_) {
            if (!acc.getter.IsEmpty()) visitor(acc.getter);
            if (!acc.setter.IsEmpty()) visitor(acc.setter);
        }
    }

protected:
    JSObject(HeapObjectType type, Shape* shape)
        : HeapObject(type, shape) {
        for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
            in_object_slots_[i] = TaggedValue::Empty();
        }
    }

private:
    bool is_frozen_{false};
    bool is_sealed_{false};
    bool is_extensible_{true};
    JSObject* prototype_{nullptr};
    TaggedValue in_object_slots_[Shape::kDefaultInObjectProperties];
    std::vector<TaggedValue> out_of_line_slots_;
    std::unordered_map<std::string, AccessorDescriptor> accessors_;
    std::unordered_map<std::string, PropertyAttributes> own_attributes_;
    std::unordered_set<std::string> deleted_properties_;
};

/**
 * @brief Managed JavaScript Array with contiguous elements and length property.
 */
class JSArray : public JSObject {
public:
    explicit JSArray(Shape* shape, size_t initial_capacity = 0)
        : JSObject(HeapObjectType::JSArray, shape) {
        if (initial_capacity > 0) {
            elements_.resize(initial_capacity, TaggedValue::Empty());
        }
    }

    explicit JSArray(Shape* shape, std::vector<TaggedValue> elements)
        : JSObject(HeapObjectType::JSArray, shape), elements_(std::move(elements)) {}

    JSArray(JSArray&& other) noexcept
        : JSObject(std::move(other)),
          elements_(std::move(other.elements_)) {}

    size_t Length() const noexcept { return elements_.size(); }
    void SetLength(size_t len) { elements_.resize(len, TaggedValue::Empty()); }

    TaggedValue GetElement(size_t index) const {
        if (index < elements_.size() && !elements_[index].IsEmpty()) {
            return elements_[index];
        }
        return TaggedValue::Undefined();
    }

    void SetElement(size_t index, TaggedValue val) {
        if (index >= elements_.size()) {
            elements_.resize(index + 1, TaggedValue::Empty());
        }
        elements_[index] = val;
        if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
            RecordWriteBarrier(this, val);
        }
    }

    void Push(TaggedValue val) {
        elements_.push_back(val);
        if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
            RecordWriteBarrier(this, val);
        }
    }

    TaggedValue Pop() {
        if (elements_.empty()) return TaggedValue::Undefined();
        TaggedValue last = elements_.back();
        elements_.pop_back();
        return last;
    }

    const std::vector<TaggedValue>& Elements() const noexcept { return elements_; }
    std::vector<TaggedValue>& Elements() noexcept { return elements_; }

    size_t ObjectSize() const override {
        return sizeof(JSArray) + elements_.capacity() * sizeof(TaggedValue);
    }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        for (auto& el : elements_) {
            if (!el.IsEmpty()) visitor(el);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSArray(std::move(*this));
    }

private:
    std::vector<TaggedValue> elements_;
};

/**
 * @brief Managed ECMAScript Map collection preserving insertion order.
 */
class JSMap : public JSObject {
public:
    explicit JSMap(Shape* shape)
        : JSObject(HeapObjectType::JSMap, shape) {}

    JSMap(JSMap&& other) noexcept
        : JSObject(std::move(other)),
          entries_(std::move(other.entries_)) {}

    size_t Size() const noexcept { return entries_.size(); }

    void Set(TaggedValue key, TaggedValue val);
    TaggedValue Get(TaggedValue key) const;
    bool Has(TaggedValue key) const;
    bool Delete(TaggedValue key);
    void Clear() { entries_.clear(); }

    const std::vector<std::pair<TaggedValue, TaggedValue>>& Entries() const noexcept { return entries_; }

    size_t ObjectSize() const override {
        return sizeof(JSMap) + entries_.capacity() * sizeof(std::pair<TaggedValue, TaggedValue>);
    }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        for (auto& [k, v] : entries_) {
            if (!k.IsEmpty()) visitor(k);
            if (!v.IsEmpty()) visitor(v);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSMap(std::move(*this));
    }

private:
    std::vector<std::pair<TaggedValue, TaggedValue>> entries_;
};

/**
 * @brief Managed ECMAScript Set collection preserving insertion order.
 */
class JSSet : public JSObject {
public:
    explicit JSSet(Shape* shape)
        : JSObject(HeapObjectType::JSSet, shape) {}

    JSSet(JSSet&& other) noexcept
        : JSObject(std::move(other)),
          values_(std::move(other.values_)) {}

    size_t Size() const noexcept { return values_.size(); }

    void Add(TaggedValue val);
    bool Has(TaggedValue val) const;
    bool Delete(TaggedValue val);
    void Clear() { values_.clear(); }

    const std::vector<TaggedValue>& Values() const noexcept { return values_; }

    size_t ObjectSize() const override {
        return sizeof(JSSet) + values_.capacity() * sizeof(TaggedValue);
    }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        for (auto& v : values_) {
            if (!v.IsEmpty()) visitor(v);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSSet(std::move(*this));
    }

private:
    std::vector<TaggedValue> values_;
};

enum class PromiseState : uint8_t {
    Pending   = 0,
    Fulfilled = 1,
    Rejected  = 2
};

class MicrotaskQueue;

struct PromiseReaction {
    TaggedValue on_fulfilled;
    TaggedValue on_rejected;
    class JSPromise* downstream_promise{nullptr};
};

/**
 * @brief Managed ECMAScript Promise conforming to Promise/A+.
 */
class JSPromise : public JSObject {
public:
    explicit JSPromise(Shape* shape)
        : JSObject(HeapObjectType::JSPromise, shape),
          state_(PromiseState::Pending),
          result_(TaggedValue::Undefined()) {}

    JSPromise(JSPromise&& other) noexcept
        : JSObject(std::move(other)),
          state_(other.state_),
          result_(other.result_),
          reactions_(std::move(other.reactions_)) {}

    PromiseState State() const noexcept { return state_; }
    TaggedValue Result() const noexcept { return result_; }

    void Fulfill(TaggedValue val, MicrotaskQueue& queue);
    void Reject(TaggedValue reason, MicrotaskQueue& queue);
    void AddReaction(TaggedValue on_fulfilled, TaggedValue on_rejected, JSPromise* downstream, MicrotaskQueue& queue);

    size_t ObjectSize() const override {
        return sizeof(JSPromise) + reactions_.capacity() * sizeof(PromiseReaction);
    }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (!result_.IsEmpty()) visitor(result_);
        for (auto& r : reactions_) {
            if (!r.on_fulfilled.IsEmpty()) visitor(r.on_fulfilled);
            if (!r.on_rejected.IsEmpty()) visitor(r.on_rejected);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSPromise(std::move(*this));
    }

private:
    PromiseState state_;
    TaggedValue result_;
    std::vector<PromiseReaction> reactions_;
};

/**
 * @brief Managed ECMAScript RegExp object.
 */
class JSRegExp : public JSObject {
public:
    JSRegExp(std::string_view pattern, std::string_view flags, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSRegExp, shape),
          pattern_(pattern),
          flags_(flags) {}

    JSRegExp(JSRegExp&& other) noexcept
        : JSObject(std::move(other)),
          pattern_(std::move(other.pattern_)),
          flags_(std::move(other.flags_)) {}

    const std::string& Pattern() const noexcept { return pattern_; }
    const std::string& Flags() const noexcept { return flags_; }

    bool Test(std::string_view str) const;

    size_t ObjectSize() const override { return sizeof(JSRegExp); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSRegExp(std::move(*this));
    }

private:
    std::string pattern_;
    std::string flags_;
};

/**
 * @brief Heap-allocated Lexical Scope Environment for closures and captured variables.
 */
class LexicalEnvironment : public HeapObject {
public:
    LexicalEnvironment(LexicalEnvironment* outer, size_t slot_count)
        : HeapObject(HeapObjectType::LexicalEnvironment, nullptr),
          outer_(outer),
          slots_(slot_count, TaggedValue::Undefined()) {}

    LexicalEnvironment* Outer() const noexcept { return outer_; }
    size_t SlotCount() const noexcept { return slots_.size(); }

    TaggedValue GetSlot(size_t index) const {
        assert(index < slots_.size());
        return slots_[index];
    }

    void SetSlot(size_t index, TaggedValue val) {
        assert(index < slots_.size());
        slots_[index] = val;
        if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
            RecordWriteBarrier(this, val);
        }
    }

    TaggedValue Get(uint8_t depth, uint16_t index) const {
        const LexicalEnvironment* curr = this;
        for (uint8_t d = 0; d < depth && curr; ++d) {
            curr = curr->outer_;
        }
        if (curr && index < curr->slots_.size()) return curr->slots_[index];
        return TaggedValue::Undefined();
    }

    void Set(uint8_t depth, uint16_t index, TaggedValue val) {
        LexicalEnvironment* curr = this;
        for (uint8_t d = 0; d < depth && curr; ++d) {
            curr = curr->outer_;
        }
        if (curr && index < curr->slots_.size()) {
            curr->slots_[index] = val;
            if (AETHER_UNLIKELY(curr->IsOld() && val.IsHeapObject())) {
                RecordWriteBarrier(curr, val);
            }
        }
    }

    LexicalEnvironment(LexicalEnvironment&& other) noexcept = default;

    size_t ObjectSize() const override { return sizeof(LexicalEnvironment) + slots_.size() * sizeof(TaggedValue); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        if (outer_ != nullptr) {
            TaggedValue outer_val = TaggedValue::FromHeapObject(outer_);
            visitor(outer_val);
            if (outer_val.IsHeapObject()) {
                outer_ = outer_val.As<LexicalEnvironment>();
            }
        }
        for (auto& s : slots_) {
            if (!s.IsEmpty()) visitor(s);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) LexicalEnvironment(std::move(*this));
    }

private:
    LexicalEnvironment* outer_{nullptr};
    std::vector<TaggedValue> slots_;
};

/**
 * @brief Managed JavaScript Function object holding compiled bytecode and lexical closure scope.
 */
class JSFunction : public JSObject {
public:
    JSFunction(std::string_view name, std::shared_ptr<const BytecodeArray> bytecode, LexicalEnvironment* env = nullptr, Shape* shape = nullptr);
    JSFunction(std::string_view name, const BytecodeArray* bytecode, LexicalEnvironment* env = nullptr, Shape* shape = nullptr);

    JSFunction(JSFunction&& other) noexcept
        : JSObject(std::move(other)),
          name_(std::move(other.name_)),
          bytecode_shared_(std::move(other.bytecode_shared_)),
           bytecode_(other.bytecode_),
           environment_(other.environment_),
           ctor_prototype_(other.ctor_prototype_),
           instance_fields_(std::move(other.instance_fields_)),
           is_generator_(other.is_generator_),
           is_async_(other.is_async_),
           is_arrow_(other.is_arrow_),
           lexical_this_(other.lexical_this_),
           lexical_new_target_(other.lexical_new_target_) {}

    const std::string& Name() const noexcept { return name_; }
    const BytecodeArray* GetBytecode() const noexcept { return bytecode_; }
    void SetBytecode(const BytecodeArray* bytecode) noexcept { 
        bytecode_ = bytecode; 
        bytecode_shared_.reset();
    }
    void SetBytecode(std::shared_ptr<const BytecodeArray> bytecode) noexcept {
        bytecode_shared_ = std::move(bytecode);
        bytecode_ = bytecode_shared_.get();
    }
    std::shared_ptr<const BytecodeArray> GetSharedBytecode() const noexcept { return bytecode_shared_; }

    LexicalEnvironment* GetEnvironment() const noexcept { return environment_; }
    void SetEnvironment(LexicalEnvironment* env) noexcept { environment_ = env; }

    bool IsGenerator() const noexcept { return is_generator_; }
    void SetGenerator(bool g) noexcept { is_generator_ = g; }

    bool IsAsync() const noexcept { return is_async_; }
    void SetAsync(bool a) noexcept { is_async_ = a; }

    bool IsAsyncGenerator() const noexcept { return is_async_ && is_generator_; }

    bool IsArrow() const noexcept { return is_arrow_; }
    void SetArrow(bool a) noexcept { is_arrow_ = a; }

    TaggedValue GetLexicalThis() const noexcept { return lexical_this_; }
    void SetLexicalThis(TaggedValue t) noexcept { lexical_this_ = t; }

    TaggedValue GetLexicalNewTarget() const noexcept { return lexical_new_target_; }
    void SetLexicalNewTarget(TaggedValue nt) noexcept { lexical_new_target_ = nt; }

    TaggedValue GetConstructorPrototype() const noexcept { return ctor_prototype_; }
    void SetConstructorPrototype(TaggedValue proto) noexcept { ctor_prototype_ = proto; }
    JSObject* GetOrCreateConstructorPrototype();
    void AddInstanceField(std::string_view name, TaggedValue initial_value) {
        instance_fields_.emplace_back(std::string(name), initial_value);
    }
    const std::vector<std::pair<std::string, TaggedValue>>& InstanceFields() const noexcept { return instance_fields_; }
    const std::vector<JSObject*>& GetWithStack() const noexcept { return with_stack_; }
    void SetWithStack(std::vector<JSObject*> ws) { with_stack_ = std::move(ws); }
    bool HasWithStack() const noexcept { return !with_stack_.empty(); }

    size_t ObjectSize() const override { return sizeof(JSFunction); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override;

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSFunction(std::move(*this));
    }

private:
    std::string name_;
    std::shared_ptr<const BytecodeArray> bytecode_shared_{nullptr};
    const BytecodeArray* bytecode_{nullptr};
    LexicalEnvironment* environment_{nullptr};
    TaggedValue ctor_prototype_{TaggedValue::Empty()};
    std::vector<std::pair<std::string, TaggedValue>> instance_fields_;
    bool is_generator_{false};
    bool is_async_{false};
    bool is_arrow_{false};
    TaggedValue lexical_this_{TaggedValue::Undefined()};
    TaggedValue lexical_new_target_{TaggedValue::Undefined()};
    std::vector<JSObject*> with_stack_;
};

/**
 * @brief Managed ECMAScript Generator Object suspended/resumed across yield points.
 */
class JSGeneratorObject : public JSObject {
public:
    enum class State : uint8_t {
        SuspendedStart,
        SuspendedYield,
        Executing,
        Completed
    };

    struct SavedHandler {
        uint32_t target_offset{0};
        LexicalEnvironment* env{nullptr};
    };

    struct AsyncRequest {
        enum class Type : uint8_t { Next, Return, Throw };
        Type type{Type::Next};
        TaggedValue value{TaggedValue::Undefined()};
        JSPromise* promise{nullptr};
    };

    JSGeneratorObject(JSFunction* fn, std::vector<TaggedValue> args, TaggedValue this_val, LexicalEnvironment* env, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSGenerator, shape),
          function_(fn),
          args_(std::move(args)),
          this_val_(this_val),
          environment_(env),
          state_(State::SuspendedStart) {}

    JSGeneratorObject(JSGeneratorObject&& other) noexcept = default;

    JSFunction* Function() const noexcept { return function_; }
    State GetState() const noexcept { return state_; }
    void SetState(State s) noexcept { state_ = s; }
    uint32_t PCOffset() const noexcept { return pc_offset_; }
    void SetPCOffset(uint32_t pc) noexcept { pc_offset_ = pc; }
    std::vector<TaggedValue>& SavedRegisters() noexcept { return saved_registers_; }
    const std::vector<TaggedValue>& Args() const noexcept { return args_; }
    TaggedValue ThisVal() const noexcept { return this_val_; }
    LexicalEnvironment* Environment() const noexcept { return environment_; }
    void SetEnvironment(LexicalEnvironment* env) noexcept { environment_ = env; }

    bool IsAsync() const noexcept { return function_ && function_->IsAsync(); }
    bool IsGenerator() const noexcept { return function_ && function_->IsGenerator(); }
    bool IsAsyncGenerator() const noexcept { return function_ && function_->IsAsync() && function_->IsGenerator(); }

    JSPromise* AsyncPromise() const noexcept { return async_promise_; }
    void SetAsyncPromise(JSPromise* p) noexcept { async_promise_ = p; }
    std::vector<SavedHandler>& SavedHandlers() noexcept { return saved_handlers_; }
    const std::vector<SavedHandler>& SavedHandlers() const noexcept { return saved_handlers_; }

    std::vector<AsyncRequest>& AsyncQueue() noexcept { return async_queue_; }
    const std::vector<AsyncRequest>& AsyncQueue() const noexcept { return async_queue_; }

    bool IsRunningAsync() const noexcept { return is_running_async_; }
    void SetRunningAsync(bool r) noexcept { is_running_async_ = r; }

    size_t ObjectSize() const override {
        return sizeof(JSGeneratorObject) + saved_registers_.capacity() * sizeof(TaggedValue);
    }

    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (function_ != nullptr) {
            TaggedValue fn_val = TaggedValue::FromHeapObject(function_);
            visitor(fn_val);
            if (fn_val.IsHeapObject()) function_ = static_cast<JSFunction*>(fn_val.AsHeapObject());
        }
        if (environment_ != nullptr) {
            TaggedValue env_val = TaggedValue::FromHeapObject(environment_);
            visitor(env_val);
            if (env_val.IsHeapObject()) environment_ = static_cast<LexicalEnvironment*>(env_val.AsHeapObject());
        }
        if (async_promise_ != nullptr) {
            TaggedValue prom_val = TaggedValue::FromHeapObject(async_promise_);
            visitor(prom_val);
            if (prom_val.IsHeapObject()) async_promise_ = static_cast<JSPromise*>(prom_val.AsHeapObject());
        }
        for (auto& sh : saved_handlers_) {
            if (sh.env != nullptr) {
                TaggedValue env_val = TaggedValue::FromHeapObject(sh.env);
                visitor(env_val);
                if (env_val.IsHeapObject()) sh.env = static_cast<LexicalEnvironment*>(env_val.AsHeapObject());
            }
        }
        for (auto& req : async_queue_) {
            if (!req.value.IsEmpty()) visitor(req.value);
            if (req.promise != nullptr) {
                TaggedValue prom_val = TaggedValue::FromHeapObject(req.promise);
                visitor(prom_val);
                if (prom_val.IsHeapObject()) req.promise = static_cast<JSPromise*>(prom_val.AsHeapObject());
            }
        }
        if (!this_val_.IsEmpty()) visitor(this_val_);
        for (auto& r : saved_registers_) {
            if (!r.IsEmpty()) visitor(r);
        }
        for (auto& a : args_) {
            if (!a.IsEmpty()) visitor(a);
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSGeneratorObject(std::move(*this));
    }

private:
    JSFunction* function_{nullptr};
    std::vector<TaggedValue> args_;
    TaggedValue this_val_{TaggedValue::Undefined()};
    LexicalEnvironment* environment_{nullptr};
    std::vector<TaggedValue> saved_registers_;
    uint32_t pc_offset_{0};
    State state_{State::SuspendedStart};
    JSPromise* async_promise_{nullptr};
    std::vector<SavedHandler> saved_handlers_;
    std::vector<AsyncRequest> async_queue_;
    bool is_running_async_{false};
};

/**
 * @brief Standard ECMAScript Error Object.
 */
class JSError : public JSObject {
public:
    JSError(std::string_view name, std::string_view message, Shape* shape = nullptr);
    JSError(JSError&& other) noexcept = default;

    const std::string& Name() const noexcept { return name_; }
    const std::string& Message() const noexcept { return message_; }

    size_t ObjectSize() const override { return sizeof(JSError); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSError(std::move(*this));
    }

private:
    std::string name_;
    std::string message_;
};

/**
 * @brief Standard ECMAScript Proxy Object implementing Meta-Object Protocol (MOP).
 */
class JSProxy : public JSObject {
public:
    JSProxy(TaggedValue target, TaggedValue handler, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSProxy, shape),
          target_(target),
          handler_(handler),
          is_revoked_(false),
          is_callable_(false) {
        if (target.IsHeapObject()) {
            auto* h = target.AsHeapObject();
            if (h->IsFunction() || h->IsNativeFunction()) {
                is_callable_ = true;
            } else if (h->IsProxy()) {
                is_callable_ = static_cast<JSProxy*>(h)->IsCallable();
            }
        }
    }
    JSProxy(JSProxy&& other) noexcept = default;

    AETHER_ALWAYS_INLINE TaggedValue GetTarget() const noexcept { return target_; }
    AETHER_ALWAYS_INLINE void SetTarget(TaggedValue t) noexcept { target_ = t; }

    AETHER_ALWAYS_INLINE TaggedValue GetHandler() const noexcept { return handler_; }
    AETHER_ALWAYS_INLINE void SetHandler(TaggedValue h) noexcept { handler_ = h; }

    AETHER_ALWAYS_INLINE bool IsRevoked() const noexcept { return is_revoked_; }
    AETHER_ALWAYS_INLINE bool IsCallable() const noexcept { return is_callable_; }
    void Revoke() noexcept {
        is_revoked_ = true;
        target_ = TaggedValue::Null();
        handler_ = TaggedValue::Null();
    }

    size_t ObjectSize() const override { return sizeof(JSProxy); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (!target_.IsEmpty()) visitor(target_);
        if (!handler_.IsEmpty()) visitor(handler_);
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSProxy(std::move(*this));
    }

private:
    TaggedValue target_{TaggedValue::Null()};
    TaggedValue handler_{TaggedValue::Null()};
    bool is_revoked_{false};
    bool is_callable_{false};
};

/**
 * @brief Standard ECMAScript BigInt Object with arbitrary-precision limbs.
 */
class JSBigInt : public HeapObject {
public:
    explicit JSBigInt(int64_t val, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::JSBigInt, shape), is_negative_(val < 0) {
        uint64_t mag = (val < 0) ? (val == INT64_MIN ? (static_cast<uint64_t>(INT64_MAX) + 1ULL) : static_cast<uint64_t>(-val)) : static_cast<uint64_t>(val);
        if (mag > 0) {
            limbs_.push_back(static_cast<uint32_t>(mag & 0xFFFFFFFFULL));
            if (mag >> 32) {
                limbs_.push_back(static_cast<uint32_t>(mag >> 32));
            }
        }
        Normalize();
    }

    explicit JSBigInt(std::string_view s, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::JSBigInt, shape) {
        FromString(s);
    }

    explicit JSBigInt(bool is_neg, std::vector<uint32_t> limbs, Shape* shape = nullptr)
        : HeapObject(HeapObjectType::JSBigInt, shape), is_negative_(is_neg), limbs_(std::move(limbs)) {
        Normalize();
    }

    JSBigInt(JSBigInt&& other) noexcept = default;

    AETHER_ALWAYS_INLINE bool IsZero() const noexcept { return limbs_.empty(); }
    AETHER_ALWAYS_INLINE bool IsNegative() const noexcept { return is_negative_; }
    AETHER_ALWAYS_INLINE const std::vector<uint32_t>& Limbs() const noexcept { return limbs_; }

    int64_t Value() const noexcept {
        if (limbs_.empty()) return 0;
        uint64_t mag = limbs_[0];
        if (limbs_.size() > 1) {
            mag |= (static_cast<uint64_t>(limbs_[1]) << 32);
        }
        if (is_negative_) {
            if (mag > static_cast<uint64_t>(INT64_MAX)) return INT64_MIN;
            return -static_cast<int64_t>(mag);
        }
        if (mag > static_cast<uint64_t>(INT64_MAX)) return INT64_MAX;
        return static_cast<int64_t>(mag);
    }

    bool Equals(const JSBigInt& other) const noexcept {
        if (limbs_.empty() && other.limbs_.empty()) return true;
        if (is_negative_ != other.is_negative_) return false;
        return limbs_ == other.limbs_;
    }

    int CompareMagnitude(const JSBigInt& other) const noexcept {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() > other.limbs_.size() ? 1 : -1;
        }
        for (size_t i = limbs_.size(); i-- > 0; ) {
            if (limbs_[i] != other.limbs_[i]) {
                return limbs_[i] > other.limbs_[i] ? 1 : -1;
            }
        }
        return 0;
    }

    JSBigInt* Add(const JSBigInt& other) const {
        if (limbs_.empty()) return new JSBigInt(other.is_negative_, other.limbs_);
        if (other.limbs_.empty()) return new JSBigInt(is_negative_, limbs_);

        if (is_negative_ == other.is_negative_) {
            std::vector<uint32_t> res_limbs;
            size_t max_len = std::max(limbs_.size(), other.limbs_.size());
            res_limbs.reserve(max_len + 1);
            uint64_t carry = 0;
            for (size_t i = 0; i < max_len || carry; ++i) {
                uint64_t a = (i < limbs_.size()) ? limbs_[i] : 0;
                uint64_t b = (i < other.limbs_.size()) ? other.limbs_[i] : 0;
                uint64_t sum = a + b + carry;
                res_limbs.push_back(static_cast<uint32_t>(sum & 0xFFFFFFFFULL));
                carry = sum >> 32;
            }
            return new JSBigInt(is_negative_, std::move(res_limbs));
        }

        int cmp = CompareMagnitude(other);
        if (cmp == 0) {
            return new JSBigInt(0);
        }

        const auto& larger = (cmp > 0) ? limbs_ : other.limbs_;
        const auto& smaller = (cmp > 0) ? other.limbs_ : limbs_;
        bool result_neg = (cmp > 0) ? is_negative_ : other.is_negative_;

        std::vector<uint32_t> res_limbs;
        res_limbs.reserve(larger.size());
        uint64_t borrow = 0;
        for (size_t i = 0; i < larger.size(); ++i) {
            uint64_t a = larger[i];
            uint64_t b = (i < smaller.size()) ? smaller[i] : 0;
            if (a >= b + borrow) {
                res_limbs.push_back(static_cast<uint32_t>(a - b - borrow));
                borrow = 0;
            } else {
                res_limbs.push_back(static_cast<uint32_t>((a + (1ULL << 32)) - b - borrow));
                borrow = 1;
            }
        }
        return new JSBigInt(result_neg, std::move(res_limbs));
    }

    JSBigInt* Subtract(const JSBigInt& other) const {
        JSBigInt negated_other(!other.is_negative_, other.limbs_);
        return Add(negated_other);
    }

    JSBigInt* Multiply(const JSBigInt& other) const {
        if (limbs_.empty() || other.limbs_.empty()) return new JSBigInt(0);
        bool res_neg = (is_negative_ != other.is_negative_);
        std::vector<uint32_t> res(limbs_.size() + other.limbs_.size(), 0);
        for (size_t i = 0; i < limbs_.size(); ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < other.limbs_.size() || carry; ++j) {
                uint64_t cur = res[i + j] + carry;
                if (j < other.limbs_.size()) {
                    cur += static_cast<uint64_t>(limbs_[i]) * other.limbs_[j];
                }
                res[i + j] = static_cast<uint32_t>(cur & 0xFFFFFFFFULL);
                carry = cur >> 32;
            }
        }
        return new JSBigInt(res_neg, std::move(res));
    }

    static void DivRemInternal(const std::vector<uint32_t>& u, const std::vector<uint32_t>& v,
                               std::vector<uint32_t>& q, std::vector<uint32_t>& r) {
        if (v.empty()) return;
        if (u.empty()) {
            q.clear();
            r.clear();
            return;
        }
        if (u.size() < v.size()) {
            q.clear();
            r = u;
            return;
        }
        if (u.size() == v.size()) {
            bool u_smaller = false;
            for (size_t i = u.size(); i-- > 0; ) {
                if (u[i] != v[i]) {
                    if (u[i] < v[i]) u_smaller = true;
                    break;
                }
            }
            if (u_smaller) {
                q.clear();
                r = u;
                return;
            }
        }
        if (v.size() == 1) {
            uint64_t divisor = v[0];
            uint64_t rem = 0;
            q.resize(u.size(), 0);
            for (size_t i = u.size(); i-- > 0; ) {
                uint64_t cur = (rem << 32) | u[i];
                q[i] = static_cast<uint32_t>(cur / divisor);
                rem = cur % divisor;
            }
            while (!q.empty() && q.back() == 0) q.pop_back();
            if (rem > 0) r.push_back(static_cast<uint32_t>(rem));
            return;
        }
        r.clear();
        q.resize(u.size(), 0);
        size_t top_bits = 0;
        for (uint32_t t = u.back(); t; t >>= 1) top_bits++;
        size_t total_bits = (u.size() - 1) * 32 + top_bits;

        for (size_t bit = total_bits; bit-- > 0; ) {
            uint32_t carry = 0;
            for (size_t i = 0; i < r.size(); ++i) {
                uint64_t shifted = (static_cast<uint64_t>(r[i]) << 1) | carry;
                r[i] = static_cast<uint32_t>(shifted & 0xFFFFFFFFULL);
                carry = static_cast<uint32_t>(shifted >> 32);
            }
            if (carry) r.push_back(carry);

            size_t u_limb_idx = bit / 32;
            size_t u_bit_idx = bit % 32;
            uint32_t in_bit = (u[u_limb_idx] >> u_bit_idx) & 1;
            if (in_bit) {
                if (r.empty()) r.push_back(1);
                else r[0] |= 1;
            }
            while (!r.empty() && r.back() == 0) r.pop_back();

            bool r_ge_v = false;
            if (r.size() > v.size()) {
                r_ge_v = true;
            } else if (r.size() == v.size()) {
                r_ge_v = true;
                for (size_t i = r.size(); i-- > 0; ) {
                    if (r[i] != v[i]) {
                        r_ge_v = (r[i] > v[i]);
                        break;
                    }
                }
            }

            if (r_ge_v) {
                uint64_t borrow = 0;
                for (size_t i = 0; i < r.size(); ++i) {
                    uint64_t a = r[i];
                    uint64_t b = (i < v.size()) ? v[i] : 0;
                    if (a >= b + borrow) {
                        r[i] = static_cast<uint32_t>(a - b - borrow);
                        borrow = 0;
                    } else {
                        r[i] = static_cast<uint32_t>((a + (1ULL << 32)) - b - borrow);
                        borrow = 1;
                    }
                }
                while (!r.empty() && r.back() == 0) r.pop_back();
                q[bit / 32] |= (1U << (bit % 32));
            }
        }
        while (!q.empty() && q.back() == 0) q.pop_back();
    }

    JSBigInt* Divide(const JSBigInt& other) const {
        if (other.limbs_.empty()) return nullptr; // Signal division by zero (RangeError)
        if (limbs_.empty()) return new JSBigInt(0);
        std::vector<uint32_t> q, r;
        DivRemInternal(limbs_, other.limbs_, q, r);
        return new JSBigInt(is_negative_ != other.is_negative_, std::move(q));
    }

    JSBigInt* Remainder(const JSBigInt& other) const {
        if (other.limbs_.empty()) return nullptr; // Signal division by zero (RangeError)
        if (limbs_.empty()) return new JSBigInt(0);
        std::vector<uint32_t> q, r;
        DivRemInternal(limbs_, other.limbs_, q, r);
        return new JSBigInt(is_negative_, std::move(r));
    }

    JSBigInt* Exponentiate(const JSBigInt& other) const {
        if (other.is_negative_) return nullptr; // Signal negative exponent (RangeError)
        if (other.limbs_.empty()) return new JSBigInt(1);
        if (limbs_.empty()) return new JSBigInt(0);

        JSBigInt* base = new JSBigInt(is_negative_, limbs_);
        JSBigInt* result = new JSBigInt(1);

        auto exp_limbs = other.limbs_;
        size_t top_bits = 0;
        for (uint32_t t = exp_limbs.back(); t; t >>= 1) top_bits++;
        size_t total_bits = (exp_limbs.size() - 1) * 32 + top_bits;

        for (size_t bit = 0; bit < total_bits; ++bit) {
            uint32_t bit_val = (exp_limbs[bit / 32] >> (bit % 32)) & 1;
            if (bit_val) {
                JSBigInt* next = result->Multiply(*base);
                delete result;
                result = next;
            }
            if (bit + 1 < total_bits) {
                JSBigInt* next_base = base->Multiply(*base);
                delete base;
                base = next_base;
            }
        }
        delete base;
        return result;
    }

    bool LessThan(const JSBigInt& other) const noexcept {
        if (is_negative_ != other.is_negative_) {
            return is_negative_;
        }
        if (limbs_.size() != other.limbs_.size()) {
            return is_negative_ ? (limbs_.size() > other.limbs_.size()) : (limbs_.size() < other.limbs_.size());
        }
        for (size_t i = limbs_.size(); i-- > 0; ) {
            if (limbs_[i] != other.limbs_[i]) {
                return is_negative_ ? (limbs_[i] > other.limbs_[i]) : (limbs_[i] < other.limbs_[i]);
            }
        }
        return false;
    }

    JSBigInt* Negate() const {
        if (limbs_.empty()) return new JSBigInt(0);
        return new JSBigInt(!is_negative_, limbs_);
    }

    JSBigInt* BitwiseNot() const {
        // ~x == -x - 1
        if (limbs_.empty()) {
            // ~0n == -1n
            return new JSBigInt(-1);
        }
        if (!is_negative_) {
            // x > 0 => -(x + 1)
            auto new_limbs = limbs_;
            uint64_t carry = 1;
            for (size_t i = 0; i < new_limbs.size() && carry; ++i) {
                uint64_t sum = static_cast<uint64_t>(new_limbs[i]) + carry;
                new_limbs[i] = static_cast<uint32_t>(sum & 0xFFFFFFFFULL);
                carry = sum >> 32;
            }
            if (carry) new_limbs.push_back(static_cast<uint32_t>(carry));
            return new JSBigInt(true, std::move(new_limbs));
        } else {
            // x < 0 => +(|x| - 1)
            auto new_limbs = limbs_;
            uint64_t borrow = 1;
            for (size_t i = 0; i < new_limbs.size() && borrow; ++i) {
                if (new_limbs[i] >= borrow) {
                    new_limbs[i] -= static_cast<uint32_t>(borrow);
                    borrow = 0;
                } else {
                    new_limbs[i] = 0xFFFFFFFFU;
                    borrow = 1;
                }
            }
            while (!new_limbs.empty() && new_limbs.back() == 0) new_limbs.pop_back();
            return new JSBigInt(false, std::move(new_limbs));
        }
    }

    enum class BitwiseOp { And, Or, Xor };

    JSBigInt* BitwiseBinary(const JSBigInt& other, BitwiseOp op) const {
        bool x_is_neg = is_negative_ && !limbs_.empty();
        bool y_is_neg = other.is_negative_ && !other.limbs_.empty();

        bool res_is_neg = false;
        if (op == BitwiseOp::And) res_is_neg = (x_is_neg && y_is_neg);
        else if (op == BitwiseOp::Or) res_is_neg = (x_is_neg || y_is_neg);
        else if (op == BitwiseOp::Xor) res_is_neg = (x_is_neg != y_is_neg);

        auto get_mag_m1 = [](const JSBigInt& bi) {
            auto res = bi.limbs_;
            uint64_t borrow = 1;
            for (size_t i = 0; i < res.size() && borrow; ++i) {
                if (res[i] >= borrow) {
                    res[i] -= static_cast<uint32_t>(borrow);
                    borrow = 0;
                } else {
                    res[i] = 0xFFFFFFFFU;
                    borrow = 1;
                }
            }
            while (!res.empty() && res.back() == 0) res.pop_back();
            return res;
        };

        std::vector<uint32_t> x_m1 = x_is_neg ? get_mag_m1(*this) : std::vector<uint32_t>{};
        std::vector<uint32_t> y_m1 = y_is_neg ? get_mag_m1(other) : std::vector<uint32_t>{};

        size_t max_len = std::max(limbs_.size(), other.limbs_.size());
        if (x_is_neg) max_len = std::max(max_len, x_m1.size());
        if (y_is_neg) max_len = std::max(max_len, y_m1.size());
        if (max_len == 0) {
            return new JSBigInt(res_is_neg ? -1 : 0);
        }

        std::vector<uint32_t> c(max_len);
        for (size_t i = 0; i < max_len; ++i) {
            uint32_t a = x_is_neg ? ~((i < x_m1.size()) ? x_m1[i] : 0U)
                                  : ((i < limbs_.size()) ? limbs_[i] : 0U);
            uint32_t b = y_is_neg ? ~((i < y_m1.size()) ? y_m1[i] : 0U)
                                  : ((i < other.limbs_.size()) ? other.limbs_[i] : 0U);
            if (op == BitwiseOp::And) c[i] = a & b;
            else if (op == BitwiseOp::Or) c[i] = a | b;
            else if (op == BitwiseOp::Xor) c[i] = a ^ b;
        }

        if (!res_is_neg) {
            while (!c.empty() && c.back() == 0) c.pop_back();
            return new JSBigInt(false, std::move(c));
        } else {
            uint64_t carry = 1;
            for (size_t i = 0; i < c.size(); ++i) {
                uint64_t sum = static_cast<uint64_t>(static_cast<uint32_t>(~c[i])) + carry;
                c[i] = static_cast<uint32_t>(sum & 0xFFFFFFFFULL);
                carry = sum >> 32;
            }
            if (carry) c.push_back(static_cast<uint32_t>(carry));
            while (!c.empty() && c.back() == 0) c.pop_back();
            return new JSBigInt(true, std::move(c));
        }
    }

    JSBigInt* BitwiseAnd(const JSBigInt& other) const {
        return BitwiseBinary(other, BitwiseOp::And);
    }

    JSBigInt* BitwiseOr(const JSBigInt& other) const {
        return BitwiseBinary(other, BitwiseOp::Or);
    }

    JSBigInt* BitwiseXor(const JSBigInt& other) const {
        return BitwiseBinary(other, BitwiseOp::Xor);
    }

    JSBigInt* ShiftLeft(const JSBigInt& other) const {
        if (other.is_negative_ && !other.limbs_.empty()) {
            JSBigInt* neg_other = other.Negate();
            JSBigInt* res = ShiftRight(*neg_other);
            delete neg_other;
            return res;
        }
        if (other.limbs_.empty() || limbs_.empty()) {
            return new JSBigInt(is_negative_, limbs_);
        }
        if (other.limbs_.size() > 1 || other.limbs_[0] > 1000000) {
            return nullptr;
        }
        uint32_t shift = other.limbs_[0];
        uint32_t word_shift = shift / 32;
        uint32_t bit_shift = shift % 32;
        std::vector<uint32_t> res(word_shift, 0);
        uint64_t carry = 0;
        for (uint32_t l : limbs_) {
            uint64_t val = (static_cast<uint64_t>(l) << bit_shift) | carry;
            res.push_back(static_cast<uint32_t>(val & 0xFFFFFFFFULL));
            carry = val >> 32;
        }
        if (carry) res.push_back(static_cast<uint32_t>(carry));
        return new JSBigInt(is_negative_, std::move(res));
    }

    JSBigInt* ShiftRight(const JSBigInt& other) const {
        if (other.is_negative_ && !other.limbs_.empty()) {
            JSBigInt* neg_other = other.Negate();
            JSBigInt* res = ShiftLeft(*neg_other);
            delete neg_other;
            return res;
        }
        if (other.limbs_.empty() || limbs_.empty()) {
            return new JSBigInt(is_negative_, limbs_);
        }
        if (other.limbs_.size() > 1 || other.limbs_[0] >= limbs_.size() * 32) {
            if (is_negative_) return new JSBigInt(-1);
            else return new JSBigInt(0);
        }
        uint32_t shift = other.limbs_[0];
        uint32_t word_shift = shift / 32;
        uint32_t bit_shift = shift % 32;
        bool has_lost_bits = false;
        for (uint32_t i = 0; i < word_shift && i < limbs_.size(); ++i) {
            if (limbs_[i] != 0) has_lost_bits = true;
        }
        if (bit_shift > 0 && word_shift < limbs_.size()) {
            if ((limbs_[word_shift] & ((1U << bit_shift) - 1)) != 0) {
                has_lost_bits = true;
            }
        }
        std::vector<uint32_t> res;
        for (size_t i = word_shift; i < limbs_.size(); ++i) {
            uint64_t val = (static_cast<uint64_t>(limbs_[i]) >> bit_shift);
            if (i + 1 < limbs_.size() && bit_shift > 0) {
                val |= (static_cast<uint64_t>(limbs_[i + 1]) << (32 - bit_shift)) & 0xFFFFFFFFULL;
            }
            res.push_back(static_cast<uint32_t>(val));
        }
        while (!res.empty() && res.back() == 0) res.pop_back();
        if (is_negative_ && has_lost_bits) {
            uint64_t carry = 1;
            for (size_t i = 0; i < res.size() && carry; ++i) {
                uint64_t sum = static_cast<uint64_t>(res[i]) + carry;
                res[i] = static_cast<uint32_t>(sum & 0xFFFFFFFFULL);
                carry = sum >> 32;
            }
            if (carry) res.push_back(static_cast<uint32_t>(carry));
        }
        return new JSBigInt(is_negative_, std::move(res));
    }

    std::string StringValue() const {
        if (limbs_.empty()) return "0";
        std::vector<uint32_t> copy = limbs_;
        std::string res;
        while (!copy.empty()) {
            uint64_t rem = 0;
            for (size_t i = copy.size(); i-- > 0; ) {
                uint64_t cur = (rem << 32) | copy[i];
                copy[i] = static_cast<uint32_t>(cur / 10);
                rem = cur % 10;
            }
            while (!copy.empty() && copy.back() == 0) copy.pop_back();
            res.push_back('0' + static_cast<char>(rem));
        }
        if (is_negative_) res.push_back('-');
        std::reverse(res.begin(), res.end());
        return res;
    }

    size_t ObjectSize() const override { return sizeof(JSBigInt); }
    void IteratePointers(const std::function<void(TaggedValue&)>&) override {}
    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSBigInt(std::move(*this));
    }

private:
    void Normalize() {
        while (!limbs_.empty() && limbs_.back() == 0) {
            limbs_.pop_back();
        }
        if (limbs_.empty()) {
            is_negative_ = false;
        }
    }

    void FromString(std::string_view s) {
        limbs_.clear();
        is_negative_ = false;
        if (s.empty()) return;
        size_t idx = 0;
        if (s[idx] == '-') {
            is_negative_ = true;
            idx++;
        } else if (s[idx] == '+') {
            idx++;
        }
        if (idx >= s.size()) return;

        int base = 10;
        if (s.size() - idx >= 2 && s[idx] == '0') {
            char p = s[idx + 1];
            if (p == 'x' || p == 'X') {
                base = 16;
                idx += 2;
            } else if (p == 'b' || p == 'B') {
                base = 2;
                idx += 2;
            } else if (p == 'o' || p == 'O') {
                base = 8;
                idx += 2;
            }
        }

        for (; idx < s.size(); ++idx) {
            char c = s[idx];
            if (c == 'n' || c == 'N') break;
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;

            if (d >= static_cast<uint32_t>(base)) break;

            uint64_t carry = d;
            for (size_t i = 0; i < limbs_.size(); ++i) {
                uint64_t prod = static_cast<uint64_t>(limbs_[i]) * base + carry;
                limbs_[i] = static_cast<uint32_t>(prod & 0xFFFFFFFFULL);
                carry = prod >> 32;
            }
            if (carry) {
                limbs_.push_back(static_cast<uint32_t>(carry));
            }
        }
        Normalize();
    }

    bool is_negative_{false};
    std::vector<uint32_t> limbs_;
};

} // namespace aether

#endif // AETHER_HEAP_OBJECT_H
