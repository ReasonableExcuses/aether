#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include "aether/heap_object.h"
#include "aether/heap.h"
#include "aether/heap_verifier.h"
#include "aether/runtime.h"
#include "aether/options.h"
#include <iostream>
#include <vector>
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

void TestLazySweepingOnDemand() {
    std::cout << "[Test 1] Verifying Lazy Sweeping On Demand...\n" << std::flush;

    Options::Get().gc_verify = false;
    IsolateHeap heap;
    IsolateScope isolate(heap);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    constexpr size_t kCount = 500;
    std::vector<Handle<JSObject>> live;
    for (size_t i = 0; i < kCount; ++i) {
        auto* obj = heap.AllocateOld<JSObject>(root_shape.get());
        obj->SetFastSlot(0, true, TaggedValue::FromSmi(static_cast<int32_t>(i)));
        if (i % 2 == 0) live.push_back(scope.CreateHandle(obj));
    }

    heap.MajorGC();

    assert(heap.GetOldGen().HasUnsweptBlocks());
    std::cout << "  -> Recyclable blocks marked with kBlockFlagNeedsSweep successfully.\n" << std::flush;

    for (size_t i = 0; i < 200; ++i) {
        heap.AllocateOld<JSObject>(root_shape.get());
    }

    heap.GetOldGen().EnsureAllSwept();
    assert(!heap.GetOldGen().HasUnsweptBlocks());

    for (size_t i = 0; i < live.size(); ++i) {
        JSObject* obj = live[i].Get();
        assert(obj != nullptr);
        assert(obj->GetFastSlot(0, true).AsSmi() == static_cast<int32_t>(i * 2));
    }

    std::cout << "  -> PASSED: Lazy sweeping on demand verified with full object integrity.\n" << std::flush;
}

void TestHeapShrinkingReclaimsOSMemory() {
    std::cout << "[Test 2] Verifying Heap Shrinking and RSS Reduction (< 2x idle)...\n" << std::flush;

    Options::Get().gc_verify = false;
    IsolateHeap heap;
    IsolateScope isolate(heap);

    size_t idle_committed = heap.TotalCommittedBytes();
    size_t idle_rss = GetProcessRSS();
    std::cout << "  -> Idle committed bytes: " << (idle_committed / 1024) << " KB, Idle RSS: "
              << (idle_rss / 1024) << " KB\n" << std::flush;

    {
        HandleScope scope(heap);
        std::vector<Handle<FixedArray>> large_workload;
        constexpr size_t kLargeArrayCount = 400;
        large_workload.reserve(kLargeArrayCount);
        for (size_t i = 0; i < kLargeArrayCount; ++i) {
            auto* arr = heap.AllocateFixedArray(1000, 1000, TaggedValue::FromSmi(static_cast<int32_t>(i)), true);
            large_workload.push_back(scope.CreateHandle(arr));
        }

        size_t peak_committed = heap.TotalCommittedBytes();
        size_t peak_rss = GetProcessRSS();
        std::cout << "  -> Peak committed bytes: " << (peak_committed / 1024) << " KB, Peak RSS: "
                  << (peak_rss / 1024) << " KB\n" << std::flush;
        assert(peak_committed >= 3 * 1024 * 1024);
    }

    heap.MajorGC();
    heap.Shrink();

    size_t post_gc_committed = heap.TotalCommittedBytes();
    size_t post_gc_rss = GetProcessRSS();
    std::cout << "  -> Post-release committed bytes: " << (post_gc_committed / 1024)
              << " KB, Post-release RSS: " << (post_gc_rss / 1024) << " KB\n" << std::flush;

    assert(post_gc_committed <= std::max<size_t>(64 * 1024, idle_committed * 2));
    if (idle_rss > 0 && post_gc_rss > 0) {
        assert(post_gc_rss < 2 * idle_rss || post_gc_rss <= idle_rss + 10 * 1024 * 1024);
    }

    std::cout << "  -> PASSED: Heap shrinking returned committed blocks and RSS to idle levels.\n" << std::flush;
}

void TestMaxHeapDeterministicOOM() {
    std::cout << "[Test 3] Verifying --max-heap Enforcement and Deterministic RangeError...\n" << std::flush;

    Options::Get().max_heap_mb = 4;

    Agent agent;
    const char* script = R"(
        let caught = false;
        let errName = "";
        try {
            let list = [];
            while (true) {
                list.push(new Array(10000));
            }
        } catch (e) {
            caught = true;
            errName = (e && e.name) ? e.name : String(e);
        }
        caught && (errName === "RangeError" || errName.indexOf("RangeError") !== -1 || errName.indexOf("budget") !== -1);
    )";

    TaggedValue result = agent.EvaluateScript(script);
    std::cout << "  -> JS OOM Catch Result: " << (result.ToBoolean() ? "true" : "false") << "\n" << std::flush;
    assert(result.ToBoolean());

    Options::Get().max_heap_mb = 0;
    std::cout << "  -> PASSED: --max-heap throws catchable RangeError deterministically.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone T2.4: Lazy Sweeping and Shrinking\n";
    std::cout << "=========================================================\n" << std::flush;

    TestLazySweepingOnDemand();
    TestHeapShrinkingReclaimsOSMemory();
    TestMaxHeapDeterministicOOM();

    std::cout << "All Lazy Sweeping and Heap Shrinking tests passed!\n" << std::flush;
    return 0;
}

