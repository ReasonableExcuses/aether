#include "aether/runtime.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include <cmath>
#include <iostream>

namespace aether {

// Math Builtins
static TaggedValue Math_abs(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    TaggedValue val = args[0];
    if (val.IsSmi()) {
        int32_t s = val.AsSmi();
        return TaggedValue::FromSmi(s < 0 ? -s : s);
    }
    double d = val.AsNumber();
    return TaggedValue::FromDouble(std::abs(d));
}

static TaggedValue Math_floor(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::floor(d));
}

static TaggedValue Math_ceil(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::ceil(d));
}

static TaggedValue Math_sqrt(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::sqrt(d));
}

static TaggedValue Math_max(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(-std::numeric_limits<double>::infinity());
    if (args.size() == 1) return args[0];
    if (args[0].IsSmi() && args[1].IsSmi()) {
        return TaggedValue::FromSmi(std::max(args[0].AsSmi(), args[1].AsSmi()));
    }
    return TaggedValue::FromDouble(std::max(args[0].AsNumber(), args[1].AsNumber()));
}

static TaggedValue Math_min(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::infinity());
    if (args.size() == 1) return args[0];
    if (args[0].IsSmi() && args[1].IsSmi()) {
        return TaggedValue::FromSmi(std::min(args[0].AsSmi(), args[1].AsSmi()));
    }
    return TaggedValue::FromDouble(std::min(args[0].AsNumber(), args[1].AsNumber()));
}

static TaggedValue Math_pow(const std::vector<TaggedValue>& args) {
    if (args.size() < 2) return TaggedValue::FromDouble(std::nan(""));
    double base = args[0].AsNumber();
    double exp = args[1].AsNumber();
    return TaggedValue::FromDouble(std::pow(base, exp));
}

// Console Builtins
static TaggedValue Console_log(const std::vector<TaggedValue>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].ToString();
    }
    std::cout << "\n";
    return TaggedValue::Undefined();
}

Realm::Realm(Heap& heap) {
    root_shape_ = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());
    InitializeIntrinsics(heap);
}

void Realm::InitializeIntrinsics(Heap& heap) {
    (void)heap;
    global_object_ = new JSObject(root_shape_.get());
    math_object_ = new JSObject(root_shape_.get());
    console_object_ = new JSObject(root_shape_.get());

    // Register Math functions
    math_object_->SetNamedProperty("abs", TaggedValue::FromHeapObject(new JSNativeFunction("abs", &Math_abs)), next_shape_id_);
    math_object_->SetNamedProperty("floor", TaggedValue::FromHeapObject(new JSNativeFunction("floor", &Math_floor)), next_shape_id_);
    math_object_->SetNamedProperty("ceil", TaggedValue::FromHeapObject(new JSNativeFunction("ceil", &Math_ceil)), next_shape_id_);
    math_object_->SetNamedProperty("sqrt", TaggedValue::FromHeapObject(new JSNativeFunction("sqrt", &Math_sqrt)), next_shape_id_);
    math_object_->SetNamedProperty("max", TaggedValue::FromHeapObject(new JSNativeFunction("max", &Math_max)), next_shape_id_);
    math_object_->SetNamedProperty("min", TaggedValue::FromHeapObject(new JSNativeFunction("min", &Math_min)), next_shape_id_);
    math_object_->SetNamedProperty("pow", TaggedValue::FromHeapObject(new JSNativeFunction("pow", &Math_pow)), next_shape_id_);

    // Register Console functions
    console_object_->SetNamedProperty("log", TaggedValue::FromHeapObject(new JSNativeFunction("log", &Console_log)), next_shape_id_);

    // Attach to Global Object
    global_object_->SetNamedProperty("Math", TaggedValue::FromHeapObject(math_object_), next_shape_id_);
    global_object_->SetNamedProperty("console", TaggedValue::FromHeapObject(console_object_), next_shape_id_);
    global_object_->SetNamedProperty("globalThis", TaggedValue::FromHeapObject(global_object_), next_shape_id_);
}

TaggedValue Realm::GetGlobalProperty(std::string_view name) {
    return global_object_->GetNamedProperty(name);
}

void Realm::SetGlobalProperty(std::string_view name, TaggedValue value) {
    global_object_->SetNamedProperty(name, value, next_shape_id_);
}

Agent::Agent() {
    heap_ = std::make_unique<Heap>();
    realm_ = std::make_unique<Realm>(*heap_);
}

TaggedValue Agent::EvaluateScript(std::string_view source) {
    Lexer lexer(source);
    Parser parser(lexer);
    auto ast = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bc = emitter.Emit(ast.get());

    TaggedValue result = interpreter_.Execute(*bc);

    // Drain microtasks at script execution checkpoint
    microtasks_.PerformMicrotaskCheckpoint();

    return result;
}

} // namespace aether
