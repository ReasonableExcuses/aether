#include "aether/test262.h"
#include "aether/runtime.h"
#include "aether/interpreter.h"
#include "aether/event_loop.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>
#include <algorithm>

namespace aether {

// Built-in standard Test262 harness fallback implementations
static const char* kBuiltinAssertJs = R"JS(
function Test262Error(message) {
    this.name = "Test262Error";
    this.message = message || "";
}
Test262Error.prototype = Object.create(Error.prototype);
Test262Error.prototype.constructor = Test262Error;
Test262Error.prototype.toString = function() {
    return "Test262Error: " + this.message;
};

function assert(mustBeTrue, message) {
    if (mustBeTrue === true) {
        return;
    }
    if (mustBeTrue === undefined) {
        throw new Test262Error("assert statement must have at least one argument");
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected true but got " + mustBeTrue);
}

assert._isSameValue = function(a, b) {
    if (a === b) {
        if (a === 0) return 1 / a === 1 / b;
        return true;
    }
    if (typeof a === "number" && typeof b === "number" && isNaN(a) && isNaN(b)) {
        return true;
    }
    return false;
};

assert.sameValue = function(actual, expected, message) {
    if (assert._isSameValue(actual, expected)) {
        return;
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected SameValue(" + actual + ", " + expected + ") to be true");
};

assert.notSameValue = function(actual, unexpected, message) {
    if (!assert._isSameValue(actual, unexpected)) {
        return;
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected SameValue(" + actual + ", " + unexpected + ") to be false");
};

assert.throws = function(expectedErrorConstructor, func, message) {
    if (typeof func !== "function") {
        throw new Test262Error("assert.throws requires a function argument");
    }
    var threw = false;
    try {
        func();
    } catch (actual) {
        threw = true;
        if (typeof expectedErrorConstructor === "function") {
            if (actual instanceof expectedErrorConstructor) {
                return;
            }
            if (actual && actual.name === expectedErrorConstructor.name) {
                return;
            }
            throw new Test262Error((message ? message + ": " : "") + "Expected a " + expectedErrorConstructor.name + " to be thrown but got " + actual);
        }
        return;
    }
    if (!threw) {
        throw new Test262Error((message ? message + ": " : "") + "Expected exception to be thrown, but none was");
    }
};

assert.compareArray = function(actual, expected, message) {
    if (!Array.isArray(actual) || !Array.isArray(expected)) {
        throw new Test262Error((message ? message + ": " : "") + "Expected both arguments to be arrays");
    }
    if (actual.length !== expected.length) {
        throw new Test262Error((message ? message + ": " : "") + "Array lengths differ: " + actual.length + " vs " + expected.length);
    }
    for (var i = 0; i < actual.length; i++) {
        if (!assert._isSameValue(actual[i], expected[i])) {
            throw new Test262Error((message ? message + ": " : "") + "Array elements at index " + i + " differ");
        }
    }
};
)JS";

static const char* kBuiltinStaJs = R"JS(
var $ERROR = function(err) {
    if (typeof err === "string") {
        throw new Test262Error(err);
    }
    throw err;
};

var $DONE = function(err) {
    if (err) {
        $ERROR(err);
    }
};
)JS";

static std::string Trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && (std::isspace(static_cast<unsigned char>(s[start])))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && (std::isspace(static_cast<unsigned char>(s[end - 1])))) {
        end--;
    }
    return std::string(s.substr(start, end - start));
}

static std::vector<std::string> ParseYamlList(std::string_view text) {
    std::vector<std::string> result;
    std::string trimmed = Trim(text);
    if (trimmed.empty()) return result;

    if (trimmed.front() == '[' && trimmed.back() == ']') {
        std::string inner = trimmed.substr(1, trimmed.size() - 2);
        std::stringstream ss(inner);
        std::string item;
        while (std::getline(ss, item, ',')) {
            std::string t = Trim(item);
            if (!t.empty()) result.push_back(t);
        }
    } else {
        std::string t = Trim(text);
        if (!t.empty()) result.push_back(t);
    }
    return result;
}

Test262Metadata ParseTest262Frontmatter(std::string_view source) {
    Test262Metadata meta;
    size_t start_pos = source.find("/*---");
    if (start_pos == std::string_view::npos) {
        return meta;
    }
    size_t end_pos = source.find("---*/", start_pos + 5);
    if (end_pos == std::string_view::npos) {
        return meta;
    }

    std::string_view yaml_text = source.substr(start_pos + 5, end_pos - (start_pos + 5));
    std::stringstream ss{std::string(yaml_text)};
    std::string line;
    bool in_negative = false;

    while (std::getline(ss, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;

        if (trimmed.find("negative:") == 0) {
            meta.is_negative = true;
            in_negative = true;
            continue;
        }

        if (in_negative) {
            if (trimmed.find("phase:") == 0) {
                meta.negative_phase = Trim(trimmed.substr(6));
                continue;
            } else if (trimmed.find("type:") == 0) {
                meta.negative_type = Trim(trimmed.substr(5));
                continue;
            } else if (line.find("  ") != 0 && line.find("\t") != 0) {
                in_negative = false;
            }
        }

        if (trimmed.find("description:") == 0) {
            meta.description = Trim(trimmed.substr(12));
        } else if (trimmed.find("features:") == 0) {
            auto items = ParseYamlList(trimmed.substr(9));
            meta.features.insert(meta.features.end(), items.begin(), items.end());
        } else if (trimmed.find("flags:") == 0) {
            auto items = ParseYamlList(trimmed.substr(6));
            meta.flags.insert(meta.flags.end(), items.begin(), items.end());
        } else if (trimmed.find("includes:") == 0) {
            auto items = ParseYamlList(trimmed.substr(9));
            meta.includes.insert(meta.includes.end(), items.begin(), items.end());
        }
    }

    return meta;
}

Test262Harness::Test262Harness(std::filesystem::path harness_dir)
    : harness_dir_(std::move(harness_dir)) {
    harness_cache_["assert.js"] = kBuiltinAssertJs;
    harness_cache_["sta.js"] = kBuiltinStaJs;
}

std::string Test262Harness::LoadHarnessFile(std::string_view filename) {
    std::string key(filename);
    auto it = harness_cache_.find(key);
    if (it != harness_cache_.end()) {
        return it->second;
    }

    std::filesystem::path file_path = harness_dir_ / key;
    if (std::filesystem::exists(file_path)) {
        std::ifstream ifs(file_path, std::ios::in | std::ios::binary);
        if (ifs.is_open()) {
            std::ostringstream ss;
            ss << ifs.rdbuf();
            std::string content = ss.str();
            harness_cache_[key] = content;
            return content;
        }
    }

    if (key == "assert.js") return kBuiltinAssertJs;
    if (key == "sta.js") return kBuiltinStaJs;
    return "";
}

std::string Test262Harness::PrepareScript(std::string_view source, const Test262Metadata& meta) {
    std::ostringstream bundled;

    if (meta.HasFlag("onlyStrict")) {
        bundled << "\"use strict\";\n";
    }

    if (!meta.HasFlag("raw")) {
        bundled << LoadHarnessFile("assert.js") << "\n";
        bundled << LoadHarnessFile("sta.js") << "\n";

        for (const auto& inc : meta.includes) {
            if (inc != "assert.js" && inc != "sta.js") {
                std::string file_content = LoadHarnessFile(inc);
                if (!file_content.empty()) {
                    bundled << file_content << "\n";
                }
            }
        }
    }

    bundled << source << "\n";
    return bundled.str();
}

Test262Result Test262Harness::RunTest(std::string_view source, std::string_view test_name) {
    auto start_time = std::chrono::steady_clock::now();
    Test262Result result;
    result.test_name = std::string(test_name);

    Test262Metadata meta = ParseTest262Frontmatter(source);
    std::string prepared = PrepareScript(source, meta);

    try {
        Agent agent;
        agent.EvaluateScriptWithEventLoop(prepared);

        if (meta.is_negative) {
            result.kind = Test262ResultKind::Fail;
            result.message = "Expected negative error (" + meta.negative_type + ") in phase " + meta.negative_phase + ", but script completed successfully.";
        } else {
            if (agent.GetInterpreter().HasPendingException()) {
                TaggedValue exc = agent.GetInterpreter().PendingException();
                result.kind = Test262ResultKind::Fail;
                result.message = "Uncaught pending exception: " + exc.ToString();
            } else {
                result.kind = Test262ResultKind::Pass;
            }
        }
    } catch (const std::exception& ex) {
        std::string err = ex.what();
        if (meta.is_negative) {
            if (err.find(meta.negative_type) != std::string::npos || (meta.negative_type == "SyntaxError" && err.find("SyntaxError") != std::string::npos)) {
                result.kind = Test262ResultKind::Pass;
            } else {
                result.kind = Test262ResultKind::Fail;
                result.message = "Wrong exception thrown: expected " + meta.negative_type + ", got: " + err;
            }
        } else {
            result.kind = Test262ResultKind::Fail;
            result.message = "Exception thrown: " + err;
        }
    } catch (...) {
        result.kind = Test262ResultKind::Fail;
        result.message = "Unknown unhandled native exception thrown";
    }

    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
    result.duration_ms = elapsed.count();
    return result;
}

Test262Result Test262Harness::RunTestFile(const std::filesystem::path& file_path) {
    if (!std::filesystem::exists(file_path)) {
        Test262Result res;
        res.test_name = file_path.string();
        res.kind = Test262ResultKind::Fail;
        res.message = "File not found: " + file_path.string();
        return res;
    }

    std::ifstream ifs(file_path, std::ios::in | std::ios::binary);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();

    return RunTest(source, file_path.filename().string());
}

std::vector<Test262Result> Test262Harness::RunTestDirectory(const std::filesystem::path& dir_path) {
    std::vector<Test262Result> results;
    if (!std::filesystem::exists(dir_path)) {
        return results;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            results.push_back(RunTestFile(entry.path()));
        }
    }

    return results;
}

} // namespace aether
