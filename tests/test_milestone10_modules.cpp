#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"

#include <iostream>
#include <filesystem>
#include <fstream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestCommonJSBasicExports() {
    std::cout << "[Test 1] Verifying CommonJS require, exports, and property binding...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("math_util", R"(
        exports.add = function(a, b) {
            return a + b;
        };
        exports.multiplier = 10;
    )");

    TaggedValue res = agent.EvaluateScript(R"(
        var math = require("math_util");
        var sum = math.add(5, 7);
        sum * math.multiplier;
    )");

    assert(res.IsSmi() && res.AsSmi() == 120);

    std::cout << "  -> CommonJS require and exports verified.\n" << std::flush;
}

void TestModuleExportsReassignment() {
    std::cout << "[Test 2] Verifying module.exports function/object reassignment...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("greeter", R"(
        module.exports = function(name) {
            return "Hello, " + name + "!";
        };
    )");

    TaggedValue res = agent.EvaluateScript(R"(
        var greet = require("greeter");
        greet("AetherVM");
    )");

    assert(res.IsHeapObject() && res.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res.AsHeapObject())->Value() == "Hello, AetherVM!");

    std::cout << "  -> module.exports reassignment verified.\n" << std::flush;
}

void TestModuleCachingAndSingleton() {
    std::cout << "[Test 3] Verifying CommonJS module cache & singleton instances...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("counter_store", R"(
        exports.count = 0;
        exports.increment = function() {
            exports.count = exports.count + 1;
            return exports.count;
        };
    )");

    TaggedValue res = agent.EvaluateScript(R"(
        var c1 = require("counter_store");
        c1.increment();
        var c2 = require("counter_store");
        c2.increment();
        c1.count;
    )");

    assert(res.IsSmi() && res.AsSmi() == 2);

    std::cout << "  -> CommonJS module cache and singleton semantics verified.\n" << std::flush;
}

void TestCircularModuleDependencies() {
    std::cout << "[Test 4] Verifying circular module dependency resolution...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("circular_a", R"(
        exports.name = "A";
        var b = require("circular_b");
        exports.partner = b.name;
    )");

    loader.RegisterVirtualModule("circular_b", R"(
        exports.name = "B";
        var a = require("circular_a");
        exports.partner = a.name;
    )");

    TaggedValue res = agent.EvaluateScript(R"(
        var a = require("circular_a");
        a.name + a.partner;
    )");

    assert(res.IsHeapObject() && res.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res.AsHeapObject())->Value() == "AB");

    std::cout << "  -> Circular module dependency resolution verified.\n" << std::flush;
}

void TestProcessGlobalAndEnvironment() {
    std::cout << "[Test 5] Verifying process global (argv, version, platform, arch, cwd, nextTick)...\n" << std::flush;

    Agent agent;
    agent.GetModuleLoader().InitializeModuleGlobals({"aether", "app.js", "--verbose"});

    // 1. process.version
    TaggedValue v = agent.EvaluateScript("process.version;");
    assert(v.IsHeapObject() && v.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(v.AsHeapObject())->Value() == "v22.0.0-aether");

    // 2. process.platform & process.arch
    TaggedValue plat = agent.EvaluateScript("process.platform;");
    assert(plat.IsHeapObject() && plat.AsHeapObject()->IsString());
#if defined(_WIN32)
    assert(static_cast<JSString*>(plat.AsHeapObject())->Value() == "win32");
#elif defined(__APPLE__)
    assert(static_cast<JSString*>(plat.AsHeapObject())->Value() == "darwin");
#else
    assert(static_cast<JSString*>(plat.AsHeapObject())->Value() == "linux");
#endif

    TaggedValue arch = agent.EvaluateScript("process.arch;");
    assert(arch.IsHeapObject() && arch.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(arch.AsHeapObject())->Value() == "x64");

    // 3. process.cwd()
    TaggedValue cwd = agent.EvaluateScript("process.cwd();");
    assert(cwd.IsHeapObject() && cwd.AsHeapObject()->IsString());
    assert(!static_cast<JSString*>(cwd.AsHeapObject())->Value().empty());

    // 4. process.argv
    TaggedValue arg1 = agent.EvaluateScript("process.argv[1];");
    assert(arg1.IsHeapObject() && arg1.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(arg1.AsHeapObject())->Value() == "app.js");

    // 5. process.nextTick
    agent.EvaluateScript(R"(
        globalThis.tickResult = 0;
        process.nextTick(function() {
            globalThis.tickResult = 88;
        });
    )");
    TaggedValue postTick = agent.GetRealm().GetGlobalProperty("tickResult");
    assert(postTick.IsSmi() && postTick.AsSmi() == 88);

    std::cout << "  -> process environment globals and nextTick verified.\n" << std::flush;
}

void TestFilesystemModuleResolutionAndAsyncIntegration() {
    std::cout << "[Test 6] Verifying filesystem CommonJS resolution and async Event Loop integration...\n" << std::flush;

    std::filesystem::path temp_dir = std::filesystem::current_path() / "build" / "temp_modules";
    std::filesystem::create_directories(temp_dir);

    std::filesystem::path helper_file = temp_dir / "async_helper.js";
    {
        std::ofstream out(helper_file);
        out << R"(
            exports.asyncMultiply = function(a, b, cb) {
                setTimeout(function() {
                    cb(a * b);
                }, 5);
            };
        )";
    }

    Agent agent;

    std::string import_path = helper_file.string();
    std::replace(import_path.begin(), import_path.end(), '\\', '/');

    std::string script = R"(
        var helper = require(")" + import_path + R"(");
        globalThis.finalAsyncResult = 0;
        helper.asyncMultiply(6, 7, function(product) {
            globalThis.finalAsyncResult = product;
        });
    )";

    agent.EvaluateScript(script);

    TaggedValue pre = agent.GetRealm().GetGlobalProperty("finalAsyncResult");
    assert(pre.IsSmi() && pre.AsSmi() == 0);

    agent.RunEventLoop();

    TaggedValue post = agent.GetRealm().GetGlobalProperty("finalAsyncResult");
    assert(post.IsSmi() && post.AsSmi() == 42);

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);

    std::cout << "  -> Filesystem CommonJS loader and async event loop integration verified.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone 10 Test Suite: Full Ecosystem & Modules\n";
    std::cout << "=========================================================\n\n";

    TestCommonJSBasicExports();
    TestModuleExportsReassignment();
    TestModuleCachingAndSingleton();
    TestCircularModuleDependencies();
    TestProcessGlobalAndEnvironment();
    TestFilesystemModuleResolutionAndAsyncIntegration();

    std::cout << "\n[PASS] All 6 Milestone 10 Module & Ecosystem Verification Suites Passed!\n";
    return 0;
}
