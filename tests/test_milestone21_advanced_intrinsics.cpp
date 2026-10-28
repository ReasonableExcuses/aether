#include "aether/runtime.h"
#include "aether/heap.h"
#include "aether/advanced_intrinsics.h"
#include "aether/typed_array.h"
#include "aether/interpreter.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cassert>

using namespace aether;

static void Assert(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// [Test 1] WeakMap Core Semantics, Key Lookups & Primitive Rejection
// ============================================================================
static void Test1_WeakMapCore() {
    std::cout << "[Test 1] Verifying WeakMap set, get, has, delete and TypeError on primitives...\n";

    Agent agent;

    std::string script = R"(
        let wm = new WeakMap();
        let k1 = { id: 1 };
        let k2 = { id: 2 };
        let k3 = { id: 3 };

        wm.set(k1, 42);
        wm.set(k2, "hello");

        let r1 = wm.get(k1) === 42;
        let r2 = wm.get(k2) === "hello";
        let r3 = wm.get(k3) === undefined;
        let r4 = wm.has(k1) === true;
        let r5 = wm.has(k3) === false;

        // Overwrite
        wm.set(k1, 999);
        let r6 = wm.get(k1) === 999;

        // Delete
        let d1 = wm.delete(k1);
        let r7 = wm.has(k1) === false;
        let r8 = wm.get(k1) === undefined;
        let d2 = wm.delete(k1);

        // Primitive rejection check
        let caught1 = false;
        try {
            wm.set(123, "fail");
        } catch (e) {
            caught1 = e.name === "TypeError";
        }

        let caught2 = false;
        try {
            wm.set("primitive string", "fail");
        } catch (e) {
            caught2 = e.name === "TypeError";
        }

        r1 && r2 && r3 && r4 && r5 && r6 && (d1 === true) && r7 && r8 && (d2 === false) && caught1 && caught2;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "WeakMap operations failed");

    std::cout << "  -> WeakMap passed (1/10)\n";
}

// ============================================================================
// [Test 2] WeakSet Core Semantics, Membership & Primitive Rejection
// ============================================================================
static void Test2_WeakSetCore() {
    std::cout << "[Test 2] Verifying WeakSet add, has, delete and TypeError on primitives...\n";

    Agent agent;

    std::string script = R"(
        let ws = new WeakSet();
        let o1 = { a: 10 };
        let o2 = { b: 20 };
        let o3 = { c: 30 };

        ws.add(o1);
        ws.add(o2);

        let h1 = ws.has(o1) === true;
        let h2 = ws.has(o2) === true;
        let h3 = ws.has(o3) === false;

        // Duplicate add is idempotent
        ws.add(o1);
        let h4 = ws.has(o1) === true;

        // Delete
        let d1 = ws.delete(o1);
        let h5 = ws.has(o1);
        let d2 = ws.delete(o1);

        // Primitive rejection
        let caught1 = false;
        try {
            ws.add(42);
        } catch (e) {
            caught1 = e.name === "TypeError";
        }

        let caught2 = false;
        try {
            ws.add("string");
        } catch (e) {
            caught2 = e.name === "TypeError";
        }

        h1 && h2 && h3 && h4 && (d1 === true) && (h5 === false) && (d2 === false) && caught1 && caught2;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "WeakSet operations failed");

    std::cout << "  -> WeakSet passed (2/10)\n";
}

// ============================================================================
// [Test 3] SharedArrayBuffer Instantiation, ByteLength & Slicing
// ============================================================================
static void Test3_SharedArrayBufferBasics() {
    std::cout << "[Test 3] Verifying SharedArrayBuffer allocation, byteLength and slicing...\n";

    Agent agent;

    std::string script = R"(
        let sab = new SharedArrayBuffer(1024);
        let bl = sab.byteLength === 1024;

        let sub = sab.slice(256, 768);
        let sub_bl = sub.byteLength === 512;

        let is_sab = Object.prototype.toString.call(sab) === "[object SharedArrayBuffer]";
        let is_sub_sab = Object.prototype.toString.call(sub) === "[object SharedArrayBuffer]";

        bl && sub_bl && is_sab && is_sub_sab;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "SharedArrayBuffer basics failed");

    std::cout << "  -> SharedArrayBuffer basics passed (3/10)\n";
}

// ============================================================================
// [Test 4] Growable SharedArrayBuffer (ECMAScript 2024 Buffer-Growth)
// ============================================================================
static void Test4_GrowableSharedArrayBuffer() {
    std::cout << "[Test 4] Verifying Growable SharedArrayBuffer with maxByteLength and grow()...\n";

    Agent agent;

    std::string script = R"(
        let gsab = new SharedArrayBuffer(1024, { maxByteLength: 4096 });
        let bl1 = gsab.byteLength === 1024;
        let mbl = gsab.maxByteLength === 4096;
        let growable = gsab.growable === true;

        gsab.grow(2048);
        let bl2 = gsab.byteLength === 2048;

        // Exceeding maxByteLength throws RangeError
        let caught_max = false;
        try {
            gsab.grow(5000);
        } catch (e) {
            caught_max = e.name === "RangeError";
        }

        // Shrinking below current byteLength throws RangeError
        let caught_shrink = false;
        try {
            gsab.grow(1024);
        } catch (e) {
            caught_shrink = e.name === "RangeError";
        }

        bl1 && mbl && growable && bl2 && caught_max && caught_shrink;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "Growable SharedArrayBuffer failed");

    std::cout << "  -> Growable SharedArrayBuffer passed (4/10)\n";
}

// ============================================================================
// [Test 5] TypedArray Views Over SharedArrayBuffer & Shared Memory Access
// ============================================================================
static void Test5_TypedArraySharedArrayBuffer() {
    std::cout << "[Test 5] Verifying TypedArray wrapping SharedArrayBuffer and shared memory...\n";

    Agent agent;

    std::string script = R"(
        let sab = new SharedArrayBuffer(16);
        let i32 = new Int32Array(sab);
        let u8 = new Uint8Array(sab);

        let len = i32.length === 4;
        let bl = i32.byteLength === 16;
        let buf_match = i32.buffer === sab;

        // Write via Int32Array
        i32[0] = 0x12345678;

        // Read via Uint8Array (little-endian byte inspection)
        let b0 = u8[0] === 0x78;
        let b1 = u8[1] === 0x56;
        let b2 = u8[2] === 0x34;
        let b3 = u8[3] === 0x12;

        len && bl && buf_match && b0 && b1 && b2 && b3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "TypedArray with SharedArrayBuffer failed");

    std::cout << "  -> TypedArray with SharedArrayBuffer passed (5/10)\n";
}

// ============================================================================
// [Test 6] Atomics Arithmetic, Bitwise & CompareExchange Operations
// ============================================================================
static void Test6_AtomicsArithmeticAndBitwise() {
    std::cout << "[Test 6] Verifying Atomics load, store, add, sub, and, or, xor, exchange, compareExchange...\n";

    Agent agent;

    std::string script = R"(
        let lf1 = Atomics.isLockFree(1) === true;
        let lf2 = Atomics.isLockFree(2) === true;
        let lf4 = Atomics.isLockFree(4) === true;
        let lf8 = Atomics.isLockFree(8) === true;
        let lf3 = Atomics.isLockFree(3) === false;

        let sab = new SharedArrayBuffer(16);
        let i32 = new Int32Array(sab);

        // store and load
        let st = Atomics.store(i32, 0, 10) === 10;
        let ld = Atomics.load(i32, 0) === 10;

        // add: returns old value
        let a1 = Atomics.add(i32, 0, 5) === 10;
        let a2 = Atomics.load(i32, 0) === 15;

        // sub: returns old value
        let s1 = Atomics.sub(i32, 0, 3) === 15;
        let s2 = Atomics.load(i32, 0) === 12;

        // and (12 & 9 = 8)
        let an1 = Atomics.and(i32, 0, 9) === 12;
        let an2 = Atomics.load(i32, 0) === 8;

        // or (8 | 3 = 11)
        let o1 = Atomics.or(i32, 0, 3) === 8;
        let o2 = Atomics.load(i32, 0) === 11;

        // xor (11 ^ 6 = 13)
        let x1 = Atomics.xor(i32, 0, 6) === 11;
        let x2 = Atomics.load(i32, 0) === 13;

        // exchange
        let ex1 = Atomics.exchange(i32, 0, 100) === 13;
        let ex2 = Atomics.load(i32, 0) === 100;

        // compareExchange: success case
        let cas1 = Atomics.compareExchange(i32, 0, 100, 200) === 100;
        let cas2 = Atomics.load(i32, 0) === 200;

        // compareExchange: mismatch case (returns actual value without modifying)
        let cas3 = Atomics.compareExchange(i32, 0, 999, 300) === 200;
        let cas4 = Atomics.load(i32, 0) === 200;

        lf1 && lf2 && lf4 && lf8 && lf3 && st && ld && a1 && a2 && s1 && s2 && an1 && an2 && o1 && o2 && x1 && x2 && ex1 && ex2 && cas1 && cas2 && cas3 && cas4;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "Atomics arithmetic/bitwise failed");

    std::cout << "  -> Atomics arithmetic and bitwise passed (6/10)\n";
}

// ============================================================================
// [Test 7] Atomics.wait & Atomics.notify Multi-Threaded Futex Synchronization
// ============================================================================
static void Test7_AtomicsFutexWaitNotify() {
    std::cout << "[Test 7] Verifying Atomics.wait & Atomics.notify multi-threaded futex synchronization...\n";

    Agent agent;

    // First test immediate return cases: "not-equal" and "timed-out"
    std::string script1 = R"(
        let sab = new SharedArrayBuffer(16);
        let i32 = new Int32Array(sab);
        i32[0] = 100;

        // If value doesn't match expected, returns "not-equal" immediately
        let ne = Atomics.wait(i32, 0, 50) === "not-equal";

        // Zero timeout returns "timed-out" immediately if value matches
        let to = Atomics.wait(i32, 0, 100, 0) === "timed-out";

        ne && to;
    )";

    TaggedValue result1 = agent.EvaluateScript(script1);
    Assert(result1.IsBoolean() && result1.AsBoolean(), "Atomics.wait immediate paths failed");

    // Multi-threaded cross-thread wait/notify test
    auto* sab = new JSArrayBuffer(16, 16, true);
    auto* i32 = new JSTypedArray(TypedArrayKind::Int32, sab, 0, 4);
    int32_t* ptr = reinterpret_cast<int32_t*>(i32->Data());
    *ptr = 0;

    std::atomic<bool> worker_started{false};
    std::string wait_status = "";

    std::thread worker([ptr, &worker_started, &wait_status]() {
        worker_started = true;
        // Wait for value 0 with 3000ms timeout
        wait_status = FutexManager::Instance().Wait32(ptr, 0, 3000.0);
    });

    while (!worker_started) {
        std::this_thread::yield();
    }
    // Give worker time to enter futex sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Main thread updates value atomically and notifies waiter
    std::atomic_ref<int32_t>(*ptr).store(1, std::memory_order_release);
    uint32_t awoken = FutexManager::Instance().Notify(ptr, 1);
    Assert(awoken == 1, "Futex notify should wake 1 waiter");

    worker.join();
    Assert(wait_status == "ok", "Futex wait should return 'ok' after being notified");

    delete i32;
    delete sab;

    std::cout << "  -> Atomics futex wait/notify passed (7/10)\n";
}

// ============================================================================
// [Test 8] Atomics.waitAsync Non-Blocking Coordination
// ============================================================================
static void Test8_AtomicsWaitAsync() {
    std::cout << "[Test 8] Verifying Atomics.waitAsync sync & async return types...\n";

    Agent agent;

    std::string script = R"(
        let sab = new SharedArrayBuffer(16);
        let i32 = new Int32Array(sab);
        i32[0] = 77;

        // Immediate mismatch returns { async: false, value: "not-equal" }
        let res1 = Atomics.waitAsync(i32, 0, 10);
        let r1 = res1.async === false && res1.value === "not-equal";

        // Zero timeout returns { async: false, value: "timed-out" }
        let res2 = Atomics.waitAsync(i32, 0, 77, 0);
        let r2 = res2.async === false && res2.value === "timed-out";

        // Positive timeout returns { async: true, value: Promise }
        let res3 = Atomics.waitAsync(i32, 0, 77, 100);
        let r3 = res3.async === true && typeof res3.value === "object";

        r1 && r2 && r3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "Atomics.waitAsync failed");

    std::cout << "  -> Atomics.waitAsync passed (8/10)\n";
}

// ============================================================================
// [Test 9] WeakRef Creation, Dereferencing & Object Lifecycles
// ============================================================================
static void Test9_WeakRefCore() {
    std::cout << "[Test 9] Verifying WeakRef deref, toStringTag and primitive rejection...\n";

    Agent agent;

    std::string script = R"(
        let target = { name: "AetherVM", version: 1 };
        let ref = new WeakRef(target);

        let d1 = ref.deref() === target;
        let tag = Object.prototype.toString.call(ref) === "[object WeakRef]";

        // Reject primitives
        let caught = false;
        try {
            new WeakRef(12345);
        } catch (e) {
            caught = e.name === "TypeError";
        }

        d1 && tag && caught;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "WeakRef operations failed");

    // Direct C++ weak clearing test
    auto* obj = new JSObject(nullptr);
    JSWeakRef weak_ref(obj);
    Assert(weak_ref.Deref().AsHeapObject() == obj, "WeakRef must point to target");

    weak_ref.ClearTarget();
    Assert(weak_ref.Deref().IsUndefined(), "Cleared WeakRef deref must return undefined");
    delete obj;

    std::cout << "  -> WeakRef passed (9/10)\n";
}

// ============================================================================
// [Test 10] FinalizationRegistry Registration, Unregistration & Cleanup
// ============================================================================
static void Test10_FinalizationRegistryCore() {
    std::cout << "[Test 10] Verifying FinalizationRegistry registration, token unregistration & cleanup...\n";

    Agent agent;

    std::string script = R"(
        let reg = new FinalizationRegistry(function(held) {});
        let t1 = { id: 1 };
        let t2 = { id: 2 };
        let tok1 = { token: "t1" };
        let tok2 = { token: "t2" };

        reg.register(t1, "value1", tok1);
        reg.register(t2, "value2", tok2);

        // Unregister tok1
        let u1 = reg.unregister(tok1) === true;
        let u2 = reg.unregister(tok1) === false; // already removed

        // Primitive rejection
        let caught_target = false;
        try {
            reg.register("primitive target", "val");
        } catch (e) {
            caught_target = e.name === "TypeError";
        }

        let caught_tok = false;
        try {
            reg.unregister("primitive token");
        } catch (e) {
            caught_tok = e.name === "TypeError";
        }

        let caught_ctor = false;
        try {
            new FinalizationRegistry("not a function");
        } catch (e) {
            caught_ctor = e.name === "TypeError";
        }

        u1 && u2 && caught_target && caught_tok && caught_ctor;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "FinalizationRegistry operations failed");

    // Direct cleanup callback execution verification
    std::string cleaned_val = "";
    auto* callback = new JSNativeFunction("cleanupCb", [&cleaned_val](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsString()) {
            cleaned_val = static_cast<JSString*>(args[0].AsHeapObject())->View();
        }
        return TaggedValue::Undefined();
    });

    JSFinalizationRegistry fr(TaggedValue::FromHeapObject(callback));
    auto* target = new JSObject(nullptr);
    auto* held_str = new JSString("resource_freed_token");
    fr.Register(target, TaggedValue::FromHeapObject(held_str));

    Assert(fr.RegistrationCount() == 1, "Registration count should be 1");
    size_t count = fr.CleanupForTarget(target, [](TaggedValue cb, TaggedValue held) {
        CallFunction(cb, TaggedValue::Undefined(), {held});
    });

    Assert(count == 1, "Should have cleaned 1 registration");
    Assert(fr.RegistrationCount() == 0, "Registration count should be 0 after cleanup");
    Assert(cleaned_val == "resource_freed_token", "Cleanup callback should have received held value");

    delete held_str;
    delete target;
    delete callback;

    std::cout << "  -> FinalizationRegistry passed (10/10)" << std::endl;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "Running Milestone 21: Advanced ECMAScript Intrinsics & Concurrency Tests\n";
    std::cout << "========================================================\n";

    Test1_WeakMapCore();
    Test2_WeakSetCore();
    Test3_SharedArrayBufferBasics();
    Test4_GrowableSharedArrayBuffer();
    Test5_TypedArraySharedArrayBuffer();
    Test6_AtomicsArithmeticAndBitwise();
    Test7_AtomicsFutexWaitNotify();
    Test8_AtomicsWaitAsync();
    Test9_WeakRefCore();
    Test10_FinalizationRegistryCore();

    std::cout << "========================================================\n";
    std::cout << "ALL 10 ADVANCED INTRINSICS TESTS PASSED (10/10)!\n";
    std::cout << "========================================================\n";

    return 0;
}
