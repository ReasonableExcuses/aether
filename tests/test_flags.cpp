#include "aether/options.h"
#include "aether/heap.h"
#include "aether/heap_verifier.h"
#include "aether/runtime.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <stdexcept>

using namespace aether;

static void Assert(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "Assertion failed: " << msg << std::endl;
        throw std::runtime_error(msg);
    }
}

void TestFlagParsing() {
    std::cout << "[Test] Flag Parsing..." << std::endl;
    // Reset options
    Options::Get() = Options();

    std::vector<std::string> args = {
        "aether.exe",
        "--gc-stress=5",
        "--gc-verify",
        "--trace-gc",
        "--gc-at-safepoints",
        "--max-heap=64",
        "--interpreter-only",
        "--always-baseline",
        "--always-optimize",
        "--tier-threshold=100",
        "--stack-size=1024",
        "script.js",
        "arg1",
        "arg2"
    };

    Options::Get().Parse(args);

    Assert(Options::Get().gc_stress == 5, "gc_stress should be 5");
    Assert(Options::Get().gc_verify == true, "gc_verify should be true");
    Assert(Options::Get().trace_gc == true, "trace_gc should be true");
    Assert(Options::Get().gc_at_safepoints == true, "gc_at_safepoints should be true");
    Assert(Options::Get().max_heap_mb == 64, "max_heap_mb should be 64");
    Assert(Options::Get().interpreter_only == true, "interpreter_only should be true");
    Assert(Options::Get().always_baseline == true, "always_baseline should be true");
    Assert(Options::Get().always_optimize == true, "always_optimize should be true");
    Assert(Options::Get().tier_threshold == 100, "tier_threshold should be 100");
    Assert(Options::Get().stack_size_kb == 1024, "stack_size_kb should be 1024");

    // Script arguments should remain intact
    Assert(args.size() == 4, "Remaining args should have 4 elements");
    Assert(args[0] == "aether.exe", "args[0] should be aether.exe");
    Assert(args[1] == "script.js", "args[1] should be script.js");
    Assert(args[2] == "arg1", "args[2] should be arg1");
    Assert(args[3] == "arg2", "args[3] should be arg2");

    // Test default --gc-stress (without =N)
    Options::Get() = Options();
    std::vector<std::string> stress_default = {"aether.exe", "--gc-stress", "main.js"};
    Options::Get().Parse(stress_default);
    Assert(Options::Get().gc_stress == 1, "gc_stress without value should default to 1");
    Assert(stress_default.size() == 2, "stress_default remaining args count");
}

void TestGCStress() {
    std::cout << "[Test] GC Stress Execution..." << std::endl;
    Options::Get() = Options();
    Options::Get().gc_stress = 1; // collect every allocation

    Agent agent;
    HandleScope scope(agent.GetHeap());
    auto* obj = agent.GetHeap().Allocate<JSObject>(nullptr);
    auto h_obj = scope.CreateHandle(obj);

    uint32_t sid = 1;
    for (int i = 0; i < 50; ++i) {
        auto* str = agent.GetHeap().Allocate<JSString>("item_" + std::to_string(i));
        h_obj->SetNamedProperty("prop_" + std::to_string(i), TaggedValue::FromHeapObject(str), sid);
    }

    Assert(h_obj->GetNamedProperty("prop_49").IsString(), "prop_49 must be valid string under gc-stress");
    Options::Get() = Options();
}

void TestInterpreterOnly() {
    std::cout << "[Test] Interpreter Only..." << std::endl;
    Options::Get() = Options();
    Options::Get().interpreter_only = true;

    Agent agent;
    TaggedValue res = agent.EvaluateScript("let sum = 0; for (let i = 0; i < 200; i++) sum += i; sum;");
    Assert(res.IsNumber() && res.AsNumber() == 19900, "Sum calculation under interpreter-only");
    Options::Get() = Options();
}

void TestHeapVerifierPassesOnValidHeap() {
    std::cout << "[Test] Heap Verifier Valid Heap..." << std::endl;
    Options::Get() = Options();
    Options::Get().gc_verify = true;

    Agent agent;
    HandleScope scope(agent.GetHeap());
    auto* obj = agent.GetHeap().Allocate<JSObject>(nullptr);
    auto h_obj = scope.CreateHandle(obj);
    auto* child = agent.GetHeap().Allocate<JSObject>(nullptr);
    uint32_t sid = 1;
    h_obj->SetNamedProperty("child", TaggedValue::FromHeapObject(child), sid);

    // Verify heap manually
    HeapVerifier::VerifyHeap(agent.GetHeap(), "Unit_Test_Valid");
    Options::Get() = Options();
}

void TestHeapVerifierCatchesPoison() {
    std::cout << "[Test] Heap Verifier Poison Detection..." << std::endl;
    Agent agent;
    HandleScope scope(agent.GetHeap());

    // Create an object
    auto* obj = agent.GetHeap().Allocate<JSObject>(nullptr);
    auto h_obj = scope.CreateHandle(obj);

    // Intentionally corrupt memory to 0xDD poison bytes
    uint64_t* raw = reinterpret_cast<uint64_t*>(obj);
    uint64_t backup = *raw;
    *raw = 0xDDDDDDDDDDDDDDDDULL;

    bool caught = false;
    try {
        HeapVerifier::VerifyHeap(agent.GetHeap(), "Unit_Test_Poison");
    } catch (const std::runtime_error& err) {
        caught = true;
        std::string msg = err.what();
        Assert(msg.find("poisoned object") != std::string::npos, "Should mention poisoned object");
    }

    // Restore memory so destructor / heap cleanup doesn't explode
    *raw = backup;
    Assert(caught, "HeapVerifier must catch poisoned object");
}

void TestHeapVerifierCatchesUnrememberedPointer() {
    std::cout << "[Test] Heap Verifier Unremembered Cross-Generational Pointer..." << std::endl;
    Agent agent;
    HandleScope scope(agent.GetHeap());

    // Allocate an object in old gen
    auto* old_obj = agent.GetHeap().AllocateOld<JSObject>(nullptr);
    auto h_old = scope.CreateHandle(old_obj);
    Assert(old_obj->IsOld(), "old_obj must be in old generation");

    // Allocate an object in young gen
    auto* young_obj = agent.GetHeap().Allocate<JSObject>(nullptr);
    auto h_young = scope.CreateHandle(young_obj);
    Assert(young_obj->IsYoung(), "young_obj must be in young generation");

    // Link old_obj -> young_obj (this will mark card)
    uint32_t sid = 1;
    old_obj->SetNamedProperty("leak", TaggedValue::FromHeapObject(young_obj), sid);

    // Manually clear the card to simulate a missing write barrier
    agent.GetHeap().GetCardTable().ClearCard(old_obj);

    bool caught = false;
    try {
        HeapVerifier::VerifyHeap(agent.GetHeap(), "Unit_Test_Unremembered");
    } catch (const std::runtime_error& err) {
        caught = true;
        std::string msg = err.what();
        Assert(msg.find("Unremembered cross-generational pointer") != std::string::npos,
               "Should mention unremembered cross-generational pointer");
    }

    Assert(caught, "HeapVerifier must catch missing write barrier / unremembered card");

    // Clean up link so isolate shutdown is clean
    old_obj->SetNamedProperty("leak", TaggedValue::Undefined(), sid);
}

void TestObjectHeaderLayout() {
    std::cout << "[Test] Object Header Layout (T1.1)..." << std::endl;
    Assert(sizeof(HeapObject) == 24, "sizeof(HeapObject) must be 24 bytes (8B vptr + 8B shape + 2B type + 2B gc_metadata + 4B pad)");

    Agent agent;
    HandleScope scope(agent.GetHeap());
    auto* obj = agent.GetHeap().Allocate<JSObject>(nullptr);

    // Initial state
    Assert(!obj->IsRemembered(), "Object initially not remembered");
    Assert(!obj->IsPinned(), "Object initially not pinned");
    Assert(obj->GetAge() == 0, "Object initially age 0");

    // Remembered bit
    obj->SetRemembered(true);
    Assert(obj->IsRemembered(), "Object is remembered after SetRemembered(true)");
    obj->SetRemembered(false);
    Assert(!obj->IsRemembered(), "Object not remembered after SetRemembered(false)");

    // Pinned bit
    obj->SetPinned(true);
    Assert(obj->IsPinned(), "Object is pinned after SetPinned(true)");
    obj->SetPinned(false);
    Assert(!obj->IsPinned(), "Object not pinned after SetPinned(false)");

    // Age increments without corrupting remembered or pinned flags
    obj->SetRemembered(true);
    obj->SetPinned(true);
    for (int i = 0; i < 20; ++i) {
        obj->IncrementAge();
    }
    Assert(obj->GetAge() == 15, "Max age should cap at 15");
    Assert(obj->IsRemembered(), "Remembered flag must remain intact during age increments");
    Assert(obj->IsPinned(), "Pinned flag must remain intact during age increments");
}

int main() {
    try {
        TestFlagParsing();
        TestGCStress();
        TestInterpreterOnly();
        TestHeapVerifierPassesOnValidHeap();
        TestHeapVerifierCatchesPoison();
        TestHeapVerifierCatchesUnrememberedPointer();
        TestObjectHeaderLayout();
        std::cout << "All flag, heap verifier, and object header tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
