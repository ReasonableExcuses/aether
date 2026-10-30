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

    // Manually clear the card and remembered status to simulate a missing write barrier
    old_obj->SetRemembered(false);
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
    Assert(sizeof(HeapObject) == 24, "sizeof(HeapObject) must be 24 bytes (8B vptr + 8B shape + 2B type + 2B gc_metadata + 4B size)");

    Agent agent;
    HandleScope scope(agent.GetHeap());
    auto* obj = agent.GetHeap().Allocate<JSObject>(nullptr);

    // Initial state
    Assert(!obj->IsRemembered(), "Object initially not remembered");
    Assert(!obj->IsPinned(), "Object initially not pinned");
    Assert(obj->IsCharged(), "Initial young object marked through isolate heap");
    Assert(obj->GetAge() == 0, "Object initially age 0");
    Assert(obj->GetAllocatedSize() >= sizeof(JSObject), "Allocated size should cover sizeof(JSObject)");

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

    // Charged bit
    obj->SetCharged(true);
    Assert(obj->IsCharged(), "Object is charged after SetCharged(true)");
    obj->SetCharged(false);
    Assert(!obj->IsCharged(), "Object not charged after SetCharged(false)");

    // Age increments without corrupting remembered, pinned, or charged flags
    obj->SetRemembered(true);
    obj->SetPinned(true);
    obj->SetCharged(true);
    for (int i = 0; i < 20; ++i) {
        obj->IncrementAge();
    }
    Assert(obj->GetAge() == 15, "Max age should cap at 15");
    Assert(obj->IsRemembered(), "Remembered flag must remain intact during age increments");
    Assert(obj->IsPinned(), "Pinned flag must remain intact during age increments");
    Assert(obj->IsCharged(), "Charged flag must remain intact during age increments");
}

void TestImmixBlockAllocatorLayout() {
    std::cout << "[Test] Immix 32KB-Aligned Block Allocator Layout (T1.2)..." << std::endl;
    Assert(sizeof(ImmixBlock) == kImmixBlockSize, "sizeof(ImmixBlock) must be exactly 32 KB");
    Assert(offsetof(ImmixBlock, payload) == kHeaderSize, "Payload must start at offset 1024 (Line 8)");

    Agent agent;
    auto& heap = agent.GetHeap();
    auto& old_gen = heap.GetOldGen();

    // Allocate an object in old gen
    auto* old_obj = heap.AllocateOld<JSObject>(nullptr);
    Assert(old_obj != nullptr, "old_obj allocation must succeed");
    Assert(old_gen.Contains(old_obj), "old_gen must contain old_obj");

    // Verify 32KB alignment and BlockFor
    ImmixBlock* block = ImmixBlock::BlockFor(old_obj);
    Assert(block != nullptr, "BlockFor must not be null");
    Assert((reinterpret_cast<uintptr_t>(block) & (kImmixBlockSize - 1)) == 0, "Block must be 32 KB aligned");
    Assert(block->Contains(old_obj), "Block must contain old_obj");
    Assert(block->ContainsPayload(old_obj), "Block payload must contain old_obj");

    // Verify Object-Start Bitmap
    Assert(block->IsObjectStart(old_obj), "Object-start bit must be set for old_obj");
    Assert(old_gen.IsValidObjectStart(old_obj), "IsValidObjectStart must return true for old_obj");

    // Interior pointer should not be a valid object start
    void* interior_ptr = reinterpret_cast<uint8_t*>(old_obj) + 8;
    Assert(!block->IsObjectStart(interior_ptr), "Interior pointer must not be marked as object start");
    Assert(!old_gen.IsValidObjectStart(interior_ptr), "IsValidObjectStart must return false for interior pointer");

    // Verify ForEachObject visits old_obj
    bool found = false;
    block->ForEachObject([&](HeapObject* o) {
        if (o == old_obj) found = true;
    });
    Assert(found, "ForEachObject must find old_obj");

    // Verify O(1) line calculation
    size_t line = block->LineFor(old_obj);
    Assert(line >= kHeaderLines && line < kLinesPerBlock, "Object line must be in payload lines");
    Assert(block->line_allocated.test(line), "Object's line must be marked allocated");
}

void TestMarkingEngine() {
    std::cout << "[Test] Marking Engine Rewrite & SlotVisitor (T1.3)..." << std::endl;
    Agent agent;
    auto& heap = agent.GetHeap();
    auto& old_gen = heap.GetOldGen();

    // 1. Verify SlotVisitor virtual dispatch
    class CountingSlotVisitor final : public SlotVisitor {
    public:
        size_t count{0};
        void Visit(TaggedValue* slot) override {
            if (slot && slot->IsHeapObject()) ++count;
        }
    };

    CountingSlotVisitor counter;
    auto* arr = heap.AllocateOld<JSArray>(nullptr);
    arr->SetLength(3);
    arr->SetElement(0, TaggedValue::FromHeapObject(heap.AllocateOld<JSObject>(nullptr)));
    arr->SetElement(1, TaggedValue::FromHeapObject(heap.AllocateOld<JSObject>(nullptr)));
    arr->SetElement(2, TaggedValue::FromHeapObject(heap.AllocateOld<JSObject>(nullptr)));
    arr->IteratePointers(&counter);
    Assert(counter.count >= 3, "SlotVisitor must visit all array element slots");

    // 2. Verify O(1) Immix line marking directly via block header
    old_gen.ClearMarks();
    ImmixBlock* block = ImmixBlock::BlockFor(arr);
    size_t line = block->LineFor(arr);
    Assert(block->line_marks[line] == 0, "Line marks must be 0 after ClearMarks()");
    old_gen.MarkObject(arr);
    Assert(block->line_marks[line] == 1, "Line marks must be 1 after MarkObject()");

    // 3. Verify Header Mark Bits
    Assert(!arr->IsMarked(), "Object mark bit was not set by Immix line mark alone");
    arr->SetMarked(true);
    Assert(arr->IsMarked(), "SetMarked(true) must set kMarkBit");
    arr->SetMarked(false);
    Assert(!arr->IsMarked(), "SetMarked(false) must clear kMarkBit");

    // 4. Performance & Scalability Test on dense graph: 200,000 objects
    std::cout << "  -> Constructing 200,000-object linked chain in old gen..." << std::endl;
    HandleScope scope(heap);
    auto head = scope.CreateHandle(heap.AllocateOld<JSArray>(nullptr));
    head->SetLength(1);
    JSArray* curr = head.Get();
    for (int i = 0; i < 200000; ++i) {
        auto* next_arr = heap.AllocateOld<JSArray>(nullptr);
        next_arr->SetLength(1);
        curr->SetElement(0, TaggedValue::FromHeapObject(next_arr));
        curr = next_arr;
    }

    std::cout << "  -> Measuring 200,000-object full heap mark time..." << std::endl;
    auto start = std::chrono::steady_clock::now();
    size_t marked_count = heap.MarkFromRoots({head.Get()});
    auto end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "  -> Marked " << marked_count << " objects in " << elapsed_ms << " ms ("
              << (elapsed_ms * 1000.0 / marked_count) << " us/object)" << std::endl;
    Assert(marked_count >= 200001, "All 200,001 objects in chain must be marked");
    // Marking 200K objects with O(1) mark bits and mark stack should easily take < 50ms (typically ~10ms)
    Assert(elapsed_ms < 500.0, "Mark time for 200K objects must be under 500ms");
}

void TestConservativeStackScanning() {
    std::cout << "[Test] Conservative Stack Scanning (D2 / T1.4)..." << std::endl;
    Agent agent;
    auto& heap = agent.GetHeap();
    uint32_t shape_id = 1;

    // 1. Verify IsValidObjectStart
    auto* obj1 = heap.AllocateOld<JSObject>(nullptr);
    Assert(heap.IsValidObjectStart(obj1), "heap.IsValidObjectStart must return true for valid object start");
    void* interior = reinterpret_cast<uint8_t*>(obj1) + 8;
    Assert(!heap.IsValidObjectStart(interior), "heap.IsValidObjectStart must return false for interior pointer");
    Assert(!heap.IsValidObjectStart(nullptr), "heap.IsValidObjectStart must return false for nullptr");
    Assert(!heap.IsValidObjectStart(reinterpret_cast<void*>(0x12345)), "heap.IsValidObjectStart must return false for random address");

    // 2. Set stack base to current frame marker to enable conservative stack scan
    volatile int stack_marker = 0;
    heap.SetStackBase(const_cast<int*>(&stack_marker));

    // Allocate an object in C++ local variable without any HandleScope or extra roots
    JSObject* raw_local_obj = heap.AllocateOld<JSObject>(nullptr);
    raw_local_obj->SetNamedProperty("magic_val", TaggedValue::FromSmi(1234), shape_id);

    // Trigger Major GC
    heap.MajorGC();

    // Verify raw_local_obj was pinned and marked by the conservative stack scan
    Assert(raw_local_obj->IsPinned(), "raw_local_obj on the stack must be pinned by conservative stack scan");
    Assert(raw_local_obj->IsMarked(), "raw_local_obj on the stack must be marked");
    Assert(heap.GetOldGen().Contains(raw_local_obj), "raw_local_obj must be retained in old gen");
    Assert(raw_local_obj->GetNamedProperty("magic_val").AsSmi() == 1234, "Property value must be preserved");

    // 3. Test unrooted TaggedValue on the native stack
    JSObject* raw_local_obj2 = heap.AllocateOld<JSObject>(nullptr);
    TaggedValue tagged_stack_val = TaggedValue::FromHeapObject(raw_local_obj2);

    heap.MajorGC();

    Assert(raw_local_obj2->IsPinned(), "Object referenced by TaggedValue on stack must be pinned");
    Assert(raw_local_obj2->IsMarked(), "Object referenced by TaggedValue on stack must be marked");
    Assert(heap.GetOldGen().Contains(raw_local_obj2), "Object referenced by TaggedValue must survive");

    // 4. Verify Interpreter execution works without op_roots.Clear() or safepoint SnapshotState
    TaggedValue res = agent.EvaluateScript(R"(
        var sum = 0;
        for (var i = 0; i < 500; i = i + 1) {
            var temp = "iteration-" + i;
            sum = sum + i;
        }
        sum;
    )");
    Assert(res.IsSmi() && res.AsSmi() == 124750, "Script with safepoints and temporary strings must produce 124750");

    heap.SetStackBase(nullptr);
    std::cout << "  -> Conservative stack scanning, raw stack rooting, and dispatch loop verified." << std::endl;
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
        TestImmixBlockAllocatorLayout();
        TestMarkingEngine();
        TestConservativeStackScanning();
        std::cout << "All flag, heap verifier, and object header tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
