#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/typed_array.h"
#include "aether/bytecode.h"
#include "aether/bounded_regex.h"
#include <cctype>
#include <cmath>

namespace aether {

namespace {
thread_local size_t property_lookup_depth = 0;
struct PropertyLookupScope {
    PropertyLookupScope() { ++property_lookup_depth; }
    ~PropertyLookupScope() { --property_lookup_depth; }
};
} // namespace

static bool IsAllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

static bool TryParseArrayIndex(std::string_view s, size_t& index) noexcept {
    if (s.empty()) return false;
    size_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const size_t digit = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    index = value;
    return true;
}

static bool SameValueZero(TaggedValue a, TaggedValue b) {
    if (a == b) return true;
    if (a.IsNumber() && b.IsNumber()) {
        double da = a.AsNumber();
        double db = b.AsNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    if (a.IsHeapObject() && b.IsHeapObject()) {
        HeapObject* ha = a.AsHeapObject();
        HeapObject* hb = b.AsHeapObject();
        if (ha->IsString() && hb->IsString()) {
            return static_cast<JSString*>(ha)->Equals(static_cast<JSString*>(hb));
        }
        return ha == hb;
    }
    return false;
}

// ============================================================================
// JSString Implementation
// ============================================================================

JSString::JSString(std::string_view str, Shape* shape)
    : HeapObject(HeapObjectType::String, shape) {
    InitializeFromUtf8(str);
}

JSString::JSString(const uint16_t* utf16_data, uint32_t length, Shape* shape)
    : HeapObject(HeapObjectType::String, shape),
      length_(length),
      right_(nullptr) {
    if (length == 0 || !utf16_data) {
        length_ = 0;
        kind_ = StringKind::FlatLatin1;
        str_flags_ = kFlagIsLatin1;
        depth_ = 0;
        payload_ = nullptr;
        hash_ = 2166136261u;
        return;
    }
    CheckedStringLength(length);
    depth_ = 0;
    bool fits_latin1 = true;
    for (uint32_t i = 0; i < length; ++i) {
        if (utf16_data[i] > 255) {
            fits_latin1 = false;
            break;
        }
    }
    if (fits_latin1) {
        kind_ = StringKind::FlatLatin1;
        str_flags_ = kFlagIsLatin1;
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, length + 1);
        for (uint32_t i = 0; i < length; ++i) {
            ba->data()[i] = static_cast<uint8_t>(utf16_data[i]);
        }
        ba->data()[length] = '\0';
        payload_ = ba;
    } else {
        kind_ = StringKind::FlatUtf16;
        str_flags_ = 0;
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, (length + 1) * static_cast<uint32_t>(sizeof(uint16_t)));
        uint16_t* dst = reinterpret_cast<uint16_t*>(ba->data());
        std::memcpy(dst, utf16_data, length * sizeof(uint16_t));
        dst[length] = 0;
        payload_ = ba;
    }
    if (AETHER_UNLIKELY(IsOld() && payload_ && payload_->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(payload_));
    }
    ComputeHash();
}

JSString::JSString(JSString* left, JSString* right, Shape* shape)
    : HeapObject(HeapObjectType::String, shape),
      kind_(StringKind::Cons),
      left_(left),
      right_(right) {
    length_ = (left ? static_cast<uint32_t>(left->Length()) : 0) +
              (right ? static_cast<uint32_t>(right->Length()) : 0);
    slice_offset_ = 0;
    hash_ = 0;
    depth_ = static_cast<uint8_t>(std::max<int>(left ? left->depth_ : 0, right ? right->depth_ : 0) + 1);
    if (left && right && left->IsLatin1() && right->IsLatin1()) {
        str_flags_ |= kFlagIsLatin1;
    }
    if (AETHER_UNLIKELY(IsOld())) {
        if (left_ && left_->IsYoung()) RecordWriteBarrier(this, TaggedValue::FromHeapObject(left_));
        if (right_ && right_->IsYoung()) RecordWriteBarrier(this, TaggedValue::FromHeapObject(right_));
    }
}

JSString::JSString(JSString* parent, uint32_t offset, uint32_t length, Shape* shape)
    : HeapObject(HeapObjectType::String, shape),
      length_(length),
      kind_(StringKind::Sliced),
      slice_offset_(offset),
      parent_(parent),
      right_(nullptr) {
    hash_ = 0;
    depth_ = 0;
    if (parent && parent->IsLatin1()) {
        str_flags_ |= kFlagIsLatin1;
    }
    if (AETHER_UNLIKELY(IsOld() && parent_ && parent_->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(parent_));
    }
}

namespace {
static inline uint32_t DecodeNextCodePoint(const uint8_t*& ptr, const uint8_t* end) noexcept {
    uint8_t b0 = *ptr++;
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && ptr < end) return ((b0 & 0x1F) << 6) | (*ptr++ & 0x3F);
    if ((b0 & 0xF0) == 0xE0 && ptr + 1 < end) {
        uint32_t cp = ((b0 & 0x0F) << 12) | ((ptr[0] & 0x3F) << 6) | (ptr[1] & 0x3F);
        ptr += 2;
        return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && ptr + 2 < end) {
        uint32_t cp = ((b0 & 0x07) << 18) | ((ptr[0] & 0x3F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
        ptr += 3;
        return cp;
    }
    return b0;
}

static inline void AppendUtf16CodeUnitToUtf8(std::string& out, uint16_t cu, const uint16_t* next_cu) {
    if (cu >= 0xD800 && cu <= 0xDBFF && next_cu && *next_cu >= 0xDC00 && *next_cu <= 0xDFFF) {
        uint32_t cp = 0x10000 + (((cu - 0xD800) << 10) | (*next_cu - 0xDC00));
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cu < 0x80) {
        out.push_back(static_cast<char>(cu));
    } else if (cu < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cu >> 6)));
        out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cu >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cu >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cu & 0x3F)));
    }
}
} // namespace

void JSString::InitializeFromUtf8(std::string_view utf8_str) {
    CheckedStringLength(utf8_str.length());
    slice_offset_ = 0;
    right_ = nullptr;
    depth_ = 0;
    if (utf8_str.empty()) {
        length_ = 0;
        kind_ = StringKind::FlatLatin1;
        str_flags_ = kFlagIsLatin1;
        payload_ = nullptr;
        hash_ = 2166136261u;
        return;
    }

    bool is_ascii = true;
    for (unsigned char c : utf8_str) {
        if (c >= 0x80) { is_ascii = false; break; }
    }

    if (is_ascii) {
        length_ = static_cast<uint32_t>(utf8_str.length());
        kind_ = StringKind::FlatLatin1;
        str_flags_ = kFlagIsLatin1;
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, length_ + 1);
        std::memcpy(ba->data(), utf8_str.data(), length_);
        ba->data()[length_] = '\0';
        payload_ = ba;
        if (AETHER_UNLIKELY(IsOld() && payload_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(payload_));
        }
        ComputeHash();
        return;
    }

    bool fits_latin1 = true;
    uint32_t u16_len = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(utf8_str.data());
    const uint8_t* end = ptr + utf8_str.length();
    const uint8_t* scan = ptr;
    while (scan < end) {
        uint32_t cp = DecodeNextCodePoint(scan, end);
        u16_len += (cp > 0xFFFF) ? 2 : 1;
        if (cp > 255) fits_latin1 = false;
    }

    length_ = u16_len;
    if (fits_latin1) {
        kind_ = StringKind::FlatLatin1;
        str_flags_ = kFlagIsLatin1;
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, length_ + 1);
        uint8_t* dst = ba->data();
        scan = ptr;
        for (uint32_t idx = 0; scan < end && idx < length_; ++idx) {
            dst[idx] = static_cast<uint8_t>(DecodeNextCodePoint(scan, end));
        }
        dst[length_] = '\0';
        payload_ = ba;
    } else {
        kind_ = StringKind::FlatUtf16;
        str_flags_ = 0;
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, (length_ + 1) * static_cast<uint32_t>(sizeof(uint16_t)));
        uint16_t* dst = reinterpret_cast<uint16_t*>(ba->data());
        scan = ptr;
        uint32_t idx = 0;
        while (scan < end && idx < length_) {
            uint32_t cp = DecodeNextCodePoint(scan, end);
            if (cp > 0xFFFF) {
                dst[idx++] = static_cast<uint16_t>(0xD800 + ((cp - 0x10000) >> 10));
                dst[idx++] = static_cast<uint16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
            } else {
                dst[idx++] = static_cast<uint16_t>(cp);
            }
        }
        dst[length_] = 0;
        payload_ = ba;
    }

    if (AETHER_UNLIKELY(IsOld() && payload_->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(payload_));
    }
    ComputeHash();
}

void JSString::FlattenToLatin1(uint8_t* dst) const noexcept {
    std::vector<const JSString*> stack;
    stack.push_back(this);
    uint32_t write_pos = 0;
    while (!stack.empty()) {
        const JSString* s = stack.back();
        stack.pop_back();
        if (!s || s->length_ == 0) continue;
        if (s->kind_ == StringKind::FlatLatin1) {
            if (s->payload_) {
                std::memcpy(dst + write_pos, s->payload_->data(), s->length_);
            }
            write_pos += s->length_;
        } else if (s->kind_ == StringKind::FlatUtf16) {
            if (s->payload_) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(s->payload_->data());
                for (uint32_t i = 0; i < s->length_; ++i) {
                    dst[write_pos++] = static_cast<uint8_t>(src[i]);
                }
            }
        } else if (s->kind_ == StringKind::Cons) {
            if (s->right_) stack.push_back(s->right_);
            if (s->left_) stack.push_back(s->left_);
        } else if (s->kind_ == StringKind::Sliced) {
            if (s->parent_) {
                if (s->parent_->kind_ == StringKind::FlatLatin1 && s->parent_->payload_) {
                    std::memcpy(dst + write_pos, s->parent_->payload_->data() + s->slice_offset_, s->length_);
                    write_pos += s->length_;
                } else {
                    for (uint32_t i = 0; i < s->length_; ++i) {
                        dst[write_pos++] = static_cast<uint8_t>(s->CharCodeAt(i));
                    }
                }
            }
        }
    }
}

void JSString::FlattenToUtf16(uint16_t* dst) const noexcept {
    std::vector<const JSString*> stack;
    stack.push_back(this);
    uint32_t write_pos = 0;
    while (!stack.empty()) {
        const JSString* s = stack.back();
        stack.pop_back();
        if (!s || s->length_ == 0) continue;
        if (s->kind_ == StringKind::FlatLatin1) {
            if (s->payload_) {
                const uint8_t* src = s->payload_->data();
                for (uint32_t i = 0; i < s->length_; ++i) {
                    dst[write_pos++] = static_cast<uint16_t>(src[i]);
                }
            } else {
                write_pos += s->length_;
            }
        } else if (s->kind_ == StringKind::FlatUtf16) {
            if (s->payload_) {
                std::memcpy(dst + write_pos, s->payload_->data(), s->length_ * sizeof(uint16_t));
            }
            write_pos += s->length_;
        } else if (s->kind_ == StringKind::Cons) {
            if (s->right_) stack.push_back(s->right_);
            if (s->left_) stack.push_back(s->left_);
        } else if (s->kind_ == StringKind::Sliced) {
            if (s->parent_) {
                if (s->parent_->kind_ == StringKind::FlatLatin1 && s->parent_->payload_) {
                    const uint8_t* src = s->parent_->payload_->data() + s->slice_offset_;
                    for (uint32_t i = 0; i < s->length_; ++i) {
                        dst[write_pos++] = static_cast<uint16_t>(src[i]);
                    }
                } else if (s->parent_->kind_ == StringKind::FlatUtf16 && s->parent_->payload_) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(s->parent_->payload_->data()) + s->slice_offset_;
                    std::memcpy(dst + write_pos, src, s->length_ * sizeof(uint16_t));
                    write_pos += s->length_;
                } else {
                    for (uint32_t i = 0; i < s->length_; ++i) {
                        dst[write_pos++] = s->CharCodeAt(i);
                    }
                }
            }
        }
    }
}

void JSString::EnsureFlat() const noexcept {
    if (kind_ == StringKind::FlatLatin1 || kind_ == StringKind::FlatUtf16) {
        return;
    }
    auto* self = const_cast<JSString*>(this);
    if (length_ == 0) {
        self->kind_ = StringKind::FlatLatin1;
        self->str_flags_ |= kFlagIsLatin1;
        self->payload_ = nullptr;
        self->right_ = nullptr;
        self->depth_ = 0;
        return;
    }

    bool is_latin1 = (str_flags_ & kFlagIsLatin1) != 0;
    if (!is_latin1) {
        bool check_latin1 = true;
        for (uint32_t i = 0; i < length_; ++i) {
            if (CharCodeAt(i) > 255) {
                check_latin1 = false;
                break;
            }
        }
        if (check_latin1) {
            is_latin1 = true;
            self->str_flags_ |= kFlagIsLatin1;
        }
    }

    if (is_latin1) {
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, length_ + 1);
        uint8_t* dst = ba->data();
        FlattenToLatin1(dst);
        dst[length_] = '\0';
        self->payload_ = ba;
        self->kind_ = StringKind::FlatLatin1;
        self->right_ = nullptr;
        self->depth_ = 0;
        if (AETHER_UNLIKELY(IsOld() && self->payload_->IsYoung())) {
            RecordWriteBarrier(self, TaggedValue::FromHeapObject(self->payload_));
        }
    } else {
        ByteArray* ba = AllocateRuntimeByteArrayFor(this, (length_ + 1) * static_cast<uint32_t>(sizeof(uint16_t)));
        uint16_t* dst = reinterpret_cast<uint16_t*>(ba->data());
        FlattenToUtf16(dst);
        dst[length_] = 0;
        self->payload_ = ba;
        self->kind_ = StringKind::FlatUtf16;
        self->right_ = nullptr;
        self->depth_ = 0;
        if (AETHER_UNLIKELY(IsOld() && self->payload_->IsYoung())) {
            RecordWriteBarrier(self, TaggedValue::FromHeapObject(self->payload_));
        }
    }
}

JSString* JSString::Concat(JSString* left, JSString* right) {
    if (!left || left->length_ == 0) return right ? right : AllocateRuntime<JSString>("");
    if (!right || right->length_ == 0) return left;

    uint64_t total = static_cast<uint64_t>(left->length_) + right->length_;
    if (AETHER_UNLIKELY(total > kMaxStringLength)) {
        throw std::length_error("String length exceeds maximum allowed length");
    }

    uint32_t total_len = static_cast<uint32_t>(total);
    if (total_len <= 14 && left->IsLatin1() && right->IsLatin1()) {
        ByteArray* ba = AllocateRuntimeByteArrayFor(nullptr, total_len + 1);
        uint8_t* dst = ba->data();
        for (uint32_t i = 0; i < left->length_; ++i) dst[i] = static_cast<uint8_t>(left->CharCodeAt(i));
        for (uint32_t i = 0; i < right->length_; ++i) dst[left->length_ + i] = static_cast<uint8_t>(right->CharCodeAt(i));
        dst[total_len] = '\0';
        auto* res = AllocateRuntime<JSString>("");
        res->length_ = total_len;
        res->kind_ = StringKind::FlatLatin1;
        res->str_flags_ = kFlagIsLatin1;
        res->payload_ = ba;
        res->ComputeHash();
        return res;
    }

    if (left->depth_ >= kMaxConsDepth) {
        left->EnsureFlat();
    }
    if (right->depth_ >= kMaxConsDepth) {
        right->EnsureFlat();
    }

    return AllocateRuntime<JSString>(left, right);
}

JSString* JSString::Slice(JSString* str, uint32_t offset, uint32_t length) {
    if (!str || length == 0 || offset >= str->length_) return AllocateRuntime<JSString>("");
    if (offset + length > str->length_) length = str->length_ - offset;
    if (offset == 0 && length == str->length_) return str;

    if (length <= 8 && str->IsLatin1()) {
        ByteArray* ba = AllocateRuntimeByteArrayFor(nullptr, length + 1);
        uint8_t* dst = ba->data();
        for (uint32_t i = 0; i < length; ++i) dst[i] = static_cast<uint8_t>(str->CharCodeAt(offset + i));
        dst[length] = '\0';
        auto* res = AllocateRuntime<JSString>("");
        res->length_ = length;
        res->kind_ = StringKind::FlatLatin1;
        res->str_flags_ = kFlagIsLatin1;
        res->payload_ = ba;
        res->ComputeHash();
        return res;
    }

    JSString* root = str;
    uint32_t root_offset = offset;
    if (root->kind_ == StringKind::Sliced) {
        root_offset += root->slice_offset_;
        root = root->parent_;
    }

    if (root && root->kind_ == StringKind::Cons) {
        root->EnsureFlat();
    }

    return AllocateRuntime<JSString>(root, root_offset, length);
}

void JSString::ComputeHash() const noexcept {
    if (kind_ != StringKind::FlatLatin1 && kind_ != StringKind::FlatUtf16) {
        EnsureFlat();
    }
    uint32_t h = 2166136261u;
    if (kind_ == StringKind::FlatLatin1 && payload_ != nullptr) {
        const uint8_t* p = payload_->data();
        for (uint32_t i = 0; i < length_; ++i) {
            h ^= p[i];
            h *= 16777619u;
        }
    } else if (kind_ == StringKind::FlatUtf16 && payload_ != nullptr) {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(payload_->data());
        for (uint32_t i = 0; i < length_; ++i) {
            uint16_t cu = p[i];
            h ^= static_cast<uint8_t>(cu & 0xFF);
            h *= 16777619u;
            h ^= static_cast<uint8_t>(cu >> 8);
            h *= 16777619u;
        }
    }
    if (h == 0) h = 1;
    hash_ = h;
}

uint16_t JSString::CharCodeAt(uint32_t index) const noexcept {
    if (index >= length_) return 0;
    const JSString* curr = this;
    uint32_t idx = index;
    while (curr) {
        switch (curr->kind_) {
            case StringKind::FlatLatin1:
                return curr->payload_ ? curr->payload_->data()[idx] : 0;
            case StringKind::FlatUtf16: {
                const uint16_t* u16 = reinterpret_cast<const uint16_t*>(curr->payload_->data());
                return u16[idx];
            }
            case StringKind::Cons: {
                uint32_t left_len = curr->left_ ? curr->left_->length_ : 0;
                if (idx < left_len) {
                    curr = curr->left_;
                } else {
                    idx -= left_len;
                    curr = curr->right_;
                }
                break;
            }
            case StringKind::Sliced: {
                idx += curr->slice_offset_;
                curr = curr->parent_;
                break;
            }
        }
    }
    return 0;
}

JSString* JSString::CharAt(uint32_t index) const {
    if (index >= length_) {
        return AllocateRuntime<JSString>("");
    }
    uint16_t cu = CharCodeAt(index);
    if (cu <= 255) {
        char c = static_cast<char>(cu);
        return AllocateRuntime<JSString>(std::string_view(&c, 1));
    }
    return AllocateRuntime<JSString>(&cu, 1);
}

bool JSString::Equals(const JSString* other) const noexcept {
    if (this == other) return true;
    if (!other) return false;
    if (length_ != other->length_) return false;
    if (hash_ != 0 && other->hash_ != 0 && hash_ != other->hash_) return false;
    if (length_ == 0) return true;
    EnsureFlat();
    other->EnsureFlat();
    if (kind_ == StringKind::FlatLatin1 && other->kind_ == StringKind::FlatLatin1) {
        if (payload_ && other->payload_) {
            return std::memcmp(payload_->data(), other->payload_->data(), length_) == 0;
        }
        return payload_ == other->payload_;
    }
    if (kind_ == StringKind::FlatUtf16 && other->kind_ == StringKind::FlatUtf16) {
        if (payload_ && other->payload_) {
            return std::memcmp(payload_->data(), other->payload_->data(), length_ * sizeof(uint16_t)) == 0;
        }
        return payload_ == other->payload_;
    }
    for (uint32_t i = 0; i < length_; ++i) {
        if (CharCodeAt(i) != other->CharCodeAt(i)) return false;
    }
    return true;
}

int JSString::Compare(const JSString* other) const noexcept {
    if (this == other) return 0;
    if (!other) return 1;
    EnsureFlat();
    other->EnsureFlat();
    if (kind_ == StringKind::FlatLatin1 && other->kind_ == StringKind::FlatLatin1 && payload_ && other->payload_) {
        uint32_t min_len = std::min(length_, other->length_);
        int cmp = std::memcmp(payload_->data(), other->payload_->data(), min_len);
        if (cmp != 0) return cmp < 0 ? -1 : 1;
        if (length_ == other->length_) return 0;
        return length_ < other->length_ ? -1 : 1;
    }
    uint32_t min_len = std::min(length_, other->length_);
    for (uint32_t i = 0; i < min_len; ++i) {
        uint16_t c1 = CharCodeAt(i);
        uint16_t c2 = other->CharCodeAt(i);
        if (c1 != c2) return c1 < c2 ? -1 : 1;
    }
    if (length_ == other->length_) return 0;
    return length_ < other->length_ ? -1 : 1;
}

std::string JSString::ToUtf8() const {
    if (length_ == 0) return "";
    if (kind_ != StringKind::FlatLatin1 && kind_ != StringKind::FlatUtf16) {
        EnsureFlat();
    }
    if (kind_ == StringKind::FlatLatin1) {
        if (!payload_) return "";
        std::string out;
        out.reserve(length_);
        const uint8_t* data = payload_->data();
        for (uint32_t i = 0; i < length_; ++i) {
            uint8_t c = data[i];
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return out;
    }
    if (kind_ == StringKind::FlatUtf16) {
        if (!payload_) return "";
        std::string out;
        out.reserve(length_ * 2);
        const uint16_t* u16 = reinterpret_cast<const uint16_t*>(payload_->data());
        uint32_t i = 0;
        while (i < length_) {
            uint16_t cu = u16[i++];
            const uint16_t* next = (i < length_) ? &u16[i] : nullptr;
            AppendUtf16CodeUnitToUtf8(out, cu, next);
            if (cu >= 0xD800 && cu <= 0xDBFF && next && *next >= 0xDC00 && *next <= 0xDFFF) ++i;
        }
        return out;
    }
    return "";
}

void JSString::IteratePointers(SlotVisitor* visitor) {
    switch (kind_) {
        case StringKind::FlatLatin1:
        case StringKind::FlatUtf16:
            if (payload_ != nullptr) {
                TaggedValue val = TaggedValue::FromHeapObject(payload_);
                visitor->Visit(&val);
                if (val.IsHeapObject()) payload_ = val.As<ByteArray>();
            }
            break;
        case StringKind::Cons:
            if (left_ != nullptr) {
                TaggedValue val = TaggedValue::FromHeapObject(left_);
                visitor->Visit(&val);
                if (val.IsHeapObject()) left_ = val.As<JSString>();
            }
            if (right_ != nullptr) {
                TaggedValue val = TaggedValue::FromHeapObject(right_);
                visitor->Visit(&val);
                if (val.IsHeapObject()) right_ = val.As<JSString>();
            }
            break;
        case StringKind::Sliced:
            if (parent_ != nullptr) {
                TaggedValue val = TaggedValue::FromHeapObject(parent_);
                visitor->Visit(&val);
                if (val.IsHeapObject()) parent_ = val.As<JSString>();
            }
            break;
    }
}

void JSObject::PrepareFastSlot(uint32_t slot_index, bool in_object) {
    if (in_object) {
        if (AETHER_UNLIKELY(slot_index >= Shape::kDefaultInObjectProperties)) {
            throw std::length_error("invalid in-object property slot");
        }
        return;
    }
    if (AETHER_UNLIKELY(slot_index < Shape::kDefaultInObjectProperties)) {
        throw std::length_error("invalid out-of-line property slot");
    }
    if (AETHER_UNLIKELY(slot_index >= kMaxObjectProperties)) {
        throw std::length_error("object property limit exceeded");
    }
    size_t ool_idx = slot_index - Shape::kDefaultInObjectProperties;
    auto& extra = EnsureExtra();
    uint32_t needed_len = static_cast<uint32_t>(ool_idx + 1);
    if (!extra.out_of_line_slots) {
        uint32_t initial_cap = std::max<uint32_t>(4, needed_len);
        extra.out_of_line_slots = AllocateRuntimeFixedArray(needed_len, initial_cap, TaggedValue::Empty());
        if (AETHER_UNLIKELY(IsOld() && extra.out_of_line_slots->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(extra.out_of_line_slots));
        }
    } else if (needed_len > extra.out_of_line_slots->Capacity()) {
        uint32_t old_cap = extra.out_of_line_slots->Capacity();
        uint32_t old_len = extra.out_of_line_slots->Length();
        uint32_t new_cap = std::max(needed_len, old_cap * 2);
        FixedArray* new_ool = AllocateRuntimeFixedArray(needed_len, new_cap, TaggedValue::Empty());
        std::memcpy(new_ool->elements(), extra.out_of_line_slots->elements(), old_len * sizeof(TaggedValue));
        extra.out_of_line_slots = new_ool;
        if (AETHER_UNLIKELY(IsOld() && new_ool->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(new_ool));
        }
        if (AETHER_UNLIKELY(new_ool->IsOld())) {
            for (uint32_t i = 0; i < old_len; ++i) {
                TaggedValue el = new_ool->elements()[i];
                if (el.IsHeapObject()) {
                    RecordWriteBarrier(new_ool, el);
                }
            }
        }
    } else if (needed_len > extra.out_of_line_slots->Length()) {
        extra.out_of_line_slots->SetLength(needed_len);
    }
}

TaggedValue JSObject::GetNamedProperty(std::string_view key) const {
    if (property_lookup_depth > kMaxPrototypeChainDepth) return TaggedValue::Undefined();
    PropertyLookupScope lookup_scope;
    if (IsDeletedProperty(key)) {
        if (prototype_ != nullptr) {
            return prototype_->GetNamedProperty(key);
        }
        return TaggedValue::Undefined();
    }
    if (IsArray()) {
        if (key == "length") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSArray*>(this)->Length()));
        }
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            return arr->GetElement(idx);
        }
    } else if (IsMap()) {
        if (key == "size") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSMap*>(this)->Size()));
        }
    } else if (IsSet()) {
        if (key == "size") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSSet*>(this)->Size()));
        }
    } else if (IsArrayBuffer()) {
        auto* ab = static_cast<const JSArrayBuffer*>(this);
        if (key == "byteLength") {
            return TaggedValue::FromDouble(static_cast<double>(ab->ByteLength()));
        }
        if (key == "maxByteLength") {
            return TaggedValue::FromDouble(static_cast<double>(ab->MaxByteLength()));
        }
        if (key == "growable") {
            return TaggedValue::FromBoolean(ab->IsGrowable());
        }
    } else if (IsTypedArray()) {
        auto* ta = static_cast<const JSTypedArray*>(this);
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            if (idx < ta->Length()) {
                return ta->GetElement(idx);
            }
            return TaggedValue::Undefined();
        }
    }

    if (const AccessorDescriptor* acc = GetAccessor(key)) {
        if (acc->getter.IsHeapObject() && acc->getter.AsHeapObject()->IsFunction()) {
            return CallFunction(acc->getter, TaggedValue::FromHeapObject(const_cast<JSObject*>(this)), {});
        }
        return TaggedValue::Undefined();
    }

    Shape* shape = GetShape();
    if (shape == nullptr && Realm::Current()) {
        shape = Realm::Current()->GetRootShape();
        const_cast<JSObject*>(this)->SetShape(shape);
    }
    if (AETHER_LIKELY(shape != nullptr)) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) {
                return val;
            }
        }

        if (IsFunction()) {
            if (key == "name") {
                if (GetType() == HeapObjectType::JSFunction) {
                    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<const JSFunction*>(this)->Name()));
                } else if (GetType() == HeapObjectType::JSNativeFunction) {
                    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<const JSNativeFunction*>(this)->Name()));
                }
            }
            if (key == "length") {
                if (GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<const JSFunction*>(this);
                    uint32_t len = fn->GetBytecode() ? fn->GetBytecode()->Length() : 0;
                    return TaggedValue::FromSmi(static_cast<int32_t>(len));
                } else if (GetType() == HeapObjectType::JSNativeFunction) {
                    auto* fn = static_cast<const JSNativeFunction*>(this);
                    return TaggedValue::FromSmi(static_cast<int32_t>(fn->Length()));
                }
            }
        }

        if (IsUserFunction() && key == "prototype") {
            auto* fn = const_cast<JSFunction*>(static_cast<const JSFunction*>(this));
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                if (fn->GetConstructorPrototype().IsEmpty()) {
                    return TaggedValue::FromHeapObject(fn->GetOrCreateConstructorPrototype());
                }
                return fn->GetConstructorPrototype();
            }
            return TaggedValue::Undefined();
        }

        // Check instance prototype if explicitly assigned
        if (prototype_ != nullptr) {
            TaggedValue val = prototype_->GetNamedProperty(key);
            if (!val.IsUndefined()) {
                return val;
            }
        }

        // Walk shape prototype chain
        TaggedValue proto = shape->Prototype();
        size_t depth = 0;
        while (proto.IsHeapObject() && depth++ < kMaxPrototypeChainDepth) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
                if (const AccessorDescriptor* p_acc = js_proto->GetAccessor(key)) {
                    if (p_acc->getter.IsHeapObject() && p_acc->getter.AsHeapObject()->IsFunction()) {
                        return CallFunction(p_acc->getter, TaggedValue::FromHeapObject(const_cast<JSObject*>(this)), {});
                    }
                    return TaggedValue::Undefined();
                }
                Shape* p_shape = js_proto->GetShape();
                if (p_shape != nullptr) {
                    const PropertyDescriptor* p_desc = p_shape->LookupProperty(key);
                    if (p_desc != nullptr) {
                        TaggedValue p_val = js_proto->GetFastSlot(p_desc->slot_index, p_desc->is_in_object);
                        if (!p_val.IsEmpty()) {
                            return p_val;
                        }
                    }
                }
                proto = p_shape ? p_shape->Prototype() : TaggedValue::Null();
            } else {
                break;
            }
        }
    }
    return TaggedValue::Undefined();
}

bool JSObject::HasNamedProperty(std::string_view key) const {
    if (property_lookup_depth > kMaxPrototypeChainDepth) return false;
    PropertyLookupScope lookup_scope;
    if (IsDeletedProperty(key)) {
        if (prototype_ != nullptr && prototype_->HasNamedProperty(key)) return true;
        return false;
    }
    if (IsArray()) {
        if (key == "length") return true;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            return arr->HasElement(idx);
        }
    } else if (IsMap()) {
        if (key == "size") return true;
    } else if (IsSet()) {
        if (key == "size") return true;
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            return idx < static_cast<const JSTypedArray*>(this)->Length();
        }
    }

    if (HasAccessor(key)) return true;

    Shape* shape = GetShape();
    if (shape == nullptr && Realm::Current()) {
        shape = Realm::Current()->GetRootShape();
    }
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) return true;
        }

        if (prototype_ != nullptr && prototype_->HasNamedProperty(key)) {
            return true;
        }

        if (IsUserFunction() && key == "prototype") {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                return true;
            }
            return false;
        }

        TaggedValue proto = shape->Prototype();
        size_t depth = 0;
        while (proto.IsHeapObject() && depth++ < kMaxPrototypeChainDepth) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
                if (js_proto->HasAccessor(key)) return true;
                Shape* p_shape = js_proto->GetShape();
                if (p_shape != nullptr) {
                    const PropertyDescriptor* p_desc = p_shape->LookupProperty(key);
                    if (p_desc != nullptr) {
                        TaggedValue p_val = js_proto->GetFastSlot(p_desc->slot_index, p_desc->is_in_object);
                        if (!p_val.IsEmpty()) return true;
                    }
                }
                proto = p_shape ? p_shape->Prototype() : TaggedValue::Null();
            } else {
                break;
            }
        }
    }
    return false;
}

bool JSObject::DeleteNamedProperty(std::string_view key) {
    if (IsArray()) {
        if (key == "length") return false;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            auto* arr = static_cast<JSArray*>(this);
            if (idx < arr->Length()) {
                PropertyAttributes attrs = GetOwnPropertyAttributes(key);
                if (attrs & PropertyAttributes::Configurable) {
                    arr->SetElement(idx, TaggedValue::Empty());
                    return true;
                }
                return false;
            }
        }
    }
    if (IsFunction()) {
        if (key == "length" || key == "name") {
            PropertyAttributes attrs = GetOwnPropertyAttributes(key);
            if (attrs & PropertyAttributes::Configurable) {
                MarkDeletedProperty(key);
                RemoveOwnAttributeOverride(key);
                return true;
            }
            return false;
        }
    }
    if (auto* acc = GetAccessor(key)) {
        PropertyAttributes attrs = acc->attributes;
        if (HasOwnAttributeOverride(key)) {
            attrs = GetOwnAttributeOverride(key);
        }
        if (attrs & PropertyAttributes::Configurable) {
            MarkDeletedProperty(key);
            RemoveAccessor(key);
            RemoveOwnAttributeOverride(key);
            return true;
        }
        return false;
    }
    Shape* shape = GetShape();
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) {
                PropertyAttributes attrs = GetOwnPropertyAttributes(key);
                if (attrs & PropertyAttributes::Configurable) {
                    MarkDeletedProperty(key);
                    RemoveOwnAttributeOverride(key);
                    SetFastSlot(desc->slot_index, desc->is_in_object, TaggedValue::Empty());
                    return true;
                }
                return false;
            }
        }
    }
    return true;
}

bool JSObject::HasOwnProperty(std::string_view key) const {
    if (IsDeletedProperty(key)) return false;
    if (HasAccessor(key)) return true;
    Shape* shape = GetShape();
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) return true;
        }
    }
    if (IsArray()) {
        if (key == "length") return true;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            return arr->HasElement(idx);
        }
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
        if (GetType() == HeapObjectType::JSFunction) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (key == "prototype" && !fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) return true;
        }
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            return idx < static_cast<const JSTypedArray*>(this)->Length();
        }
    }
    return false;
}

PropertyAttributes JSObject::GetOwnPropertyAttributes(std::string_view key) const {
    PropertyAttributes attrs = PropertyAttributes::None;
    if (HasOwnAttributeOverride(key)) {
        attrs = GetOwnAttributeOverride(key);
    } else if (const AccessorDescriptor* accessor = GetAccessor(key)) {
        attrs = accessor->attributes;
    } else if (IsArray()) {
        size_t idx = 0;
        if (key == "length") {
            attrs = PropertyAttributes::Writable;
        } else if (TryParseArrayIndex(key, idx)) {
            if (idx < static_cast<const JSArray*>(this)->Length()) {
                attrs = PropertyAttributes::Default;
            }
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else if (IsFunction()) {
        if (key == "name" || key == "length") {
            attrs = PropertyAttributes::Configurable;
        } else if (key == "prototype" && IsUserFunction()) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                attrs = PropertyAttributes::Writable;
            }
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx) && idx < static_cast<const JSTypedArray*>(this)->Length()) {
            attrs = PropertyAttributes::Writable | PropertyAttributes::Enumerable;
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else {
        Shape* shape = GetShape();
        if (shape != nullptr) {
            if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                if (!val.IsEmpty()) {
                    attrs = desc->attributes;
                }
            }
        }
    }

    uint8_t bits = static_cast<uint8_t>(attrs);
    if (IsFrozen()) {
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Writable);
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Configurable);
    } else if (IsSealed()) {
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Configurable);
    }
    return static_cast<PropertyAttributes>(bits);
}

void JSObject::SetOwnPropertyAttributes(std::string_view key, PropertyAttributes attributes) {
    CheckPropertyGrowth(key);
    if (!HasOwnAttributeOverride(key) && OwnAttributeOverrideCount() >= kMaxObjectProperties) {
        throw std::length_error("object property limit exceeded");
    }
    SetOwnAttributeOverride(key, attributes);
}

void JSObject::CheckPropertyGrowth(std::string_view key) const {
    if (key.size() > kMaxStringLength) throw std::length_error("property key is too long");
    const Shape* shape = GetShape();
    if (HasAccessor(key) || HasOwnAttributeOverride(key) ||
        (shape && shape->LookupProperty(key))) return;
    if (OwnAttributeOverrideCount() >= kMaxObjectProperties ||
        AccessorCount() >= kMaxObjectProperties) {
        throw std::length_error("object property limit exceeded");
    }
}

std::vector<std::string> JSObject::GetOwnPropertyNames() const {
    std::vector<std::string> indices;
    std::vector<std::string> string_keys;
    std::vector<std::string> symbol_keys;
    std::unordered_set<std::string> seen;

    auto add_key = [&](const std::string& name) {
        if (name.rfind("__private__", 0) == 0) return;
        if (IsDeletedProperty(name)) return;
        if (seen.insert(name).second) {
            size_t parsed_index = 0;
            if (TryParseArrayIndex(name, parsed_index)) {
                indices.push_back(name);
            } else if (name.rfind("@@", 0) == 0) {
                symbol_keys.push_back(name);
            } else {
                string_keys.push_back(name);
            }
        }
    };

    if (IsArray()) {
        const auto* arr = static_cast<const JSArray*>(this);
        for (size_t i = 0; i < arr->Length(); ++i) {
            if (arr->HasElement(i)) {
                add_key(std::to_string(i));
            }
        }
        add_key("length");
    } else if (IsFunction()) {
        add_key("length");
        add_key("name");
        if (IsUserFunction()) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync()) {
                add_key("prototype");
            }
        }
    }

    // Collect accessor names
    if (auto* extra = GetExtraData()) {
        if (extra->accessors) {
            for (const auto& [name, acc] : *extra->accessors) {
                add_key(std::string(name));
            }
        }
    }

    // Collect shape property names ordered by slot_index (creation order)
    Shape* shape = GetShape();
    if (shape != nullptr) {
        std::vector<std::pair<uint32_t, std::string>> ordered_props;
        for (const auto& [name, desc] : shape->GetPropertyMap()) {
            TaggedValue val = GetFastSlot(desc.slot_index, desc.is_in_object);
            if (!val.IsEmpty()) {
                ordered_props.emplace_back(desc.slot_index, name);
            }
        }
        std::sort(ordered_props.begin(), ordered_props.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (const auto& [slot, name] : ordered_props) {
            add_key(name);
        }
    }

    // Sort integer indices ascending numerically
    std::sort(indices.begin(), indices.end(), [](const std::string& a, const std::string& b) {
        size_t ai = 0;
        size_t bi = 0;
        const bool a_ok = TryParseArrayIndex(a, ai);
        const bool b_ok = TryParseArrayIndex(b, bi);
        if (a_ok && b_ok) return ai < bi;
        return a < b;
    });

    std::vector<std::string> result;
    result.reserve(indices.size() + string_keys.size() + symbol_keys.size());
    result.insert(result.end(), indices.begin(), indices.end());
    result.insert(result.end(), string_keys.begin(), string_keys.end());
    result.insert(result.end(), symbol_keys.begin(), symbol_keys.end());
    return result;
}

void JSObject::SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id) {
    std::string key_str(key);
    key = key_str;
    PinnedScope pin_this(this);
    PinnedScope pin_val(value.IsHeapObject() ? value.AsHeapObject() : nullptr);
    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap && value.IsHeapObject()) heap = IsolateHeap::HeapForObject(value.AsHeapObject());
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    if (value.IsHeapObject() && heap->Contains(value.AsHeapObject())) {
        scope.CreateHandle(value.AsHeapObject());
    }
    CheckPropertyGrowth(key);
    Shape* current_shape = GetShape();
    if (current_shape == nullptr) {
        if (Realm::Current()) {
            current_shape = Realm::Current()->GetRootShape();
        } else {
            static Shape s_fallback_root_shape(0, nullptr, "", TaggedValue::Null());
            current_shape = &s_fallback_root_shape;
        }
    }
    assert(current_shape != nullptr && "Object must have a valid shape");

    if (IsFrozen()) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
        }
        return;
    }

    bool is_private = key.rfind("__private__", 0) == 0;
    if (const AccessorDescriptor* acc = GetAccessor(key)) {
        if (acc->setter.IsHeapObject() && acc->setter.AsHeapObject()->IsFunction()) {
            CallFunction(acc->setter, TaggedValue::FromHeapObject(this), {value});
        } else if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot set property without a setter")));
            }
        }
        return;
    }

    if (IsFunction()) {
        if (key == "length" || key == "name") {
            if (Interpreter::Current() != nullptr) {
                if (!HasOwnAttributeOverride(key) || !(GetOwnAttributeOverride(key) & PropertyAttributes::Writable)) {
                    if (Interpreter::Current()->IsStrictMode()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot assign to read-only property '" + std::string(key) + "'")));
                    }
                    return;
                }
            } else if (key == "length" && GetType() == HeapObjectType::JSNativeFunction && value.IsSmi()) {
                static_cast<JSNativeFunction*>(this)->SetLength(static_cast<uint32_t>(value.AsSmi()));
            }
        }
    }

    if (IsUserFunction() && key == "prototype") {
        static_cast<JSFunction*>(this)->SetConstructorPrototype(value);
        return;
    }

    if (IsArray()) {
        if (key == "length") {
            const double length = value.ToDouble();
            if (!std::isfinite(length) || length < 0 ||
                length > static_cast<double>(kMaxArrayLength) || std::trunc(length) != length) {
                throw std::length_error("invalid array length");
            }
            static_cast<JSArray*>(this)->SetLength(static_cast<size_t>(length));
            ClearDeletedProperty(key);
            return;
        }
        if (IsAllDigits(key)) {
            size_t idx = 0;
            if (!TryParseArrayIndex(key, idx)) {
                throw std::length_error("array index overflow");
            }
            static_cast<JSArray*>(this)->SetElement(idx, value);
            ClearDeletedProperty(key);
            return;
        }
    }

    const PropertyDescriptor* desc = current_shape->LookupProperty(key);
    if (desc != nullptr) {
        TaggedValue cur_val = GetFastSlot(desc->slot_index, desc->is_in_object);
        if (!cur_val.IsEmpty()) {
            if (!(GetOwnPropertyAttributes(key) & PropertyAttributes::Writable)) {
                if (Interpreter::Current() && (is_private || Interpreter::Current()->IsStrictMode())) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
                }
                return;
            }
            // Fast path: Shape already contains property; overwrite existing slot
            SetFastSlot(desc->slot_index, desc->is_in_object, value);
            ClearDeletedProperty(key);
            return;
        }
    }

    // A setter or non-writable property on the prototype chain handles assignment
    TaggedValue proto_val = prototype_ ? TaggedValue::FromHeapObject(prototype_) : (current_shape ? current_shape->Prototype() : TaggedValue::Null());
    size_t prototype_depth = 0;
    while (proto_val.IsHeapObject()) {
        if (++prototype_depth > kMaxPrototypeChainDepth) {
            throw std::length_error("prototype chain depth exceeded");
        }
        HeapObject* p_obj = proto_val.AsHeapObject();
        if (p_obj->IsJSObject()) {
            JSObject* p = static_cast<JSObject*>(p_obj);
            if (const AccessorDescriptor* acc = p->GetAccessor(key)) {
                if (acc->setter.IsHeapObject() && acc->setter.AsHeapObject()->IsFunction()) {
                    CallFunction(acc->setter, TaggedValue::FromHeapObject(this), {value});
                } else if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot set property without a setter")));
                    }
                }
                return;
            }
            if (is_private && p->HasOwnProperty(key)) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot assign to private method")));
                }
                return;
            }
            if (p->HasOwnProperty(key)) {
                PropertyAttributes attrs = p->GetOwnPropertyAttributes(key);
                if (!(attrs & PropertyAttributes::Writable)) {
                    if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                        if (Interpreter::Current()) {
                            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
                        }
                    }
                    return;
                }
                break;
            }
            proto_val = p->GetPrototype() ? TaggedValue::FromHeapObject(p->GetPrototype()) : (p->GetShape() ? p->GetShape()->Prototype() : TaggedValue::Null());
        } else {
            break;
        }
    }

    if (desc != nullptr) {
        // Slot is Empty() (property was deleted). Check extensibility before re-adding!
        if (IsSealed() || !IsExtensible()) {
            if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot add property to a non-extensible object")));
            }
            return;
        }
        if (auto state = CurrentMemoryBudgetState()) {
            if (state->HasTotalLimit() && state->ChargedBytes() >= state->TotalLimit()) {
                throw std::length_error("memory budget exceeded");
            }
        }
        PrepareFastSlot(desc->slot_index, desc->is_in_object);
        SetFastSlot(desc->slot_index, desc->is_in_object, value);
        ClearDeletedProperty(key);
        return;
    }
    if (IsSealed() || !IsExtensible()) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot add property to a non-extensible object")));
        }
        return;
    }

    // The transition allocates the next slot at PropertyCount().  Validate
    // and reserve it before publishing the child shape so a failed growth
    // cannot leave a shape whose slot metadata points past storage.
    if (AETHER_UNLIKELY(current_shape->PropertyCount() >= kMaxObjectProperties)) {
        throw std::length_error("object property limit exceeded");
    }
    if (auto state = CurrentMemoryBudgetState()) {
        if (state->HasTotalLimit() && state->ChargedBytes() >= state->TotalLimit()) {
            throw std::length_error("memory budget exceeded");
        }
    }
    const uint32_t next_slot = current_shape->PropertyCount();
    PrepareFastSlot(next_slot, next_slot < Shape::kDefaultInObjectProperties);
    Shape* next_shape = current_shape->FindOrCreateTransition(key, next_shape_id, current_shape->Prototype());
    // All allocating operations have succeeded; publish storage before shape.
    SetFastSlot(next_slot, next_slot < Shape::kDefaultInObjectProperties, value);
    SetShape(next_shape);
    ++next_shape_id;
    ClearDeletedProperty(key);
}

// JSMap Implementation
void JSMap::Set(TaggedValue key, TaggedValue val) {
    PinnedScope pin_this(this);
    PinnedScope pin_k(key.IsHeapObject() ? key.AsHeapObject() : nullptr);
    PinnedScope pin_v(val.IsHeapObject() ? val.AsHeapObject() : nullptr);
    size_t count = Size();
    if (table_) {
        for (size_t i = 0; i < count; ++i) {
            TaggedValue k = table_->Get(static_cast<uint32_t>(i * 2));
            if (SameValueZero(k, key)) {
                table_->Set(static_cast<uint32_t>(i * 2 + 1), val);
                return;
            }
        }
    }

    if (AETHER_UNLIKELY(count >= kMaxObjectProperties)) {
        throw std::length_error("map capacity limit exceeded");
    }

    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    if (key.IsHeapObject() && heap->Contains(key.AsHeapObject())) {
        scope.CreateHandle(key.AsHeapObject());
    }
    if (val.IsHeapObject() && heap->Contains(val.AsHeapObject())) {
        scope.CreateHandle(val.AsHeapObject());
    }
    if (table_ && heap->Contains(table_)) {
        scope.CreateHandle(table_);
    }

    uint32_t new_len = static_cast<uint32_t>((count + 1) * 2);
    uint32_t current_cap = table_ ? table_->Capacity() : 0;

    if (!table_ || new_len > current_cap) {
        uint32_t new_cap = std::max(uint32_t(8), std::max(new_len, current_cap * 2));
        FixedArray* new_table = AllocateRuntimeFixedArrayFor(this, new_len, new_cap, TaggedValue::Empty());
        if (table_) {
            for (uint32_t i = 0; i < table_->Length(); ++i) {
                new_table->Set(i, table_->Get(i));
            }
        }
        table_ = new_table;
        if (AETHER_UNLIKELY(IsOld() && table_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(table_));
        }
    } else {
        table_->SetLength(new_len);
    }

    uint32_t base = static_cast<uint32_t>(count * 2);
    table_->Set(base + 0, key);
    table_->Set(base + 1, val);
}

TaggedValue JSMap::Get(TaggedValue key) const {
    if (!table_) return TaggedValue::Undefined();
    size_t count = Size();
    for (size_t i = 0; i < count; ++i) {
        TaggedValue k = table_->Get(static_cast<uint32_t>(i * 2));
        if (SameValueZero(k, key)) {
            return table_->Get(static_cast<uint32_t>(i * 2 + 1));
        }
    }
    return TaggedValue::Undefined();
}

bool JSMap::Has(TaggedValue key) const {
    if (!table_) return false;
    size_t count = Size();
    for (size_t i = 0; i < count; ++i) {
        TaggedValue k = table_->Get(static_cast<uint32_t>(i * 2));
        if (SameValueZero(k, key)) {
            return true;
        }
    }
    return false;
}

bool JSMap::Delete(TaggedValue key) {
    if (!table_) return false;
    size_t count = Size();
    for (size_t i = 0; i < count; ++i) {
        TaggedValue k = table_->Get(static_cast<uint32_t>(i * 2));
        if (SameValueZero(k, key)) {
            for (size_t j = i; j + 1 < count; ++j) {
                table_->Set(static_cast<uint32_t>(j * 2), table_->Get(static_cast<uint32_t>((j + 1) * 2)));
                table_->Set(static_cast<uint32_t>(j * 2 + 1), table_->Get(static_cast<uint32_t>((j + 1) * 2 + 1)));
            }
            uint32_t new_len = static_cast<uint32_t>((count - 1) * 2);
            table_->SetLength(new_len);
            table_->Set(new_len + 0, TaggedValue::Empty());
            table_->Set(new_len + 1, TaggedValue::Empty());
            return true;
        }
    }
    return false;
}

std::vector<std::pair<TaggedValue, TaggedValue>> JSMap::Entries() const {
    std::vector<std::pair<TaggedValue, TaggedValue>> result;
    if (!table_) return result;
    size_t count = Size();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.emplace_back(table_->Get(static_cast<uint32_t>(i * 2)),
                            table_->Get(static_cast<uint32_t>(i * 2 + 1)));
    }
    return result;
}

// JSSet Implementation
void JSSet::Add(TaggedValue val) {
    PinnedScope pin_this(this);
    PinnedScope pin_v(val.IsHeapObject() ? val.AsHeapObject() : nullptr);
    if (Has(val)) return;

    size_t count = Size();
    if (AETHER_UNLIKELY(count >= kMaxObjectProperties)) {
        throw std::length_error("set capacity limit exceeded");
    }

    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    if (val.IsHeapObject() && heap->Contains(val.AsHeapObject())) {
        scope.CreateHandle(val.AsHeapObject());
    }
    if (table_ && heap->Contains(table_)) {
        scope.CreateHandle(table_);
    }

    uint32_t new_len = static_cast<uint32_t>(count + 1);
    uint32_t current_cap = table_ ? table_->Capacity() : 0;

    if (!table_ || new_len > current_cap) {
        uint32_t new_cap = std::max(uint32_t(4), std::max(new_len, current_cap * 2));
        FixedArray* new_table = AllocateRuntimeFixedArrayFor(this, new_len, new_cap, TaggedValue::Empty());
        if (table_) {
            for (uint32_t i = 0; i < table_->Length(); ++i) {
                new_table->Set(i, table_->Get(i));
            }
        }
        table_ = new_table;
        if (AETHER_UNLIKELY(IsOld() && table_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(table_));
        }
    } else {
        table_->SetLength(new_len);
    }

    table_->Set(static_cast<uint32_t>(count), val);
}

bool JSSet::Has(TaggedValue val) const {
    if (!table_) return false;
    size_t count = Size();
    for (size_t i = 0; i < count; ++i) {
        if (SameValueZero(table_->Get(static_cast<uint32_t>(i)), val)) {
            return true;
        }
    }
    return false;
}

bool JSSet::Delete(TaggedValue val) {
    if (!table_) return false;
    size_t count = Size();
    for (size_t i = 0; i < count; ++i) {
        if (SameValueZero(table_->Get(static_cast<uint32_t>(i)), val)) {
            for (size_t j = i; j + 1 < count; ++j) {
                table_->Set(static_cast<uint32_t>(j), table_->Get(static_cast<uint32_t>(j + 1)));
            }
            uint32_t new_len = static_cast<uint32_t>(count - 1);
            table_->SetLength(new_len);
            table_->Set(new_len, TaggedValue::Empty());
            return true;
        }
    }
    return false;
}

std::vector<TaggedValue> JSSet::Values() const {
    std::vector<TaggedValue> result;
    if (!table_) return result;
    size_t count = Size();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(table_->Get(static_cast<uint32_t>(i)));
    }
    return result;
}

// JSPromise Implementation
void JSPromise::Fulfill(TaggedValue val, MicrotaskQueue& queue) {
    if (state_ != PromiseState::Pending) return;
    state_ = PromiseState::Fulfilled;
    result_ = val;
    if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
        RecordWriteBarrier(this, val);
    }

    if (reactions_) {
        size_t count = ReactionCount();
        for (size_t i = 0; i < count; ++i) {
            TaggedValue on_fulfilled = reactions_->Get(static_cast<uint32_t>(i * 3 + 0));
            TaggedValue on_rejected = reactions_->Get(static_cast<uint32_t>(i * 3 + 1));
            TaggedValue downstream_val = reactions_->Get(static_cast<uint32_t>(i * 3 + 2));
            auto* downstream = (downstream_val.IsHeapObject() && downstream_val.AsHeapObject()->IsJSObject())
                ? static_cast<JSPromise*>(downstream_val.AsHeapObject()) : nullptr;
            queue.Enqueue([on_fulfilled, val, downstream, &queue]() {
                if (!on_fulfilled.IsEmpty() && !on_fulfilled.IsUndefined()) {
                    TaggedValue res = CallFunction(on_fulfilled, TaggedValue::Undefined(), {val});
                    if (downstream) downstream->Fulfill(res, queue);
                } else {
                    if (downstream) downstream->Fulfill(val, queue);
                }
            }, {on_fulfilled, val, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
        }
        reactions_ = nullptr;
    }
}

void JSPromise::Reject(TaggedValue reason, MicrotaskQueue& queue) {
    if (state_ != PromiseState::Pending) return;
    state_ = PromiseState::Rejected;
    result_ = reason;
    if (AETHER_UNLIKELY(IsOld() && reason.IsHeapObject())) {
        RecordWriteBarrier(this, reason);
    }

    if (reactions_) {
        size_t count = ReactionCount();
        for (size_t i = 0; i < count; ++i) {
            TaggedValue on_fulfilled = reactions_->Get(static_cast<uint32_t>(i * 3 + 0));
            TaggedValue on_rejected = reactions_->Get(static_cast<uint32_t>(i * 3 + 1));
            TaggedValue downstream_val = reactions_->Get(static_cast<uint32_t>(i * 3 + 2));
            auto* downstream = (downstream_val.IsHeapObject() && downstream_val.AsHeapObject()->IsJSObject())
                ? static_cast<JSPromise*>(downstream_val.AsHeapObject()) : nullptr;
            queue.Enqueue([on_rejected, reason, downstream, &queue]() {
                if (!on_rejected.IsEmpty() && !on_rejected.IsUndefined()) {
                    TaggedValue res = CallFunction(on_rejected, TaggedValue::Undefined(), {reason});
                    if (downstream) downstream->Fulfill(res, queue);
                } else {
                    if (downstream) downstream->Reject(reason, queue);
                }
            }, {on_rejected, reason, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
        }
        reactions_ = nullptr;
    }
}

void JSPromise::AddReaction(TaggedValue on_fulfilled, TaggedValue on_rejected, JSPromise* downstream, MicrotaskQueue& queue) {
    if (state_ == PromiseState::Fulfilled) {
        TaggedValue val = result_;
        queue.Enqueue([on_fulfilled, val, downstream, &queue]() {
            if (!on_fulfilled.IsEmpty() && !on_fulfilled.IsUndefined()) {
                TaggedValue res = CallFunction(on_fulfilled, TaggedValue::Undefined(), {val});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Fulfill(val, queue);
            }
        }, {on_fulfilled, val, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    } else if (state_ == PromiseState::Rejected) {
        TaggedValue reason = result_;
        queue.Enqueue([on_rejected, reason, downstream, &queue]() {
            if (!on_rejected.IsEmpty() && !on_rejected.IsUndefined()) {
                TaggedValue res = CallFunction(on_rejected, TaggedValue::Undefined(), {reason});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Reject(reason, queue);
            }
        }, {on_rejected, reason, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    } else {
        size_t current_count = ReactionCount();
        if (AETHER_UNLIKELY(current_count >= kMaxObjectProperties)) {
            throw std::length_error("promise reaction limit exceeded");
        }

        IsolateHeap* heap = IsolateHeap::HeapForObject(this);
        if (!heap) heap = IsolateHeap::Current();
        if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
        HandleScope scope(*heap);
        if (heap->Contains(this)) {
            scope.CreateHandle(this);
        }
        if (on_fulfilled.IsHeapObject() && heap->Contains(on_fulfilled.AsHeapObject())) {
            scope.CreateHandle(on_fulfilled.AsHeapObject());
        }
        if (on_rejected.IsHeapObject() && heap->Contains(on_rejected.AsHeapObject())) {
            scope.CreateHandle(on_rejected.AsHeapObject());
        }
        if (downstream && heap->Contains(downstream)) {
            scope.CreateHandle(downstream);
        }
        if (reactions_ && heap->Contains(reactions_)) {
            scope.CreateHandle(reactions_);
        }

        uint32_t new_len = static_cast<uint32_t>((current_count + 1) * 3);
        uint32_t current_cap = reactions_ ? reactions_->Capacity() : 0;

        if (!reactions_ || new_len > current_cap) {
            uint32_t new_cap = std::max(uint32_t(6), std::max(new_len, current_cap * 2));
            FixedArray* new_arr = AllocateRuntimeFixedArrayFor(this, new_len, new_cap, TaggedValue::Empty());
            if (reactions_) {
                for (uint32_t i = 0; i < reactions_->Length(); ++i) {
                    new_arr->Set(i, reactions_->Get(i));
                }
            }
            reactions_ = new_arr;
            if (AETHER_UNLIKELY(IsOld() && reactions_->IsYoung())) {
                RecordWriteBarrier(this, TaggedValue::FromHeapObject(reactions_));
            }
        } else {
            reactions_->SetLength(new_len);
        }

        uint32_t base = static_cast<uint32_t>(current_count * 3);
        reactions_->Set(base + 0, on_fulfilled);
        reactions_->Set(base + 1, on_rejected);
        reactions_->Set(base + 2, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Null());
    }
}

// JSRegExp Implementation
bool JSRegExp::Test(std::string_view str) const {
    return RegexSearch(Pattern(), Flags(), str).has_value();
}

const std::string& JSFunction::Name() const noexcept {
    static const std::string kEmpty;
    return fn_data_ ? fn_data_->name : kEmpty;
}

std::vector<JSFunction::InstanceField> JSFunction::InstanceFields() const noexcept {
    std::vector<InstanceField> result;
    if (!fn_data_ || !fn_data_->instance_fields) return result;
    size_t count = fn_data_->instance_fields->Length() / 3;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        TaggedValue name_val = fn_data_->instance_fields->Get(static_cast<uint32_t>(i * 3));
        TaggedValue init_val = fn_data_->instance_fields->Get(static_cast<uint32_t>(i * 3 + 1));
        TaggedValue method_val = fn_data_->instance_fields->Get(static_cast<uint32_t>(i * 3 + 2));
        std::string name;
        if (name_val.IsHeapObject() && name_val.AsHeapObject()->IsString()) {
            name = name_val.As<JSString>()->Value();
        }
        bool is_method = method_val.IsBoolean() ? method_val.AsBoolean() : false;
        result.emplace_back(std::move(name), init_val, is_method);
    }
    return result;
}

void JSFunction::AddInstanceField(std::string_view name, TaggedValue initial_val, bool is_method) {
    auto& d = EnsureFnData();
    size_t current_count = d.instance_fields ? (d.instance_fields->Length() / 3) : 0;
    if (AETHER_UNLIKELY(current_count >= kMaxObjectProperties)) {
        throw std::length_error("instance field limit exceeded");
    }

    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    if (initial_val.IsHeapObject() && heap->Contains(initial_val.AsHeapObject())) {
        scope.CreateHandle(initial_val.AsHeapObject());
    }
    if (d.instance_fields && heap->Contains(d.instance_fields)) {
        scope.CreateHandle(d.instance_fields);
    }

    auto* name_str = AllocateRuntime<JSString>(name);
    if (name_str && heap->Contains(name_str)) {
        scope.CreateHandle(name_str);
    }
    TaggedValue name_val = TaggedValue::FromHeapObject(name_str);

    size_t new_count = current_count + 1;
    uint32_t new_len = static_cast<uint32_t>(new_count * 3);
    uint32_t current_cap = d.instance_fields ? d.instance_fields->Capacity() : 0;

    if (!d.instance_fields || new_len > current_cap) {
        uint32_t new_cap = std::max(uint32_t(6), std::max(new_len, current_cap * 2));
        FixedArray* new_arr = AllocateRuntimeFixedArrayFor(this, new_len, new_cap, TaggedValue::Empty());
        if (d.instance_fields) {
            for (uint32_t i = 0; i < d.instance_fields->Length(); ++i) {
                new_arr->Set(i, d.instance_fields->Get(i));
            }
        }
        d.instance_fields = new_arr;
        if (AETHER_UNLIKELY(IsOld() && d.instance_fields->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(d.instance_fields));
        }
    } else {
        d.instance_fields->SetLength(new_len);
    }

    uint32_t base = static_cast<uint32_t>(current_count * 3);
    d.instance_fields->Set(base + 0, name_val);
    d.instance_fields->Set(base + 1, initial_val);
    d.instance_fields->Set(base + 2, TaggedValue::FromBoolean(is_method));
}

std::vector<JSObject*> JSFunction::GetWithStack() const {
    std::vector<JSObject*> result;
    if (!fn_data_ || !fn_data_->with_stack) return result;
    result.reserve(fn_data_->with_stack->Length());
    for (uint32_t i = 0; i < fn_data_->with_stack->Length(); ++i) {
        TaggedValue val = fn_data_->with_stack->Get(i);
        if (val.IsHeapObject() && val.AsHeapObject()->IsJSObject()) {
            result.push_back(val.As<JSObject>());
        }
    }
    return result;
}

void JSFunction::SetWithStack(const std::vector<JSObject*>& ws) {
    if (ws.empty()) {
        if (fn_data_) fn_data_->with_stack = nullptr;
        return;
    }
    auto& d = EnsureFnData();

    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    for (auto* obj : ws) {
        if (obj && heap->Contains(obj)) {
            scope.CreateHandle(obj);
        }
    }

    d.with_stack = AllocateRuntimeFixedArrayFor(this, static_cast<uint32_t>(ws.size()), static_cast<uint32_t>(ws.size()), TaggedValue::Empty());
    for (size_t i = 0; i < ws.size(); ++i) {
        TaggedValue obj_val = ws[i] ? TaggedValue::FromHeapObject(ws[i]) : TaggedValue::Null();
        d.with_stack->Set(static_cast<uint32_t>(i), obj_val);
    }
    if (AETHER_UNLIKELY(IsOld() && d.with_stack->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(d.with_stack));
    }
}

void JSFunction::SetWithStack(FixedArray* ws) {
    if (!ws || ws->Length() == 0) {
        if (fn_data_) fn_data_->with_stack = nullptr;
        return;
    }
    auto& d = EnsureFnData();
    d.with_stack = ws;
    if (AETHER_UNLIKELY(IsOld() && d.with_stack->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(d.with_stack));
    }
}

void JSFunction::IteratePointers(SlotVisitor* visitor) {
    JSObject::IteratePointers(visitor);
    if (!fn_data_) return;
    if (!fn_data_->ctor_prototype.IsEmpty()) {
        visitor->Visit(&fn_data_->ctor_prototype);
    }
    if (fn_data_->lexical_this.IsHeapObject()) {
        visitor->Visit(&fn_data_->lexical_this);
    }
    if (fn_data_->lexical_new_target.IsHeapObject()) {
        visitor->Visit(&fn_data_->lexical_new_target);
    }
    if (fn_data_->environment != nullptr) {
        TaggedValue env_val = TaggedValue::FromHeapObject(fn_data_->environment);
        visitor->Visit(&env_val);
        if (env_val.IsHeapObject()) {
            fn_data_->environment = env_val.As<LexicalEnvironment>();
        }
    }
    if (fn_data_->bytecode) {
        const_cast<BytecodeArray*>(fn_data_->bytecode)->IterateConstants(visitor);
    }
    if (fn_data_->with_stack != nullptr) {
        TaggedValue ws_val = TaggedValue::FromHeapObject(fn_data_->with_stack);
        visitor->Visit(&ws_val);
        if (ws_val.IsHeapObject()) {
            fn_data_->with_stack = ws_val.As<FixedArray>();
        }
    }
    if (fn_data_->instance_fields != nullptr) {
        TaggedValue if_val = TaggedValue::FromHeapObject(fn_data_->instance_fields);
        visitor->Visit(&if_val);
        if (if_val.IsHeapObject()) {
            fn_data_->instance_fields = if_val.As<FixedArray>();
        }
    }
}

JSFunction::JSFunction(std::string_view name, std::shared_ptr<const BytecodeArray> bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)) {
    if (!name.empty() || bytecode || env) {
        auto& d = EnsureFnData();
        d.name = detail::ChargedString(name, d.name_storage);
        d.bytecode_shared = std::move(bytecode);
        d.bytecode = d.bytecode_shared.get();
        d.environment = env;
    }
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSFunction::JSFunction(std::string_view name, const BytecodeArray* bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)) {
    if (!name.empty() || bytecode || env) {
        auto& d = EnsureFnData();
        d.name = detail::ChargedString(name, d.name_storage);
        d.bytecode = bytecode;
        d.environment = env;
    }
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSObject* JSFunction::GetOrCreateConstructorPrototype() {
    auto& d = EnsureFnData();
    if (d.ctor_prototype.IsEmpty()) {
        Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* proto = AllocateRuntime<JSObject>(root_shape);
        if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
            proto->SetPrototype(Realm::Current()->GetObjectPrototype());
        }
        uint32_t sid = Realm::Current() ? Realm::Current()->NextShapeId() : 1;
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(this), sid);
        d.ctor_prototype = TaggedValue::FromHeapObject(proto);
        return proto;
    }
    if (d.ctor_prototype.IsHeapObject() && d.ctor_prototype.AsHeapObject()->IsJSObject()) {
        return d.ctor_prototype.As<JSObject>();
    }
    return nullptr;
}

JSError::JSError(std::string_view name, std::string_view message, Shape* shape)
    : JSObject(HeapObjectType::JSError, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(detail::ChargedString(name, name_storage_)),
      message_(detail::ChargedString(message, message_storage_)) {
    uint32_t sid = 8000;
    SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(name_)), sid);
    SetNamedProperty("message", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(message_)), sid);
    if (Realm::Current()) {
        TaggedValue ctor_val = Realm::Current()->GetGlobalProperty(name_);
        if (!ctor_val.IsEmpty() && ctor_val.IsHeapObject()) {
            SetNamedProperty("constructor", ctor_val, sid);
            if (ctor_val.AsHeapObject()->IsJSObject()) {
                TaggedValue proto_val = static_cast<JSObject*>(ctor_val.AsHeapObject())->GetNamedProperty("prototype");
                if (proto_val.IsHeapObject() && proto_val.AsHeapObject()->IsJSObject()) {
                    SetPrototype(static_cast<JSObject*>(proto_val.AsHeapObject()));
                }
            }
        }
    }
}

JSBigInt* JSBigInt::Create(int64_t value) {
    return Create(JSBigInt(value));
}

JSBigInt* JSBigInt::Create(uint64_t value) {
    return Create(JSBigInt(value));
}

JSBigInt* JSBigInt::Create(bool negative, const std::vector<uint32_t>& limbs) {
    return Create(JSBigInt(negative, limbs));
}

JSBigInt* JSBigInt::Create(bool negative, std::vector<uint32_t>&& limbs) {
    return Create(JSBigInt(negative, std::move(limbs)));
}

JSBigInt* JSBigInt::Create(JSBigInt&& value) {
    return AllocateRuntime<JSBigInt>(std::move(value));
}

JSBigInt* JSBigInt::Create(bool negative, std::vector<uint32_t>&& limbs, MemoryCharge&& charge) {
    JSBigInt value(0);
    value.is_negative_ = negative;
    value.limbs_storage_ = std::move(charge);
    value.limbs_ = std::move(limbs);
    value.Normalize();
    return Create(std::move(value));
}

JSArrayBuffer* JSArrayBuffer::Slice(int64_t begin, int64_t end, Shape* shape) const {
    if (is_detached_) throw std::length_error("ArrayBuffer is detached");
    const int64_t len = static_cast<int64_t>(byte_length_);
    const size_t start = static_cast<size_t>(NormalizeIndex(begin, len));
    const size_t finish = static_cast<size_t>(NormalizeIndex(end, len));
    const size_t count = finish > start ? finish - start : 0;
    const bool shared = is_shared_;
    std::optional<HandleScope> scope;
    Handle<JSArrayBuffer> source;
    if (auto* heap = IsolateHeap::Current()) {
        scope.emplace(*heap);
        source = scope->CreateHandle(const_cast<JSArrayBuffer*>(this));
    }
    auto* copy = shared ? AllocateRuntime<JSArrayBuffer>(count, count, true, shape)
                        : AllocateRuntime<JSArrayBuffer>(count, shape);
    const auto* original = scope ? source.Get() : this;
    if (count != 0) std::memcpy(copy->Data(), original->Data() + start, count);
    return copy;
}

JSTypedArray* JSTypedArray::Subarray(int64_t begin, int64_t end, Shape* shape) const {
    if (!buffer_ || buffer_->IsDetached()) throw std::length_error("TypedArray buffer is detached");
    const int64_t len = static_cast<int64_t>(length_);
    const size_t start = static_cast<size_t>(NormalizeIndex(begin, len));
    const size_t finish = static_cast<size_t>(NormalizeIndex(end, len));
    const size_t count = finish > start ? finish - start : 0;
    const size_t delta = CheckedStorageBytes(start, BytesPerElement());
    if (delta > std::numeric_limits<size_t>::max() - byte_offset_) {
        throw std::length_error("TypedArray subarray offset overflow");
    }
    JSTypedArray pending(kind_, buffer_, byte_offset_ + delta, count, shape);
    JSArrayBuffer* buffer = buffer_;
    std::optional<HandleScope> scope;
    Handle<JSArrayBuffer> source;
    if (auto* heap = IsolateHeap::Current()) {
        scope.emplace(*heap);
        source = scope->CreateHandle(buffer);
    }
    // A managed allocation may move the rooted backing object. Publish the
    // refreshed pointer after allocation rather than passing a stale raw arg.
    pending.buffer_ = nullptr;
    auto* result = AllocateRuntime<JSTypedArray>(std::move(pending));
    result->buffer_ = scope ? source.Get() : buffer;
    if (result->IsOld()) RecordWriteBarrier(result, TaggedValue::FromHeapObject(result->buffer_));
    return result;
}


JSArray::JSArray(Shape* shape, size_t initial_capacity)
    : JSObject(HeapObjectType::JSArray, shape) {
    if (AETHER_UNLIKELY(initial_capacity > kMaxArrayLength)) {
        throw std::length_error("array length exceeds maximum");
    }
    if (initial_capacity > 0) {
        elements_ = AllocateRuntimeFixedArrayFor(this,
                                                 static_cast<uint32_t>(initial_capacity),
                                                 static_cast<uint32_t>(initial_capacity),
                                                 TaggedValue::Empty());
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
    }
}

JSArray::JSArray(Shape* shape, const std::vector<TaggedValue>& elements)
    : JSObject(HeapObjectType::JSArray, shape) {
    if (AETHER_UNLIKELY(elements.size() > kMaxArrayLength)) {
        throw std::length_error("array length exceeds maximum");
    }
    uint32_t len = static_cast<uint32_t>(elements.size());
    if (len > 0) {
        elements_ = AllocateRuntimeFixedArrayFor(this, len, len, TaggedValue::Empty());
        std::memcpy(elements_->elements(), elements.data(), len * sizeof(TaggedValue));
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (auto val : elements) {
                if (val.IsHeapObject()) RecordWriteBarrier(elements_, val);
            }
        }
    }
}

JSArray::JSArray(Shape* shape, std::vector<TaggedValue>&& elements)
    : JSArray(shape, elements) {}

void JSArray::SetLength(size_t len) {
    if (AETHER_UNLIKELY(len > kMaxArrayLength)) {
        throw std::length_error("array length exceeds the maximum length");
    }
    uint32_t new_len = static_cast<uint32_t>(len);
    if (!elements_) {
        if (new_len > 0) {
            elements_ = AllocateRuntimeFixedArrayFor(this, new_len, new_len, TaggedValue::Empty());
            if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
                RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
            }
        }
        return;
    }
    if (new_len <= elements_->Capacity()) {
        uint32_t old_len = elements_->Length();
        if (new_len > old_len) {
            for (uint32_t i = old_len; i < new_len; ++i) {
                elements_->elements()[i] = TaggedValue::Empty();
            }
        } else if (new_len < old_len) {
            for (uint32_t i = new_len; i < old_len; ++i) {
                elements_->elements()[i] = TaggedValue::Empty();
            }
        }
        elements_->SetLength(new_len);
    } else {
        uint32_t old_len = elements_->Length();
        uint32_t old_cap = elements_->Capacity();
        uint32_t new_cap = std::max(new_len, old_cap * 2);
        FixedArray* new_elements = AllocateRuntimeFixedArrayFor(this, new_len, new_cap, TaggedValue::Empty());
        std::memcpy(new_elements->elements(), elements_->elements(), old_len * sizeof(TaggedValue));
        elements_ = new_elements;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (uint32_t i = 0; i < old_len; ++i) {
                TaggedValue el = elements_->elements()[i];
                if (el.IsHeapObject()) RecordWriteBarrier(elements_, el);
            }
        }
    }
}

void JSArray::SetElement(size_t index, TaggedValue val) {
    PinnedScope pin_this(this);
    PinnedScope pin_val(val.IsHeapObject() ? val.AsHeapObject() : nullptr);
    IsolateHeap* heap = IsolateHeap::HeapForObject(this);
    if (!heap && val.IsHeapObject()) heap = IsolateHeap::HeapForObject(val.AsHeapObject());
    if (!heap) heap = IsolateHeap::Current();
    if (!heap) heap = &IsolateHeap::RuntimeFallbackHeap();
    HandleScope scope(*heap);
    if (heap->Contains(this)) {
        scope.CreateHandle(this);
    }
    if (val.IsHeapObject() && heap->Contains(val.AsHeapObject())) {
        scope.CreateHandle(val.AsHeapObject());
    }
    if (elements_ && heap->Contains(elements_)) {
        scope.CreateHandle(elements_);
    }
    if (AETHER_UNLIKELY(index >= kMaxArrayLength)) {
        throw std::length_error("array index exceeds the maximum length");
    }
    uint32_t idx = static_cast<uint32_t>(index);
    uint32_t needed_len = idx + 1;
    if (!elements_) {
        uint32_t initial_cap = std::max<uint32_t>(4, needed_len);
        elements_ = AllocateRuntimeFixedArrayFor(this, needed_len, initial_cap, TaggedValue::Empty());
        elements_->elements()[idx] = val;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld() && val.IsHeapObject())) {
            RecordWriteBarrier(elements_, val);
        }
    } else if (needed_len > elements_->Capacity()) {
        uint32_t old_len = elements_->Length();
        uint32_t old_cap = elements_->Capacity();
        uint32_t new_cap = std::max(needed_len, old_cap * 2);
        FixedArray* new_elements = AllocateRuntimeFixedArrayFor(this, needed_len, new_cap, TaggedValue::Empty());
        std::memcpy(new_elements->elements(), elements_->elements(), old_len * sizeof(TaggedValue));
        new_elements->elements()[idx] = val;
        elements_ = new_elements;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (uint32_t i = 0; i <= idx; ++i) {
                TaggedValue el = elements_->elements()[i];
                if (el.IsHeapObject()) RecordWriteBarrier(elements_, el);
            }
        }
    } else {
        if (needed_len > elements_->Length()) {
            uint32_t old_len = elements_->Length();
            for (uint32_t i = old_len; i < needed_len; ++i) {
                elements_->elements()[i] = TaggedValue::Empty();
            }
            elements_->SetLength(needed_len);
        }
        elements_->elements()[idx] = val;
        if (AETHER_UNLIKELY(elements_->IsOld() && val.IsHeapObject())) {
            RecordWriteBarrier(elements_, val);
        }
    }
}

void JSArray::Push(TaggedValue val) {
    if (AETHER_UNLIKELY(Length() >= kMaxArrayLength)) {
        throw std::length_error("array length exceeds the maximum length");
    }
    uint32_t cur_len = elements_ ? elements_->Length() : 0;
    SetElement(cur_len, val);
}

TaggedValue JSArray::Pop() {
    if (!elements_ || elements_->Length() == 0) return TaggedValue::Undefined();
    uint32_t last_idx = elements_->Length() - 1;
    TaggedValue last = elements_->elements()[last_idx];
    elements_->elements()[last_idx] = TaggedValue::Empty();
    elements_->SetLength(last_idx);
    return last.IsEmpty() ? TaggedValue::Undefined() : last;
}

TaggedValue JSArray::Shift() {
    if (!elements_ || elements_->Length() == 0) return TaggedValue::Undefined();
    TaggedValue first = elements_->elements()[0];
    uint32_t old_len = elements_->Length();
    if (old_len > 1) {
        std::memmove(elements_->elements(), elements_->elements() + 1, (old_len - 1) * sizeof(TaggedValue));
    }
    elements_->elements()[old_len - 1] = TaggedValue::Empty();
    elements_->SetLength(old_len - 1);
    return first.IsEmpty() ? TaggedValue::Undefined() : first;
}

void JSArray::Unshift(const TaggedValue* items, size_t count) {
    if (count == 0) return;
    if (AETHER_UNLIKELY(Length() + count > kMaxArrayLength)) {
        throw std::length_error("array length exceeds maximum");
    }
    uint32_t cnt = static_cast<uint32_t>(count);
    uint32_t old_len = elements_ ? elements_->Length() : 0;
    uint32_t new_len = old_len + cnt;
    if (!elements_ || new_len > elements_->Capacity()) {
        uint32_t old_cap = elements_ ? elements_->Capacity() : 0;
        uint32_t new_cap = std::max(new_len, old_cap * 2);
        FixedArray* new_elements = AllocateRuntimeFixedArrayFor(this,
                                                                new_len, new_cap, TaggedValue::Empty());
        if (old_len > 0) {
            std::memcpy(new_elements->elements() + cnt, elements_->elements(), old_len * sizeof(TaggedValue));
        }
        std::memcpy(new_elements->elements(), items, cnt * sizeof(TaggedValue));
        elements_ = new_elements;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (uint32_t i = 0; i < new_len; ++i) {
                TaggedValue el = elements_->elements()[i];
                if (el.IsHeapObject()) RecordWriteBarrier(elements_, el);
            }
        }
    } else {
        if (old_len > 0) {
            std::memmove(elements_->elements() + cnt, elements_->elements(), old_len * sizeof(TaggedValue));
        }
        std::memcpy(elements_->elements(), items, cnt * sizeof(TaggedValue));
        elements_->SetLength(new_len);
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (size_t i = 0; i < count; ++i) {
                if (items[i].IsHeapObject()) RecordWriteBarrier(elements_, items[i]);
            }
        }
    }
}

void JSArray::Reverse() {
    if (elements_ && elements_->Length() > 1) {
        std::reverse(elements_->elements(), elements_->elements() + elements_->Length());
    }
}

void JSArray::Splice(size_t start, size_t delete_count, const TaggedValue* items, size_t insert_count) {
    uint32_t old_len = elements_ ? elements_->Length() : 0;
    if (start > old_len) start = old_len;
    if (start + delete_count > old_len) delete_count = old_len - start;
    if (AETHER_UNLIKELY(static_cast<uint64_t>(old_len) - delete_count + insert_count > kMaxArrayLength)) {
        throw std::length_error("array length exceeds maximum");
    }
    uint32_t new_len = static_cast<uint32_t>(old_len - delete_count + insert_count);
    size_t tail = old_len - (start + delete_count);
    if (!elements_ || new_len > elements_->Capacity()) {
        uint32_t old_cap = elements_ ? elements_->Capacity() : 0;
        uint32_t new_cap = std::max(new_len, old_cap * 2);
        FixedArray* new_elements = AllocateRuntimeFixedArrayFor(this,
                                                                new_len, new_cap, TaggedValue::Empty());
        if (start > 0 && elements_) {
            std::memcpy(new_elements->elements(), elements_->elements(), start * sizeof(TaggedValue));
        }
        if (insert_count > 0 && items) {
            std::memcpy(new_elements->elements() + start, items, insert_count * sizeof(TaggedValue));
        }
        if (tail > 0 && elements_) {
            std::memcpy(new_elements->elements() + start + insert_count,
                        elements_->elements() + start + delete_count,
                        tail * sizeof(TaggedValue));
        }
        elements_ = new_elements;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (uint32_t i = 0; i < new_len; ++i) {
                TaggedValue el = elements_->elements()[i];
                if (el.IsHeapObject()) RecordWriteBarrier(elements_, el);
            }
        }
    } else {
        if (tail > 0) {
            std::memmove(elements_->elements() + start + insert_count,
                         elements_->elements() + start + delete_count,
                         tail * sizeof(TaggedValue));
        }
        if (insert_count > 0 && items) {
            std::memcpy(elements_->elements() + start, items, insert_count * sizeof(TaggedValue));
        }
        if (new_len < old_len) {
            for (uint32_t i = new_len; i < old_len; ++i) {
                elements_->elements()[i] = TaggedValue::Empty();
            }
        }
        elements_->SetLength(new_len);
        if (AETHER_UNLIKELY(elements_->IsOld() && insert_count > 0 && items)) {
            for (size_t i = 0; i < insert_count; ++i) {
                if (items[i].IsHeapObject()) RecordWriteBarrier(elements_, items[i]);
            }
        }
    }
}

void JSArray::ReserveElements(size_t length) {
    if (AETHER_UNLIKELY(length > kMaxArrayLength)) {
        throw std::length_error("array capacity exceeds the maximum length");
    }
    uint32_t new_cap = static_cast<uint32_t>(length);
    if (!elements_) {
        if (new_cap > 0) {
            elements_ = AllocateRuntimeFixedArrayFor(this, 0, new_cap, TaggedValue::Empty());
            if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
                RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
            }
        }
        return;
    }
    if (new_cap > elements_->Capacity()) {
        uint32_t old_len = elements_->Length();
        FixedArray* new_elements = AllocateRuntimeFixedArrayFor(this, old_len, new_cap, TaggedValue::Empty());
        std::memcpy(new_elements->elements(), elements_->elements(), old_len * sizeof(TaggedValue));
        elements_ = new_elements;
        if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
        }
        if (AETHER_UNLIKELY(elements_->IsOld())) {
            for (uint32_t i = 0; i < old_len; ++i) {
                TaggedValue el = elements_->elements()[i];
                if (el.IsHeapObject()) RecordWriteBarrier(elements_, el);
            }
        }
    }
}

void JSArray::ReplaceElements(const std::vector<TaggedValue>& elements) {
    uint32_t len = static_cast<uint32_t>(elements.size());
    if (len == 0) {
        if (elements_) elements_->SetLength(0);
        return;
    }
    elements_ = AllocateRuntimeFixedArrayFor(this, len, len, TaggedValue::Empty());
    std::memcpy(elements_->elements(), elements.data(), len * sizeof(TaggedValue));
    if (AETHER_UNLIKELY(IsOld() && elements_->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(elements_));
    }
    if (AETHER_UNLIKELY(elements_->IsOld())) {
        for (auto val : elements) {
            if (val.IsHeapObject()) RecordWriteBarrier(elements_, val);
        }
    }
}

LexicalEnvironment::LexicalEnvironment(LexicalEnvironment* outer, size_t slot_count)
    : HeapObject(HeapObjectType::LexicalEnvironment, nullptr),
      outer_(outer) {
    if (AETHER_UNLIKELY(slot_count > kMaxObjectProperties)) {
        throw std::length_error("lexical environment slot limit exceeded");
    }
    if (slot_count > 0) {
        slots_ = AllocateRuntimeFixedArrayFor(this,
                                              static_cast<uint32_t>(slot_count),
                                              static_cast<uint32_t>(slot_count),
                                              TaggedValue::Undefined());
        if (AETHER_UNLIKELY(IsOld() && slots_->IsYoung())) {
            RecordWriteBarrier(this, TaggedValue::FromHeapObject(slots_));
        }
    }
    if (AETHER_UNLIKELY(IsOld() && outer_ && outer_->IsYoung())) {
        RecordWriteBarrier(this, TaggedValue::FromHeapObject(outer_));
    }
}

} // namespace aether
