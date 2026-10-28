#include "aether/interpreter.h"
#include "aether/baseline_jit.h"
#include "aether/optimizing_jit.h"
#include "aether/background_compiler.h"
#include "aether/runtime.h"
#include "aether/typed_array.h"
#include "aether/options.h"
#include <cmath>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aether {

namespace {
IsolateHeap& ExecutionHeap() {
    auto* heap = IsolateHeap::Current();
    return heap ? *heap : IsolateHeap::RuntimeFallbackHeap();
}

// Shared by all interpreters on the thread, including native callbacks that
// reenter with another interpreter. Check before entering the large opcode loop.
struct NativeStackGuard {
    static inline thread_local uintptr_t origin = 0;
    uintptr_t previous;
    bool exhausted;
    explicit NativeStackGuard(const void* marker) : previous(origin), exhausted(false) {
        const auto current = reinterpret_cast<uintptr_t>(marker);
        if (!origin) origin = current;
        const auto consumed = current > origin ? current - origin : origin - current;
#if defined(AETHER_SANITIZE_ACTIVE) || defined(__SANITIZE_ADDRESS__) || defined(_MSVC_SANITIZE_ADDRESS)
        constexpr size_t kDefaultStackBudget = 2048 * 1024; // 2 MB for ASan/TSan frame bloat
#else
        constexpr size_t kDefaultStackBudget = 256 * 1024;  // 256 KB release default
#endif
        size_t limit = Options::Get().stack_size_kb > 0 ? (Options::Get().stack_size_kb * 1024) : kDefaultStackBudget;
        exhausted = consumed >= limit;
#ifdef _WIN32
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        if (current >= low && current <= high && current - low < 64 * 1024) exhausted = true;
#endif
    }
    ~NativeStackGuard() { origin = previous; }
};

struct ValueRestore {
    TaggedValue& slot;
    TaggedValue saved;
    explicit ValueRestore(TaggedValue& value) : slot(value), saved(value) {}
    ~ValueRestore() { slot = saved; }
};

struct InstructionRootsGuard {
    HandleScope*& slot;
    HandleScope* previous;
    InstructionRootsGuard(HandleScope*& slot, HandleScope& roots) : slot(slot), previous(slot) { slot = &roots; }
    ~InstructionRootsGuard() { slot = previous; }
};
} // namespace

Interpreter::~Interpreter() {
    if (!root_heap_) return;
    for (auto* root : {&acc_, &current_new_target_, &pending_exception_,
                      &last_environment_root_, &resource_exception_, &stack_exception_, &last_result_}) {
        root_heap_->UnregisterExtraRoot(root);
    }
}

void Interpreter::EnsureRoots() {
    if (root_heap_) {
        assert(root_heap_ == &ExecutionHeap() && "Interpreter belongs to one isolate");
        return;
    }
    auto& heap = ExecutionHeap();
    TaggedValue* roots[] = {&acc_, &current_new_target_, &pending_exception_,
                           &last_environment_root_, &resource_exception_, &stack_exception_, &last_result_};
    size_t registered = 0;
    try {
        for (auto* root : roots) { heap.RegisterExtraRoot(root); ++registered; }
    } catch (...) {
        while (registered) heap.UnregisterExtraRoot(roots[--registered]);
        throw;
    }
    root_heap_ = &heap;
}

void Interpreter::InitializeResourceExceptions() {
    EnsureRoots();
    NoGCScope no_gc(root_heap_);
    if (resource_exception_.IsUndefined()) {
        resource_exception_ = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("RangeError", "Memory budget exceeded"));
    }
    if (stack_exception_.IsUndefined()) {
        stack_exception_ = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("RangeError", "Maximum call stack size exceeded"));
    }
}

void Interpreter::SetResourceException(TaggedValue exception) {
    EnsureRoots();
    resource_exception_ = exception;
}

void Interpreter::ReportResourceError(std::string_view message) noexcept {
    // Never collect here: the failing native operation may have unpublished
    // temporaries. The emergency object is already rooted and needs no budget.
    try {
        SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("RangeError", message)));
    } catch (...) {
        SetPendingException(resource_exception_);
    }
}

TaggedValue RaiseResourceError(std::string_view message) noexcept {
    if (auto* interpreter = Interpreter::Current()) interpreter->ReportResourceError(message);
    return TaggedValue::Undefined();
}

void Interpreter::SnapshotState(HandleScope& roots, const ExecutionFrame* frame) {
    roots.CreateHandle(acc_);
    roots.CreateHandle(current_new_target_);
    roots.CreateHandle(pending_exception_);
    roots.CreateHandle(last_environment_root_);
    roots.CreateHandle(last_lookup_base_);
    for (auto* obj : with_stack_) roots.CreateHandle(obj);
    if (!frame) return;
    roots.CreateHandle(frame->GetThis());
    roots.CreateHandle(frame->GetEnvironment());
    roots.CreateHandle(frame->GetGenerator());
    for (auto value : frame->GetRegisters()) roots.CreateHandle(value);
    for (auto value : frame->Args()) roots.CreateHandle(value);
    for (const auto& handler : frame->Handlers()) roots.CreateHandle(handler.env);
}

TaggedValue Interpreter::PublishResult(TaggedValue result) {
    // A coercion/getter can call JS twice in one opcode. Keep the first result
    // alive through the second call, until the caller's next instruction.
    if (instruction_roots_) {
        instruction_roots_->CreateHandle(result);
        instruction_roots_->CreateHandle(last_environment_root_);
    }
    last_result_ = result;
    return result;
}

static bool StringLessThanUtf16(std::string_view s1, std::string_view s2) {
    bool has_high1 = false;
    for (char c : s1) {
        if (static_cast<uint8_t>(c) >= 0xF0) { has_high1 = true; break; }
    }
    bool has_high2 = false;
    for (char c : s2) {
        if (static_cast<uint8_t>(c) >= 0xF0) { has_high2 = true; break; }
    }
    if (!has_high1 && !has_high2) {
        return s1 < s2;
    }
    auto to_u16 = [](std::string_view utf8) {
        std::vector<uint16_t> u16;
        size_t i = 0;
        while (i < utf8.size()) {
            uint8_t c = static_cast<uint8_t>(utf8[i++]);
            uint32_t cp = 0;
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0) {
                if (i < utf8.size()) {
                    cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i++]) & 0x3F);
                }
            } else if ((c & 0xF0) == 0xE0) {
                if (i + 1 < utf8.size()) {
                    cp = ((c & 0x0F) << 12) |
                         ((static_cast<uint8_t>(utf8[i]) & 0x3F) << 6) |
                         (static_cast<uint8_t>(utf8[i + 1]) & 0x3F);
                    i += 2;
                }
            } else if ((c & 0xF8) == 0xF0) {
                if (i + 2 < utf8.size()) {
                    cp = ((c & 0x07) << 18) |
                         ((static_cast<uint8_t>(utf8[i]) & 0x3F) << 12) |
                         ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) |
                         (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
                    i += 3;
                }
            }
            if (cp < 0x10000) {
                u16.push_back(static_cast<uint16_t>(cp));
            } else {
                cp -= 0x10000;
                u16.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
                u16.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
            }
        }
        return u16;
    };
    std::vector<uint16_t> u1 = to_u16(s1);
    std::vector<uint16_t> u2 = to_u16(s2);
    return u1 < u2;
}

static JSBigInt* StringToBigInt(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r' || s.front() == '\f' || s.front() == '\v')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' || s.back() == '\f' || s.back() == '\v')) {
        s.remove_suffix(1);
    }
    if (s.empty()) return AllocateRuntime<JSBigInt>(0);
    bool is_neg = false;
    if (s.front() == '+') {
        s.remove_prefix(1);
    } else if (s.front() == '-') {
        is_neg = true;
        s.remove_prefix(1);
    }
    if (s.empty()) return nullptr;
    int base = 10;
    if (s.size() >= 2 && s[0] == '0') {
        char prefix = s[1];
        if (prefix == 'x' || prefix == 'X') {
            if (is_neg) return nullptr;
            base = 16;
            s.remove_prefix(2);
        } else if (prefix == 'o' || prefix == 'O') {
            if (is_neg) return nullptr;
            base = 8;
            s.remove_prefix(2);
        } else if (prefix == 'b' || prefix == 'B') {
            if (is_neg) return nullptr;
            base = 2;
            s.remove_prefix(2);
        }
    }
    if (s.empty()) return nullptr;
    for (char c : s) {
        if (base == 10 && (c < '0' || c > '9')) return nullptr;
        else if (base == 16 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return nullptr;
        else if (base == 8 && (c < '0' || c > '7')) return nullptr;
        else if (base == 2 && (c != '0' && c != '1')) return nullptr;
    }
    std::string full_str;
    if (is_neg) full_str += '-';
    if (base == 16) full_str += "0x";
    else if (base == 8) full_str += "0o";
    else if (base == 2) full_str += "0b";
    full_str.append(s);
    return AllocateRuntime<JSBigInt>(full_str);
}

static JSBigInt* DoubleToBigInt(double d) {
    if (d == 0.0) return AllocateRuntime<JSBigInt>(0);
    bool is_neg = (d < 0.0);
    d = std::abs(d);
    int exp = 0;
    double mant = std::frexp(d, &exp);
    uint64_t mant_int = static_cast<uint64_t>(mant * 9007199254740992.0); // 2^53
    int shift = exp - 53;
    std::vector<uint32_t> limbs;
    limbs.push_back(static_cast<uint32_t>(mant_int & 0xFFFFFFFFULL));
    limbs.push_back(static_cast<uint32_t>(mant_int >> 32));
    if (shift > 0) {
        int word_shift = shift / 32;
        int bit_shift = shift % 32;
        std::vector<uint32_t> res(word_shift, 0);
        uint64_t carry = 0;
        for (uint32_t l : limbs) {
            uint64_t val = (static_cast<uint64_t>(l) << bit_shift) | carry;
            res.push_back(static_cast<uint32_t>(val & 0xFFFFFFFFULL));
            carry = val >> 32;
        }
        if (carry) res.push_back(static_cast<uint32_t>(carry));
        limbs = std::move(res);
    } else if (shift < 0) {
        int rshift = -shift;
        if (rshift >= 64) {
            limbs.clear();
        } else {
            uint64_t val = mant_int >> rshift;
            limbs.clear();
            limbs.push_back(static_cast<uint32_t>(val & 0xFFFFFFFFULL));
            limbs.push_back(static_cast<uint32_t>(val >> 32));
        }
    }
    return AllocateRuntime<JSBigInt>(is_neg, std::move(limbs));
}

static int CompareBigIntAndNumber(const JSBigInt& bi, double d) {
    if (std::isnan(d)) return 2; // unordered
    if (std::isinf(d)) {
        return (d > 0.0) ? -1 : 1;
    }
    if (d == std::trunc(d)) {
        JSBigInt* bi_d = DoubleToBigInt(d);
        int res = 0;
        if (bi.LessThan(*bi_d)) res = -1;
        else if (bi_d->LessThan(bi)) res = 1;
        return res;
    }
    double d_int = std::trunc(d);
    double d_frac = d - d_int;
    JSBigInt* bi_d = DoubleToBigInt(d_int);
    int res = 0;
    if (bi.LessThan(*bi_d)) res = -1;
    else if (bi_d->LessThan(bi)) res = 1;
    else {
        res = (d_frac > 0.0) ? -1 : 1;
    }
    return res;
}

static inline int32_t ToInt32(TaggedValue v) {
    if (AETHER_LIKELY(v.IsSmi())) return v.AsSmi();
    double d = v.ToDouble();
    if (std::isnan(d) || std::isinf(d) || d == 0.0) return 0;
    d = std::trunc(d);
    double two32 = 4294967296.0;
    double int32_bit = std::fmod(d, two32);
    if (int32_bit < 0.0) int32_bit += two32;
    if (int32_bit >= 2147483648.0) return static_cast<int32_t>(int32_bit - two32);
    return static_cast<int32_t>(int32_bit);
}

static inline uint32_t ToUint32(TaggedValue v) {
    return static_cast<uint32_t>(ToInt32(v));
}

TaggedValue Interpreter::ToPrimitive(TaggedValue input, const char* hint) {
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(input);
    if (!input.IsHeapObject()) return input;
    HeapObject* ho = input.AsHeapObject();
    if (!ho->IsJSObject() && !ho->IsProxy()) return input;

    std::string prim_key = "@@toPrimitive";
    TaggedValue to_prim_fn;
    if (ho->IsProxy()) {
        to_prim_fn = ProxyGet(static_cast<JSProxy*>(ho), prim_key, input);
    } else {
        to_prim_fn = static_cast<JSObject*>(ho)->GetNamedProperty(prim_key);
    }
    if (has_pending_exception_) return TaggedValue::Undefined();

    if (!to_prim_fn.IsUndefined() && !to_prim_fn.IsNull()) {
        roots.CreateHandle(to_prim_fn);
        if (!to_prim_fn.IsHeapObject() || !to_prim_fn.AsHeapObject()->IsFunction()) {
            SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Symbol.toPrimitive is not a function")));
            return TaggedValue::Undefined();
        }
        TaggedValue hint_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(hint));
        TaggedValue res = CallFunction(to_prim_fn, input, {hint_val});
        if (has_pending_exception_) return TaggedValue::Undefined();
        if (!res.IsHeapObject() || (!res.AsHeapObject()->IsJSObject() && !res.AsHeapObject()->IsProxy())) {
            return PublishResult(res);
        }
        SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("TypeError", "Cannot convert object to primitive value")));
        return TaggedValue::Undefined();
    }

    const char* first_method = (std::strcmp(hint, "string") == 0) ? "toString" : "valueOf";
    const char* second_method = (std::strcmp(hint, "string") == 0) ? "valueOf" : "toString";

    for (const char* m_name : {first_method, second_method}) {
        TaggedValue method;
        if (ho->IsProxy()) {
            method = ProxyGet(static_cast<JSProxy*>(ho), m_name, input);
        } else {
            method = static_cast<JSObject*>(ho)->GetNamedProperty(m_name);
        }
        if (has_pending_exception_) return TaggedValue::Undefined();
        if (method.IsHeapObject() && method.AsHeapObject()->IsFunction()) {
            roots.CreateHandle(method);
            TaggedValue res = CallFunction(method, input, {});
            if (has_pending_exception_) return TaggedValue::Undefined();
            if (!res.IsHeapObject() || (!res.AsHeapObject()->IsJSObject() && !res.AsHeapObject()->IsProxy())) {
                return PublishResult(res);
            }
        }
    }

    SetPendingException(TaggedValue::FromHeapObject(
        AllocateRuntime<JSError>("TypeError", "Cannot convert object to primitive value")));
    return TaggedValue::Undefined();
}

TaggedValue Interpreter::ToStringOp(TaggedValue val) {
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(val);
    if (val.IsHeapObject()) {
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsString()) return val;
        if (ho->IsSymbol()) {
            SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a string")));
            return TaggedValue::Undefined();
        }
        if (ho->IsJSObject() || ho->IsProxy()) {
            TaggedValue prim = ToPrimitive(val, "string");
            if (has_pending_exception_) return TaggedValue::Undefined();
            if (prim.IsHeapObject() && prim.AsHeapObject()->IsString()) return prim;
            if (prim.IsHeapObject() && prim.AsHeapObject()->IsSymbol()) {
                SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a string")));
                return TaggedValue::Undefined();
            }
            return PublishResult(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(prim.ToString())));
        }
    }
    return PublishResult(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(val.ToString())));
}

TaggedValue Interpreter::ToNumeric(TaggedValue val) {
    if (val.IsHeapObject()) {
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsJSObject() || ho->IsProxy()) {
            val = ToPrimitive(val, "number");
            if (has_pending_exception_) return TaggedValue::Undefined();
        }
    }
    if (val.IsBigInt()) {
        return val;
    }
    if (val.IsSymbol()) {
        SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a number")));
        return TaggedValue::Undefined();
    }
    if (val.IsSmi() || val.IsDouble()) {
        return val;
    }
    return TaggedValue::FromDouble(val.ToDouble());
}

TaggedValue Interpreter::ToPropertyKey(TaggedValue val) {
    if (val.IsHeapObject()) {
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsString() || ho->IsSymbol()) {
            return val;
        }
        TaggedValue prim = ToPrimitive(val, "string");
        if (has_pending_exception_) return TaggedValue::Undefined();
        if (prim.IsHeapObject() && prim.AsHeapObject()->IsSymbol()) {
            return prim;
        }
        return ToStringOp(prim);
    }
    if (val.IsSmi()) {
        return val;
    }
    return ToStringOp(val);
}

Interpreter::RelationalResult Interpreter::AbstractRelationalComparison(TaggedValue x, TaggedValue y, bool left_first) {
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(x);
    roots.CreateHandle(y);
    TaggedValue px, py;
    if (left_first) {
        px = ToPrimitive(x, "number");
        roots.CreateHandle(px);
        if (has_pending_exception_) return RelationalResult::Undefined;
        py = ToPrimitive(y, "number");
        roots.CreateHandle(py);
        if (has_pending_exception_) return RelationalResult::Undefined;
    } else {
        py = ToPrimitive(y, "number");
        roots.CreateHandle(py);
        if (has_pending_exception_) return RelationalResult::Undefined;
        px = ToPrimitive(x, "number");
        roots.CreateHandle(px);
        if (has_pending_exception_) return RelationalResult::Undefined;
    }

    if (px.IsHeapObject() && px.AsHeapObject()->IsString() &&
        py.IsHeapObject() && py.AsHeapObject()->IsString()) {
        std::string_view s1 = static_cast<JSString*>(px.AsHeapObject())->View();
        std::string_view s2 = static_cast<JSString*>(py.AsHeapObject())->View();
        return StringLessThanUtf16(s1, s2) ? RelationalResult::True : RelationalResult::False;
    }

    if (px.IsBigInt() && py.IsHeapObject() && py.AsHeapObject()->IsString()) {
        std::string_view s = static_cast<JSString*>(py.AsHeapObject())->View();
        JSBigInt* ny = StringToBigInt(s);
        if (!ny) return RelationalResult::Undefined;
        auto* bx = static_cast<JSBigInt*>(px.AsHeapObject());
        bool res = bx->LessThan(*ny);
        return res ? RelationalResult::True : RelationalResult::False;
    }

    if (px.IsHeapObject() && px.AsHeapObject()->IsString() && py.IsBigInt()) {
        std::string_view s = static_cast<JSString*>(px.AsHeapObject())->View();
        JSBigInt* nx = StringToBigInt(s);
        if (!nx) return RelationalResult::Undefined;
        auto* by = static_cast<JSBigInt*>(py.AsHeapObject());
        bool res = nx->LessThan(*by);
        return res ? RelationalResult::True : RelationalResult::False;
    }

    TaggedValue nx = ToNumeric(px);
    if (has_pending_exception_) return RelationalResult::Undefined;
    TaggedValue ny = ToNumeric(py);
    if (has_pending_exception_) return RelationalResult::Undefined;

    if (nx.IsBigInt() && ny.IsBigInt()) {
        auto* bx = static_cast<JSBigInt*>(nx.AsHeapObject());
        auto* by = static_cast<JSBigInt*>(ny.AsHeapObject());
        return bx->LessThan(*by) ? RelationalResult::True : RelationalResult::False;
    }

    if (nx.IsBigInt() && !ny.IsBigInt()) {
        auto* bx = static_cast<JSBigInt*>(nx.AsHeapObject());
        double dy = ny.ToDouble();
        int cmp = CompareBigIntAndNumber(*bx, dy);
        if (cmp == 2) return RelationalResult::Undefined;
        return (cmp == -1) ? RelationalResult::True : RelationalResult::False;
    }

    if (!nx.IsBigInt() && ny.IsBigInt()) {
        auto* by = static_cast<JSBigInt*>(ny.AsHeapObject());
        double dx = nx.ToDouble();
        int cmp = CompareBigIntAndNumber(*by, dx);
        if (cmp == 2) return RelationalResult::Undefined;
        return (cmp == 1) ? RelationalResult::True : RelationalResult::False;
    }

    double dx = nx.ToDouble();
    double dy = ny.ToDouble();
    if (std::isnan(dx) || std::isnan(dy)) return RelationalResult::Undefined;
    return (dx < dy) ? RelationalResult::True : RelationalResult::False;
}

bool Interpreter::IsStrictlyEqual(TaggedValue x, TaggedValue y) {
    if (x.IsSmi() && y.IsSmi()) return x.AsSmi() == y.AsSmi();
    if (x.IsNumber() && y.IsNumber()) {
        double d1 = x.ToDouble();
        double d2 = y.ToDouble();
        if (std::isnan(d1) || std::isnan(d2)) return false;
        return d1 == d2;
    }
    if (x.IsBoolean() && y.IsBoolean()) return x.ToBoolean() == y.ToBoolean();
    if (x.IsNull() && y.IsNull()) return true;
    if (x.IsUndefined() && y.IsUndefined()) return true;
    if (x.IsHeapObject() && y.IsHeapObject()) {
        HeapObject* h1 = x.AsHeapObject();
        HeapObject* h2 = y.AsHeapObject();
        if (h1 == h2) return true;
        if (h1->IsString() && h2->IsString()) {
            return static_cast<JSString*>(h1)->View() == static_cast<JSString*>(h2)->View();
        }
        if (h1->IsBigInt() && h2->IsBigInt()) {
            return static_cast<JSBigInt*>(h1)->Equals(*static_cast<JSBigInt*>(h2));
        }
        if (h1->IsSymbol() && h2->IsSymbol()) {
            return h1 == h2;
        }
        return false;
    }
    return false;
}

bool Interpreter::IsLooselyEqual(TaggedValue x, TaggedValue y) {
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(x);
    roots.CreateHandle(y);
    // 1. Same type
    if ((x.IsNumber() && y.IsNumber()) ||
        (x.IsBoolean() && y.IsBoolean()) ||
        (x.IsNull() && y.IsNull()) ||
        (x.IsUndefined() && y.IsUndefined()) ||
        (x.IsHeapObject() && y.IsHeapObject() && x.AsHeapObject()->GetType() == y.AsHeapObject()->GetType())) {
        return IsStrictlyEqual(x, y);
    }

    // 2 & 3. null and undefined
    if (x.IsNull() && y.IsUndefined()) return true;
    if (x.IsUndefined() && y.IsNull()) return true;

    // 5 & 6. Number and String
    if (x.IsNumber() && y.IsHeapObject() && y.AsHeapObject()->IsString()) {
        return IsLooselyEqual(x, TaggedValue::FromDouble(y.ToDouble()));
    }
    if (x.IsHeapObject() && x.AsHeapObject()->IsString() && y.IsNumber()) {
        return IsLooselyEqual(TaggedValue::FromDouble(x.ToDouble()), y);
    }

    // 7 & 8. BigInt and String
    if (x.IsBigInt() && y.IsHeapObject() && y.AsHeapObject()->IsString()) {
        std::string_view s = static_cast<JSString*>(y.AsHeapObject())->View();
        JSBigInt* n = StringToBigInt(s);
        if (!n) return false;
        bool res = static_cast<JSBigInt*>(x.AsHeapObject())->Equals(*n);
        return res;
    }
    if (x.IsHeapObject() && x.AsHeapObject()->IsString() && y.IsBigInt()) {
        return IsLooselyEqual(y, x);
    }

    // 9 & 10. Boolean
    if (x.IsBoolean()) {
        return IsLooselyEqual(TaggedValue::FromDouble(x.ToBoolean() ? 1.0 : 0.0), y);
    }
    if (y.IsBoolean()) {
        return IsLooselyEqual(x, TaggedValue::FromDouble(y.ToBoolean() ? 1.0 : 0.0));
    }

    // 11 & 12. Primitive and Object
    auto is_prim = [](TaggedValue v) {
        if (!v.IsHeapObject()) return true;
        auto t = v.AsHeapObject()->GetType();
        return t == HeapObjectType::String || t == HeapObjectType::Symbol || t == HeapObjectType::JSBigInt;
    };
    auto is_obj = [](TaggedValue v) {
        if (!v.IsHeapObject()) return false;
        auto* ho = v.AsHeapObject();
        return ho->IsJSObject() || ho->IsProxy();
    };

    if (is_prim(x) && is_obj(y)) {
        TaggedValue py = ToPrimitive(y, "default");
        if (has_pending_exception_) return false;
        return IsLooselyEqual(x, py);
    }
    if (is_obj(x) && is_prim(y)) {
        TaggedValue px = ToPrimitive(x, "default");
        if (has_pending_exception_) return false;
        return IsLooselyEqual(px, y);
    }

    // 13. BigInt and Number
    if (x.IsBigInt() && y.IsNumber()) {
        double d = y.ToDouble();
        if (std::isnan(d) || std::isinf(d)) return false;
        if (d != std::trunc(d)) return false;
        JSBigInt* bi_d = DoubleToBigInt(d);
        bool eq = static_cast<JSBigInt*>(x.AsHeapObject())->Equals(*bi_d);
        return eq;
    }
    if (x.IsNumber() && y.IsBigInt()) {
        return IsLooselyEqual(y, x);
    }

    return false;
}

TaggedValue Interpreter::Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args, TaggedValue this_val, LexicalEnvironment* env) {
    char stack_marker;
    NativeStackGuard stack_guard(&stack_marker);
    try {
        InitializeResourceExceptions();
        if (stack_guard.exhausted || call_depth_ >= kMaxCallDepth) {
            SetPendingException(stack_exception_);
            return pending_exception_;
        }
        HandleScope entry_roots(ExecutionHeap());
        SnapshotState(entry_roots);
        entry_roots.CreateHandle(this_val);
        entry_roots.CreateHandle(env);
        for (auto value : args) entry_roots.CreateHandle(value);
        for (auto value : bytecode.Constants()) entry_roots.CreateHandle(value);
        ValueRestore accumulator(acc_);
        struct LookupGuard {
            Interpreter& interp;
            JSObject* base;
            std::string name;
            int reg;
            ~LookupGuard() {
                interp.last_lookup_base_ = base;
                interp.last_lookup_name_ = std::move(name);
                interp.last_lookup_reg_ = reg;
            }
        } lookup{*this, last_lookup_base_, std::move(last_lookup_name_), last_lookup_reg_};
        ClearLastLookup();
        return PublishResult(ExecuteImpl(bytecode, args, this_val, env));
    } catch (const std::length_error& e) {
        ReportResourceError(e.what());
    } catch (const std::bad_alloc&) {
        ReportResourceError();
    }
    return pending_exception_;
}

TaggedValue Interpreter::ExecuteImpl(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args, TaggedValue this_val, LexicalEnvironment* env) {
    struct InterpreterScope {
        Interpreter* prev;
        InterpreterScope(Interpreter* cur) : prev(Interpreter::s_current_interpreter_) {
            Interpreter::s_current_interpreter_ = cur;
        }
        ~InterpreterScope() {
            Interpreter::s_current_interpreter_ = prev;
        }
    };
    InterpreterScope current_scope(this);

    struct CallDepthGuard {
        uint32_t& depth;
        explicit CallDepthGuard(uint32_t& d) : depth(d) { depth++; }
        ~CallDepthGuard() { depth--; }
    } depth_guard(call_depth_);

    if (!Options::Get().interpreter_only) {
        // 1. Direct dispatch to Tier 2 Optimizing JIT if already compiled
        if (auto t2 = bytecode.GetTier2SharedCode()) {
            TaggedValue res = BaselineJIT::Invoke(*t2, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
            if (!res.IsEmpty()) return res;
        }

        // 2. Direct dispatch to Tier 1 Baseline JIT if already compiled; promote to Tier 2 when hot
        if (auto t1 = bytecode.GetTier1SharedCode()) {
            bytecode.IncrementInvocationCount();
            size_t opt_threshold = Options::Get().always_optimize ? 1 :
                (Options::Get().tier_threshold > 0 ? static_cast<size_t>(Options::Get().tier_threshold) : OptimizingJIT::kInvocationThreshold);
            if (bytecode.InvocationCount() >= opt_threshold && OptimizingJIT::CanCompile(bytecode)) {
                if (BackgroundCompiler::IsEnabled() && BackgroundCompiler::Current()) {
                    if (bytecode.TryBeginTier2Compilation()) {
                        auto fb_snap = feedback_.Snapshot();
                        BackgroundCompiler::Current()->Enqueue(bytecode, fb_snap.get());
                    }
                } else {
                    auto opt_code = OptimizingJIT::Compile(bytecode, &feedback_);
                    if (opt_code) {
                        bytecode.SetTier2Code(std::move(opt_code));
                        TaggedValue res = BaselineJIT::Invoke(*bytecode.GetTier2Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
                        if (!res.IsEmpty()) return res;
                    }
                }
            }
            TaggedValue res = BaselineJIT::Invoke(*t1, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
            if (!res.IsEmpty()) return res;
        }

        // 3. Increment invocation profile counter; promote to Tier 2 if hot enough, or Tier 1
        bytecode.IncrementInvocationCount();
        size_t opt_threshold = Options::Get().always_optimize ? 1 :
            (Options::Get().tier_threshold > 0 ? static_cast<size_t>(Options::Get().tier_threshold) : OptimizingJIT::kInvocationThreshold);
        if (bytecode.InvocationCount() >= opt_threshold && OptimizingJIT::CanCompile(bytecode)) {
            if (BackgroundCompiler::IsEnabled() && BackgroundCompiler::Current()) {
                if (bytecode.TryBeginTier2Compilation()) {
                    auto fb_snap = feedback_.Snapshot();
                    BackgroundCompiler::Current()->Enqueue(bytecode, fb_snap.get());
                }
            } else {
                auto opt_code = OptimizingJIT::Compile(bytecode, &feedback_);
                if (opt_code) {
                    bytecode.SetTier2Code(std::move(opt_code));
                    TaggedValue res = BaselineJIT::Invoke(*bytecode.GetTier2Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
                    if (!res.IsEmpty()) return res;
                }
            }
        }
        size_t baseline_threshold = Options::Get().always_baseline ? 1 :
            (Options::Get().tier_threshold > 0 ? static_cast<size_t>(Options::Get().tier_threshold) : BaselineJIT::kInvocationThreshold);
        if (bytecode.InvocationCount() >= baseline_threshold && BaselineJIT::CanCompile(bytecode)) {
            auto jit_code = BaselineJIT::Compile(bytecode);
            if (jit_code) {
                bytecode.SetTier1Code(std::move(jit_code));
                TaggedValue res = BaselineJIT::Invoke(*bytecode.GetTier1Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
                if (!res.IsEmpty()) return res;
            }
        }
    }

    bool prev_strict = is_strict_mode_;
    is_strict_mode_ = bytecode.IsStrict();
    struct StrictGuard {
        Interpreter* interp;
        bool prev;
        ~StrictGuard() { interp->is_strict_mode_ = prev; }
    } strict_guard{this, prev_strict};

    TaggedValue effective_this = this_val;
    if (!is_strict_mode_ && (effective_this.IsUndefined() || effective_this.IsNull()) && Realm::Current() && Realm::Current()->GetGlobalObject()) {
        effective_this = TaggedValue::FromHeapObject(Realm::Current()->GetGlobalObject());
    }

    ExecutionFrame frame(bytecode, args, effective_this, env);
    bool is_outermost = (top_level_frame_ == nullptr);
    if (is_outermost) {
        top_level_frame_ = &frame;
    }
    struct TopFrameGuard {
        Interpreter* interp;
        bool is_outermost;
        ~TopFrameGuard() {
            if (is_outermost) interp->top_level_frame_ = nullptr;
        }
    } top_frame_guard{this, is_outermost};

    acc_ = TaggedValue::Undefined();
    return RunExecutionLoop(frame, bytecode.Code().data(), nullptr);
}

TaggedValue Interpreter::ResumeGenerator(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection, bool* out_is_await) {
    char stack_marker;
    NativeStackGuard stack_guard(&stack_marker);
    try {
        InitializeResourceExceptions();
        if (stack_guard.exhausted || call_depth_ >= kMaxCallDepth) {
            SetPendingException(stack_exception_);
            return pending_exception_;
        }
        HandleScope roots(ExecutionHeap());
        SnapshotState(roots);
        roots.CreateHandle(gen);
        roots.CreateHandle(sent_val);
        ValueRestore accumulator(acc_);
        struct LookupGuard {
            Interpreter& interp;
            JSObject* base;
            std::string name;
            int reg;
            ~LookupGuard() {
                interp.last_lookup_base_ = base;
                interp.last_lookup_name_ = std::move(name);
                interp.last_lookup_reg_ = reg;
            }
        } lookup{*this, last_lookup_base_, std::move(last_lookup_name_), last_lookup_reg_};
        ClearLastLookup();
        auto* previous = Current();
        SetCurrent(this);
        struct Guard {
            Interpreter* previous;
            uint32_t& depth;
            ~Guard() { --depth; Interpreter::SetCurrent(previous); }
        } guard{previous, call_depth_};
        ++call_depth_;
        return PublishResult(ResumeGeneratorImpl(gen, sent_val, is_rejection, out_is_await));
    } catch (const std::length_error& e) {
        ReportResourceError(e.what());
    } catch (const std::bad_alloc&) {
        ReportResourceError();
    }
    if (gen) gen->SetState(JSGeneratorObject::State::Completed);
    return pending_exception_;
}

TaggedValue Interpreter::ResumeGeneratorImpl(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection, bool* out_is_await) {
    if (!gen) return TaggedValue::Undefined();
    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        if (gen->IsAsync()) return TaggedValue::Undefined();
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    }
    if (gen->GetState() == JSGeneratorObject::State::Executing) {
        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Generator is already running"));
        has_pending_exception_ = true;
        pending_exception_ = exc;
        return exc;
    }

    gen->SetState(JSGeneratorObject::State::Executing);
    const BytecodeArray& bytecode = *gen->Function()->GetBytecode();
    ExecutionFrame frame(bytecode, gen->Args(), gen->ThisVal(), gen->Environment());
    frame.SetGenerator(gen);

    const uint8_t* code_start = bytecode.Code().data();
    const uint8_t* start_pc = code_start;
    if (gen->PCOffset() > 0) {
        frame.GetRegisters() = gen->SavedRegisters();
        for (const auto& sh : gen->SavedHandlers()) {
            frame.PushExceptionHandler(code_start + sh.target_offset, sh.env);
        }
        start_pc = code_start + gen->PCOffset();
        acc_ = sent_val;
    } else {
        acc_ = TaggedValue::Undefined();
    }

    if (is_rejection) {
        if (frame.HasExceptionHandler()) {
            auto h = frame.PopExceptionHandler();
            start_pc = h.target_pc;
            frame.SetEnvironment(h.env);
            acc_ = sent_val;
            has_pending_exception_ = false;
        } else {
            has_pending_exception_ = true;
            pending_exception_ = sent_val;
            gen->SetState(JSGeneratorObject::State::Completed);
            return pending_exception_;
        }
    }

    bool prev_strict = is_strict_mode_;
    is_strict_mode_ = bytecode.IsStrict();
    struct StrictGuard {
        Interpreter* interp;
        bool prev;
        ~StrictGuard() { interp->is_strict_mode_ = prev; }
    } strict_guard{this, prev_strict};

    bool yielded = false;
    TaggedValue result = RunExecutionLoop(frame, start_pc, &yielded, out_is_await);

    if (has_pending_exception_) {
        gen->SetState(JSGeneratorObject::State::Completed);
        return pending_exception_;
    }

    if (gen->IsAsync()) {
        if (yielded) {
            return result;
        } else {
            gen->SetState(JSGeneratorObject::State::Completed);
            return result;
        }
    }

    if (yielded) {
        return TaggedValue::FromHeapObject(CreateIterResult(result, false));
    } else {
        gen->SetState(JSGeneratorObject::State::Completed);
        return TaggedValue::FromHeapObject(CreateIterResult(result, true));
    }
}

void Interpreter::RunAsyncStep(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection) {
    if (!gen) return;
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(gen);
    roots.CreateHandle(sent_val);
    JSPromise* outer_promise = gen->AsyncPromise();
    ClearPendingException();

    TaggedValue step_result = ResumeGenerator(gen, sent_val, is_rejection);

    if (HasPendingException()) {
        TaggedValue reason = PendingException();
        ClearPendingException();
        if (outer_promise) {
            outer_promise->Reject(reason, CurrentMicrotaskQueue());
        }
        return;
    }

    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        if (outer_promise) {
            outer_promise->Fulfill(step_result, CurrentMicrotaskQueue());
        }
        return;
    }

    JSPromise* awaited_promise = nullptr;
    if (step_result.IsHeapObject() && step_result.AsHeapObject()->IsPromise()) {
        awaited_promise = static_cast<JSPromise*>(step_result.AsHeapObject());
    } else {
        awaited_promise = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
            awaited_promise->SetPrototype(Realm::Current()->GetPromisePrototype());
        }
        awaited_promise->Fulfill(step_result, CurrentMicrotaskQueue());
    }

    auto* on_fulfilled = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
        TaggedValue v = args.empty() ? TaggedValue::Undefined() : args[0];
        this->RunAsyncStep(gen, v, false);
        return TaggedValue::Undefined();
    });
    on_fulfilled->AddCapturedRoot(TaggedValue::FromHeapObject(gen));

    auto* on_rejected = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
        TaggedValue r = args.empty() ? TaggedValue::Undefined() : args[0];
        this->RunAsyncStep(gen, r, true);
        return TaggedValue::Undefined();
    });
    on_rejected->AddCapturedRoot(TaggedValue::FromHeapObject(gen));

    awaited_promise->AddReaction(TaggedValue::FromHeapObject(on_fulfilled), TaggedValue::FromHeapObject(on_rejected), nullptr, CurrentMicrotaskQueue());
}

void Interpreter::RunAsyncGeneratorStep(JSGeneratorObject* gen) {
    if (!gen) return;
    if (gen->IsRunningAsync()) return;
    if (gen->AsyncQueue().empty()) return;

    gen->SetRunningAsync(true);
    ExecuteAsyncGeneratorRequest(gen);
}

void Interpreter::ExecuteAsyncGeneratorRequest(JSGeneratorObject* gen, TaggedValue resume_val, bool is_rejection) {
    HandleScope roots(ExecutionHeap());
    roots.CreateHandle(gen);
    roots.CreateHandle(resume_val);
    if (!gen || gen->AsyncQueue().empty()) {
        if (gen) gen->SetRunningAsync(false);
        return;
    }

    auto& req = gen->AsyncQueue().front();
    JSPromise* promise = req.promise;

    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        TaggedValue res_val = req.type == JSGeneratorObject::AsyncRequest::Type::Return
            ? req.value : TaggedValue::Undefined();
        if (req.type == JSGeneratorObject::AsyncRequest::Type::Throw) {
            if (promise) promise->Reject(req.value, CurrentMicrotaskQueue());
        } else {
            if (promise) promise->Fulfill(TaggedValue::FromHeapObject(CreateIterResult(res_val, true)), CurrentMicrotaskQueue());
        }
        gen->AsyncQueue().erase(gen->AsyncQueue().begin());
        gen->SetRunningAsync(false);
        RunAsyncGeneratorStep(gen);
        return;
    }

    if (req.type == JSGeneratorObject::AsyncRequest::Type::Return) {
        gen->SetState(JSGeneratorObject::State::Completed);
        if (promise) promise->Fulfill(TaggedValue::FromHeapObject(CreateIterResult(req.value, true)), CurrentMicrotaskQueue());
        gen->AsyncQueue().erase(gen->AsyncQueue().begin());
        gen->SetRunningAsync(false);
        RunAsyncGeneratorStep(gen);
        return;
    }

    TaggedValue sent_val = resume_val.IsEmpty() ? req.value : resume_val;
    bool rejection = is_rejection || (resume_val.IsEmpty() && req.type == JSGeneratorObject::AsyncRequest::Type::Throw);

    ClearPendingException();
    bool was_await = false;
    TaggedValue step_result = ResumeGenerator(gen, sent_val, rejection, &was_await);

    if (HasPendingException()) {
        TaggedValue reason = PendingException();
        ClearPendingException();
        gen->SetState(JSGeneratorObject::State::Completed);
        if (promise) promise->Reject(reason, CurrentMicrotaskQueue());
        gen->AsyncQueue().erase(gen->AsyncQueue().begin());
        gen->SetRunningAsync(false);
        RunAsyncGeneratorStep(gen);
        return;
    }

    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        if (promise) promise->Fulfill(TaggedValue::FromHeapObject(CreateIterResult(step_result, true)), CurrentMicrotaskQueue());
        gen->AsyncQueue().erase(gen->AsyncQueue().begin());
        gen->SetRunningAsync(false);
        RunAsyncGeneratorStep(gen);
        return;
    }

    if (was_await) {
        JSPromise* awaited_promise = nullptr;
        if (step_result.IsHeapObject() && step_result.AsHeapObject()->IsPromise()) {
            awaited_promise = static_cast<JSPromise*>(step_result.AsHeapObject());
        } else {
            awaited_promise = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
                awaited_promise->SetPrototype(Realm::Current()->GetPromisePrototype());
            }
            awaited_promise->Fulfill(step_result, CurrentMicrotaskQueue());
        }

        auto* on_fulfilled = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
            TaggedValue v = args.empty() ? TaggedValue::Undefined() : args[0];
            this->ExecuteAsyncGeneratorRequest(gen, v, false);
            return TaggedValue::Undefined();
        });
        on_fulfilled->AddCapturedRoot(TaggedValue::FromHeapObject(gen));

        auto* on_rejected = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
            TaggedValue r = args.empty() ? TaggedValue::Undefined() : args[0];
            this->ExecuteAsyncGeneratorRequest(gen, r, true);
            return TaggedValue::Undefined();
        });
        on_rejected->AddCapturedRoot(TaggedValue::FromHeapObject(gen));

        awaited_promise->AddReaction(TaggedValue::FromHeapObject(on_fulfilled), TaggedValue::FromHeapObject(on_rejected), nullptr, CurrentMicrotaskQueue());
    } else {
        if (promise) promise->Fulfill(TaggedValue::FromHeapObject(CreateIterResult(step_result, false)), CurrentMicrotaskQueue());
        gen->AsyncQueue().erase(gen->AsyncQueue().begin());
        gen->SetRunningAsync(false);
        RunAsyncGeneratorStep(gen);
    }
}

static void SyncFrameBeforeEval(ExecutionFrame& frame, JSObject* glob, uint32_t& next_shape_id) {
    if (!glob) return;
    for (const auto& [name, reg] : frame.GetBytecode().GetRegisterScope()) {
        if (reg < frame.GetRegisters().size()) {
            TaggedValue val = frame.GetRegister(reg);
            if (!val.IsEmpty() && !val.IsUndefined()) {
                glob->SetNamedProperty(name, val, next_shape_id);
            } else if (val.IsUndefined() && !glob->HasOwnProperty(name)) {
                glob->SetNamedProperty(name, val, next_shape_id);
            }
        }
    }
    if (frame.GetEnvironment()) {
        for (const auto& [name, slot] : frame.GetBytecode().GetContextSlots()) {
            TaggedValue val = frame.GetEnvironment()->Get(0, slot);
            if (!val.IsEmpty() && !val.IsUndefined()) {
                glob->SetNamedProperty(name, val, next_shape_id);
            } else if (val.IsUndefined() && !glob->HasOwnProperty(name)) {
                glob->SetNamedProperty(name, val, next_shape_id);
            }
        }
    }
}

static void SyncFrameAfterEval(ExecutionFrame& frame, JSObject* glob) {
    if (!glob) return;
    for (const auto& [name, reg] : frame.GetBytecode().GetRegisterScope()) {
        if (reg < frame.GetRegisters().size() && (glob->HasNamedProperty(name) || HasProperty(TaggedValue::FromHeapObject(glob), name))) {
            frame.SetRegister(reg, glob->GetNamedProperty(name));
        }
    }
    if (frame.GetEnvironment()) {
        for (const auto& [name, slot] : frame.GetBytecode().GetContextSlots()) {
            if (glob->HasNamedProperty(name) || HasProperty(TaggedValue::FromHeapObject(glob), name)) {
                frame.GetEnvironment()->Set(0, slot, glob->GetNamedProperty(name));
            }
        }
    }
}

TaggedValue Interpreter::RunExecutionLoop(ExecutionFrame& frame, const uint8_t* start_pc, bool* out_yielded, bool* out_is_await) {
    const BytecodeArray& bytecode = frame.GetBytecode();
    auto& heap = ExecutionHeap();
    HandleScope frame_roots(heap);
    SnapshotState(frame_roots, &frame);
    for (auto value : bytecode.Constants()) frame_roots.CreateHandle(value);
    HandleScope op_roots(heap);
    InstructionRootsGuard roots_guard(instruction_roots_, op_roots);

    struct EnvGuard {
        Interpreter& interp;
        ExecutionFrame& frame;
        ~EnvGuard() {
            interp.last_environment_root_ = frame.GetEnvironment()
                ? TaggedValue::FromHeapObject(frame.GetEnvironment()) : TaggedValue::Undefined();
        }
    } env_guard{*this, frame};

    struct WithGuard {
        Interpreter& interp;
        size_t initial_depth;
        ~WithGuard() {
            if (initial_depth < interp.with_stack_.size()) {
                interp.with_stack_.resize(initial_depth);
            }
        }
    } with_guard{*this, with_stack_.size()};

    const uint8_t* code_start = bytecode.Code().data();
    const uint8_t* pc = start_pc;
    const uint8_t* code_end = code_start + bytecode.Code().size();

    if (Realm::Current() && Realm::Current()->GetGlobalObject() &&
        frame.GetThis().IsHeapObject() && frame.GetThis().AsHeapObject() == Realm::Current()->GetGlobalObject()) {
        JSObject* glob = Realm::Current()->GetGlobalObject();
        for (const auto& [name, reg] : bytecode.GetRegisterScope()) {
            if (!glob->HasOwnProperty(name)) {
                glob->SetNamedProperty(name, TaggedValue::Undefined(), next_shape_id_);
                glob->SetOwnPropertyAttributes(name, PropertyAttributes::Writable | PropertyAttributes::Enumerable);
            }
        }
    }

    auto do_call = [&](TaggedValue callee_val, TaggedValue rec_this, const std::vector<TaggedValue>& call_args, bool is_direct_eval) -> bool {
        HandleScope call_roots(heap);
        call_roots.CreateHandle(callee_val);
        call_roots.CreateHandle(rec_this);
        for (auto value : call_args) call_roots.CreateHandle(value);
        if (!callee_val.IsHeapObject() || (!callee_val.AsHeapObject()->IsFunction() && !callee_val.AsHeapObject()->IsProxy())) {
            SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "callee is not a function")));
            return false;
        }
        if (callee_val.AsHeapObject()->IsProxy()) {
            acc_ = ProxyApply(static_cast<JSProxy*>(callee_val.AsHeapObject()), rec_this, call_args);
        } else if (callee_val.AsHeapObject()->IsNativeFunction()) {
            auto* native_fn = static_cast<JSNativeFunction*>(callee_val.AsHeapObject());
            TaggedValue this_arg = is_direct_eval ? frame.GetThis() : rec_this;
            if (native_fn->Name() == "eval") {
                JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                if (is_direct_eval) {
                    SyncFrameBeforeEval(frame, glob, next_shape_id_);
                    acc_ = native_fn->Invoke(this_arg, call_args);
                    SyncFrameAfterEval(frame, glob);
                } else {
                    if (top_level_frame_) {
                        SyncFrameBeforeEval(*top_level_frame_, glob, next_shape_id_);
                    }
                    acc_ = native_fn->Invoke(this_arg, call_args);
                    if (top_level_frame_) {
                        SyncFrameAfterEval(*top_level_frame_, glob);
                    }
                }
            } else {
                acc_ = native_fn->Invoke(this_arg, call_args);
            }
        } else {
            auto* func = static_cast<JSFunction*>(callee_val.AsHeapObject());
            if (!func->GetEnvironment() && frame.GetEnvironment()) {
                func->SetEnvironment(frame.GetEnvironment());
            }
            TaggedValue effective_this = func->IsArrow() ? func->GetLexicalThis() : rec_this;
            if (func->IsAsyncGenerator()) {
                auto* gen_obj = AllocateRuntime<JSGeneratorObject>(func, call_args, effective_this, func->GetEnvironment(), GetRootShape());
                if (Realm::Current() && Realm::Current()->GetAsyncGeneratorPrototype()) {
                    gen_obj->SetPrototype(Realm::Current()->GetAsyncGeneratorPrototype());
                }
                acc_ = TaggedValue::FromHeapObject(gen_obj);
            } else if (func->IsGenerator()) {
                auto* gen_obj = AllocateRuntime<JSGeneratorObject>(func, call_args, effective_this, func->GetEnvironment(), GetRootShape());
                if (Realm::Current() && Realm::Current()->GetGeneratorPrototype()) {
                    gen_obj->SetPrototype(Realm::Current()->GetGeneratorPrototype());
                }
                acc_ = TaggedValue::FromHeapObject(gen_obj);
            } else if (func->IsAsync()) {
                auto* gen_obj = AllocateRuntime<JSGeneratorObject>(func, call_args, effective_this, func->GetEnvironment(), GetRootShape());
                auto* promise = AllocateRuntime<JSPromise>(GetRootShape());
                if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
                    promise->SetPrototype(Realm::Current()->GetPromisePrototype());
                }
                gen_obj->SetAsyncPromise(promise);
                RunAsyncStep(gen_obj, TaggedValue::Undefined(), false);
                acc_ = TaggedValue::FromHeapObject(promise);
            } else {
                size_t prev_with_size = with_stack_.size();
                if (func->HasWithStack()) {
                    for (auto* obj : func->GetWithStack()) {
                        with_stack_.push_back(obj);
                    }
                }
                TaggedValue prev_target = current_new_target_;
                current_new_target_ = func->IsArrow() ? func->GetLexicalNewTarget() : TaggedValue::Undefined();
                acc_ = Execute(*func->GetBytecode(), call_args, effective_this, func->GetEnvironment());
                current_new_target_ = prev_target;
                with_stack_.resize(prev_with_size);
            }
        }
        return true;
    };

    while (pc < code_end) {
        try {
        if (!op_roots.Slots().empty()) op_roots.Clear();
        if (AETHER_UNLIKELY(heap.GCAllowed() && (Options::Get().gc_at_safepoints || heap.HeapPressure() >= kGCPressureThresholdPct))) {
            HandleScope safepoint_roots(heap);
            SnapshotState(safepoint_roots, &frame);
            heap.CollectAtSafepoint();
        }
        Opcode op = static_cast<Opcode>(*pc++);

        switch (op) {
            case Opcode::LdaConstant: {
                uint16_t idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue const_val = bytecode.Constants()[idx];
                if (const_val.IsHeapObject() && const_val.AsHeapObject()->IsFunction()) {
                    auto* fn = static_cast<JSFunction*>(const_val.AsHeapObject());
                    if (!fn->GetEnvironment() && frame.GetEnvironment()) {
                        fn->SetEnvironment(frame.GetEnvironment());
                        if (fn->GetOrCreateConstructorPrototype()) {
                            auto* proto = fn->GetOrCreateConstructorPrototype();
                            for (const auto& name : proto->GetOwnPropertyNames()) {
                                if (proto->HasAccessor(name)) continue;
                                TaggedValue prop_val = proto->GetNamedProperty(name);
                                if (prop_val.IsHeapObject() && prop_val.AsHeapObject()->IsFunction()) {
                                    auto* m_fn = static_cast<JSFunction*>(prop_val.AsHeapObject());
                                    if (!m_fn->GetEnvironment()) {
                                        m_fn->SetEnvironment(frame.GetEnvironment());
                                    }
                                }
                            }
                            for (const auto& [acc_name, acc_desc] : proto->GetAccessors()) {
                                (void)acc_name;
                                if (acc_desc.getter.IsHeapObject() && acc_desc.getter.AsHeapObject()->IsFunction()) {
                                    auto* g_fn = static_cast<JSFunction*>(acc_desc.getter.AsHeapObject());
                                    if (!g_fn->GetEnvironment()) {
                                        g_fn->SetEnvironment(frame.GetEnvironment());
                                    }
                                }
                                if (acc_desc.setter.IsHeapObject() && acc_desc.setter.AsHeapObject()->IsFunction()) {
                                    auto* s_fn = static_cast<JSFunction*>(acc_desc.setter.AsHeapObject());
                                    if (!s_fn->GetEnvironment()) {
                                        s_fn->SetEnvironment(frame.GetEnvironment());
                                    }
                                }
                            }
                        }
                    }
                }
                acc_ = const_val;
                break;
            }
            case Opcode::LdaZero: {
                acc_ = TaggedValue::FromSmi(0);
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                acc_ = TaggedValue::FromSmi(imm);
                break;
            }
            case Opcode::LdaUndefined: {
                acc_ = TaggedValue::Undefined();
                break;
            }
            case Opcode::LdaNull: {
                acc_ = TaggedValue::Null();
                break;
            }
            case Opcode::LdaTrue: {
                acc_ = TaggedValue::True();
                break;
            }
            case Opcode::LdaFalse: {
                acc_ = TaggedValue::False();
                break;
            }
            case Opcode::LdaEmpty: {
                acc_ = TaggedValue::Empty();
                break;
            }
            case Opcode::Ldar: {
                uint8_t reg = *pc++;
                acc_ = frame.GetRegister(reg);
                if (acc_.IsEmpty()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", "Cannot access lexical variable before initialization"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                }
                break;
            }
            case Opcode::Star: {
                uint8_t reg = *pc++;
                frame.SetRegister(reg, acc_);
                break;
            }
            case Opcode::Mov: {
                uint8_t src = *pc++;
                uint8_t dst = *pc++;
                frame.SetRegister(dst, frame.GetRegister(src));
                break;
            }
            case Opcode::Add: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                // Fast path: Both are Smi
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t sum = static_cast<int64_t>(acc_.AsSmi()) + rhs.AsSmi();
                    if (AETHER_LIKELY(sum <= INT32_MAX && sum >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(sum));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(sum));
                    }
                } else {
                    TaggedValue lval = acc_;
                    TaggedValue rval = rhs;
                    if (lval.IsHeapObject() && (lval.AsHeapObject()->IsJSObject() || lval.AsHeapObject()->IsProxy())) {
                        lval = ToPrimitive(lval, "default");
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                    }
                    if (rval.IsHeapObject() && (rval.AsHeapObject()->IsJSObject() || rval.AsHeapObject()->IsProxy())) {
                        rval = ToPrimitive(rval, "default");
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                    }
                    if ((lval.IsHeapObject() && lval.AsHeapObject()->IsString()) ||
                        (rval.IsHeapObject() && rval.AsHeapObject()->IsString())) {
                        // String concatenation
                        TaggedValue s1_val = ToStringOp(lval);
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        TaggedValue s2_val = ToStringOp(rval);
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        std::string s1 = static_cast<JSString*>(s1_val.AsHeapObject())->Value();
                        std::string s2 = static_cast<JSString*>(s2_val.AsHeapObject())->Value();
                        // Enterprise: prevent string bomb attacks
                        if (AETHER_UNLIKELY(s1.size() + s2.size() > kMaxStringLength)) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("RangeError", "String length exceeds maximum allowed length")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s1 + s2));
                    } else {
                        if (lval.IsSymbol() || rval.IsSymbol()) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a number")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        if (lval.IsBigInt() && rval.IsBigInt()) {
                            auto* b1 = static_cast<JSBigInt*>(lval.AsHeapObject());
                            auto* b2 = static_cast<JSBigInt*>(rval.AsHeapObject());
                            acc_ = TaggedValue::FromHeapObject(b1->Add(*b2));
                        } else if (lval.IsBigInt() || rval.IsBigInt()) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        } else {
                            acc_ = TaggedValue::FromDouble(lval.ToDouble() + rval.ToDouble());
                        }
                    }
                }
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t diff = static_cast<int64_t>(acc_.AsSmi()) - rhs.AsSmi();
                    if (AETHER_LIKELY(diff <= INT32_MAX && diff >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(diff));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(diff));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    TaggedValue rnum = ToNumeric(rhs);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt() != rnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt()) {
                        auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                        auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                        acc_ = TaggedValue::FromHeapObject(b1->Subtract(*b2));
                    } else {
                        acc_ = TaggedValue::FromDouble(lnum.ToDouble() - rnum.ToDouble());
                    }
                }
                break;
            }
            case Opcode::Mul: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t prod = static_cast<int64_t>(acc_.AsSmi()) * rhs.AsSmi();
                    if (AETHER_LIKELY(prod <= INT32_MAX && prod >= INT32_MIN && !(prod == 0 && (acc_.AsSmi() < 0 || rhs.AsSmi() < 0)))) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(prod));
                    } else if (prod == 0 && (acc_.AsSmi() < 0 || rhs.AsSmi() < 0)) {
                        acc_ = TaggedValue::FromDouble(-0.0);
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(prod));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    TaggedValue rnum = ToNumeric(rhs);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt() != rnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt()) {
                        auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                        auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                        acc_ = TaggedValue::FromHeapObject(b1->Multiply(*b2));
                    } else {
                        acc_ = TaggedValue::FromDouble(lnum.ToDouble() * rnum.ToDouble());
                    }
                }
                break;
            }
            case Opcode::Div: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi() && rhs.AsSmi() != 0)) {
                    const int64_t n = acc_.AsSmi();
                    const int64_t d = rhs.AsSmi();
                    const int64_t quotient = n / d;
                    if (n % d == 0 && quotient >= INT32_MIN && quotient <= INT32_MAX && !(n == 0 && d < 0)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(quotient));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(n) / static_cast<double>(d));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    TaggedValue rnum = ToNumeric(rhs);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt() != rnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt()) {
                        auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                        auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                        JSBigInt* res = b1->Divide(*b2);
                        if (!res) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("RangeError", "Division by zero")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        acc_ = TaggedValue::FromHeapObject(res);
                    } else {
                        acc_ = TaggedValue::FromDouble(lnum.ToDouble() / rnum.ToDouble());
                    }
                }
                break;
            }
            case Opcode::Mod: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi() && rhs.AsSmi() != 0)) {
                    const int32_t n = acc_.AsSmi();
                    const int32_t d = rhs.AsSmi();
                    // ECMAScript remainder preserves the dividend sign. The
                    // INT32_MIN/-1 pair is -0 and must not execute the
                    // overflowing C++ signed remainder operation.
                    if (n == INT32_MIN && d == -1) {
                        acc_ = TaggedValue::FromDouble(-0.0);
                    } else {
                        const int32_t rem = n % d;
                        acc_ = (rem == 0 && n < 0)
                            ? TaggedValue::FromDouble(-0.0)
                            : TaggedValue::FromSmi(rem);
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    TaggedValue rnum = ToNumeric(rhs);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt() != rnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    if (lnum.IsBigInt()) {
                        auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                        auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                        JSBigInt* res = b1->Remainder(*b2);
                        if (!res) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("RangeError", "Division by zero")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        acc_ = TaggedValue::FromHeapObject(res);
                    } else {
                        acc_ = TaggedValue::FromDouble(std::fmod(lnum.ToDouble(), rnum.ToDouble()));
                    }
                }
                break;
            }
            case Opcode::Exp: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }

                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }

                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    JSBigInt* res = b1->Exponentiate(*b2);
                    if (!res) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("RangeError", "Cannot exponentiate BigInt with negative exponent")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromHeapObject(res);
                } else {
                    double base = lnum.ToDouble();
                    double exp = rnum.ToDouble();
                    if (std::isnan(exp)) {
                        acc_ = TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
                    } else if (exp == 0.0) {
                        acc_ = TaggedValue::FromDouble(1.0);
                    } else if (std::isnan(base)) {
                        acc_ = TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
                    } else if (std::abs(base) == 1.0 && std::isinf(exp)) {
                        acc_ = TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
                    } else {
                        acc_ = TaggedValue::FromDouble(std::pow(base, exp));
                    }
                }
                break;
            }
            case Opcode::BitwiseAnd: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    acc_ = TaggedValue::FromHeapObject(b1->BitwiseAnd(*b2));
                } else {
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) & ToInt32(rnum));
                }
                break;
            }
            case Opcode::BitwiseOr: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    acc_ = TaggedValue::FromHeapObject(b1->BitwiseOr(*b2));
                } else {
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) | ToInt32(rnum));
                }
                break;
            }
            case Opcode::BitwiseXor: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    acc_ = TaggedValue::FromHeapObject(b1->BitwiseXor(*b2));
                } else {
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) ^ ToInt32(rnum));
                }
                break;
            }
            case Opcode::ShiftLeft: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    JSBigInt* res = b1->ShiftLeft(*b2);
                    if (!res) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("RangeError", "Maximum BigInt size exceeded")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromHeapObject(res);
                } else {
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) << (ToUint32(rnum) & 0x1F));
                }
                break;
            }
            case Opcode::ShiftRight: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() != rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt()) {
                    auto* b1 = static_cast<JSBigInt*>(lnum.AsHeapObject());
                    auto* b2 = static_cast<JSBigInt*>(rnum.AsHeapObject());
                    JSBigInt* res = b1->ShiftRight(*b2);
                    if (!res) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("RangeError", "Maximum BigInt size exceeded")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromHeapObject(res);
                } else {
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) >> (ToUint32(rnum) & 0x1F));
                }
                break;
            }
            case Opcode::ShiftRightUnsigned: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue lnum = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                TaggedValue rnum = ToNumeric(rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (lnum.IsBigInt() || rnum.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "BigInts have no unsigned right shift, use >> instead")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                uint32_t a = ToUint32(lnum);
                uint32_t shift = ToUint32(rnum) & 0x1F;
                uint32_t res = a >> shift;
                if (res <= static_cast<uint32_t>(INT32_MAX)) {
                    acc_ = TaggedValue::FromSmi(static_cast<int32_t>(res));
                } else {
                    acc_ = TaggedValue::FromDouble(static_cast<double>(res));
                }
                break;
            }
            case Opcode::AddSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    int64_t sum = static_cast<int64_t>(acc_.AsSmi()) + imm;
                    if (AETHER_LIKELY(sum <= INT32_MAX && sum >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(sum));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(sum));
                    }
                } else {
                    TaggedValue lval = acc_;
                    if (lval.IsHeapObject() && (lval.AsHeapObject()->IsJSObject() || lval.AsHeapObject()->IsProxy())) {
                        lval = ToPrimitive(lval, "default");
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                    }
                    if (lval.IsHeapObject() && lval.AsHeapObject()->IsString()) {
                        TaggedValue s1_val = ToStringOp(lval);
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        std::string s1 = static_cast<JSString*>(s1_val.AsHeapObject())->Value();
                        std::string s2 = std::to_string(imm);
                        if (AETHER_UNLIKELY(s1.size() + s2.size() > kMaxStringLength)) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("RangeError", "String length exceeds maximum allowed length")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s1 + s2));
                    } else {
                        TaggedValue lnum = ToNumeric(lval);
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        if (lnum.IsBigInt()) {
                            SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                        acc_ = TaggedValue::FromDouble(lnum.ToDouble() + static_cast<double>(imm));
                    }
                }
                break;
            }
            case Opcode::SubSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    int64_t diff = static_cast<int64_t>(acc_.AsSmi()) - imm;
                    if (AETHER_LIKELY(diff <= INT32_MAX && diff >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(diff));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(diff));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromDouble(lnum.ToDouble() - static_cast<double>(imm));
                }
                break;
            }
            case Opcode::MulSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    int64_t prod = static_cast<int64_t>(acc_.AsSmi()) * imm;
                    if (prod == 0 && (acc_.AsSmi() < 0 || imm < 0)) {
                        acc_ = TaggedValue::FromDouble(-0.0);
                    } else if (prod <= INT32_MAX && prod >= INT32_MIN) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(prod));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(prod));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromDouble(lnum.ToDouble() * static_cast<double>(imm));
                }
                break;
            }
            case Opcode::DivSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;
                if (AETHER_LIKELY(acc_.IsSmi() && imm != 0)) {
                    int64_t n = acc_.AsSmi();
                    int64_t d = imm;
                    int64_t q = n / d;
                    if (n % d == 0 && q >= INT32_MIN && q <= INT32_MAX && !(n == 0 && d < 0)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(q));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(n) / static_cast<double>(d));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromDouble(lnum.ToDouble() / static_cast<double>(imm));
                }
                break;
            }
            case Opcode::ModSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;
                if (AETHER_LIKELY(acc_.IsSmi() && imm != 0)) {
                    int32_t n = acc_.AsSmi();
                    int32_t d = imm;
                    if (n == INT32_MIN && d == -1) {
                        acc_ = TaggedValue::FromDouble(-0.0);
                    } else {
                        int32_t rem = n % d;
                        acc_ = (rem == 0 && n < 0) ? TaggedValue::FromDouble(-0.0) : TaggedValue::FromSmi(rem);
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromDouble(std::fmod(lnum.ToDouble(), static_cast<double>(imm)));
                }
                break;
            }
            case Opcode::BitwiseAndSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() & imm);
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) & imm);
                }
                break;
            }
            case Opcode::BitwiseOrSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() | imm);
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) | imm);
                }
                break;
            }
            case Opcode::BitwiseXorSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() ^ imm);
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) ^ imm);
                }
                break;
            }
            case Opcode::ShiftLeftSmi: {
                uint8_t imm = *pc++;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() << (imm & 0x1F));
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) << (imm & 0x1F));
                }
                break;
            }
            case Opcode::ShiftRightSmi: {
                uint8_t imm = *pc++;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() >> (imm & 0x1F));
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    acc_ = TaggedValue::FromSmi(ToInt32(lnum) >> (imm & 0x1F));
                }
                break;
            }
            case Opcode::ShiftRightUnsignedSmi: {
                uint8_t imm = *pc++;
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    uint32_t res = static_cast<uint32_t>(acc_.AsSmi()) >> (imm & 0x1F);
                    if (res <= static_cast<uint32_t>(INT32_MAX)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(res));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(res));
                    }
                } else {
                    TaggedValue lnum = ToNumeric(acc_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (lnum.IsBigInt()) {
                        SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot mix BigInt and other types, use explicit conversions")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    uint32_t res = ToUint32(lnum) >> (imm & 0x1F);
                    if (res <= static_cast<uint32_t>(INT32_MAX)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(res));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(res));
                    }
                }
                break;
            }
            case Opcode::TestEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                bool eq = IsLooselyEqual(acc_, rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(eq);
                break;
            }
            case Opcode::TestStrictEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                acc_ = TaggedValue::FromBoolean(IsStrictlyEqual(acc_, rhs));
                break;
            }
            case Opcode::TestNotEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                bool eq = IsLooselyEqual(acc_, rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(!eq);
                break;
            }
            case Opcode::TestStrictNotEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                acc_ = TaggedValue::FromBoolean(!IsStrictlyEqual(acc_, rhs));
                break;
            }
            case Opcode::TestLessThan: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                RelationalResult res = AbstractRelationalComparison(acc_, rhs, true);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(res == RelationalResult::True);
                break;
            }
            case Opcode::TestGreaterThan: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                RelationalResult res = AbstractRelationalComparison(rhs, acc_, false);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(res == RelationalResult::True);
                break;
            }
            case Opcode::TestLessThanOrEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                RelationalResult res = AbstractRelationalComparison(rhs, acc_, false);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(res == RelationalResult::False);
                break;
            }
            case Opcode::TestGreaterThanOrEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                RelationalResult res = AbstractRelationalComparison(acc_, rhs, true);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(res == RelationalResult::False);
                break;
            }
            case Opcode::TestInstanceOf: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                TaggedValue res = InstanceofOperator(acc_, rhs);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = res;
                break;
            }
            case Opcode::TestIn: {
                uint8_t rhs_reg = *pc++;
                TaggedValue rhs_val = frame.GetRegister(rhs_reg);
                TaggedValue prop_val = acc_;

                if (!rhs_val.IsHeapObject() || !rhs_val.AsHeapObject()->IsJSObject()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot use 'in' operator to search for '" + prop_val.ToString() + "' in non-object"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                std::string prop_name = prop_val.ToString();
                bool res = HasProperty(rhs_val, prop_name);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(res);
                break;
            }
            case Opcode::TestInWith: {
                uint8_t rhs_reg = *pc++;
                TaggedValue rhs_val = frame.GetRegister(rhs_reg);
                TaggedValue prop_val = acc_;

                if (!rhs_val.IsHeapObject() || !rhs_val.AsHeapObject()->IsJSObject()) {
                    acc_ = TaggedValue::FromBoolean(false);
                    break;
                }

                std::string prop_name = prop_val.ToString();
                bool res = HasProperty(rhs_val, prop_name);
                if (res) {
                    TaggedValue unscopables = GetProperty(rhs_val, "@@unscopables");
                    if (unscopables.IsHeapObject() && unscopables.AsHeapObject()->IsJSObject()) {
                        TaggedValue blocked = GetProperty(unscopables, prop_name);
                        if (blocked.ToBoolean()) {
                            res = false;
                        }
                    }
                }
                acc_ = TaggedValue::FromBoolean(res);
                break;
            }
            case Opcode::TestPrivateIn: {
                uint16_t name_idx = pc[0] | (pc[1] << 8); pc += 2;
                if (!acc_.IsHeapObject() || !acc_.AsHeapObject()->IsJSObject()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot use 'in' operator to search for private field in non-object"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                std::string raw_name = static_cast<JSString*>(bytecode.Constants()[name_idx].AsHeapObject())->Value();
                JSObject* js_obj = static_cast<JSObject*>(acc_.AsHeapObject());
                std::string priv_key = "__private__" + raw_name;
                std::string get_key = "__get__#" + raw_name;
                std::string set_key = "__set__#" + raw_name;
                std::string hash_key = "#" + raw_name;
                std::string static_hash = "__static__#" + raw_name;
                std::string static_get = "__static____get__#" + raw_name;
                std::string static_set = "__static____set__#" + raw_name;

                bool found = js_obj->HasNamedProperty(priv_key) ||
                             js_obj->HasNamedProperty(get_key) ||
                             js_obj->HasNamedProperty(set_key) ||
                             js_obj->HasNamedProperty(hash_key) ||
                             js_obj->HasNamedProperty(static_hash) ||
                             js_obj->HasNamedProperty(static_get) ||
                             js_obj->HasNamedProperty(static_set) ||
                             HasProperty(acc_, priv_key) ||
                             HasProperty(acc_, get_key) ||
                             HasProperty(acc_, set_key) ||
                             HasProperty(acc_, hash_key);
                acc_ = TaggedValue::FromBoolean(found);

                break;
            }
            case Opcode::TestNull: {
                acc_ = TaggedValue::FromBoolean(acc_.IsNull());
                break;
            }
            case Opcode::TestUndefined: {
                acc_ = TaggedValue::FromBoolean(acc_.IsUndefined());
                break;
            }
            case Opcode::TestTypeOf: {
                uint16_t type_const = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue expected_val = bytecode.Constants()[type_const];
                std::string expected = (expected_val.IsHeapObject() && expected_val.AsHeapObject()->IsString()) ?
                                       static_cast<JSString*>(expected_val.AsHeapObject())->Value() : "";
                std::string actual = "";
                if (acc_.IsSmi() || acc_.IsDouble()) {
                    actual = "number";
                } else if (acc_.IsBoolean()) {
                    actual = "boolean";
                } else if (acc_.IsUndefined()) {
                    actual = "undefined";
                } else if (acc_.IsNull()) {
                    actual = "object";
                } else if (acc_.IsSymbol()) {
                    actual = "symbol";
                } else if (acc_.IsHeapObject()) {
                    auto* h = acc_.AsHeapObject();
                    if (h->IsString()) {
                        actual = "string";
                    } else if (h->IsBigInt()) {
                        actual = "bigint";
                    } else if (h->IsFunction() || h->IsNativeFunction() || (h->IsProxy() && static_cast<JSProxy*>(h)->IsCallable())) {
                        actual = "function";
                    } else {
                        actual = "object";
                    }
                } else {
                    actual = "undefined";
                }
                acc_ = TaggedValue::FromBoolean(actual == expected);
                break;
            }
            case Opcode::Jump: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                pc += offset;
                break;
            }
            case Opcode::JumpIfTrue: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.ToBoolean()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfFalse: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.ToBoolean()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNotNullish: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.IsNull() && !acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNullish: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.IsNull() || acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfUndefined: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNotUndefined: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNull: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.IsNull()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNotNull: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.IsNull()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::LdaNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                if (!obj_val.IsHeapObject()) {
                    if (obj_val.IsNumber() && Realm::Current() && Realm::Current()->GetNumberPrototype()) {
                        acc_ = Realm::Current()->GetNumberPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    if (obj_val.IsBoolean() && Realm::Current() && Realm::Current()->GetBooleanPrototype()) {
                        acc_ = Realm::Current()->GetBooleanPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    if (obj_val.IsString() && Realm::Current() && Realm::Current()->GetStringPrototype()) {
                        if (prop_key == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(obj_val.ToString().length()));
                            break;
                        }
                        acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    if (obj_val.IsUndefined() || obj_val.IsNull()) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot read properties of undefined or null"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (obj_val.AsHeapObject()->IsSymbol()) {
                    if (Realm::Current() && Realm::Current()->GetSymbolPrototype()) {
                        acc_ = Realm::Current()->GetSymbolPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (obj_val.AsHeapObject()->IsBigInt()) {
                    if (Realm::Current() && Realm::Current()->GetBigIntPrototype()) {
                        acc_ = Realm::Current()->GetBigIntPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (obj_val.AsHeapObject()->IsProxy()) {
                    acc_ = ProxyGet(static_cast<JSProxy*>(obj_val.AsHeapObject()), prop_key, obj_val);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                if (prop_key == "new.target") {
                    acc_ = current_new_target_;
                    break;
                }

                if (obj_val.AsHeapObject()->IsFunction()) {
                    if (prop_key == "name") {
                        if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<JSFunction*>(obj_val.AsHeapObject())->Name()));
                            break;
                        } else if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSNativeFunction) {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<JSNativeFunction*>(obj_val.AsHeapObject())->Name()));
                            break;
                        }
                    }
                    if (prop_key == "length") {
                        if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                            auto* fn = static_cast<JSFunction*>(obj_val.AsHeapObject());
                            uint32_t len = fn->GetBytecode() ? fn->GetBytecode()->Length() : 0;
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(len));
                            break;
                        }
                    }

                    if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                        auto* fn = static_cast<JSFunction*>(obj_val.AsHeapObject());
                        if (prop_key == "prototype") {
                            if (!fn->IsArrow() && !fn->IsAsync()) {
                                if (fn->GetConstructorPrototype().IsEmpty()) {
                                    acc_ = TaggedValue::FromHeapObject(fn->GetOrCreateConstructorPrototype());
                                } else {
                                    acc_ = fn->GetConstructorPrototype();
                                }
                                break;
                            }
                        }
                    }
                }

                if (obj_val.AsHeapObject()->IsString()) {
                    auto* str_obj = static_cast<JSString*>(obj_val.AsHeapObject());
                    if (prop_key == "length") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(str_obj->Length()));
                        break;
                    }
                    if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                        acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (obj_val.AsHeapObject()->IsTypedArray()) {
                    auto* t_arr = static_cast<JSTypedArray*>(obj_val.AsHeapObject());
                    if (prop_key == "length") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->Length()));
                        break;
                    }
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteLength()));
                        break;
                    }
                    if (prop_key == "byteOffset") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteOffset()));
                        break;
                    }
                    if (prop_key == "buffer") {
                        acc_ = t_arr->Buffer() ? TaggedValue::FromHeapObject(t_arr->Buffer()) : TaggedValue::Undefined();
                        break;
                    }
                    if (prop_key == "BYTES_PER_ELEMENT") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->BytesPerElement()));
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsArrayBuffer()) {
                    auto* ab = static_cast<JSArrayBuffer*>(obj_val.AsHeapObject());
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(ab->ByteLength()));
                        break;
                    }
                    if (prop_key == "maxByteLength") {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(ab->MaxByteLength()));
                        break;
                    }
                    if (prop_key == "growable") {
                        acc_ = TaggedValue::FromBoolean(ab->IsGrowable());
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsDataView()) {
                    auto* dv = static_cast<JSDataView*>(obj_val.AsHeapObject());
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(dv->ByteLength()));
                        break;
                    }
                    if (prop_key == "byteOffset") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(dv->ByteOffset()));
                        break;
                    }
                    if (prop_key == "buffer") {
                        acc_ = dv->Buffer() ? TaggedValue::FromHeapObject(dv->Buffer()) : TaggedValue::Undefined();
                        break;
                    }
                }

                assert(obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();
                if (obj_shape == nullptr && Realm::Current()) {
                    obj_shape = Realm::Current()->GetRootShape();
                    js_obj->SetShape(obj_shape);
                }

                if (obj_shape && AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape)) {
                    // Monomorphic IC Hit: Direct O(1) slot read
                    acc_ = js_obj->GetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object);
                } else {
                    // IC Miss: Slow path lookup and IC transition
                    acc_ = js_obj->GetNamedProperty(prop_key);

                    // Update IC to Monomorphic
                    if (obj_shape) {
                        if (const PropertyDescriptor* desc = obj_shape->LookupProperty(prop_key)) {
                            ic_slot.state = FeedbackSlot::State::Monomorphic;
                            ic_slot.cached_shape = obj_shape;
                            ic_slot.cached_slot = desc->slot_index;
                            ic_slot.cached_in_object = desc->is_in_object;
                        }
                    }
                }
                break;
            }
            case Opcode::LdaPrivate: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8); pc += 2;
                TaggedValue obj = frame.GetRegister(obj_reg);
                std::string raw_name = static_cast<JSString*>(bytecode.Constants()[name_idx].AsHeapObject())->Value();
                std::string key = "__private__" + raw_name;
                std::string getter_key = "__get__#" + raw_name;
                if (!obj.IsHeapObject() || !obj.AsHeapObject()->IsJSObject()) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Private member access on non-object")));
                    return pending_exception_;
                }
                if (HasProperty(obj, getter_key)) {
                    TaggedValue getter = GetProperty(obj, getter_key);
                    frame_roots.CreateHandle(getter);
                    if (getter.IsHeapObject() && getter.AsHeapObject()->IsFunction()) {
                        auto* fn = static_cast<JSFunction*>(getter.AsHeapObject());
                        acc_ = Execute(*fn->GetBytecode(), {}, obj, fn->GetEnvironment());
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                        }
                        break;
                    }
                }
                acc_ = static_cast<JSObject*>(obj.AsHeapObject())->GetNamedProperty(key);
                break;
            }
            case Opcode::StaPrivate: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8); pc += 2;
                TaggedValue obj = frame.GetRegister(obj_reg);
                std::string raw_name = static_cast<JSString*>(bytecode.Constants()[name_idx].AsHeapObject())->Value();
                std::string key = "__private__" + raw_name;
                std::string setter_key = "__set__#" + raw_name;
                if (!obj.IsHeapObject() || !obj.AsHeapObject()->IsJSObject()) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Private member access on non-object")));
                    return pending_exception_;
                }
                JSObject* js_obj = static_cast<JSObject*>(obj.AsHeapObject());
                if (HasProperty(obj, setter_key)) {
                    TaggedValue setter = GetProperty(obj, setter_key);
                    frame_roots.CreateHandle(setter);
                    if (setter.IsHeapObject() && setter.AsHeapObject()->IsFunction()) {
                        auto* fn = static_cast<JSFunction*>(setter.AsHeapObject());
                        Execute(*fn->GetBytecode(), {acc_}, obj, fn->GetEnvironment());
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                        }
                        break;
                    }
                }
                if (!js_obj->HasNamedProperty(key) && !HasProperty(obj, key)) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot write private member to an object whose class did not declare it"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                if (js_obj->HasNamedProperty(key)) {
                    PropertyAttributes attrs = js_obj->GetOwnPropertyAttributes(key);
                    if (!(attrs & PropertyAttributes::Writable)) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot assign to private method"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                }
                uint32_t sid = 10000;
                js_obj->SetNamedProperty(key, acc_, sid);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::StaNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                if (!obj_val.IsHeapObject()) {
                    if (obj_val.IsNullOrUndefined()) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot set property '" + std::string(prop_key) + "' of " + (obj_val.IsNull() ? "null" : "undefined")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    if (is_strict_mode_) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot create property '" + std::string(prop_key) + "' on primitive"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                    }
                    break;
                }

                if (obj_val.AsHeapObject()->IsProxy()) {
                    ProxySet(static_cast<JSProxy*>(obj_val.AsHeapObject()), prop_key, acc_, obj_val);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<JSFunction*>(obj_val.AsHeapObject());
                    if (prop_key == "prototype") {
                        fn->SetConstructorPrototype(acc_);
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsString() || obj_val.AsHeapObject()->IsSymbol()) {
                    // String and Symbol primitives are immutable in ECMAScript
                    if (is_strict_mode_) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property of primitive"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                    }
                    break;
                }

                assert(obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();

                if (AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape && !js_obj->IsFrozen() && js_obj->IsExtensible())) {
                    // Monomorphic IC Hit: Direct O(1) slot write
                    js_obj->SetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object, acc_);
                } else {
                    // IC Miss: Set property and update Shape / IC
                    js_obj->SetNamedProperty(prop_key, acc_, next_shape_id_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    Shape* new_shape = js_obj->GetShape();
                    if (new_shape) {
                        if (const PropertyDescriptor* desc = new_shape->LookupProperty(prop_key)) {
                            ic_slot.state = FeedbackSlot::State::Monomorphic;
                            ic_slot.cached_shape = new_shape;
                            ic_slot.cached_slot = desc->slot_index;
                            ic_slot.cached_in_object = desc->is_in_object;
                        }
                    }
                }
                break;
            }
            case Opcode::LogicalNot: {
                acc_ = TaggedValue::FromBoolean(!acc_.ToBoolean());
                break;
            }
            case Opcode::Negate: {
                if (acc_.IsHeapObject()) {
                    auto* ho = acc_.AsHeapObject();
                    if (ho->IsJSObject() || ho->IsProxy()) {
                        acc_ = ToPrimitive(acc_, "number");
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                    }
                }
                if (acc_.IsBigInt()) {
                    auto* b = static_cast<JSBigInt*>(acc_.AsHeapObject());
                    acc_ = TaggedValue::FromHeapObject(b->Negate());
                    break;
                }
                if (acc_.IsSymbol()) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a number")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    int32_t val = acc_.AsSmi();
                    if (val == 0) {
                        acc_ = TaggedValue::FromDouble(-0.0);
                    } else if (AETHER_LIKELY(val != INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(-val);
                    } else {
                        acc_ = TaggedValue::FromDouble(-static_cast<double>(val));
                    }
                } else {
                    acc_ = TaggedValue::FromDouble(-acc_.ToDouble());
                }
                break;
            }
            case Opcode::BitwiseNot: {
                TaggedValue num = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (num.IsBigInt()) {
                    auto* b = static_cast<JSBigInt*>(num.AsHeapObject());
                    acc_ = TaggedValue::FromHeapObject(b->BitwiseNot());
                } else {
                    int32_t val = ToInt32(num);
                    acc_ = TaggedValue::FromSmi(~val);
                }
                break;
            }
            case Opcode::TypeOf: {
                if (acc_.IsSmi() || acc_.IsDouble()) {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("number"));
                } else if (acc_.IsBoolean()) {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("boolean"));
                } else if (acc_.IsUndefined()) {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("undefined"));
                } else if (acc_.IsNull()) {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("object"));
                } else if (acc_.IsSymbol()) {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("symbol"));
                } else if (acc_.IsHeapObject()) {
                    auto* h = acc_.AsHeapObject();
                    if (h->IsString()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("string"));
                    } else if (h->IsBigInt()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("bigint"));
                    } else if (h->IsFunction() || h->IsNativeFunction() || (h->IsProxy() && static_cast<JSProxy*>(h)->IsCallable())) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("function"));
                    } else {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("object"));
                    }
                } else {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("undefined"));
                }
                break;
            }
            case Opcode::TypeOfGlobal: {
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                bool has_prop = false;
                TaggedValue val = TaggedValue::Undefined();
                if (prop_key == "new.target") {
                    has_prop = true;
                    val = current_new_target_;
                } else if (glob) {
                    std::string key_str(prop_key);
                    if (glob->GetAccessor(key_str) != nullptr) {
                        has_prop = true;
                        val = glob->GetNamedProperty(key_str);
                    } else if (glob->GetShape() && glob->GetShape()->LookupProperty(key_str) != nullptr) {
                        has_prop = true;
                        val = glob->GetNamedProperty(key_str);
                    } else if (glob->HasNamedProperty(key_str)) {
                        has_prop = true;
                        val = glob->GetNamedProperty(key_str);
                    } else if (HasProperty(TaggedValue::FromHeapObject(glob), key_str)) {
                        has_prop = true;
                        val = GetProperty(TaggedValue::FromHeapObject(glob), key_str);
                    }
                }
                if (has_prop) {
                    if (val.IsSmi() || val.IsDouble()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("number"));
                    } else if (val.IsBoolean()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("boolean"));
                    } else if (val.IsUndefined()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("undefined"));
                    } else if (val.IsNull()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("object"));
                    } else if (val.IsSymbol()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("symbol"));
                    } else if (val.IsHeapObject()) {
                        auto* h = val.AsHeapObject();
                        if (h->IsString()) {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("string"));
                        } else if (h->IsBigInt()) {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("bigint"));
                        } else if (h->IsFunction() || h->IsNativeFunction() || (h->IsProxy() && static_cast<JSProxy*>(h)->IsCallable())) {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("function"));
                        } else {
                            acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("object"));
                        }
                    } else {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("undefined"));
                    }
                } else {
                    acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("undefined"));
                }
                break;
            }
            case Opcode::LdaGlobal: {
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                if (prop_key == "new.target") {
                    acc_ = current_new_target_;
                    break;
                }

                JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                std::string key_str(prop_key);
                bool found_in_with = false;
                for (auto it = with_stack_.rbegin(); it != with_stack_.rend(); ++it) {
                    JSObject* w = *it;
                    if (w && (w->HasNamedProperty(key_str) || HasProperty(TaggedValue::FromHeapObject(w), key_str))) {
                        TaggedValue unscopables = GetProperty(TaggedValue::FromHeapObject(w), "@@unscopables");
                        if (unscopables.IsHeapObject() && unscopables.AsHeapObject()->IsJSObject()) {
                            TaggedValue blocked = GetProperty(unscopables, key_str);
                            if (blocked.ToBoolean()) {
                                continue;
                            }
                        }
                        last_lookup_base_ = w;
                        last_lookup_name_ = key_str;
                        acc_ = GetProperty(TaggedValue::FromHeapObject(w), key_str);
                        found_in_with = true;
                        break;
                    }
                }
                if (found_in_with) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                const auto& reg_scope = frame.GetBytecode().GetRegisterScope();
                auto reg_it = reg_scope.find(key_str);
                if (reg_it != reg_scope.end()) {
                    TaggedValue reg_val = frame.GetRegister(reg_it->second);
                    if (reg_val.IsEmpty() && frame.GetBytecode().IsLexicalName(key_str)) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", "Cannot access '" + key_str + "' before initialization"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    last_lookup_reg_ = reg_it->second;
                    last_lookup_name_ = key_str;
                    acc_ = reg_val;
                    break;
                }

                const auto& ctx_slots = frame.GetBytecode().GetContextSlots();
                auto cs_it = ctx_slots.find(key_str);
                if (cs_it != ctx_slots.end() && frame.GetEnvironment()) {
                    last_lookup_name_ = key_str;
                    acc_ = frame.GetEnvironment()->Get(0, cs_it->second);
                    break;
                }

                if (glob && (glob->HasNamedProperty(key_str) || HasProperty(TaggedValue::FromHeapObject(glob), key_str))) {
                    last_lookup_base_ = glob;
                    last_lookup_name_ = key_str;
                    acc_ = GetProperty(TaggedValue::FromHeapObject(glob), key_str);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", key_str + " is not defined"));
                if (frame.HasExceptionHandler()) {
                    auto h = frame.PopExceptionHandler();
                    pc = h.target_pc;
                    frame.SetEnvironment(h.env);
                    acc_ = exc;
                } else {
                    has_pending_exception_ = true;
                    pending_exception_ = exc;
                    return exc;
                }
                break;
            }
            case Opcode::StaGlobal: {
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();
                std::string key_str(prop_key);

                if (last_lookup_base_ != nullptr && last_lookup_name_ == key_str) {
                    JSObject* target = last_lookup_base_;
                    ClearLastLookup();
                    if (is_strict_mode_ && !target->HasNamedProperty(key_str) && !HasProperty(TaggedValue::FromHeapObject(target), key_str)) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", key_str + " is not defined"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    target->SetNamedProperty(key_str, acc_, next_shape_id_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                } else if (last_lookup_reg_ != -1 && last_lookup_name_ == key_str) {
                    uint8_t dst_reg = static_cast<uint8_t>(last_lookup_reg_);
                    ClearLastLookup();
                    frame.SetRegister(dst_reg, acc_);
                    break;
                }
                ClearLastLookup();

                bool found_in_with = false;
                for (auto it = with_stack_.rbegin(); it != with_stack_.rend(); ++it) {
                    JSObject* w = *it;
                    if (w && (w->HasNamedProperty(key_str) || HasProperty(TaggedValue::FromHeapObject(w), key_str))) {
                        TaggedValue unscopables = GetProperty(TaggedValue::FromHeapObject(w), "@@unscopables");
                        if (unscopables.IsHeapObject() && unscopables.AsHeapObject()->IsJSObject()) {
                            TaggedValue blocked = GetProperty(unscopables, key_str);
                            if (blocked.ToBoolean()) {
                                continue;
                            }
                        }
                        w->SetNamedProperty(key_str, acc_, next_shape_id_);
                        found_in_with = true;
                        break;
                    } else if (w) {
                        if (is_strict_mode_) {
                            TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", key_str + " is not defined"));
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = exc;
                            } else {
                                has_pending_exception_ = true;
                                pending_exception_ = exc;
                                return exc;
                            }
                            found_in_with = true;
                            break;
                        }
                    }
                }
                if (found_in_with) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                const auto& reg_scope = frame.GetBytecode().GetRegisterScope();
                auto reg_it = reg_scope.find(key_str);
                if (reg_it != reg_scope.end()) {
                    frame.SetRegister(reg_it->second, acc_);
                    JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                    if (glob && frame.GetThis().IsHeapObject() && frame.GetThis().AsHeapObject() == glob) {
                        glob->SetNamedProperty(key_str, acc_, next_shape_id_);
                    }
                    break;
                }

                const auto& ctx_slots = frame.GetBytecode().GetContextSlots();
                auto cs_it = ctx_slots.find(key_str);
                if (cs_it != ctx_slots.end() && frame.GetEnvironment()) {
                    frame.GetEnvironment()->Set(0, cs_it->second, acc_);
                    break;
                }

                JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                if (glob) {
                    if (is_strict_mode_ && !glob->HasNamedProperty(key_str) && !HasProperty(TaggedValue::FromHeapObject(glob), key_str)) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("ReferenceError", key_str + " is not defined"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    glob->SetNamedProperty(key_str, acc_, next_shape_id_);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                }
                break;
            }
            case Opcode::DeleteGlobal: {
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string key_str(static_cast<JSString*>(prop_name_val.AsHeapObject())->View());

                // 1. Check with-stack
                bool found_in_with = false;
                for (auto it = with_stack_.rbegin(); it != with_stack_.rend(); ++it) {
                    JSObject* w = *it;
                    if (w && (w->HasNamedProperty(key_str) || HasProperty(TaggedValue::FromHeapObject(w), key_str))) {
                        acc_ = TaggedValue::FromBoolean(DeleteProperty(TaggedValue::FromHeapObject(w), key_str));
                        found_in_with = true;
                        break;
                    }
                }
                if (found_in_with) break;

                // 2. Local register scope or context slots -> cannot delete, return false
                const auto& reg_scope = frame.GetBytecode().GetRegisterScope();
                if (reg_scope.find(key_str) != reg_scope.end()) {
                    acc_ = TaggedValue::False();
                    break;
                }
                const auto& ctx_slots = frame.GetBytecode().GetContextSlots();
                if (ctx_slots.find(key_str) != ctx_slots.end()) {
                    acc_ = TaggedValue::False();
                    break;
                }

                // 3. Global object
                JSObject* glob = Realm::Current() ? Realm::Current()->GetGlobalObject() : nullptr;
                if (glob) {
                    if (glob->HasNamedProperty(key_str) || HasProperty(TaggedValue::FromHeapObject(glob), key_str)) {
                        acc_ = TaggedValue::FromBoolean(DeleteProperty(TaggedValue::FromHeapObject(glob), key_str));
                        break;
                    }
                }

                // 4. Unresolvable reference: in non-strict mode returns true
                acc_ = TaggedValue::True();
                break;
            }
            case Opcode::ToString: {
                acc_ = ToStringOp(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::ToNumeric: {
                acc_ = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::ToPropertyKey: {
                acc_ = ToPropertyKey(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::CheckObjectCoercible: {
                uint8_t reg = *pc++;
                TaggedValue val = frame.GetRegister(reg);
                if (val.IsNull() || val.IsUndefined()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot read properties of " + std::string(val.IsNull() ? "null" : "undefined")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                }
                break;
            }
            case Opcode::PushWith: {
                uint8_t reg = *pc++;
                TaggedValue val = frame.GetRegister(reg);
                if (val.IsHeapObject() && val.AsHeapObject()->IsJSObject()) {
                    with_stack_.push_back(static_cast<JSObject*>(val.AsHeapObject()));
                }
                break;
            }
            case Opcode::PopWith: {
                if (!with_stack_.empty()) {
                    with_stack_.pop_back();
                }
                break;
            }
            case Opcode::Inc: {
                TaggedValue num = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (num.IsBigInt()) {
                    auto* b = static_cast<JSBigInt*>(num.AsHeapObject());
                    JSBigInt one(1);
                    acc_ = TaggedValue::FromHeapObject(b->Add(one));
                } else if (num.IsSmi()) {
                    int32_t val = num.AsSmi();
                    if (val < INT32_MAX) {
                        acc_ = TaggedValue::FromSmi(val + 1);
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(val) + 1.0);
                    }
                } else {
                    acc_ = TaggedValue::FromDouble(num.AsNumber() + 1.0);
                }
                break;
            }
            case Opcode::Dec: {
                TaggedValue num = ToNumeric(acc_);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (num.IsBigInt()) {
                    auto* b = static_cast<JSBigInt*>(num.AsHeapObject());
                    JSBigInt one(1);
                    acc_ = TaggedValue::FromHeapObject(b->Subtract(one));
                } else if (num.IsSmi()) {
                    int32_t val = num.AsSmi();
                    if (val > INT32_MIN) {
                        acc_ = TaggedValue::FromSmi(val - 1);
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(val) - 1.0);
                    }
                } else {
                    acc_ = TaggedValue::FromDouble(num.AsNumber() - 1.0);
                }
                break;
            }
            case Opcode::CreateObject: {
                auto* obj = AllocateRuntime<JSObject>(GetRootShape());
                if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
                    obj->SetPrototype(Realm::Current()->GetObjectPrototype());
                }
                acc_ = TaggedValue::FromHeapObject(obj);
                break;
            }
            case Opcode::CreateArray: {
                uint8_t start_reg = *pc++;
                uint16_t count = pc[0] | (pc[1] << 8);
                pc += 2;
                auto* arr = AllocateRuntime<JSArray>(GetRootShape(), static_cast<size_t>(count));
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                for (uint16_t i = 0; i < count; ++i) {
                    arr->SetElement(i, frame.GetRegister(static_cast<uint8_t>(start_reg + i)));
                }
                acc_ = TaggedValue::FromHeapObject(arr);
                break;
            }
            case Opcode::CreateEmptyArrayLiteral: {
                auto* arr = AllocateRuntime<JSArray>(GetRootShape(), 0);
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                acc_ = TaggedValue::FromHeapObject(arr);
                break;
            }
            case Opcode::CreateRegExpLiteral: {
                uint16_t pat_idx = pc[0] | (pc[1] << 8); pc += 2;
                uint16_t flags_idx = pc[0] | (pc[1] << 8); pc += 2;
                TaggedValue pat_val = bytecode.Constants()[pat_idx];
                TaggedValue flags_val = bytecode.Constants()[flags_idx];
                std::string pat = (pat_val.IsHeapObject() && pat_val.AsHeapObject()->IsString()) ?
                                  static_cast<JSString*>(pat_val.AsHeapObject())->Value() : "";
                std::string flags = (flags_val.IsHeapObject() && flags_val.AsHeapObject()->IsString()) ?
                                    static_cast<JSString*>(flags_val.AsHeapObject())->Value() : "";
                auto* re = AllocateRuntime<JSRegExp>(pat, flags, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
                if (Realm::Current() && Realm::Current()->GetRegExpPrototype()) {
                    re->SetPrototype(Realm::Current()->GetRegExpPrototype());
                }
                acc_ = TaggedValue::FromHeapObject(re);
                break;
            }
            case Opcode::ForInEnumerate: {
                uint8_t obj_reg = *pc++;
                TaggedValue target = frame.GetRegister(obj_reg);
                auto* arr = AllocateRuntime<JSArray>(GetRootShape(), 0);
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                if (target.IsHeapObject() && target.AsHeapObject()->IsJSObject()) {
                    JSObject* obj = static_cast<JSObject*>(target.AsHeapObject());
                    std::vector<std::string> keys = obj->GetOwnPropertyNames();
                    for (const auto& k : keys) {
                        arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(k)));
                    }
                }
                acc_ = TaggedValue::FromHeapObject(arr);
                break;
            }
            case Opcode::ForInNext: {
                uint8_t obj_reg = *pc++;
                uint8_t enum_reg = *pc++;
                uint8_t idx_reg = *pc++;
                TaggedValue target = frame.GetRegister(obj_reg);
                TaggedValue enum_val = frame.GetRegister(enum_reg);
                TaggedValue idx_val = frame.GetRegister(idx_reg);
                acc_ = TaggedValue::Undefined();
                if (enum_val.IsHeapObject() && enum_val.AsHeapObject()->IsArray() && idx_val.IsSmi()) {
                    auto* arr = static_cast<JSArray*>(enum_val.AsHeapObject());
                    int32_t idx = idx_val.AsSmi();
                    if (idx >= 0 && static_cast<size_t>(idx) < arr->Length()) {
                        TaggedValue key_val = arr->Elements()[idx];
                        if (target.IsHeapObject() && target.AsHeapObject()->IsJSObject()) {
                            std::string key_str = key_val.ToString();
                            if (HasProperty(target, key_str)) {
                                acc_ = key_val;
                            }
                        } else {
                            acc_ = key_val;
                        }
                    }
                }
                break;
            }
            case Opcode::ArrayPush: {
                uint8_t arr_reg = *pc++;
                uint8_t val_reg = *pc++;
                TaggedValue arr_val = frame.GetRegister(arr_reg);
                TaggedValue val = frame.GetRegister(val_reg);
                if (arr_val.IsHeapObject() && arr_val.AsHeapObject()->IsArray()) {
                    static_cast<JSArray*>(arr_val.AsHeapObject())->Push(val);
                }
                break;
            }
            case Opcode::ArraySpread: {
                uint8_t target_arr_reg = *pc++;
                uint8_t src_reg = *pc++;
                TaggedValue target_val = frame.GetRegister(target_arr_reg);
                TaggedValue src_val = frame.GetRegister(src_reg);
                if (target_val.IsHeapObject() && target_val.AsHeapObject()->IsArray()) {
                    auto* target_arr = static_cast<JSArray*>(target_val.AsHeapObject());
                    if (!src_val.IsHeapObject() && !src_val.IsString()) {
                        SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Spread target is not iterable")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    HeapObject* src_ho = src_val.IsHeapObject() ? src_val.AsHeapObject() : nullptr;
                    // Fast path: untampered JSArray without own @@iterator
                    bool has_own_iter = src_ho && ((src_ho->GetShape() && src_ho->GetShape()->LookupProperty("@@iterator")) || static_cast<JSObject*>(src_ho)->HasAccessor("@@iterator"));
                    if (src_ho && src_ho->IsArray() && !has_own_iter) {
                        auto* src_arr = static_cast<JSArray*>(src_ho);
                        for (size_t i = 0; i < src_arr->Length(); ++i) {
                            target_arr->Push(src_arr->GetElement(i));
                        }
                        break;
                    }
                    if (src_ho && src_ho->IsTypedArray()) {
                        auto* src_ta = static_cast<JSTypedArray*>(src_ho);
                        for (size_t i = 0; i < src_ta->Length(); ++i) {
                            target_arr->Push(src_ta->GetElement(i));
                        }
                        break;
                    }
                    if (src_ho && src_ho->IsString()) {
                        auto* src_str = static_cast<JSString*>(src_ho);
                        for (size_t i = 0; i < src_str->Length(); ++i) {
                            target_arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(src_str->View().substr(i, 1))));
                        }
                        break;
                    }

                    // Generic iterator protocol
                    TaggedValue iter_fn = TaggedValue::Undefined();
                    if (src_ho) {
                        if (src_ho->IsProxy()) {
                            iter_fn = ProxyGet(static_cast<JSProxy*>(src_ho), "@@iterator", src_val);
                        } else if (src_ho->IsJSObject()) {
                            iter_fn = static_cast<JSObject*>(src_ho)->GetNamedProperty("@@iterator");
                        }
                    }
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (!iter_fn.IsHeapObject() || !iter_fn.AsHeapObject()->IsFunction()) {
                        SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Spread target is not iterable")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    TaggedValue iterator = CallFunction(iter_fn, src_val, {});
                    frame_roots.CreateHandle(iterator);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    if (!iterator.IsHeapObject() || (!iterator.AsHeapObject()->IsJSObject() && !iterator.AsHeapObject()->IsProxy())) {
                        SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Iterator is not an object")));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }

                    // Iterate calling next()
                    while (true) {
                        HandleScope iteration_roots(heap);
                        InstructionRootsGuard iteration_guard(instruction_roots_, iteration_roots);
                        if (heap.GCAllowed() && (Options::Get().gc_at_safepoints || heap.HeapPressure() >= kGCPressureThresholdPct)) heap.CollectAtSafepoint();
                        TaggedValue next_fn;
                        if (iterator.AsHeapObject()->IsProxy()) {
                            next_fn = ProxyGet(static_cast<JSProxy*>(iterator.AsHeapObject()), "next", iterator);
                        } else {
                            next_fn = static_cast<JSObject*>(iterator.AsHeapObject())->GetNamedProperty("next");
                        }
                        if (has_pending_exception_) break;
                        if (!next_fn.IsHeapObject() || !next_fn.AsHeapObject()->IsFunction()) {
                            SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "iterator.next is not a function")));
                            break;
                        }
                        iteration_roots.CreateHandle(next_fn);
                        TaggedValue next_res = CallFunction(next_fn, iterator, {});
                        iteration_roots.CreateHandle(next_res);
                        if (has_pending_exception_) break;
                        if (!next_res.IsHeapObject() || (!next_res.AsHeapObject()->IsJSObject() && !next_res.AsHeapObject()->IsProxy())) {
                            SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Iterator result is not an object")));
                            break;
                        }
                        TaggedValue done_val;
                        if (next_res.AsHeapObject()->IsProxy()) {
                            done_val = ProxyGet(static_cast<JSProxy*>(next_res.AsHeapObject()), "done", next_res);
                        } else {
                            done_val = static_cast<JSObject*>(next_res.AsHeapObject())->GetNamedProperty("done");
                        }
                        if (has_pending_exception_) break;
                        if (done_val.ToBoolean()) {
                            break;
                        }
                        TaggedValue item_val;
                        if (next_res.AsHeapObject()->IsProxy()) {
                            item_val = ProxyGet(static_cast<JSProxy*>(next_res.AsHeapObject()), "value", next_res);
                        } else {
                            item_val = static_cast<JSObject*>(next_res.AsHeapObject())->GetNamedProperty("value");
                        }
                        if (has_pending_exception_) break;
                        target_arr->Push(item_val);
                    }

                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                }
                break;
            }
            case Opcode::ObjectSpread: {
                uint8_t target_reg = *pc++;
                uint8_t src_reg = *pc++;
                TaggedValue target_val = frame.GetRegister(target_reg);
                TaggedValue src_val = frame.GetRegister(src_reg);
                if (target_val.IsHeapObject() && target_val.AsHeapObject()->IsJSObject() &&
                    src_val.IsHeapObject() && src_val.AsHeapObject()->IsJSObject()) {
                    auto* target_obj = static_cast<JSObject*>(target_val.AsHeapObject());
                    auto* src_obj = static_cast<JSObject*>(src_val.AsHeapObject());
                    TaggedValue keys_val = GetOwnKeys(src_val);
                    frame_roots.CreateHandle(keys_val);
                    if (keys_val.IsHeapObject() && keys_val.AsHeapObject()->IsArray()) {
                        auto* k_arr = static_cast<JSArray*>(keys_val.AsHeapObject());
                        for (size_t i = 0; i < k_arr->Length(); ++i) {
                            HandleScope iteration_roots(heap);
                            InstructionRootsGuard iteration_guard(instruction_roots_, iteration_roots);
                            std::string k_str = k_arr->GetElement(i).ToString();
                            bool is_enumerable = false;
                            if (src_obj->IsProxy()) {
                                TaggedValue desc = ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(src_obj), k_str);
                                if (desc.IsHeapObject() && desc.AsHeapObject()->IsJSObject()) {
                                    TaggedValue enum_val = static_cast<JSObject*>(desc.AsHeapObject())->GetNamedProperty("enumerable");
                                    is_enumerable = enum_val.IsUndefined() || enum_val.ToBoolean();
                                } else {
                                    is_enumerable = true;
                                }
                            } else {
                                is_enumerable = (src_obj->GetOwnPropertyAttributes(k_str) & PropertyAttributes::Enumerable) != 0;
                            }
                            if (is_enumerable) {
                                TaggedValue val = GetProperty(src_val, k_str);
                                target_obj->SetNamedProperty(k_str, val, next_shape_id_);
                            }
                        }
                    }
                }
                break;
            }
            case Opcode::CreateRestArray: {
                uint8_t dest_reg = *pc++;
                uint16_t start_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                size_t count = (frame.Args().size() > start_idx) ? (frame.Args().size() - start_idx) : 0;
                auto* arr = AllocateRuntime<JSArray>(GetRootShape(), count);
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                for (size_t i = 0; i < count; ++i) {
                    arr->SetElement(i, frame.Args()[start_idx + i]);
                }
                frame.SetRegister(dest_reg, TaggedValue::FromHeapObject(arr));
                break;
            }
            case Opcode::CreateArguments: {
                uint8_t dest_reg = *pc++;
                Shape* root_shape = GetRootShape();
                auto* args_obj = AllocateRuntime<JSObject>(root_shape);
                if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
                    args_obj->SetPrototype(Realm::Current()->GetObjectPrototype());
                }
                args_obj->SetNamedProperty("length", TaggedValue::FromSmi(static_cast<int32_t>(frame.Args().size())), next_shape_id_);
                for (size_t i = 0; i < frame.Args().size(); ++i) {
                    args_obj->SetNamedProperty(std::to_string(i), frame.Args()[i], next_shape_id_);
                }
                args_obj->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Arguments")), next_shape_id_);
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    TaggedValue iter_fn = Realm::Current()->GetArrayPrototype()->GetNamedProperty("values");
                    if (iter_fn.IsUndefined() || iter_fn.IsNull()) {
                        iter_fn = Realm::Current()->GetArrayPrototype()->GetNamedProperty("@@iterator");
                    }
                    if (!iter_fn.IsUndefined() && !iter_fn.IsNull()) {
                        args_obj->SetNamedProperty("@@iterator", iter_fn, next_shape_id_);
                    }
                }
                frame.SetRegister(dest_reg, TaggedValue::FromHeapObject(args_obj));
                break;
            }
            case Opcode::LdaKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);

                if (!obj_val.IsHeapObject() && obj_val.IsNullOrUndefined()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot read properties of " + (obj_val.IsNull() ? std::string("null") : std::string("undefined"))));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                if (!key_val.IsSmi() && !key_val.IsSymbol() && (!key_val.IsHeapObject() || !key_val.AsHeapObject()->IsString())) {
                    key_val = ToPropertyKey(key_val);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    frame.SetRegister(key_reg, key_val);
                }

                if (!obj_val.IsHeapObject()) {
                    std::string key_str;
                    if (key_val.IsSymbol()) {
                        key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        key_str = std::to_string(key_val.AsSmi());
                    } else {
                        key_str = key_val.ToString();
                    }

                    if (obj_val.IsNumber() && Realm::Current() && Realm::Current()->GetNumberPrototype()) {
                        acc_ = Realm::Current()->GetNumberPrototype()->GetNamedProperty(key_str);
                        break;
                    }
                    if (obj_val.IsBoolean() && Realm::Current() && Realm::Current()->GetBooleanPrototype()) {
                        acc_ = Realm::Current()->GetBooleanPrototype()->GetNamedProperty(key_str);
                        break;
                    }
                    if (obj_val.IsString() && Realm::Current() && Realm::Current()->GetStringPrototype()) {
                        if (key_str == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(obj_val.ToString().length()));
                            break;
                        }
                        acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(key_str);
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                HeapObject* h_obj = obj_val.AsHeapObject();
                if (h_obj->IsProxy()) {
                    std::string key_str;
                    if (key_val.IsSymbol()) {
                        key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        key_str = std::to_string(key_val.AsSmi());
                    } else {
                        key_str = key_val.ToString();
                    }
                    acc_ = ProxyGet(static_cast<JSProxy*>(h_obj), key_str, obj_val);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                if (key_val.IsSymbol()) {
                    auto* sym = static_cast<JSSymbol*>(key_val.AsHeapObject());
                    HeapObject* ho = obj_val.AsHeapObject();
                    if (ho->IsJSObject()) {
                        acc_ = static_cast<JSObject*>(ho)->GetNamedProperty(sym->Key());
                    } else if (ho->IsString()) {
                        if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                            acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(sym->Key());
                        } else {
                            acc_ = TaggedValue::Undefined();
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                    break;
                }

                if (h_obj->IsArray()) {
                    auto* arr = static_cast<JSArray*>(h_obj);
                    if (key_val.IsSmi()) {
                        int32_t idx = key_val.AsSmi();
                        if (idx >= 0 && static_cast<size_t>(idx) < arr->Length() && !arr->Elements()[idx].IsEmpty()) {
                            acc_ = arr->Elements()[idx];
                        } else if (idx >= 0) {
                            acc_ = arr->GetNamedProperty(std::to_string(idx));
                        } else {
                            acc_ = arr->GetNamedProperty(std::to_string(idx));
                        }
                    } else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        if (d >= 0.0 && std::floor(d) == d) {
                            size_t idx = static_cast<size_t>(d);
                            if (idx < arr->Length() && !arr->Elements()[idx].IsEmpty()) {
                                acc_ = arr->Elements()[idx];
                            } else {
                                acc_ = arr->GetNamedProperty(std::to_string(idx));
                            }
                        } else {
                            acc_ = arr->GetNamedProperty(key_val.ToString());
                        }
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        acc_ = arr->GetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else {
                        acc_ = arr->GetNamedProperty(key_val.ToString());
                    }
                } else if (h_obj->IsString()) {
                    auto* str_obj = static_cast<JSString*>(h_obj);
                    int32_t idx = -1;
                    if (key_val.IsSmi()) idx = key_val.AsSmi();
                    else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        if (d >= 0.0 && std::floor(d) == d) idx = static_cast<int32_t>(d);
                    }
                    if (idx >= 0 && static_cast<size_t>(idx) < str_obj->Length()) {
                        acc_ = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str_obj->View().substr(idx, 1)));
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        std::string_view k = static_cast<JSString*>(key_val.AsHeapObject())->View();
                        if (k == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(str_obj->Length()));
                        } else if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                            acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(k);
                        } else {
                            acc_ = TaggedValue::Undefined();
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                } else if (h_obj->IsTypedArray()) {
                    auto* t_arr = static_cast<JSTypedArray*>(h_obj);
                    if (key_val.IsSmi()) {
                        int32_t idx = key_val.AsSmi();
                        acc_ = idx >= 0 ? t_arr->GetElement(static_cast<size_t>(idx)) : TaggedValue::Undefined();
                    } else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        acc_ = (d >= 0.0 && std::floor(d) == d) ? t_arr->GetElement(static_cast<size_t>(d)) : TaggedValue::Undefined();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        std::string_view k = static_cast<JSString*>(key_val.AsHeapObject())->View();
                        if (k == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->Length()));
                        } else if (k == "byteLength") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteLength()));
                        } else if (k == "byteOffset") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteOffset()));
                        } else if (k == "buffer") {
                            acc_ = t_arr->Buffer() ? TaggedValue::FromHeapObject(t_arr->Buffer()) : TaggedValue::Undefined();
                        } else if (k == "BYTES_PER_ELEMENT") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->BytesPerElement()));
                        } else {
                            acc_ = t_arr->GetNamedProperty(k);
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                } else if (h_obj->IsJSObject()) {
                    auto* js_obj = static_cast<JSObject*>(h_obj);
                    if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        acc_ = js_obj->GetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        acc_ = js_obj->GetNamedProperty(std::to_string(key_val.AsSmi()));
                    } else {
                        acc_ = js_obj->GetNamedProperty(key_val.ToString());
                    }
                } else {
                    acc_ = TaggedValue::Undefined();
                }
                break;
            }
            case Opcode::StaKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);

                if (!obj_val.IsHeapObject() && obj_val.IsNullOrUndefined()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot set properties of " + (obj_val.IsNull() ? std::string("null") : std::string("undefined"))));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                if (!key_val.IsSmi() && !key_val.IsSymbol() && (!key_val.IsHeapObject() || !key_val.AsHeapObject()->IsString())) {
                    key_val = ToPropertyKey(key_val);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                    frame.SetRegister(key_reg, key_val);
                }

                if (obj_val.IsHeapObject()) {
                    HeapObject* h_obj = obj_val.AsHeapObject();
                    if (h_obj->IsProxy()) {
                        std::string key_str;
                        if (key_val.IsSymbol()) {
                            key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                        } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                        } else if (key_val.IsSmi()) {
                            key_str = std::to_string(key_val.AsSmi());
                        } else {
                            key_str = key_val.ToString();
                        }
                        ProxySet(static_cast<JSProxy*>(h_obj), key_str, acc_, obj_val);
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                        }
                        break;
                    }

                    if (key_val.IsSymbol()) {
                        auto* sym = static_cast<JSSymbol*>(key_val.AsHeapObject());
                        HeapObject* ho = obj_val.AsHeapObject();
                        if (ho->IsJSObject()) {
                            static_cast<JSObject*>(ho)->SetNamedProperty(sym->Key(), acc_, next_shape_id_);
                        }
                        break;
                    }
                    if (h_obj->IsArray()) {
                        auto* arr = static_cast<JSArray*>(h_obj);
                        if (arr->IsFrozen()) {
                            if (is_strict_mode_) {
                                TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property"));
                                if (frame.HasExceptionHandler()) {
                                    auto h = frame.PopExceptionHandler();
                                    pc = h.target_pc;
                                    frame.SetEnvironment(h.env);
                                    acc_ = exc;
                                } else {
                                    has_pending_exception_ = true;
                                    pending_exception_ = exc;
                                    return exc;
                                }
                            }
                            break;
                        }
                        if (key_val.IsSmi()) {
                            int32_t idx = key_val.AsSmi();
                            if (idx >= 0) arr->SetElement(static_cast<size_t>(idx), acc_);
                        } else if (key_val.IsNumber()) {
                            double d = key_val.AsNumber();
                            if (d >= 0.0 && std::floor(d) == d) {
                                arr->SetElement(static_cast<size_t>(d), acc_);
                            } else {
                                arr->SetNamedProperty(key_val.ToString(), acc_, next_shape_id_);
                            }
                        } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            arr->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        } else {
                            arr->SetNamedProperty(key_val.ToString(), acc_, next_shape_id_);
                        }
                    } else if (h_obj->IsTypedArray()) {
                        auto* t_arr = static_cast<JSTypedArray*>(h_obj);
                        if (key_val.IsSmi()) {
                            int32_t idx = key_val.AsSmi();
                            if (idx >= 0) t_arr->SetElement(static_cast<size_t>(idx), acc_);
                        } else if (key_val.IsNumber()) {
                            double d = key_val.AsNumber();
                            if (d >= 0.0 && std::floor(d) == d) t_arr->SetElement(static_cast<size_t>(d), acc_);
                        } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            t_arr->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        }
                    } else if (h_obj->IsJSObject()) {
                        auto* js_obj = static_cast<JSObject*>(h_obj);
                        if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            js_obj->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        } else if (key_val.IsSmi()) {
                            js_obj->SetNamedProperty(std::to_string(key_val.AsSmi()), acc_, next_shape_id_);
                        } else {
                            js_obj->SetNamedProperty(key_val.ToString(), acc_, next_shape_id_);
                        }
                    }

                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                        break;
                    }
                }
                break;
            }
            case Opcode::DeleteNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                if (obj_val.IsNull() || obj_val.IsUndefined()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot delete property of null or undefined"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                bool success = DeleteProperty(obj_val, prop_key);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (bytecode.IsStrict() && !success) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot delete property in strict mode"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(success);
                break;
            }
            case Opcode::DeleteKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                if (obj_val.IsNull() || obj_val.IsUndefined()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot delete property of null or undefined"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                TaggedValue key_val = frame.GetRegister(key_reg);

                std::string key_str;
                if (key_val.IsSymbol()) {
                    key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                    key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                } else if (key_val.IsSmi()) {
                    key_str = std::to_string(key_val.AsSmi());
                } else {
                    key_str = key_val.ToString();
                }

                bool success = DeleteProperty(obj_val, key_str);
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (bytecode.IsStrict() && !success) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot delete property in strict mode"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                acc_ = TaggedValue::FromBoolean(success);
                break;
            }
            case Opcode::DefineGetter: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue obj_val = frame.GetRegister(obj_reg);
                if (obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject()) {
                    JSObject* obj = static_cast<JSObject*>(obj_val.AsHeapObject());
                    TaggedValue name_val = bytecode.Constants()[name_idx];
                    assert(name_val.IsHeapObject() && name_val.AsHeapObject()->IsString());
                    std::string key_str(static_cast<JSString*>(name_val.AsHeapObject())->View());
                    const auto* existing = obj->GetAccessor(key_str);
                    TaggedValue setter = existing ? existing->setter : TaggedValue::Undefined();
                    obj->SetAccessor(key_str, acc_, setter, PropertyAttributes::Enumerable | PropertyAttributes::Configurable);
                }
                break;
            }
            case Opcode::DefineSetter: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue obj_val = frame.GetRegister(obj_reg);
                if (obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject()) {
                    JSObject* obj = static_cast<JSObject*>(obj_val.AsHeapObject());
                    TaggedValue name_val = bytecode.Constants()[name_idx];
                    assert(name_val.IsHeapObject() && name_val.AsHeapObject()->IsString());
                    std::string key_str(static_cast<JSString*>(name_val.AsHeapObject())->View());
                    const auto* existing = obj->GetAccessor(key_str);
                    TaggedValue getter = existing ? existing->getter : TaggedValue::Undefined();
                    obj->SetAccessor(key_str, getter, acc_, PropertyAttributes::Enumerable | PropertyAttributes::Configurable);
                }
                break;
            }
            case Opcode::DefineGetterKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);
                if (obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject()) {
                    JSObject* obj = static_cast<JSObject*>(obj_val.AsHeapObject());
                    std::string key_str;
                    if (key_val.IsSymbol()) {
                        key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        key_str = std::to_string(key_val.AsSmi());
                    } else {
                        key_str = key_val.ToString();
                    }
                    const auto* existing = obj->GetAccessor(key_str);
                    TaggedValue setter = existing ? existing->setter : TaggedValue::Undefined();
                    obj->SetAccessor(key_str, acc_, setter, PropertyAttributes::Enumerable | PropertyAttributes::Configurable);
                }
                break;
            }
            case Opcode::DefineSetterKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);
                if (obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject()) {
                    JSObject* obj = static_cast<JSObject*>(obj_val.AsHeapObject());
                    std::string key_str;
                    if (key_val.IsSymbol()) {
                        key_str = static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        key_str = std::string(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        key_str = std::to_string(key_val.AsSmi());
                    } else {
                        key_str = key_val.ToString();
                    }
                    const auto* existing = obj->GetAccessor(key_str);
                    TaggedValue getter = existing ? existing->getter : TaggedValue::Undefined();
                    obj->SetAccessor(key_str, getter, acc_, PropertyAttributes::Enumerable | PropertyAttributes::Configurable);
                }
                break;
            }
            case Opcode::LdaThis: {
                acc_ = frame.GetThis();
                break;
            }
            case Opcode::CreateClosure: {
                uint16_t const_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue proto_val = bytecode.Constants()[const_idx];
                assert(proto_val.IsHeapObject() && proto_val.AsHeapObject()->IsFunction());
                auto* proto_fn = static_cast<JSFunction*>(proto_val.AsHeapObject());
                auto* closure = AllocateRuntime<JSFunction>(proto_fn->Name(),
                                               proto_fn->GetSharedBytecode() ? proto_fn->GetSharedBytecode() : std::shared_ptr<const BytecodeArray>(proto_fn->GetBytecode(), [](const BytecodeArray*){}), 
                                               frame.GetEnvironment());
                if (proto_fn->GetPrototype()) {
                    closure->SetPrototype(proto_fn->GetPrototype());
                } else if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
                    closure->SetPrototype(Realm::Current()->GetFunctionPrototype());
                }
                closure->SetGenerator(proto_fn->IsGenerator());
                closure->SetAsync(proto_fn->IsAsync());
                closure->SetArrow(proto_fn->IsArrow());
                if (proto_fn->IsArrow()) {
                    closure->SetLexicalThis(frame.GetThis());
                    closure->SetLexicalNewTarget(current_new_target_);
                }
                if (!with_stack_.empty()) {
                    closure->SetWithStack(with_stack_);
                }
                acc_ = TaggedValue::FromHeapObject(closure);
                break;
            }
            case Opcode::CreateFunctionContext: {
                uint16_t slot_count = pc[0] | (pc[1] << 8);
                pc += 2;
                auto* new_env = AllocateRuntime<LexicalEnvironment>(frame.GetEnvironment(), slot_count);
                frame.SetEnvironment(new_env);
                last_environment_root_ = TaggedValue::FromHeapObject(new_env);
                break;
            }
            case Opcode::LdaContextSlot: {
                uint8_t depth = *pc++;
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                acc_ = cur_env ? cur_env->Get(depth, slot) : TaggedValue::Undefined();
                break;
            }
            case Opcode::StaContextSlot: {
                uint8_t depth = *pc++;
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                if (cur_env) {
                    cur_env->Set(depth, slot, acc_);
                }
                break;
            }
            case Opcode::LdaCurrentContextSlot: {
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                acc_ = cur_env ? cur_env->Get(0, slot) : TaggedValue::Undefined();
                break;
            }
            case Opcode::StaCurrentContextSlot: {
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                if (cur_env) {
                    cur_env->Set(0, slot, acc_);
                }
                break;
            }
            case Opcode::LdaImmutableContextSlot: {
                uint8_t depth = *pc++;
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                acc_ = cur_env ? cur_env->Get(depth, slot) : TaggedValue::Undefined();
                break;
            }
            case Opcode::Construct: {
                uint8_t ctor_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue ctor_val = frame.GetRegister(ctor_reg);
                if (!ctor_val.IsHeapObject() || (!ctor_val.AsHeapObject()->IsFunction() && !ctor_val.AsHeapObject()->IsProxy())) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "value is not a constructor"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (ctor_val.AsHeapObject()->IsProxy()) {
                    acc_ = ProxyConstruct(static_cast<JSProxy*>(ctor_val.AsHeapObject()), call_args);
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                if (ctor_val.AsHeapObject()->IsNativeFunction()) {
                    auto* native_fn = static_cast<JSNativeFunction*>(ctor_val.AsHeapObject());
                    if (!native_fn->IsConstructor()) {
                        TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function is not a constructor"));
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = exc;
                        } else {
                            has_pending_exception_ = true;
                            pending_exception_ = exc;
                            return exc;
                        }
                        break;
                    }
                    TaggedValue prev_target = current_new_target_;
                    current_new_target_ = ctor_val;
                    acc_ = native_fn->Invoke(TaggedValue::Undefined(), call_args);
                    current_new_target_ = prev_target;
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }

                auto* ctor_fn = static_cast<JSFunction*>(ctor_val.AsHeapObject());
                if (ctor_fn->IsArrow() || ctor_fn->IsAsync() || ctor_fn->IsGenerator()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function is not a constructor"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }
                if (!ctor_fn->GetEnvironment() && frame.GetEnvironment()) {
                    ctor_fn->SetEnvironment(frame.GetEnvironment());
                }
                JSObject* proto = ctor_fn->GetOrCreateConstructorPrototype();
                auto* instance = AllocateRuntime<JSObject>(GetRootShape());
                if (proto != nullptr) {
                    instance->SetPrototype(proto);
                } else if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
                    instance->SetPrototype(Realm::Current()->GetObjectPrototype());
                }
                TaggedValue instance_val = TaggedValue::FromHeapObject(instance);
                uint32_t field_sid = 11000;
                for (const auto& f : ctor_fn->InstanceFields()) {
                    std::string property = f.name.size() > 0 && f.name[0] == '#'
                        ? "__private__" + f.name.substr(1) : f.name;
                    instance->SetNamedProperty(property, f.initial_value, field_sid);
                    if (f.is_method) {
                        instance->SetOwnPropertyAttributes(property, PropertyAttributes::None);
                    }
                }

                TaggedValue prev_target = current_new_target_;
                current_new_target_ = ctor_val;
                TaggedValue ret = Execute(*ctor_fn->GetBytecode(), call_args, instance_val, ctor_fn->GetEnvironment());
                current_new_target_ = prev_target;
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                } else {
                    if (ret.IsHeapObject() && ret.AsHeapObject()->IsJSObject()) {
                        acc_ = ret;
                    } else {
                        acc_ = instance_val;
                    }
                }
                break;
            }
            case Opcode::CallMethod: {
                uint8_t callee_reg = *pc++;
                uint8_t this_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                TaggedValue rec_this = frame.GetRegister(this_reg);
                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (!do_call(callee_val, rec_this, call_args, false)) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::CallMethodWithSpread: {
                uint8_t callee_reg = *pc++;
                uint8_t this_reg = *pc++;
                uint8_t arr_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                TaggedValue rec_this = frame.GetRegister(this_reg);
                TaggedValue arr_val = frame.GetRegister(arr_reg);
                std::vector<TaggedValue> call_args;
                if (arr_val.IsHeapObject() && arr_val.AsHeapObject()->IsArray()) {
                    auto* arr = static_cast<JSArray*>(arr_val.AsHeapObject());
                    call_args.reserve(arr->Length());
                    for (size_t i = 0; i < arr->Length(); ++i) {
                        call_args.push_back(arr->GetElement(i));
                    }
                }

                if (!do_call(callee_val, rec_this, call_args, false)) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::ToNumber: {
                if (acc_.IsHeapObject()) {
                    auto* ho = acc_.AsHeapObject();
                    if (ho->IsJSObject() || ho->IsProxy()) {
                        acc_ = ToPrimitive(acc_, "number");
                        if (has_pending_exception_) {
                            if (frame.HasExceptionHandler()) {
                                auto h = frame.PopExceptionHandler();
                                pc = h.target_pc;
                                frame.SetEnvironment(h.env);
                                acc_ = pending_exception_;
                                has_pending_exception_ = false;
                            } else {
                                return pending_exception_;
                            }
                            break;
                        }
                    }
                }
                if (acc_.IsBigInt()) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert a BigInt value to a number")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (acc_.IsSymbol()) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert a Symbol value to a number")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }
                if (AETHER_UNLIKELY(!acc_.IsNumber())) {
                    acc_ = TaggedValue::FromDouble(acc_.ToDouble());
                }
                break;
            }
            case Opcode::CallSuper: {
                uint8_t callee_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                if (!callee_val.IsHeapObject() || (!callee_val.AsHeapObject()->IsFunction() && !callee_val.AsHeapObject()->IsProxy())) {
                    SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "super is not a function")));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                    break;
                }

                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                TaggedValue effective_this = frame.GetThis();
                if (callee_val.AsHeapObject()->IsNativeFunction()) {
                    auto* native_fn = static_cast<JSNativeFunction*>(callee_val.AsHeapObject());
                    acc_ = native_fn->Invoke(effective_this, call_args);
                } else if (callee_val.AsHeapObject()->IsFunction()) {
                    auto* func = static_cast<JSFunction*>(callee_val.AsHeapObject());
                    if (!func->GetEnvironment() && frame.GetEnvironment()) {
                        func->SetEnvironment(frame.GetEnvironment());
                    }
                    acc_ = Execute(*func->GetBytecode(), call_args, effective_this, func->GetEnvironment());
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                acc_ = effective_this;
                break;
            }
            case Opcode::Call: {
                uint8_t callee_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (!do_call(callee_val, TaggedValue::Undefined(), call_args, false)) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::CallDirectEval: {
                uint8_t callee_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (!do_call(callee_val, TaggedValue::Undefined(), call_args, true)) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::CallWithSpread: {
                uint8_t callee_reg = *pc++;
                uint8_t arr_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                TaggedValue arr_val = frame.GetRegister(arr_reg);
                std::vector<TaggedValue> call_args;
                if (arr_val.IsHeapObject() && arr_val.AsHeapObject()->IsArray()) {
                    auto* arr = static_cast<JSArray*>(arr_val.AsHeapObject());
                    call_args.reserve(arr->Length());
                    for (size_t i = 0; i < arr->Length(); ++i) {
                        call_args.push_back(arr->GetElement(i));
                    }
                }

                if (!do_call(callee_val, TaggedValue::Undefined(), call_args, false)) {
                    if (has_pending_exception_) {
                        if (frame.HasExceptionHandler()) {
                            auto h = frame.PopExceptionHandler();
                            pc = h.target_pc;
                            frame.SetEnvironment(h.env);
                            acc_ = pending_exception_;
                            has_pending_exception_ = false;
                        } else {
                            return pending_exception_;
                        }
                    }
                    break;
                }
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::PushExceptionHandler: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                const uint8_t* target = pc + offset;
                frame.PushExceptionHandler(target, frame.GetEnvironment());
                break;
            }
            case Opcode::PopExceptionHandler: {
                frame.PopExceptionHandler();
                break;
            }
            case Opcode::Throw: {
                TaggedValue exc = acc_;
                if (frame.HasExceptionHandler()) {
                    auto h = frame.PopExceptionHandler();
                    pc = h.target_pc;
                    frame.SetEnvironment(h.env);
                    acc_ = exc;
                } else {
                    has_pending_exception_ = true;
                    pending_exception_ = exc;
                    if (frame.GetGenerator()) {
                        frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                    }
                    return exc;
                }
                break;
            }
            case Opcode::Await: {
                if (frame.GetGenerator()) {
                    auto* gen = frame.GetGenerator();
                    TaggedValue yielded_val = acc_;
                    gen->SetPCOffset(static_cast<uint32_t>(pc - code_start));
                    gen->SavedRegisters() = frame.GetRegisters();
                    gen->SetEnvironment(frame.GetEnvironment());
                    gen->SavedHandlers().clear();
                    for (const auto& h : frame.Handlers()) {
                        gen->SavedHandlers().push_back({static_cast<uint32_t>(h.target_pc - code_start), h.env});
                    }
                    gen->SetState(JSGeneratorObject::State::SuspendedYield);
                    if (out_yielded) *out_yielded = true;
                    if (out_is_await) *out_is_await = true;
                    return yielded_val;
                }
                acc_ = TaggedValue::Undefined();
                break;
            }
            case Opcode::Yield: {
                if (frame.GetGenerator()) {
                    auto* gen = frame.GetGenerator();
                    TaggedValue yielded_val = acc_;
                    gen->SetPCOffset(static_cast<uint32_t>(pc - code_start));
                    gen->SavedRegisters() = frame.GetRegisters();
                    gen->SetEnvironment(frame.GetEnvironment());
                    gen->SavedHandlers().clear();
                    for (const auto& h : frame.Handlers()) {
                        gen->SavedHandlers().push_back({static_cast<uint32_t>(h.target_pc - code_start), h.env});
                    }
                    gen->SetState(JSGeneratorObject::State::SuspendedYield);
                    if (out_yielded) *out_yielded = true;
                    if (out_is_await) *out_is_await = false;
                    return yielded_val;
                }
                acc_ = TaggedValue::Undefined();
                break;
            }
            case Opcode::Debugger: {
                // Breakpoint hook / no-op in normal execution
                break;
            }
            case Opcode::Return: {
                if (frame.GetGenerator()) {
                    frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                }
                return acc_;
            }
            case Opcode::Halt: {
                if (frame.GetGenerator()) {
                    frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                }
                return acc_;
            }
            default:
                assert(false && "Unhandled bytecode opcode");
                return TaggedValue::Undefined();
        }
        } catch (const std::length_error& e) {
            ReportResourceError(e.what());
        } catch (const std::bad_alloc&) {
            ReportResourceError();
        }
        if (has_pending_exception_) {
            if (!frame.HasExceptionHandler()) return pending_exception_;
            auto handler = frame.PopExceptionHandler();
            pc = handler.target_pc;
            frame.SetEnvironment(handler.env);
            acc_ = pending_exception_;
            ClearPendingException();
        }
    }

    if (frame.GetGenerator()) {
        frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
    }
    return acc_;
}

} // namespace aether
