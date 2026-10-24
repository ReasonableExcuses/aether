#undef NDEBUG
#include <cassert>
#include "aether/runtime.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace aether;

namespace {
std::unique_ptr<BytecodeArray> Compile(std::string_view source) {
    Lexer lexer(source);
    Parser parser(lexer);
    auto ast = parser.ParseProgram();
    BytecodeEmitter emitter;
    return emitter.Emit(ast.get());
}

struct Fixture {
    Heap heap{4096};
    IsolateScope isolate{heap};
    Interpreter interpreter;
    Fixture() { interpreter.InitializeResourceExceptions(); }
    TaggedValue Run(const BytecodeArray& code, const std::vector<TaggedValue>& args = {},
                    TaggedValue receiver = TaggedValue::Undefined()) {
        interpreter.ClearPendingException();
        auto result = interpreter.Execute(code, args, receiver);
        if (interpreter.HasPendingException()) {
            std::cerr << "Unexpected JS exception: " << result.ToString() << '\n';
        }
        assert(!interpreter.HasPendingException());
        return result;
    }
    void SmallBudget() {
        const auto live = heap.TotalAllocatedBytes();
        heap.SetHeapBudget(std::max(live * 2, live + 64 * 1024));
    }
};

void TestRecurringCollectionAndClosures() {
    Fixture f;
    auto code = Compile(R"(
        var marker = "environment-marker";
        function keepEnvironment() { return marker; }
        function make(seed) {
            var saved = "retained-" + seed;
            return function(n) {
                var i = 0;
                while (i < n) {
                    var garbage = "garbage-payload-" + i;
                    i = i + 1;
                }
                try { throw saved; } catch (error) { return error; }
            };
        }
        var closure = make(42);
        closure(4000);
    )");
    f.SmallBudget();
    auto result = f.Run(*code);
    assert(result.ToString() == "retained-42");
    assert(f.heap.MajorGCCount() >= 3);
    assert(f.heap.TotalAllocatedBytes() < f.heap.HeapBudget());
    auto before = f.heap.TotalAllocatedBytes();
    assert(f.heap.CollectAtSafepoint());
    assert(f.heap.TotalAllocatedBytes() <= before);
    // The most recent result and environment are also live between invocations.
    assert(result.ToString() == "retained-42");
    assert(f.interpreter.GetLastEnvironment() != nullptr);
    assert(f.interpreter.GetLastEnvironment()->Get(0, code->GetContextSlots().at("marker")).ToString() == "environment-marker");
}

void TestNestedCoercionReturnRootsAndLiterals() {
    Fixture f;
    auto code = Compile(R"(
        var left = { valueOf: function() { return "left-" + 7; } };
        var right = { valueOf: function() {
            var i = 0;
            while (i < 3000) { var junk = "temporary-" + i; i = i + 1; }
            return "-right";
        } };
        var answer = left + right;
        function unusedUntilAfterGC() { return "bytecode-literal"; }
        answer + unusedUntilAfterGC();
    )");
    f.SmallBudget();
    assert(f.Run(*code).ToString() == "left-7-rightbytecode-literal");
    assert(f.heap.MajorGCCount() >= 2);
}

void TestThisWithAndNewTargetRoots() {
    Fixture f;
    auto code = Compile(R"(
        var i = 0;
        while (i < 2000) { var junk = "garbage-" + i; i = i + 1; }
        var result;
        with (this) { result = label; }
        result + new.target;
    )");
    auto* receiver = AllocateRuntime<JSObject>(f.interpreter.GetRootShape());
    uint32_t sid = 1;
    receiver->SetNamedProperty("label", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("receiver-")), sid);
    f.interpreter.SetNewTarget(TaggedValue::FromHeapObject(AllocateRuntime<JSString>("target")));
    f.SmallBudget();
    assert(f.Run(*code, {}, TaggedValue::FromHeapObject(receiver)).ToString() == "receiver-target");
    assert(f.heap.MajorGCCount() > 0);
}

void TestNativeAndDirectRecursion() {
    Fixture f;
    auto direct = Compile(R"(
        function recurse() { return recurse(); }
        var caught = "no";
        try { recurse(); } catch (error) { caught = error.name; }
        caught;
    )");
    assert(f.Run(*direct).ToString() == "RangeError");

    auto factory = Compile(R"(
        (function(bounce) {
            function recurse() { return bounce(recurse); }
            try { recurse(); } catch (error) { return error.name; }
            return "not caught";
        });
    )");
    auto function = f.Run(*factory);
    HandleScope roots(f.heap);
    roots.CreateHandle(function);
    auto* bounce = AllocateRuntime<JSNativeFunction>("bounce", [](const std::vector<TaggedValue>& args) {
        return CallFunction(args[0], TaggedValue::Undefined(), {});
    });
    roots.CreateHandle(bounce);
    assert(f.Run(*function.As<JSFunction>()->GetBytecode(), {TaggedValue::FromHeapObject(bounce)}).ToString() == "RangeError");
    // Unwinding must restore the accumulator, depth and interpreter state.
    auto after = Compile("21 + 21;");
    assert(f.Run(*after).AsSmi() == 42);
}

void TestGeneratorRootsAndHandlerEnvironment() {
    Fixture f;
    auto factory = Compile(R"(
        (function*() {
            var retained = "generator-" + 42;
            function capture() { return retained; }
            try {
                yield capture();
                var i = 0;
                while (i < 2500) { var junk = "discard-" + i; i = i + 1; }
                throw capture();
            } catch (error) { return error; }
        });
    )");
    auto function = f.Run(*factory);
    HandleScope roots(f.heap);
    roots.CreateHandle(function);
    auto* gen = AllocateRuntime<JSGeneratorObject>(function.As<JSFunction>(), std::vector<TaggedValue>{},
        TaggedValue::Undefined(), function.As<JSFunction>()->GetEnvironment(), f.interpreter.GetRootShape());
    roots.CreateHandle(gen);
    f.SmallBudget();
    auto first = f.interpreter.ResumeGenerator(gen, TaggedValue::Undefined());
    assert(!f.interpreter.HasPendingException());
    assert(first.As<JSObject>()->GetNamedProperty("value").ToString() == "generator-42");
    assert(f.heap.CollectAtSafepoint());
    auto last = f.interpreter.ResumeGenerator(gen, TaggedValue::Undefined());
    assert(!f.interpreter.HasPendingException());
    assert(last.As<JSObject>()->GetNamedProperty("value").ToString() == "generator-42");
    assert(last.As<JSObject>()->GetNamedProperty("done").ToBoolean());
    assert(f.heap.MajorGCCount() >= 2);
}

void TestCatchableArrayAndNativeResourceErrors() {
    Fixture f;
    auto code = Compile(R"(
        var count = 0;
        var array = [];
        try { array[268435456] = 1; } catch (error) { if (error.name === "RangeError") count = count + 1; }
        try { array.length = -1; } catch (error) { if (error.name === "RangeError") count = count + 1; }
        count;
    )");
    assert(f.Run(*code).AsSmi() == 2);

    auto factory = Compile(R"(
        (function(fail) {
            try { fail(); } catch (error) { return error.name; }
            return "not caught";
        });
    )");
    auto function = f.Run(*factory);
    HandleScope roots(f.heap);
    roots.CreateHandle(function);
    for (bool bad_alloc : {false, true}) {
        auto* fail = AllocateRuntime<JSNativeFunction>("fail", [bad_alloc](const std::vector<TaggedValue>&) -> TaggedValue {
            if (bad_alloc) throw std::bad_alloc();
            throw std::length_error("oversized native result");
        });
        roots.CreateHandle(fail);
        assert(f.Run(*function.As<JSFunction>()->GetBytecode(), {TaggedValue::FromHeapObject(fail)}).ToString() == "RangeError");
    }
}

void TestFullBudgetPropertyError() {
    Fixture f;
    auto factory = Compile(R"(
        (function(object) {
            try { object.additionalProperty = 42; } catch (error) { return error; }
            return null;
        });
    )");
    auto function = f.Run(*factory);
    HandleScope roots(f.heap);
    roots.CreateHandle(function);
    auto* object = AllocateRuntime<JSObject>(f.interpreter.GetRootShape());
    roots.CreateHandle(object);
    const auto budget = f.heap.HeapBudget();
    f.heap.SetHeapBudget(f.heap.TotalAllocatedBytes());
    const auto collections = f.heap.MajorGCCount();
    {
        // Even explicit safepoints must respect a host's unpublished temporaries.
        NoGCScope no_gc(f.heap);
        auto result = f.Run(*function.As<JSFunction>()->GetBytecode(), {TaggedValue::FromHeapObject(object)});
        assert(result.IsHeapObject() && result.AsHeapObject()->IsError());
        assert(result.As<JSError>()->Name() == "RangeError");
        assert(!f.heap.CollectAtSafepoint());
    }
    assert(f.heap.MajorGCCount() == collections);
    f.heap.SetHeapBudget(budget);
    assert(!object->HasOwnProperty("additionalProperty"));
}

void TestBigIntManagedConversions() {
    Fixture f;
    auto code = Compile(R"(
        var i = 0;
        var ok = true;
        while (i < 500) {
            ok = ok && 42n == 42 && 42n == "42" && 41n < "42" && "41" < 42n && 42n < 42.5;
            i = i + 1;
        }
        ok;
    )");
    f.SmallBudget();
    assert(f.Run(*code).ToBoolean());
    assert(f.heap.MajorGCCount() > 0);
}
} // namespace

int main() {
    std::cout << "Recurring collection and closures\n" << std::flush;
    TestRecurringCollectionAndClosures();
    std::cout << "Nested coercions and literals\n" << std::flush;
    TestNestedCoercionReturnRootsAndLiterals();
    std::cout << "This, with and new.target\n" << std::flush;
    TestThisWithAndNewTargetRoots();
    std::cout << "Direct and native recursion\n" << std::flush;
    TestNativeAndDirectRecursion();
    std::cout << "Generator roots\n" << std::flush;
    TestGeneratorRootsAndHandlerEnvironment();
    std::cout << "Catchable resource errors\n" << std::flush;
    TestCatchableArrayAndNativeResourceErrors();
    std::cout << "Full-budget property error\n" << std::flush;
    TestFullBudgetPropertyError();
    std::cout << "Managed BigInt conversions\n" << std::flush;
    TestBigIntManagedConversions();
    std::cout << "Hardening execution tests passed (8 groups).\n";
}
