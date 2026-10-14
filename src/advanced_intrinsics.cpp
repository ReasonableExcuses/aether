#include "aether/advanced_intrinsics.h"
#include "aether/heap.h"
#include "aether/interpreter.h"
#include "aether/event_loop.h"

#include <thread>
#include <limits>
#include <string>

namespace aether {

// ============================================================================
// Helper: ValueToNumber
// ============================================================================

static inline double ValueToNumber(TaggedValue v) {
    if (v.IsNumber()) return v.AsNumber();
    if (v.IsBoolean()) return v.AsBoolean() ? 1.0 : 0.0;
    if (v.IsNull()) return 0.0;
    if (v.IsUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (v.IsHeapObject() && v.AsHeapObject()->IsString()) {
        std::string s(static_cast<JSString*>(v.AsHeapObject())->View());
        size_t first = s.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) return 0.0;
        size_t last = s.find_last_not_of(" \t\n\r");
        std::string trimmed = s.substr(first, last - first + 1);
        try {
            size_t idx = 0;
            double d = std::stod(trimmed, &idx);
            if (idx == trimmed.length()) return d;
            return std::numeric_limits<double>::quiet_NaN();
        } catch (...) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// ============================================================================
// Managed Async Wait Thread Pool
// ============================================================================

AsyncWaitManager& AsyncWaitManager::Instance() {
    static AsyncWaitManager s_mgr;
    return s_mgr;
}

void AsyncWaitManager::SpawnWait(int32_t* ptr, int32_t expected, double timeout, JSPromise* promise, Agent* agent, Realm* realm) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (realm) {
        realm->RegisterRootValue(std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(promise)));
    }

    auto task = std::make_shared<WaitTask>();
    task->agent = agent;

    task->worker = std::thread([ptr, expected, timeout, promise, task]() {
        std::string wait_res = FutexManager::Instance().Wait32(ptr, expected, timeout);
        if (task->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        if (task->agent) {
            task->agent->GetMicrotasks().Enqueue([promise, wait_res, task]() {
                if (task->cancelled.load(std::memory_order_acquire)) return;
                MicrotaskQueue dummy;
                promise->Fulfill(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(wait_res)), dummy);
            });
        } else {
            CurrentMicrotaskQueue().Enqueue([promise, wait_res, task]() {
                if (task->cancelled.load(std::memory_order_acquire)) return;
                MicrotaskQueue dummy;
                promise->Fulfill(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(wait_res)), dummy);
            });
        }
    });

    tasks_.push_back(task);
}

void AsyncWaitManager::CancelAndJoinForAgent(Agent* agent) {
    std::vector<std::shared_ptr<WaitTask>> agent_tasks;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if ((*it)->agent == agent) {
                (*it)->cancelled.store(true, std::memory_order_release);
                agent_tasks.push_back(*it);
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!agent_tasks.empty()) {
        FutexManager::Instance().Clear();
        for (auto& t : agent_tasks) {
            if (t->worker.joinable()) {
                t->worker.join();
            }
        }
    }
}

void AsyncWaitManager::Shutdown() {
    std::vector<std::shared_ptr<WaitTask>> all_tasks;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& t : tasks_) {
            t->cancelled.store(true, std::memory_order_release);
            all_tasks.push_back(t);
        }
        tasks_.clear();
    }
    if (!all_tasks.empty()) {
        FutexManager::Instance().Clear();
        for (auto& t : all_tasks) {
            if (t->worker.joinable()) {
                t->worker.join();
            }
        }
    }
}

// ============================================================================
// Helper: Atomic Operations Dispatcher for Atomics.*
// ============================================================================

template <typename Op>
static TaggedValue PerformAtomicOp(const std::vector<TaggedValue>& args, std::string_view op_name, Op&& op) {
    if (args.size() < 2) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": requires at least 2 arguments")));
        }
        return TaggedValue::Undefined();
    }

    if (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsTypedArray()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": argument 0 must be a TypedArray")));
        }
        return TaggedValue::Undefined();
    }

    auto* ta = static_cast<JSTypedArray*>(args[0].AsHeapObject());
    if (!ta->Buffer() || !ta->Buffer()->IsShared()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": typedArray must be backed by a SharedArrayBuffer")));
        }
        return TaggedValue::Undefined();
    }

    if (ta->Kind() == TypedArrayKind::Uint8Clamped ||
        ta->Kind() == TypedArrayKind::Float32 ||
        ta->Kind() == TypedArrayKind::Float64) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": unsupported TypedArray element type")));
        }
        return TaggedValue::Undefined();
    }

    int64_t idx = -1;
    if (args[1].IsSmi()) {
        idx = args[1].AsSmi();
    } else if (args[1].IsNumber()) {
        idx = static_cast<int64_t>(args[1].AsNumber());
    } else {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": invalid index")));
        }
        return TaggedValue::Undefined();
    }

    if (idx < 0 || static_cast<size_t>(idx) >= ta->Length()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("RangeError", std::string(op_name) + ": index out of bounds")));
        }
        return TaggedValue::Undefined();
    }

    size_t uidx = static_cast<size_t>(idx);
    uint8_t* ptr = ta->Data() + uidx * ta->BytesPerElement();

    switch (ta->Kind()) {
        case TypedArrayKind::Int8: {
            auto* p = reinterpret_cast<int8_t*>(ptr);
            std::atomic_ref<int8_t> ref(*p);
            return TaggedValue::FromSmi(op(ref, static_cast<int8_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0)));
        }
        case TypedArrayKind::Uint8: {
            auto* p = reinterpret_cast<uint8_t*>(ptr);
            std::atomic_ref<uint8_t> ref(*p);
            return TaggedValue::FromSmi(op(ref, static_cast<uint8_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0)));
        }
        case TypedArrayKind::Int16: {
            auto* p = reinterpret_cast<int16_t*>(ptr);
            std::atomic_ref<int16_t> ref(*p);
            return TaggedValue::FromSmi(op(ref, static_cast<int16_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0)));
        }
        case TypedArrayKind::Uint16: {
            auto* p = reinterpret_cast<uint16_t*>(ptr);
            std::atomic_ref<uint16_t> ref(*p);
            return TaggedValue::FromSmi(op(ref, static_cast<uint16_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0)));
        }
        case TypedArrayKind::Int32: {
            auto* p = reinterpret_cast<int32_t*>(ptr);
            std::atomic_ref<int32_t> ref(*p);
            return TaggedValue::FromSmi(op(ref, static_cast<int32_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0)));
        }
        case TypedArrayKind::Uint32: {
            auto* p = reinterpret_cast<uint32_t*>(ptr);
            std::atomic_ref<uint32_t> ref(*p);
            return TaggedValue::FromDouble(static_cast<double>(op(ref, static_cast<uint32_t>(args.size() > 2 ? ValueToNumber(args[2]) : 0))));
        }
        case TypedArrayKind::BigInt64: {
            auto* p = reinterpret_cast<int64_t*>(ptr);
            std::atomic_ref<int64_t> ref(*p);
            int64_t arg_val = 0;
            if (args.size() > 2) {
                if (args[2].IsSmi()) arg_val = args[2].AsSmi();
                else if (args[2].IsDouble()) arg_val = static_cast<int64_t>(args[2].AsDouble());
                else arg_val = static_cast<int64_t>(ValueToNumber(args[2]));
            }
            return TaggedValue::FromDouble(static_cast<double>(op(ref, arg_val)));
        }
        case TypedArrayKind::BigUint64: {
            auto* p = reinterpret_cast<uint64_t*>(ptr);
            std::atomic_ref<uint64_t> ref(*p);
            uint64_t arg_val = 0;
            if (args.size() > 2) {
                if (args[2].IsSmi()) arg_val = static_cast<uint64_t>(args[2].AsSmi());
                else if (args[2].IsDouble()) arg_val = static_cast<uint64_t>(args[2].AsDouble());
                else arg_val = static_cast<uint64_t>(ValueToNumber(args[2]));
            }
            return TaggedValue::FromDouble(static_cast<double>(op(ref, arg_val)));
        }
        default: {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", std::string(op_name) + ": unsupported TypedArray element type")));
            }
            return TaggedValue::Undefined();
        }
    }
}

// ============================================================================
// Initialization Entrypoint: InitializeAdvancedIntrinsics
// ============================================================================

void InitializeAdvancedIntrinsics(Realm& realm) {
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();
    JSObject* global = realm.GetGlobalObject();

    auto make_native = [root](std::string_view name, JSNativeFunction::CallbackType cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto make_method = [root](std::string_view name, JSNativeFunction::MethodCallbackType cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto set_string_tag = [root, &next_sid](JSObject* target, std::string_view tag) {
        auto* tag_str = AllocateRuntime<JSString>(tag);
        target->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(tag_str), next_sid);
        target->SetNamedProperty("toStringTag", TaggedValue::FromHeapObject(tag_str), next_sid);
    };

    // ========================================================================
    // 1. WeakMap Intrinsic
    // ========================================================================
    auto* weak_map_proto = AllocateRuntime<JSObject>(root);
    set_string_tag(weak_map_proto, "WeakMap");

    weak_map_proto->SetNamedProperty("set", TaggedValue::FromHeapObject(make_method("set", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakMap()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakMap.prototype.set called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* wm = static_cast<JSWeakMap*>(this_val.AsHeapObject());
        TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!k.IsHeapObject() || !k.AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Invalid value used as weak map key")));
            }
            return TaggedValue::Undefined();
        }
        TaggedValue v = args.size() > 1 ? args[1] : TaggedValue::Undefined();
        wm->Set(k.AsHeapObject(), v);
        return this_val;
    })), next_sid);

    weak_map_proto->SetNamedProperty("get", TaggedValue::FromHeapObject(make_method("get", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakMap()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakMap.prototype.get called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* wm = static_cast<JSWeakMap*>(this_val.AsHeapObject());
        TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!k.IsHeapObject() || !k.AsHeapObject()->IsECMAObject()) {
            return TaggedValue::Undefined();
        }
        return wm->Get(k.AsHeapObject());
    })), next_sid);

    weak_map_proto->SetNamedProperty("has", TaggedValue::FromHeapObject(make_method("has", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakMap()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakMap.prototype.has called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* wm = static_cast<JSWeakMap*>(this_val.AsHeapObject());
        TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!k.IsHeapObject() || !k.AsHeapObject()->IsECMAObject()) {
            return TaggedValue::FromBoolean(false);
        }
        return TaggedValue::FromBoolean(wm->Has(k.AsHeapObject()));
    })), next_sid);

    weak_map_proto->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_method("delete", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakMap()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakMap.prototype.delete called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* wm = static_cast<JSWeakMap*>(this_val.AsHeapObject());
        TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!k.IsHeapObject() || !k.AsHeapObject()->IsECMAObject()) {
            return TaggedValue::FromBoolean(false);
        }
        return TaggedValue::FromBoolean(wm->Delete(k.AsHeapObject()));
    })), next_sid);

    auto* weak_map_ctor = make_native("WeakMap", [root, weak_map_proto](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* wm = AllocateRuntime<JSWeakMap>(root);
        wm->SetPrototype(weak_map_proto);

        // Populate from iterable if provided
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsArray()) {
            auto* arr = static_cast<JSArray*>(args[0].AsHeapObject());
            for (size_t i = 0; i < arr->Length(); ++i) {
                TaggedValue elem = arr->GetElement(i);
                if (elem.IsHeapObject() && elem.AsHeapObject()->IsArray()) {
                    auto* pair = static_cast<JSArray*>(elem.AsHeapObject());
                    if (pair->Length() >= 1) {
                        TaggedValue k = pair->GetElement(0);
                        TaggedValue v = pair->Length() >= 2 ? pair->GetElement(1) : TaggedValue::Undefined();
                        if (k.IsHeapObject() && k.AsHeapObject()->IsECMAObject()) {
                            wm->Set(k.AsHeapObject(), v);
                        }
                    }
                }
            }
        }
        return TaggedValue::FromHeapObject(wm);
    });

    weak_map_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(weak_map_proto), next_sid);
    weak_map_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(weak_map_ctor), next_sid);
    global->SetNamedProperty("WeakMap", TaggedValue::FromHeapObject(weak_map_ctor), next_sid);

    // ========================================================================
    // 2. WeakSet Intrinsic
    // ========================================================================
    auto* weak_set_proto = AllocateRuntime<JSObject>(root);
    set_string_tag(weak_set_proto, "WeakSet");

    weak_set_proto->SetNamedProperty("add", TaggedValue::FromHeapObject(make_method("add", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakSet()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakSet.prototype.add called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* ws = static_cast<JSWeakSet*>(this_val.AsHeapObject());
        TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!v.IsHeapObject() || !v.AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Invalid value used in weak set")));
            }
            return TaggedValue::Undefined();
        }
        ws->Add(v.AsHeapObject());
        return this_val;
    })), next_sid);

    weak_set_proto->SetNamedProperty("has", TaggedValue::FromHeapObject(make_method("has", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakSet()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakSet.prototype.has called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* ws = static_cast<JSWeakSet*>(this_val.AsHeapObject());
        TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!v.IsHeapObject() || !v.AsHeapObject()->IsECMAObject()) {
            return TaggedValue::FromBoolean(false);
        }
        return TaggedValue::FromBoolean(ws->Has(v.AsHeapObject()));
    })), next_sid);

    weak_set_proto->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_method("delete", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakSet()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakSet.prototype.delete called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* ws = static_cast<JSWeakSet*>(this_val.AsHeapObject());
        TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
        if (!v.IsHeapObject() || !v.AsHeapObject()->IsECMAObject()) {
            return TaggedValue::FromBoolean(false);
        }
        return TaggedValue::FromBoolean(ws->Delete(v.AsHeapObject()));
    })), next_sid);

    auto* weak_set_ctor = make_native("WeakSet", [root, weak_set_proto](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* ws = AllocateRuntime<JSWeakSet>(root);
        ws->SetPrototype(weak_set_proto);

        // Populate from iterable if provided
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsArray()) {
            auto* arr = static_cast<JSArray*>(args[0].AsHeapObject());
            for (size_t i = 0; i < arr->Length(); ++i) {
                TaggedValue elem = arr->GetElement(i);
                if (elem.IsHeapObject() && elem.AsHeapObject()->IsECMAObject()) {
                    ws->Add(elem.AsHeapObject());
                }
            }
        }
        return TaggedValue::FromHeapObject(ws);
    });

    weak_set_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(weak_set_proto), next_sid);
    weak_set_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(weak_set_ctor), next_sid);
    global->SetNamedProperty("WeakSet", TaggedValue::FromHeapObject(weak_set_ctor), next_sid);

    // ========================================================================
    // 3. SharedArrayBuffer Intrinsic
    // ========================================================================
    auto* sab_proto = AllocateRuntime<JSObject>(root);
    set_string_tag(sab_proto, "SharedArrayBuffer");

    sab_proto->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_method("slice", [sab_proto](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArrayBuffer()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method SharedArrayBuffer.prototype.slice called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* sab = static_cast<JSArrayBuffer*>(this_val.AsHeapObject());
        int64_t begin = 0;
        int64_t end = static_cast<int64_t>(sab->ByteLength());
        if (!args.empty() && args[0].IsNumber()) begin = static_cast<int64_t>(args[0].AsNumber());
        if (args.size() > 1 && args[1].IsNumber()) end = static_cast<int64_t>(args[1].AsNumber());
        auto* copy = sab->Slice(begin, end, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        copy->SetPrototype(sab_proto);
        return TaggedValue::FromHeapObject(copy);
    })), next_sid);

    sab_proto->SetNamedProperty("grow", TaggedValue::FromHeapObject(make_method("grow", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArrayBuffer()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method SharedArrayBuffer.prototype.grow called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* sab = static_cast<JSArrayBuffer*>(this_val.AsHeapObject());
        if (!sab->IsGrowable()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "SharedArrayBuffer is not growable")));
            }
            return TaggedValue::Undefined();
        }
        double new_len_d = args.empty() ? 0.0 : ValueToNumber(args[0]);
        if (new_len_d < 0.0 || new_len_d < static_cast<double>(sab->ByteLength()) || new_len_d > static_cast<double>(sab->MaxByteLength())) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("RangeError", "Invalid new byte length for SharedArrayBuffer")));
            }
            return TaggedValue::Undefined();
        }
        sab->Grow(static_cast<size_t>(new_len_d));
        return TaggedValue::Undefined();
    })), next_sid);

    auto* sab_ctor = make_native("SharedArrayBuffer", [root, sab_proto](const std::vector<TaggedValue>& args) -> TaggedValue {
        double byte_length_d = 0.0;
        if (!args.empty()) {
            byte_length_d = ValueToNumber(args[0]);
            if (std::isnan(byte_length_d) || byte_length_d < 0.0) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        new JSError("RangeError", "Invalid SharedArrayBuffer byte length")));
                }
                return TaggedValue::Undefined();
            }
        }
        size_t byte_length = static_cast<size_t>(byte_length_d);
        size_t max_byte_length = byte_length;

        // Check options: { maxByteLength: N }
        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* opts_obj = static_cast<JSObject*>(args[1].AsHeapObject());
            TaggedValue max_bl_val = opts_obj->GetNamedProperty("maxByteLength");
            if (max_bl_val.IsNumber()) {
                double mbl = max_bl_val.AsNumber();
                if (mbl < byte_length_d) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            new JSError("RangeError", "maxByteLength must be greater than or equal to byteLength")));
                    }
                    return TaggedValue::Undefined();
                }
                max_byte_length = static_cast<size_t>(mbl);
            }
        }

        auto* sab = AllocateRuntime<JSArrayBuffer>(byte_length, max_byte_length, /*is_shared=*/true, root);
        sab->SetPrototype(sab_proto);
        return TaggedValue::FromHeapObject(sab);
    });

    sab_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(sab_proto), next_sid);
    sab_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(sab_ctor), next_sid);
    global->SetNamedProperty("SharedArrayBuffer", TaggedValue::FromHeapObject(sab_ctor), next_sid);

    // ========================================================================
    // 4. Atomics Global Namespace & 13 Standard Methods
    // ========================================================================
    auto* atomics_obj = AllocateRuntime<JSObject>(root);
    set_string_tag(atomics_obj, "Atomics");

    // 1. Atomics.isLockFree(size)
    atomics_obj->SetNamedProperty("isLockFree", TaggedValue::FromHeapObject(make_native("isLockFree", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::FromBoolean(false);
        int32_t size = 0;
        if (args[0].IsSmi()) size = args[0].AsSmi();
        else if (args[0].IsNumber()) size = static_cast<int32_t>(args[0].AsNumber());
        else return TaggedValue::FromBoolean(false);
        return TaggedValue::FromBoolean(size == 1 || size == 2 || size == 4 || size == 8);
    })), next_sid);

    // 2. Atomics.load(typedArray, index)
    atomics_obj->SetNamedProperty("load", TaggedValue::FromHeapObject(make_native("load", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.load", [](auto& ref, auto) {
            return ref.load(std::memory_order_seq_cst);
        });
    })), next_sid);

    // 3. Atomics.store(typedArray, index, value)
    atomics_obj->SetNamedProperty("store", TaggedValue::FromHeapObject(make_native("store", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.store", [](auto& ref, auto val) {
            ref.store(val, std::memory_order_seq_cst);
            return val;
        });
    })), next_sid);

    // 4. Atomics.add(typedArray, index, value)
    atomics_obj->SetNamedProperty("add", TaggedValue::FromHeapObject(make_native("add", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.add", [](auto& ref, auto val) {
            return ref.fetch_add(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 5. Atomics.sub(typedArray, index, value)
    atomics_obj->SetNamedProperty("sub", TaggedValue::FromHeapObject(make_native("sub", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.sub", [](auto& ref, auto val) {
            return ref.fetch_sub(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 6. Atomics.and(typedArray, index, value)
    atomics_obj->SetNamedProperty("and", TaggedValue::FromHeapObject(make_native("and", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.and", [](auto& ref, auto val) {
            return ref.fetch_and(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 7. Atomics.or(typedArray, index, value)
    atomics_obj->SetNamedProperty("or", TaggedValue::FromHeapObject(make_native("or", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.or", [](auto& ref, auto val) {
            return ref.fetch_or(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 8. Atomics.xor(typedArray, index, value)
    atomics_obj->SetNamedProperty("xor", TaggedValue::FromHeapObject(make_native("xor", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.xor", [](auto& ref, auto val) {
            return ref.fetch_xor(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 9. Atomics.exchange(typedArray, index, value)
    atomics_obj->SetNamedProperty("exchange", TaggedValue::FromHeapObject(make_native("exchange", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        return PerformAtomicOp(args, "Atomics.exchange", [](auto& ref, auto val) {
            return ref.exchange(val, std::memory_order_seq_cst);
        });
    })), next_sid);

    // 10. Atomics.compareExchange(typedArray, index, expectedValue, replacementValue)
    atomics_obj->SetNamedProperty("compareExchange", TaggedValue::FromHeapObject(make_native("compareExchange", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.size() < 4) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.compareExchange: requires 4 arguments")));
            }
            return TaggedValue::Undefined();
        }
        if (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsTypedArray()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.compareExchange: argument 0 must be a TypedArray")));
            }
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(args[0].AsHeapObject());
        if (!ta->Buffer() || !ta->Buffer()->IsShared()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Atomics.compareExchange: typedArray buffer must be a SharedArrayBuffer")));
            }
            return TaggedValue::Undefined();
        }
        int64_t idx = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber());
        if (idx < 0 || static_cast<size_t>(idx) >= ta->Length()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("RangeError", "Atomics.compareExchange: index out of bounds")));
            }
            return TaggedValue::Undefined();
        }
        size_t uidx = static_cast<size_t>(idx);
        uint8_t* ptr = ta->Data() + uidx * ta->BytesPerElement();

        if (ta->Kind() == TypedArrayKind::Int32) {
            auto* p = reinterpret_cast<int32_t*>(ptr);
            std::atomic_ref<int32_t> ref(*p);
            int32_t expected = static_cast<int32_t>(ValueToNumber(args[2]));
            int32_t replacement = static_cast<int32_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromSmi(expected);
        } else if (ta->Kind() == TypedArrayKind::Uint32) {
            auto* p = reinterpret_cast<uint32_t*>(ptr);
            std::atomic_ref<uint32_t> ref(*p);
            uint32_t expected = static_cast<uint32_t>(ValueToNumber(args[2]));
            uint32_t replacement = static_cast<uint32_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromDouble(static_cast<double>(expected));
        } else if (ta->Kind() == TypedArrayKind::Int8) {
            auto* p = reinterpret_cast<int8_t*>(ptr);
            std::atomic_ref<int8_t> ref(*p);
            int8_t expected = static_cast<int8_t>(ValueToNumber(args[2]));
            int8_t replacement = static_cast<int8_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromSmi(expected);
        } else if (ta->Kind() == TypedArrayKind::Uint8) {
            auto* p = reinterpret_cast<uint8_t*>(ptr);
            std::atomic_ref<uint8_t> ref(*p);
            uint8_t expected = static_cast<uint8_t>(ValueToNumber(args[2]));
            uint8_t replacement = static_cast<uint8_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromSmi(expected);
        } else if (ta->Kind() == TypedArrayKind::Int16) {
            auto* p = reinterpret_cast<int16_t*>(ptr);
            std::atomic_ref<int16_t> ref(*p);
            int16_t expected = static_cast<int16_t>(ValueToNumber(args[2]));
            int16_t replacement = static_cast<int16_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromSmi(expected);
        } else if (ta->Kind() == TypedArrayKind::Uint16) {
            auto* p = reinterpret_cast<uint16_t*>(ptr);
            std::atomic_ref<uint16_t> ref(*p);
            uint16_t expected = static_cast<uint16_t>(ValueToNumber(args[2]));
            uint16_t replacement = static_cast<uint16_t>(ValueToNumber(args[3]));
            ref.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
            return TaggedValue::FromSmi(expected);
        }

        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                new JSError("TypeError", "Atomics.compareExchange: unsupported element type")));
        }
        return TaggedValue::Undefined();
    })), next_sid);

    // 11. Atomics.wait(typedArray, index, value, timeout)
    atomics_obj->SetNamedProperty("wait", TaggedValue::FromHeapObject(make_native("wait", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.size() < 3) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.wait: requires at least 3 arguments")));
            }
            return TaggedValue::Undefined();
        }
        if (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsTypedArray()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.wait: argument 0 must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(args[0].AsHeapObject());
        if (ta->Kind() != TypedArrayKind::Int32) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.wait: typedArray must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        if (!ta->Buffer() || !ta->Buffer()->IsShared()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.wait: typedArray buffer must be a SharedArrayBuffer")));
            }
            return TaggedValue::Undefined();
        }

        int64_t idx = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber());
        if (idx < 0 || static_cast<size_t>(idx) >= ta->Length()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("RangeError", "Atomics.wait: index out of bounds")));
            }
            return TaggedValue::Undefined();
        }

        int32_t expected = static_cast<int32_t>(ValueToNumber(args[2]));
        double timeout = std::numeric_limits<double>::infinity();
        if (args.size() > 3 && !args[3].IsUndefined()) {
            timeout = args[3].AsNumber();
            if (std::isnan(timeout)) timeout = std::numeric_limits<double>::infinity();
        }

        int32_t* ptr = reinterpret_cast<int32_t*>(ta->Data() + static_cast<size_t>(idx) * sizeof(int32_t));
        std::string res = FutexManager::Instance().Wait32(ptr, expected, timeout);
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
    })), next_sid);

    // 12. Atomics.notify(typedArray, index, count)
    atomics_obj->SetNamedProperty("notify", TaggedValue::FromHeapObject(make_native("notify", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.size() < 2) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.notify: requires at least 2 arguments")));
            }
            return TaggedValue::Undefined();
        }
        if (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsTypedArray()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.notify: argument 0 must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(args[0].AsHeapObject());
        if (ta->Kind() != TypedArrayKind::Int32) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.notify: typedArray must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        if (!ta->Buffer() || !ta->Buffer()->IsShared()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.notify: typedArray buffer must be a SharedArrayBuffer")));
            }
            return TaggedValue::Undefined();
        }

        int64_t idx = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber());
        if (idx < 0 || static_cast<size_t>(idx) >= ta->Length()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("RangeError", "Atomics.notify: index out of bounds")));
            }
            return TaggedValue::Undefined();
        }

        uint32_t count = UINT32_MAX;
        if (args.size() > 2 && !args[2].IsUndefined()) {
            double d = args[2].AsNumber();
            if (d >= 0.0) count = static_cast<uint32_t>(d);
        }

        void* ptr = static_cast<void*>(ta->Data() + static_cast<size_t>(idx) * sizeof(int32_t));
        uint32_t awakened = FutexManager::Instance().Notify(ptr, count);
        return TaggedValue::FromSmi(static_cast<int32_t>(awakened));
    })), next_sid);

    // 13. Atomics.waitAsync(typedArray, index, value, timeout)
    atomics_obj->SetNamedProperty("waitAsync", TaggedValue::FromHeapObject(make_native("waitAsync", [root](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.size() < 3) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.waitAsync: requires at least 3 arguments")));
            }
            return TaggedValue::Undefined();
        }
        if (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsTypedArray()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Atomics.waitAsync: argument 0 must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(args[0].AsHeapObject());
        if (ta->Kind() != TypedArrayKind::Int32) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Atomics.waitAsync: typedArray must be an Int32Array")));
            }
            return TaggedValue::Undefined();
        }
        if (!ta->Buffer() || !ta->Buffer()->IsShared()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Atomics.waitAsync: typedArray buffer must be a SharedArrayBuffer")));
            }
            return TaggedValue::Undefined();
        }

        int64_t idx = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber());
        if (idx < 0 || static_cast<size_t>(idx) >= ta->Length()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("RangeError", "Atomics.waitAsync: index out of bounds")));
            }
            return TaggedValue::Undefined();
        }

        int32_t expected = static_cast<int32_t>(ValueToNumber(args[2]));
        double timeout = std::numeric_limits<double>::infinity();
        if (args.size() > 3 && !args[3].IsUndefined()) {
            timeout = args[3].AsNumber();
            if (std::isnan(timeout)) timeout = std::numeric_limits<double>::infinity();
        }

        int32_t* ptr = reinterpret_cast<int32_t*>(ta->Data() + static_cast<size_t>(idx) * sizeof(int32_t));
        std::atomic_ref<int32_t> ref(*ptr);

        auto* res_obj = AllocateRuntime<JSObject>(root);
        uint32_t sid = 9100;

        if (ref.load(std::memory_order_seq_cst) != expected) {
            res_obj->SetNamedProperty("async", TaggedValue::FromBoolean(false), sid);
            res_obj->SetNamedProperty("value", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("not-equal")), sid);
            return TaggedValue::FromHeapObject(res_obj);
        }

        if (timeout == 0.0) {
            res_obj->SetNamedProperty("async", TaggedValue::FromBoolean(false), sid);
            res_obj->SetNamedProperty("value", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("timed-out")), sid);
            return TaggedValue::FromHeapObject(res_obj);
        }

        auto* promise = AllocateRuntime<JSPromise>(root);
        if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
            promise->SetPrototype(Realm::Current()->GetPromisePrototype());
        }

        AsyncWaitManager::Instance().SpawnWait(ptr, expected, timeout, promise, Agent::Current(), Realm::Current());

        res_obj->SetNamedProperty("async", TaggedValue::FromBoolean(true), sid);
        res_obj->SetNamedProperty("value", TaggedValue::FromHeapObject(promise), sid);
        return TaggedValue::FromHeapObject(res_obj);
    })), next_sid);

    global->SetNamedProperty("Atomics", TaggedValue::FromHeapObject(atomics_obj), next_sid);

    // ========================================================================
    // 5. WeakRef Intrinsic
    // ========================================================================
    auto* weak_ref_proto = AllocateRuntime<JSObject>(root);
    set_string_tag(weak_ref_proto, "WeakRef");

    weak_ref_proto->SetNamedProperty("deref", TaggedValue::FromHeapObject(make_method("deref", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsWeakRef()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method WeakRef.prototype.deref called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* wr = static_cast<JSWeakRef*>(this_val.AsHeapObject());
        return wr->Deref();
    })), next_sid);

    auto* weak_ref_ctor = make_native("WeakRef", [root, weak_ref_proto](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "WeakRef: target must be an object")));
            }
            return TaggedValue::Undefined();
        }
        auto* wr = AllocateRuntime<JSWeakRef>(args[0].AsHeapObject(), root);
        wr->SetPrototype(weak_ref_proto);
        return TaggedValue::FromHeapObject(wr);
    });

    weak_ref_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(weak_ref_proto), next_sid);
    weak_ref_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(weak_ref_ctor), next_sid);
    global->SetNamedProperty("WeakRef", TaggedValue::FromHeapObject(weak_ref_ctor), next_sid);

    // ========================================================================
    // 6. FinalizationRegistry Intrinsic
    // ========================================================================
    auto* fr_proto = AllocateRuntime<JSObject>(root);
    set_string_tag(fr_proto, "FinalizationRegistry");

    fr_proto->SetNamedProperty("register", TaggedValue::FromHeapObject(make_method("register", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFinalizationRegistry()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method FinalizationRegistry.prototype.register called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* fr = static_cast<JSFinalizationRegistry*>(this_val.AsHeapObject());
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "FinalizationRegistry.prototype.register: target must be an object")));
            }
            return TaggedValue::Undefined();
        }
        TaggedValue target = args[0];
        TaggedValue held = args.size() > 1 ? args[1] : TaggedValue::Undefined();
        HeapObject* token = nullptr;
        if (args.size() > 2 && !args[2].IsUndefined()) {
            if (!args[2].IsHeapObject() || !args[2].AsHeapObject()->IsECMAObject()) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        new JSError("TypeError", "FinalizationRegistry.prototype.register: unregisterToken must be an object")));
                }
                return TaggedValue::Undefined();
            }
            token = args[2].AsHeapObject();
        }

        fr->Register(target.AsHeapObject(), held, token);
        return TaggedValue::Undefined();
    })), next_sid);

    fr_proto->SetNamedProperty("unregister", TaggedValue::FromHeapObject(make_method("unregister", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFinalizationRegistry()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "Method FinalizationRegistry.prototype.unregister called on incompatible receiver")));
            }
            return TaggedValue::Undefined();
        }
        auto* fr = static_cast<JSFinalizationRegistry*>(this_val.AsHeapObject());
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "FinalizationRegistry.prototype.unregister: unregisterToken must be an object")));
            }
            return TaggedValue::Undefined();
        }

        bool removed = fr->Unregister(args[0].AsHeapObject());
        return TaggedValue::FromBoolean(removed);
    })), next_sid);

    // cleanupSome / triggerCleanup helper for testability & microtask pump
    fr_proto->SetNamedProperty("cleanupSome", TaggedValue::FromHeapObject(make_method("cleanupSome", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFinalizationRegistry()) {
            return TaggedValue::Undefined();
        }
        auto* fr = static_cast<JSFinalizationRegistry*>(this_val.AsHeapObject());
        HeapObject* target = args.empty() ? nullptr : (args[0].IsHeapObject() ? args[0].AsHeapObject() : nullptr);
        size_t count = fr->CleanupForTarget(target, [](TaggedValue cb, TaggedValue held) {
            CallFunction(cb, TaggedValue::Undefined(), {held});
        });
        return TaggedValue::FromSmi(static_cast<int32_t>(count));
    })), next_sid);

    auto* fr_ctor = make_native("FinalizationRegistry", [root, fr_proto](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    new JSError("TypeError", "FinalizationRegistry: cleanupCallback must be a function")));
            }
            return TaggedValue::Undefined();
        }
        auto* fr = AllocateRuntime<JSFinalizationRegistry>(args[0], root);
        fr->SetPrototype(fr_proto);
        return TaggedValue::FromHeapObject(fr);
    });

    fr_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(fr_proto), next_sid);
    fr_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(fr_ctor), next_sid);
    global->SetNamedProperty("FinalizationRegistry", TaggedValue::FromHeapObject(fr_ctor), next_sid);
}

} // namespace aether
