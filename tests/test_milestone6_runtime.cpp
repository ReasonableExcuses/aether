#include "aether/common.h"
#include "aether/value.h"
#include "aether/runtime.h"

#include <iostream>
#include <cassert>

using namespace aether;

void TestAgentAndRealmInitialization() {
    std::cout << "[Test 1] Verifying Agent, Realm, and Global Object Instantiation...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();

    JSObject* global = realm.GetGlobalObject();
    assert(global != nullptr);
    assert(global->GetShape() != nullptr);
    (void)global;

    TaggedValue math_val = realm.GetGlobalProperty("Math");
    assert(math_val.IsHeapObject() && math_val.AsHeapObject()->IsJSObject());

    TaggedValue console_val = realm.GetGlobalProperty("console");
    assert(console_val.IsHeapObject() && console_val.AsHeapObject()->IsJSObject());

    std::cout << "  -> Realm and Intrinsics (Math, console, globalThis) initialized cleanly.\n" << std::flush;
}

void TestMathIntrinsics() {
    std::cout << "[Test 2] Verifying Builtin Math Intrinsics...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();
    JSObject* math = static_cast<JSObject*>(realm.GetGlobalProperty("Math").AsHeapObject());

    // Math.abs(-50) = 50
    TaggedValue abs_fn_val = math->GetNamedProperty("abs");
    assert(abs_fn_val.IsHeapObject() && abs_fn_val.AsHeapObject()->IsNativeFunction());
    auto* abs_fn = static_cast<JSNativeFunction*>(abs_fn_val.AsHeapObject());
    TaggedValue res_abs = abs_fn->Callback()({ TaggedValue::FromSmi(-50) });
    assert(res_abs.IsSmi() && res_abs.AsSmi() == 50);

    // Math.sqrt(81) = 9
    TaggedValue sqrt_fn_val = math->GetNamedProperty("sqrt");
    auto* sqrt_fn = static_cast<JSNativeFunction*>(sqrt_fn_val.AsHeapObject());
    TaggedValue res_sqrt = sqrt_fn->Callback()({ TaggedValue::FromDouble(81.0) });
    assert(res_sqrt.IsDouble() && res_sqrt.AsDouble() == 9.0);

    // Math.max(12, 99) = 99
    TaggedValue max_fn_val = math->GetNamedProperty("max");
    auto* max_fn = static_cast<JSNativeFunction*>(max_fn_val.AsHeapObject());
    TaggedValue res_max = max_fn->Callback()({ TaggedValue::FromSmi(12), TaggedValue::FromSmi(99) });
    assert(res_max.IsSmi() && res_max.AsSmi() == 99);

    // Math.pow(2, 10) = 1024
    TaggedValue pow_fn_val = math->GetNamedProperty("pow");
    auto* pow_fn = static_cast<JSNativeFunction*>(pow_fn_val.AsHeapObject());
    TaggedValue res_pow = pow_fn->Callback()({ TaggedValue::FromDouble(2.0), TaggedValue::FromDouble(10.0) });
    assert(res_pow.IsDouble() && res_pow.AsDouble() == 1024.0);

    std::cout << "  -> Builtin Math functions (abs, sqrt, max, pow) verified successfully.\n" << std::flush;
}

void TestMicrotaskQueueCheckpoint() {
    std::cout << "[Test 3] Verifying ECMA-262 Microtask Queue FIFO Checkpoints...\n" << std::flush;

    MicrotaskQueue queue;
    int step = 0;

    queue.Enqueue([&step]() {
        assert(step == 0);
        step = 1;
    });

    queue.Enqueue([&step]() {
        assert(step == 1);
        step = 2;
    });

    queue.Enqueue([&step]() {
        assert(step == 2);
        step = 3;
    });

    assert(queue.Size() == 3);
    queue.PerformMicrotaskCheckpoint();
    assert(queue.Empty());
    assert(step == 3);

    std::cout << "  -> Microtask Queue drained in strict FIFO order across checkpoint.\n" << std::flush;
}

void TestEndToEndScriptEvaluation() {
    std::cout << "[Test 4] Verifying End-to-End ECMAScript Script Evaluation via Agent...\n" << std::flush;

    Agent agent;

    std::string_view js_code =
        "var x = 15;\n"
        "var y = 25;\n"
        "var total = 0;\n"
        "var i = 0;\n"
        "while (i < 5) {\n"
        "    total = total + (x + y);\n"
        "    i = i + 1;\n"
        "}\n"
        "return total;\n";

    TaggedValue result = agent.EvaluateScript(js_code);

    std::cout << "  -> Script executed. Result: " << result.ToString() << "\n" << std::flush;
    // (15 + 25) * 5 = 200
    assert(result.IsSmi());
    assert(result.AsSmi() == 200);
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 6 (ECMAScript Runtime) Tests       \n"
              << "========================================================\n\n" << std::flush;

    TestAgentAndRealmInitialization();
    std::cout << "\n" << std::flush;
    TestMathIntrinsics();
    std::cout << "\n" << std::flush;
    TestMicrotaskQueueCheckpoint();
    std::cout << "\n" << std::flush;
    TestEndToEndScriptEvaluation();

    std::cout << "\n========================================================\n"
              << "   ALL ECMASCRIPT RUNTIME TESTS PASSED (4/4)            \n"
              << "========================================================\n" << std::flush;
    return 0;
}
