#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include "aether/heap_object.h"
#include "aether/heap.h"
#include "aether/bytecode.h"
#include "aether/runtime.h"
#include "aether/options.h"
#include "aether/assembler.h"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
static size_t GetProcessRSS() {
    PROCESS_MEMORY_COUNTERS info;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        return info.WorkingSetSize;
    }
    return 0;
}
#else
#include <unistd.h>
#include <sys/resource.h>
static size_t GetProcessRSS() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss * 1024;
    }
    return 0;
}
#endif

using namespace aether;

void TestWeakShapeTransitions() {
    std::cout << "[Test 1] Verifying Weak Shape Transitions & Pruning...\n" << std::flush;

    Options::Get().gc_verify = false;
    Agent agent;
    IsolateHeap& heap = agent.GetHeap();
    Realm& realm = agent.GetRealm();

    Shape* root = realm.GetRootShape();
    assert(root != nullptr);
    size_t initial_shapes = root->TotalTreeShapeCount();
    std::cout << "  -> Initial tree shape count: " << initial_shapes << "\n" << std::flush;

    uint32_t next_shape_id = 100;
    {
        HandleScope scope(heap);
        constexpr size_t kNumShapes = 1000;
        for (size_t i = 0; i < kNumShapes; ++i) {
            auto* obj = heap.Allocate<JSObject>(root);
            std::string prop = "prop_" + std::to_string(i);
            obj->SetNamedProperty(prop, TaggedValue::FromSmi(static_cast<int32_t>(i)), next_shape_id);
        }

        size_t expanded_shapes = root->TotalTreeShapeCount();
        std::cout << "  -> Tree shape count with 1000 temporary objects: " << expanded_shapes << "\n" << std::flush;
        assert(expanded_shapes >= initial_shapes + kNumShapes);
    }

    // All temporary objects are out of scope and dead.
    // MajorGC must prune dead shape transitions.
    heap.MajorGC();

    size_t pruned_shapes = root->TotalTreeShapeCount();
    std::cout << "  -> Tree shape count after MajorGC: " << pruned_shapes << "\n" << std::flush;
    assert(pruned_shapes <= initial_shapes);
    std::cout << "  -> PASSED: All dead shape transitions pruned successfully.\n" << std::flush;
}

void TestICCachePruningOnDeadShape() {
    std::cout << "[Test 2] Verifying IC Cache Reset on Pruned Dead Shapes...\n" << std::flush;

    Agent agent;
    IsolateHeap& heap = agent.GetHeap();
    Realm& realm = agent.GetRealm();

    Shape* root = realm.GetRootShape();
    uint32_t next_shape_id = 5000;

    auto bc = std::make_shared<BytecodeArray>(2, 0);
    FeedbackSlot& slot = bc->GetFeedbackVector().GetSlot(1);

    Shape* dead_shape = nullptr;
    {
        HandleScope scope(heap);
        auto* obj = heap.Allocate<JSObject>(root);
        obj->SetNamedProperty("transient_field", TaggedValue::FromSmi(42), next_shape_id);
        dead_shape = obj->GetShape();
        assert(dead_shape != nullptr && dead_shape != root);

        // Populate IC slot with transient shape
        slot.state = FeedbackSlot::State::Monomorphic;
        slot.cached_shape = dead_shape;
        slot.cached_slot = 0;
        slot.cached_in_object = true;
    }

    // Register a function using this bytecode in the heap
    HandleScope func_scope(heap);
    auto* fn = heap.Allocate<JSFunction>("test_ic_func", bc, nullptr, root);
    func_scope.CreateHandle(fn);

    // MajorGC prunes dead_shape
    heap.MajorGC();

    // The slot must be reset to Uninitialized because cached_shape was dead
    assert(slot.state == FeedbackSlot::State::Uninitialized);
    assert(slot.cached_shape == nullptr);
    std::cout << "  -> PASSED: IC slot safely reset to Uninitialized with null cached_shape.\n" << std::flush;
}

void TestColdFunctionCodeFlushing() {
    std::cout << "[Test 3] Verifying Code Flushing for Cold Functions...\n" << std::flush;

    Agent agent;
    IsolateHeap& heap = agent.GetHeap();
    Realm& realm = agent.GetRealm();

    auto bc = std::make_shared<BytecodeArray>(2, 0);
    // Simulate Tier 1 JIT code
    auto exec_mem = std::make_shared<ExecutableMemory>(std::vector<uint8_t>{0xc3});
    bc->SetTier1Code(exec_mem);
    assert(bc->GetTier1Code() != nullptr);

    // Register a function using this bytecode in the heap
    HandleScope func_scope(heap);
    auto* fn = heap.Allocate<JSFunction>("cold_fn", bc, nullptr, realm.GetRootShape());
    func_scope.CreateHandle(fn);

    // Case A: Function was invoked since last GC -> code is kept
    bc->IncrementInvocationCount();
    heap.MajorGC();
    assert(bc->GetTier1Code() != nullptr);
    assert(bc->InvocationCount() == 0); // reset for next cycle
    std::cout << "  -> Active function retained Tier 1 JIT code.\n" << std::flush;

    // Case B: Function was NOT invoked during this GC cycle (cold) -> code is flushed
    heap.MajorGC();
    assert(bc->GetTier1Code() == nullptr);
    std::cout << "  -> PASSED: Cold function JIT code was flushed successfully.\n" << std::flush;
}

void TestOneMillionShapesAcceptance() {
    std::cout << "[Test 4] Verifying Acceptance: 1M Distinct Shapes Discarded without Unbounded Growth...\n" << std::flush;

    Agent agent;
    IsolateHeap& heap = agent.GetHeap();
    Realm& realm = agent.GetRealm();
    Shape* root = realm.GetRootShape();

    size_t idle_rss = GetProcessRSS();
    size_t baseline_shapes = root->TotalTreeShapeCount();
    std::cout << "  -> Baseline shapes: " << baseline_shapes << ", Idle RSS: " << (idle_rss / 1024) << " KB\n" << std::flush;

    auto start = std::chrono::steady_clock::now();
    uint32_t next_shape_id = 10000;
    constexpr size_t kTotalIterations = 1000000;
    constexpr size_t kBatchSize = 20000;

    for (size_t i = 0; i < kTotalIterations; i += kBatchSize) {
        {
            HandleScope scope(heap);
            for (size_t j = 0; j < kBatchSize; ++j) {
                auto* obj = heap.Allocate<JSObject>(root);
                std::string prop = "s_" + std::to_string(i + j);
                obj->SetNamedProperty(prop, TaggedValue::FromSmi(static_cast<int32_t>(j)), next_shape_id);
            }
        }
        heap.MajorGC();
        heap.Shrink();
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();
    size_t final_shapes = root->TotalTreeShapeCount();
    size_t final_rss = GetProcessRSS();

    std::cout << "  -> Completed 1,000,000 distinct shapes in " << elapsed_s << "s\n"
              << "  -> Final shapes: " << final_shapes << " (strictly bounded)\n"
              << "  -> Final RSS: " << (final_rss / 1024) << " KB\n" << std::flush;

    // In a system with weak transitions, after discarding all objects,
    // shape count must not exceed a tiny bound (e.g. <= baseline_shapes + 5).
    assert(final_shapes <= baseline_shapes + 5);
    // RSS must remain bounded (< 3x idle).
    if (idle_rss > 0 && final_rss > 0) {
        assert(final_rss < idle_rss * 3);
    }

    std::cout << "  -> PASSED: 1M distinct shapes discarded without unbounded memory growth.\n" << std::flush;
}

int main() {
    std::cout << "=== Running Dead Metadata Collection Tests (T2.5) ===\n" << std::flush;
    try {
        TestWeakShapeTransitions();
        TestICCachePruningOnDeadShape();
        TestColdFunctionCodeFlushing();
        TestOneMillionShapesAcceptance();
        std::cout << "\nALL DEAD METADATA COLLECTION TESTS PASSED (100%)\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: Uncaught exception: " << ex.what() << "\n";
        return 1;
    }
}

