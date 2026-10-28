#undef NDEBUG
#include <cassert>

#include "aether/test262.h"
#include "aether/runtime.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <filesystem>

using namespace aether;

void TestFrontmatterParsing() {
    std::cout << "[Test 1] Verifying Test262 YAML Frontmatter Metadata Parser...\n" << std::flush;

    std::string sample = R"(
/*---
description: Declarations in lexical declarations
features: [let, const]
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
includes: [assert.js, sta.js]
---*/

let x = 10;
)";

    Test262Metadata meta = ParseTest262Frontmatter(sample);
    assert(meta.description == "Declarations in lexical declarations");
    assert(meta.features.size() == 2 && meta.features[0] == "let" && meta.features[1] == "const");
    assert(meta.flags.size() == 1 && meta.flags[0] == "onlyStrict");
    assert(meta.HasFlag("onlyStrict"));
    assert(meta.is_negative);
    assert(meta.negative_phase == "parse");
    assert(meta.negative_type == "SyntaxError");
    assert(meta.includes.size() == 2 && meta.includes[0] == "assert.js");

    std::cout << "  -> YAML frontmatter parser and flag extraction verified.\n" << std::flush;
}

static std::string FindTest262Dir() {
    if (std::filesystem::exists("test/test262")) return "test/test262";
    if (std::filesystem::exists("../test/test262")) return "../test/test262";
    if (std::filesystem::exists("../../test/test262")) return "../../test/test262";
    return "test/test262";
}

void TestDirectHarnessAssertions() {
    std::cout << "[Test 2] Verifying Native Test262 Harness Assertions and Errors...\n" << std::flush;

    Test262Harness harness(FindTest262Dir() + "/harness");

    // Positive assertion test
    std::string script1 = R"(
/*---
includes: [assert.js]
---*/
assert.sameValue(100, 100, "Equal values");
assert.notSameValue(100, 200, "Different values");
)";
    Test262Result res1 = harness.RunTest(script1, "direct_assertion_test.js");
    if (res1.kind != Test262ResultKind::Pass) {
        std::cerr << "res1 failed: " << res1.message << "\n";
    }
    assert(res1.kind == Test262ResultKind::Pass);

    // Negative parse test
    std::string script2 = R"(
/*---
negative:
  phase: parse
  type: SyntaxError
---*/
let invalid syntax ; =
)";
    Test262Result res2 = harness.RunTest(script2, "negative_syntax_test.js");
    assert(res2.kind == Test262ResultKind::Pass);

    // Negative throw test
    std::string script3 = R"(
/*---
includes: [assert.js]
---*/
assert.throws(TypeError, function() {
    throw new TypeError("custom failure");
});
)";
    Test262Result res3 = harness.RunTest(script3, "assert_throws_test.js");
    if (res3.kind != Test262ResultKind::Pass) {
        std::cerr << "res3 failed: " << res3.message << "\n";
    }
    assert(res3.kind == Test262ResultKind::Pass);

    std::cout << "  -> Positive and negative harness assertion evaluation verified.\n" << std::flush;
}

void TestCuratedTest262Suite() {
    std::cout << "[Test 3] Verifying Complete Curated Test262 Conformance Suite...\n" << std::flush;

    Test262Harness harness(FindTest262Dir() + "/harness");
    std::filesystem::path suite_dir = FindTest262Dir() + "/test";

    std::vector<Test262Result> results = harness.RunTestDirectory(suite_dir);
    assert(!results.empty());

    size_t passed = 0;
    size_t failed = 0;
    double total_ms = 0.0;

    for (const auto& r : results) {
        total_ms += r.duration_ms;
        if (r.kind == Test262ResultKind::Pass) {
            passed++;
        } else {
            failed++;
            std::cerr << "  FAILED: " << r.test_name << " - " << r.message << "\n";
        }
    }

    std::cout << "  Total Test262 Cases Evaluated: " << results.size() << "\n";
    std::cout << "  Passed: " << passed << " | Failed: " << failed << "\n";
    std::cout << "  Pass Rate: " << std::fixed << std::setprecision(1) << (100.0 * passed / results.size()) << "%\n";
    std::cout << "  Total Execution Duration: " << total_ms << " ms\n";

    assert(failed == 0);
    std::cout << "  -> Curated Test262 Conformance Suite 100% Passed!\n" << std::flush;
}

void TestNegativeErrorTypeMismatch() {
    std::cout << "[Test 4] Verifying Negative Test Strict Error-Type Validation...\n" << std::flush;

    Test262Harness harness("test/test262/harness");

    // Case 1: Test expects TypeError, but code produces a SyntaxError.
    // The harness MUST mark this as FAIL, never PASS!
    std::string wrong_err_script = R"(
/*---
negative:
  phase: parse
  type: TypeError
---*/
let invalid syntax ; =
)";
    Test262Result res1 = harness.RunTest(wrong_err_script, "negative_mismatch_syntax_for_typeerror.js");
    assert(res1.kind == Test262ResultKind::Fail);
    assert(res1.message.find("TypeError") != std::string::npos);

    // Case 2: Test expects TypeError, and code throws TypeError.
    // The harness MUST mark this as PASS.
    std::string matching_type_script = R"(
/*---
negative:
  phase: runtime
  type: TypeError
---*/
throw new TypeError("correct type error");
)";
    Test262Result res2 = harness.RunTest(matching_type_script, "negative_match_typeerror.js");
    assert(res2.kind == Test262ResultKind::Pass);

    // Case 3: Test expects ReferenceError, but code throws TypeError.
    // The harness MUST mark this as FAIL.
    std::string wrong_runtime_script = R"(
/*---
negative:
  phase: runtime
  type: ReferenceError
---*/
throw new TypeError("wrong error type");
)";
    Test262Result res3 = harness.RunTest(wrong_runtime_script, "negative_mismatch_runtime.js");
    assert(res3.kind == Test262ResultKind::Fail);
    assert(res3.message.find("ReferenceError") != std::string::npos);

    // Case 4: Test expects SyntaxError and produces SyntaxError.
    // The harness MUST mark this as PASS.
    std::string matching_syntax_script = R"(
/*---
negative:
  phase: parse
  type: SyntaxError
---*/
let bad +++ syntax;
)";
    Test262Result res4 = harness.RunTest(matching_syntax_script, "negative_match_syntaxerror.js");
    assert(res4.kind == Test262ResultKind::Pass);

    std::cout << "  -> Negative test error matching strictly verified: mismatch errors correctly marked as failures.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Test262 Conformance Suite & Harness Tests\n";
    std::cout << "=========================================================\n\n";

    TestFrontmatterParsing();
    TestDirectHarnessAssertions();
    TestCuratedTest262Suite();
    TestNegativeErrorTypeMismatch();

    std::cout << "\n[PASS] All Test262 Conformance Suite Tests Passed Successfully!\n";
    return 0;
}
