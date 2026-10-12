#ifndef AETHER_HEAP_OBJECT_H
#define AETHER_HEAP_OBJECT_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <new>
#include <utility>

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
               type_ == HeapObjectType::JSProxy;
    }
    AETHER_ALWAYS_INLINE bool IsECMAObject() const noexcept {
        return IsJSObject() || IsFunction();
    }
    AETHER_ALWAYS_INLINE bool IsPlainObject() const noexcept { return type_ == HeapObjectType::JSObject; }
    AETHER_ALWAYS_INLINE bool IsString() const noexcept { return type_ == HeapObjectType::String; }
    AETHER_ALWAYS_INLINE bool IsArray() const noexcept { return type_ == HeapObjectType::JSArray; }
    AETHER_ALWAYS_INLINE bool IsFunction() const noexcept { return type_ == HeapObjectType::JSFunction || type_ == HeapObjectType::JSNativeFunction; }
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
          prototype_(other.prototype_),
          out_of_line_slots_(std::move(other.out_of_line_slots_)) {
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
    bool DeleteNamedProperty(std::string_view key);
    std::vector<std::string> GetOwnPropertyNames() const;

    JSObject* GetPrototype() const noexcept { return prototype_; }
    void SetPrototype(JSObject* proto) noexcept {
        prototype_ = proto;
        if (AETHER_UNLIKELY(IsOld() && proto != nullptr)) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(proto));
        }
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
    }

protected:
    JSObject(HeapObjectType type, Shape* shape)
        : HeapObject(type, shape) {
        for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
            in_object_slots_[i] = TaggedValue::Empty();
        }
    }

private:
    JSObject* prototype_{nullptr};
    TaggedValue in_object_slots_[Shape::kDefaultInObjectProperties];
    std::vector<TaggedValue> out_of_line_slots_;
};

/**
 * @brief Managed JavaScript Array with contiguous elements and length property.
 */
class JSArray : public JSObject {
public:
    explicit JSArray(Shape* shape, size_t initial_capacity = 0)
        : JSObject(HeapObjectType::JSArray, shape) {
        if (initial_capacity > 0) {
            elements_.resize(initial_capacity, TaggedValue::Undefined());
        }
    }

    explicit JSArray(Shape* shape, std::vector<TaggedValue> elements)
        : JSObject(HeapObjectType::JSArray, shape), elements_(std::move(elements)) {}

    JSArray(JSArray&& other) noexcept
        : JSObject(std::move(other)),
          elements_(std::move(other.elements_)) {}

    size_t Length() const noexcept { return elements_.size(); }
    void SetLength(size_t len) { elements_.resize(len, TaggedValue::Undefined()); }

    TaggedValue GetElement(size_t index) const {
        if (index < elements_.size()) {
            return elements_[index];
        }
        return TaggedValue::Undefined();
    }

    void SetElement(size_t index, TaggedValue val) {
        if (index >= elements_.size()) {
            elements_.resize(index + 1, TaggedValue::Undefined());
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
    JSFunction(std::string_view name, std::shared_ptr<const BytecodeArray> bytecode, LexicalEnvironment* env = nullptr, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSFunction, shape),
          name_(name),
          bytecode_shared_(std::move(bytecode)),
          bytecode_(bytecode_shared_.get()),
          environment_(env) {}

    JSFunction(std::string_view name, const BytecodeArray* bytecode, LexicalEnvironment* env = nullptr, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSFunction, shape),
          name_(name),
          bytecode_(bytecode),
          environment_(env) {}

    JSFunction(JSFunction&& other) noexcept
        : JSObject(std::move(other)),
          name_(std::move(other.name_)),
          bytecode_shared_(std::move(other.bytecode_shared_)),
          bytecode_(other.bytecode_),
          environment_(other.environment_),
          is_generator_(other.is_generator_),
          is_async_(other.is_async_) {}

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

    size_t ObjectSize() const override { return sizeof(JSFunction); }
    void IteratePointers(const std::function<void(TaggedValue&)>& visitor) override {
        JSObject::IteratePointers(visitor);
        if (environment_ != nullptr) {
            TaggedValue env_val = TaggedValue::FromHeapObject(environment_);
            visitor(env_val);
            if (env_val.IsHeapObject()) {
                environment_ = env_val.As<LexicalEnvironment>();
            }
        }
    }

    HeapObject* RelocateTo(void* dst) override {
        return new (dst) JSFunction(std::move(*this));
    }

private:
    std::string name_;
    std::shared_ptr<const BytecodeArray> bytecode_shared_{nullptr};
    const BytecodeArray* bytecode_{nullptr};
    LexicalEnvironment* environment_{nullptr};
    bool is_generator_{false};
    bool is_async_{false};
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
    JSError(std::string_view name, std::string_view message, Shape* shape = nullptr)
        : JSObject(HeapObjectType::JSError, shape),
          name_(name),
          message_(message) {
        uint32_t sid = 8000;
        SetNamedProperty("name", TaggedValue::FromHeapObject(new JSString(name_)), sid);
        SetNamedProperty("message", TaggedValue::FromHeapObject(new JSString(message_)), sid);
    }
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
          is_revoked_(false) {}
    JSProxy(JSProxy&& other) noexcept = default;

    AETHER_ALWAYS_INLINE TaggedValue GetTarget() const noexcept { return target_; }
    AETHER_ALWAYS_INLINE void SetTarget(TaggedValue t) noexcept { target_ = t; }

    AETHER_ALWAYS_INLINE TaggedValue GetHandler() const noexcept { return handler_; }
    AETHER_ALWAYS_INLINE void SetHandler(TaggedValue h) noexcept { handler_ = h; }

    AETHER_ALWAYS_INLINE bool IsRevoked() const noexcept { return is_revoked_; }
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
};

} // namespace aether

#endif // AETHER_HEAP_OBJECT_H
