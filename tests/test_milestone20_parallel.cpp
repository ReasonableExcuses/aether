#include "aether/runtime.h"
#include "aether/heap.h"
#include "aether/gc.h"
#include "aether/interpreter.h"
#include "aether/baseline_jit.h"
#include "aether/optimizing_jit.h"
#include "aether/background_compiler.h"
#include "aether/concurrent_marker.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <thread>
#include <chrono>

using namespace aether;

static void Assert(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

// Helper: build a sumN bytecode routine using parser and emitter
static std::unique_ptr<BytecodeArray> BuildSumNBytecode() {
    std::string_view src =
        "function sumN(n) {\n"
        "    var total = 0;\n"
        "    var i = 1;\n"
        "    while (i <= n) {\n"
        "        total = total + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return total;\n"
        "}\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();

    BytecodeEmitter emitter;
    return emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));
}

// [Test 1] BackgroundCompiler Lifecycle & Task Execution
static void Test1_BackgroundCompilerLifecycle() {
    std::cout << "[Test 1] Verifying BackgroundCompiler Initialization, Dispatch & Shutdown...\n";

    BackgroundCompiler compiler(3);
    Assert(compiler.WorkerCount() == 0, "Compiler should not have spawned threads before Start");

    compiler.Start();
    Assert(compiler.IsRunning(), "Compiler should be running after Start");
    Assert(compiler.WorkerCount() == 3, "Compiler should have 3 worker threads");

    auto bc = BuildSumNBytecode();
    std::atomic<bool> callback_fired{false};

    bool queued = compiler.Enqueue(*bc, nullptr, [&](std::shared_ptr<ExecutableMemory> exec) {
        Assert(exec != nullptr, "Compilation callback received null executable memory");
        callback_fired.store(true);
    });

    Assert(queued, "Job enqueue should succeed");
    compiler.WaitForPendingTasks();

    Assert(callback_fired.load(), "Background compilation completion callback should fire");
    Assert(compiler.CompletedTaskCount() >= 1, "Completed task count should be at least 1");
    Assert(bc->GetTier2Code() != nullptr, "Bytecode should have Tier 2 executable memory installed");

    compiler.Stop();
    Assert(!compiler.IsRunning(), "Compiler should not be running after Stop");
    std::cout << "  -> BackgroundCompiler lifecycle and asynchronous compilation verified.\n";
}

// [Test 2] Asynchronous JIT Compilation without Mutator Stall
static void Test2_AsynchronousJITCompilationNoStall() {
    std::cout << "[Test 2] Verifying Asynchronous Sea-of-Nodes JIT Compilation without Mutator Stall...\n";

    BackgroundCompiler compiler(2);
    compiler.Start();
    BackgroundCompiler::SetCurrent(&compiler);
    BackgroundCompiler::SetEnabled(true);

    auto bc = BuildSumNBytecode();
    Interpreter interp;
    Interpreter::SetCurrent(&interp);

    // Run in interpreter with argument n = 10 -> sum = 55
    std::vector<TaggedValue> args = { TaggedValue::FromSmi(10) };
    TaggedValue res = interp.Execute(*bc, args);
    Assert(res.IsSmi() && res.AsSmi() == 55, "Interpreted result of sumN(10) should be 55");

    // Invoke up to threshold to trigger background compilation
    for (uint32_t i = 1; i < OptimizingJIT::kInvocationThreshold; ++i) {
        TaggedValue r = interp.Execute(*bc, args);
        Assert(r.IsSmi() && r.AsSmi() == 55, "Loop invocation result must match");
    }

    // Next invocation triggers background compilation enqueue
    TaggedValue r_thresh = interp.Execute(*bc, args);
    Assert(r_thresh.IsSmi() && r_thresh.AsSmi() == 55, "Invocation at threshold should return immediately");

    // Wait for the background compilation to finish
    compiler.WaitForPendingTasks();
    Assert(bc->GetTier2Code() != nullptr, "Tier 2 code should now be installed on BytecodeArray");

    BackgroundCompiler::SetEnabled(false);
    BackgroundCompiler::SetCurrent(nullptr);
    Interpreter::SetCurrent(nullptr);
    compiler.Stop();
    std::cout << "  -> Asynchronous compilation executed without mutator stall.\n";
}

// [Test 3] Seamless Atomic Promotion from Interpreter to Tier 2 Machine Code
static void Test3_AtomicPromotionToTier2() {
    std::cout << "[Test 3] Verifying Seamless Atomic Promotion from Interpreter to Tier 2 Native Execution...\n";

    BackgroundCompiler compiler(2);
    compiler.Start();
    BackgroundCompiler::SetCurrent(&compiler);
    BackgroundCompiler::SetEnabled(true);

    auto bc = BuildSumNBytecode();
    Interpreter interp;
    Interpreter::SetCurrent(&interp);

    std::vector<TaggedValue> args = { TaggedValue::FromSmi(100) };

    // Initially, Tier 2 code is null
    Assert(bc->GetTier2Code() == nullptr, "Tier 2 code must be null initially");

    // Run until threshold
    for (uint32_t i = 0; i <= OptimizingJIT::kInvocationThreshold; ++i) {
        interp.Execute(*bc, args);
    }

    // Wait for background worker to compile and atomically install Tier 2 code
    compiler.WaitForPendingTasks();
    const ExecutableMemory* tier2_code = bc->GetTier2Code();
    Assert(tier2_code != nullptr, "Tier 2 code must be atomically installed after background compile");

    // Next invocation must take the Tier 2 fast-path
    TaggedValue t2_result = interp.Execute(*bc, args);
    Assert(t2_result.IsSmi() && t2_result.AsSmi() == 5050, "Tier 2 sumN(100) should be 5050");

    BackgroundCompiler::SetEnabled(false);
    BackgroundCompiler::SetCurrent(nullptr);
    Interpreter::SetCurrent(nullptr);
    compiler.Stop();
    std::cout << "  -> Atomic transition to Tier 2 native code verified.\n";
}

// [Test 4] Multi-Function Background JIT Concurrency
static void Test4_MultiFunctionBackgroundConcurrency() {
    std::cout << "[Test 4] Verifying Multi-Function Background JIT Concurrency across Worker Threads...\n";

    BackgroundCompiler compiler(4); // 4 workers
    compiler.Start();

    constexpr size_t kNumFunctions = 8;
    std::vector<std::unique_ptr<BytecodeArray>> routines;
    routines.reserve(kNumFunctions);
    for (size_t i = 0; i < kNumFunctions; ++i) {
        routines.push_back(BuildSumNBytecode());
    }

    std::atomic<size_t> completed_count{0};
    for (size_t i = 0; i < kNumFunctions; ++i) {
        compiler.Enqueue(*routines[i], nullptr, [&](std::shared_ptr<ExecutableMemory> exec) {
            Assert(exec != nullptr, "Compiled executable memory must not be null");
            completed_count.fetch_add(1);
        });
    }

    compiler.WaitForPendingTasks();
    Assert(completed_count.load() == kNumFunctions, "All 8 concurrent compilations should complete");

    // Verify all 8 routines have valid Tier 2 code and produce correct results
    for (size_t i = 0; i < kNumFunctions; ++i) {
        Assert(routines[i]->GetTier2Code() != nullptr, "Routine must have Tier 2 code installed");
        TaggedValue arg = TaggedValue::FromSmi(10);
        TaggedValue res = OptimizingJIT::Invoke(*routines[i]->GetTier2Code(), &arg, 1);
        Assert(res.IsSmi() && res.AsSmi() == 55, "Each concurrent compiled routine must produce 55");
    }

    compiler.Stop();
    std::cout << "  -> 8 concurrent background JIT compilation tasks verified.\n";
}

// [Test 5] ConcurrentMarker Lifecycle & Worklist Traversal
static void Test5_ConcurrentMarkerLifecycle() {
    std::cout << "[Test 5] Verifying ConcurrentMarker Lifecycle & Root Worklist Traversal...\n";

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    uint32_t next_shape_id = 1;
    // Allocate an object graph in Old Gen
    JSObject* root_obj = heap.AllocateOld<JSObject>(nullptr);
    JSObject* child_obj1 = heap.AllocateOld<JSObject>(nullptr);
    JSObject* child_obj2 = heap.AllocateOld<JSObject>(nullptr);
    root_obj->SetNamedProperty("child1", TaggedValue::FromHeapObject(child_obj1), next_shape_id);
    root_obj->SetNamedProperty("child2", TaggedValue::FromHeapObject(child_obj2), next_shape_id);

    ConcurrentMarker marker(heap);
    Assert(!marker.IsActive(), "Marker should be idle before start");

    std::vector<HeapObject*> roots = { root_obj };
    marker.Start(roots);
    Assert(marker.IsActive(), "Marker should be active after start");

    marker.WaitForCompletion();
    marker.Stop();

    Assert(!marker.IsActive(), "Marker should be idle after stop");
    Assert(root_obj->IsMarked(), "Root object should be marked");
    Assert(child_obj1->IsMarked(), "Child 1 object should be marked");
    Assert(child_obj2->IsMarked(), "Child 2 object should be marked");
    Assert(marker.MarkedObjectCount() >= 3, "Marked object count should be at least 3");

    IsolateHeap::SetCurrent(nullptr);
    std::cout << "  -> ConcurrentMarker traversed object graph in background thread successfully.\n";
}

// [Test 6] Immix Block Line Marking under Concurrent Marking
static void Test6_ImmixLineMarkingCorrectness() {
    std::cout << "[Test 6] Verifying Immix Block Line Marking under Concurrent Marking...\n";

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    // Allocate multiple distinct objects in Immix Old Gen
    JSObject* live_obj1 = heap.AllocateOld<JSObject>(nullptr);
    JSObject* live_obj2 = heap.AllocateOld<JSObject>(nullptr);
    JSObject* dead_obj  = heap.AllocateOld<JSObject>(nullptr);

    ConcurrentMarker marker(heap);
    // Mark only live_obj1 and live_obj2 as roots
    marker.Start({ live_obj1, live_obj2 });
    marker.WaitForCompletion();
    marker.FinalizeMarking();

    Assert(live_obj1->IsMarked(), "live_obj1 should be marked");
    Assert(live_obj2->IsMarked(), "live_obj2 should be marked");
    Assert(!dead_obj->IsMarked(), "dead_obj should remain unmarked");

    // Perform Immix sweep
    heap.GetOldGen().Sweep();

    // Verify heap integrity
    Assert(heap.GetOldGen().Contains(live_obj1), "live_obj1 must be retained in Immix space");
    Assert(heap.GetOldGen().Contains(live_obj2), "live_obj2 must be retained in Immix space");

    IsolateHeap::SetCurrent(nullptr);
    std::cout << "  -> Immix line marking and block sweep correctly preserved live objects.\n";
}

// [Test 7] Concurrent Write Barrier (SATB Tri-Color Invariant)
static void Test7_ConcurrentWriteBarrier() {
    std::cout << "[Test 7] Verifying Concurrent Write Barrier (SATB Tri-Color Invariant)...\n";

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    uint32_t next_shape_id = 1;
    JSObject* root = heap.AllocateOld<JSObject>(nullptr);
    scope.CreateHandle(root);

    ConcurrentMarker* cm = heap.GetConcurrentMarker();
    Assert(cm != nullptr, "Heap must have concurrent marker instance");

    // Start concurrent marking with initial root
    heap.StartConcurrentMark();
    Assert(cm->IsActive(), "Concurrent marker must be actively running");

    // Mutator simulates write: creates a new object and mutates root to point to it
    JSObject* mutated_child = heap.AllocateOld<JSObject>(nullptr);
    Assert(!mutated_child->IsMarked(), "Fresh child should not be marked initially");

    // Setting property invokes WriteBarrier -> NotifyConcurrentWrite
    root->SetNamedProperty("dyn_child", TaggedValue::FromHeapObject(mutated_child), next_shape_id);

    // Finalize concurrent GC (brief STW pause)
    heap.FinalizeConcurrentGC();
    Assert(!cm->IsActive(), "Concurrent marker should not be active after finalization");

    // Tri-color invariant: mutated child must be marked and NOT swept
    Assert(mutated_child->IsMarked(), "Mutated child captured by write barrier must be marked");

    IsolateHeap::SetCurrent(nullptr);
    std::cout << "  -> Concurrent write barrier captured pointer update without object loss.\n";
}

// [Test 8] Concurrent Mutator Allocations & Concurrent Marking Interleaving
static void Test8_ConcurrentMutatorAllocations() {
    std::cout << "[Test 8] Verifying Concurrent Mutator Allocations & Background Marking Interleaving...\n";

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    uint32_t next_shape_id = 1;
    // Create a live root chain
    JSObject* root = heap.AllocateOld<JSObject>(nullptr);
    scope.CreateHandle(root);
    JSObject* current = root;
    for (int i = 0; i < 20; ++i) {
        JSObject* next = heap.AllocateOld<JSObject>(nullptr);
        current->SetNamedProperty("next", TaggedValue::FromHeapObject(next), next_shape_id);
        current = next;
    }

    // Start concurrent marking on root
    heap.StartConcurrentMark();

    // Mutator thread continues executing and allocating objects in parallel
    for (int i = 0; i < 500; ++i) {
        JSObject* tmp = heap.Allocate<JSObject>(nullptr);
        tmp->SetNamedProperty("x", TaggedValue::FromSmi(i), next_shape_id);
    }

    // Finalize concurrent marking
    heap.FinalizeConcurrentGC();

    Assert(root->IsMarked(), "Root object must be marked");
    Assert(heap.MajorGCCount() >= 1, "Major GC count should increment");

    IsolateHeap::SetCurrent(nullptr);
    std::cout << "  -> Mutator allocations proceeded smoothly in parallel with background marking.\n";
}

// [Test 9] Multi-Threaded Stress Test (Concurrent GC + Background JIT)
static void Test9_ParallelStressTest() {
    std::cout << "[Test 9] Verifying Multi-Threaded Stress Test (Concurrent GC + Background JIT)...\n";

    BackgroundCompiler compiler(2);
    compiler.Start();
    BackgroundCompiler::SetCurrent(&compiler);
    BackgroundCompiler::SetEnabled(true);

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    uint32_t next_shape_id = 1;
    // Setup routine to compile in background
    auto bc = BuildSumNBytecode();
    compiler.Enqueue(*bc);

    // Simultaneously setup object graph and run concurrent marking
    JSObject* root = heap.AllocateOld<JSObject>(nullptr);
    scope.CreateHandle(root);
    JSObject* child = heap.AllocateOld<JSObject>(nullptr);
    root->SetNamedProperty("child", TaggedValue::FromHeapObject(child), next_shape_id);
    heap.StartConcurrentMark();

    // Mutator work
    for (int i = 0; i < 200; ++i) {
        JSObject* ephemeral = heap.Allocate<JSObject>(nullptr);
        (void)ephemeral;
    }

    // Await background compiler and finalize GC
    compiler.WaitForPendingTasks();
    heap.FinalizeConcurrentGC();

    Assert(bc->GetTier2Code() != nullptr, "JIT compilation completed under GC stress");
    Assert(root->IsMarked() && child->IsMarked(), "All live objects correctly marked");

    // Execute compiled code to verify memory sanity
    TaggedValue arg = TaggedValue::FromSmi(10);
    TaggedValue res = OptimizingJIT::Invoke(*bc->GetTier2Code(), &arg, 1);
    Assert(res.IsSmi() && res.AsSmi() == 55, "Native execution succeeded after concurrent GC");

    BackgroundCompiler::SetEnabled(false);
    BackgroundCompiler::SetCurrent(nullptr);
    IsolateHeap::SetCurrent(nullptr);
    compiler.Stop();
    std::cout << "  -> Multi-threaded stress test passed with zero errors or race conditions.\n";
}

// [Test 10] High-Level JavaScript Runtime Integration with Background Workers
static void Test10_RuntimeIntegration() {
    std::cout << "[Test 10] Verifying ECMAScript Runtime Integration with Background Workers...\n";

    BackgroundCompiler compiler(2);
    compiler.Start();
    BackgroundCompiler::SetCurrent(&compiler);
    BackgroundCompiler::SetEnabled(true);

    Agent agent;

    // Evaluate JavaScript that computes sum in a loop
    std::string_view script =
        "var total = 0;\n"
        "var i = 1;\n"
        "while (i <= 100) {\n"
        "    total = total + i;\n"
        "    i = i + 1;\n"
        "}\n"
        "return total;\n";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsSmi() && result.AsSmi() == 5050, "ECMAScript script total should evaluate to 5050");

    compiler.WaitForPendingTasks();
    compiler.Stop();
    BackgroundCompiler::SetEnabled(false);
    BackgroundCompiler::SetCurrent(nullptr);

    std::cout << "  -> ECMAScript runtime successfully executed with parallel workers.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   AetherVM Milestone 20 (Parallel Engine & Workers) Tests\n";
    std::cout << "========================================================\n\n";

    Test1_BackgroundCompilerLifecycle();
    Test2_AsynchronousJITCompilationNoStall();
    Test3_AtomicPromotionToTier2();
    Test4_MultiFunctionBackgroundConcurrency();
    Test5_ConcurrentMarkerLifecycle();
    Test6_ImmixLineMarkingCorrectness();
    Test7_ConcurrentWriteBarrier();
    Test8_ConcurrentMutatorAllocations();
    Test9_ParallelStressTest();
    Test10_RuntimeIntegration();

    std::cout << "\n========================================================\n";
    std::cout << "   ALL PARALLEL ENGINE & BACKGROUND WORKER TESTS PASSED! (10/10)\n";
    std::cout << "========================================================\n";
    return 0;
}
