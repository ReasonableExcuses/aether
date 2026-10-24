#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"
#include "aether/advanced_intrinsics.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace aether;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Probe final : public JSObject {
public:
    explicit Probe(int& destroyed) : JSObject(nullptr), destroyed_(destroyed) {}
    ~Probe() override { ++destroyed_; }
    HeapObject* RelocateTo(void*) override { std::terminate(); } // stable runtime objects only
    size_t ObjectSize() const override { return sizeof(Probe); }
private:
    int& destroyed_;
};

void NativeAndQueueRoots() {
    int destroyed = 0;
    Heap heap;
    IsolateScope isolate(heap);
    auto* captured = AllocateRuntime<Probe>(destroyed);
    auto* native = AllocateRuntime<JSNativeFunction>("capture", [&heap, captured](const std::vector<TaggedValue>&) {
        Check(!heap.GCAllowed(), "native scope allowed collection");
        heap.MajorGC();
        Check(!heap.CollectAtSafepoint(), "native reentry safepoint collected");
        return TaggedValue::FromHeapObject(captured);
    });
    native->AddCapturedRoot(TaggedValue::FromHeapObject(captured));
    {
        HandleScope roots(heap);
        roots.CreateHandle(native);
        heap.MajorGC();
        Check(destroyed == 0, "native capture collected");
        Check(native->Invoke(TaggedValue::Undefined(), {}).AsHeapObject() == captured, "capture changed");
        native->SetCapturedRoots({});
        heap.MajorGC();
        Check(destroyed == 1, "replaced native capture stayed rooted");
    }
    heap.MajorGC();

    MicrotaskQueue queue;
    queue.InitializeRoots(heap);
    auto* queued = AllocateRuntime<Probe>(destroyed);
    bool ran = false;
    queue.Enqueue([&] {
        heap.MajorGC();
        Check(destroyed == 1, "running task lost its root");
        ran = true;
    }, {TaggedValue::FromHeapObject(queued)});
    heap.MajorGC();
    Check(destroyed == 1, "queued capture collected");
    queue.PerformMicrotaskCheckpoint();
    heap.MajorGC();
    Check(ran && destroyed == 2, "finished task retained capture");

    auto* deferred = AllocateRuntime<Probe>(destroyed);
    int sequence = 0;
    auto pending = queue.Prepare([&] { Check(sequence++ == 1, "deferred FIFO order"); },
                                 {TaggedValue::FromHeapObject(deferred)});
    heap.MajorGC();
    Check(destroyed == 2 && queue.Empty() && queue.HasPendingWork(), "unpublished task root lost");
    queue.Enqueue([&] { Check(sequence++ == 0, "ready FIFO order"); });
    std::thread worker([pending] { pending.Publish(); });
    worker.join();
    queue.PerformMicrotaskCheckpoint();
    heap.MajorGC();
    Check(sequence == 2 && destroyed == 3, "published task did not retire");

    auto* cancelled = AllocateRuntime<Probe>(destroyed);
    auto cancel = queue.Prepare([] { throw std::runtime_error("cancelled task ran"); },
                                {TaggedValue::FromHeapObject(cancelled)});
    cancel.Cancel();
    heap.MajorGC();
    Check(destroyed == 4, "cancelled task retained roots");
    queue.PerformMicrotaskCheckpoint();

    auto* worker_value = AllocateRuntime<Probe>(destroyed);
    auto pre_root = queue.Prepare([] {}, {TaggedValue::FromHeapObject(worker_value)});
    std::thread producer([&] {
        queue.Enqueue([&] { ran = true; }, {TaggedValue::FromHeapObject(worker_value)});
    });
    producer.join();
    pre_root.Cancel();
    heap.MajorGC();
    Check(destroyed == 4, "worker publication lost roots");
    queue.Clear();
    heap.MajorGC();
    Check(destroyed == 5, "queue clear retained roots");
}

void TimersAndModules() {
    int destroyed = 0;
    Agent agent;
    auto& heap = agent.GetHeap();
    HandleScope host_roots(heap);
    host_roots.CreateHandle(agent.GetRealm().GetGlobalObject());

    auto* argument = AllocateRuntime<Probe>(destroyed);
    bool ran = false;
    auto* callback = AllocateRuntime<JSNativeFunction>("timer", [&](const std::vector<TaggedValue>& args) {
        Check(args.size() == 1 && args[0].AsHeapObject() == argument, "timer argument changed");
        ran = true;
        return TaggedValue::Undefined();
    });
    auto& loop = agent.GetEventLoop();
    loop.SetTimeout(TaggedValue::FromHeapObject(callback), 0, {TaggedValue::FromHeapObject(argument)});
    heap.MajorGC();
    Check(destroyed == 0, "timer argument collected");
    loop.Step(agent);
    heap.MajorGC();
    Check(ran && destroyed == 1, "completed timer retained roots");

    auto* cancelled = AllocateRuntime<Probe>(destroyed);
    auto id = loop.SetTimeout(TaggedValue::Undefined(), INT64_MAX, {TaggedValue::FromHeapObject(cancelled)});
    loop.ClearTimeout(id);
    heap.MajorGC();
    Check(destroyed == 2, "cancelled future timer retained arguments");
    auto* immediate = AllocateRuntime<Probe>(destroyed);
    auto imm = loop.SetImmediate(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(immediate)});
    loop.ClearImmediate(imm);
    heap.MajorGC();
    Check(destroyed == 3, "cancelled immediate retained arguments");

    {
        auto module = std::make_shared<SourceTextModuleRecord>("root-test.mjs", agent.GetRealm());
        module->Parse("export function value() { return 'module-literal'; } export const label = 'kept';");
        auto* value = AllocateRuntime<Probe>(destroyed);
        uint32_t sid = 900;
        module->GetEnvironment()->SetNamedProperty("probe", TaggedValue::FromHeapObject(value), sid);
        auto* meta = module->GetImportMeta(&agent.GetRealm());
        heap.MajorGC();
        Check(destroyed == 3 && meta->GetNamedProperty("filename").ToString() == "root-test.mjs", "module cache root lost");
        module->Link(agent.GetModuleLoader());
        heap.MajorGC();
        module->Evaluate(agent);
        auto* ns = module->GetNamespaceObject(agent);
        heap.MajorGC();
        Check(ns->GetNamedProperty("label").ToString() == "kept", "namespace lost");
        auto result = CallFunction(ns->GetNamedProperty("value"), TaggedValue::Undefined(), {});
        Check(result.ToString() == "module-literal", "nested AST literal lost before emission");
    }
    heap.MajorGC();
    Check(destroyed == 4, "released module retained its environment");

    std::weak_ptr<SourceTextModuleRecord> weak_a;
    {
        ModuleLoader loader(agent);
        loader.RegisterVirtualModule("a.mjs", "import 'b.mjs'; export const a = 1;");
        loader.RegisterVirtualModule("b.mjs", "import 'a.mjs'; export const b = 2;");
        auto a = loader.LoadModule("a.mjs");
        loader.LinkModule(a.get());
        weak_a = a;
    }
    ModuleLoader::SetCurrent(&agent.GetModuleLoader());
    Check(weak_a.expired(), "cyclic ESM cache leaked records and roots");
}

void ResourceErrorsAndWaits() {
    Agent agent;
    auto& heap = agent.GetHeap();
    auto& interpreter = agent.GetInterpreter();
    HandleScope roots(heap);
    roots.CreateHandle(agent.GetRealm().GetGlobalObject());
    interpreter.InitializeResourceExceptions();
    auto* throwing = AllocateRuntime<JSNativeFunction>("allocation", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) throw std::bad_alloc();
        throw std::length_error("too large");
    });
    roots.CreateHandle(throwing);
    const size_t budget = heap.HeapBudget();
    heap.SetHeapBudget(heap.TotalAllocatedBytes());
    for (auto args : {std::vector<TaggedValue>{}, std::vector<TaggedValue>{TaggedValue::True()}}) {
        throwing->Invoke(TaggedValue::Undefined(), args);
        Check(interpreter.HasPendingException(), "native resource failure escaped JS exception channel");
        Check(interpreter.PendingException().As<JSError>()->GetNamedProperty("name").ToString() == "RangeError", "resource error type");
        interpreter.ClearPendingException();
    }
    heap.SetHeapBudget(budget);

    auto constructor = agent.GetRealm().GetGlobalProperty("SharedArrayBuffer");
    for (double bad : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e100}) {
        CallFunction(constructor, TaggedValue::Undefined(), {TaggedValue::FromDouble(bad)});
        Check(interpreter.HasPendingException(), "invalid SharedArrayBuffer length accepted");
        interpreter.ClearPendingException();
        auto* options = AllocateRuntime<JSObject>(agent.GetRealm().GetRootShape());
        uint32_t sid = 900;
        options->SetNamedProperty("maxByteLength", TaggedValue::FromDouble(bad), sid);
        CallFunction(constructor, TaggedValue::Undefined(), {TaggedValue::FromSmi(0), TaggedValue::FromHeapObject(options)});
        Check(interpreter.HasPendingException(), "invalid maxByteLength accepted");
        interpreter.ClearPendingException();
    }

    auto wait_result = agent.EvaluateScript("Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 30);");
    auto* promise = wait_result.As<JSObject>()->GetNamedProperty("value").As<JSPromise>();
    // Deliberately keep no host root: the deferred task owns promise + buffer.
    heap.MajorGC();
    Check(promise->State() == PromiseState::Pending, "wait promise did not stay pending");
    agent.RunEventLoop();
    Check(promise->State() == PromiseState::Fulfilled && promise->Result().ToString() == "timed-out", "wait completion not delivered");
    Check(!agent.GetMicrotasks().HasPendingWork(), "completed wait retained pending task");
    heap.MajorGC();
    Check(heap.BudgetState()->ArrayBufferBytes() == 0, "completed wait retained backing store");

    agent.EvaluateScript("Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0);");
    heap.MajorGC();
    AsyncWaitManager::Instance().CancelAndJoinForAgent(&agent);
    agent.GetMicrotasks().PerformMicrotaskCheckpoint();
    heap.MajorGC();
    Check(!agent.GetMicrotasks().HasPendingWork() && heap.BudgetState()->ArrayBufferBytes() == 0,
          "cancelled wait retained promise/backing-store roots");
}
}

int main() {
    try {
        NativeAndQueueRoots();
        TimersAndModules();
        ResourceErrorsAndWaits();
        std::cout << "async/native/module hardening tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
