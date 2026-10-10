#ifndef AETHER_TEST262_H
#define AETHER_TEST262_H

#include "aether/common.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <memory>

namespace aether {

class Agent;

/**
 * @brief Parsed metadata from official Test262 YAML frontmatter.
 */
struct Test262Metadata {
    std::string description;
    std::string info;
    std::vector<std::string> features;
    std::vector<std::string> flags;
    std::vector<std::string> includes;
    bool is_negative{false};
    std::string negative_phase;
    std::string negative_type;

    bool HasFlag(std::string_view flag) const noexcept {
        for (const auto& f : flags) {
            if (f == flag) return true;
        }
        return false;
    }

    bool HasInclude(std::string_view inc) const noexcept {
        for (const auto& i : includes) {
            if (i == inc) return true;
        }
        return false;
    }
};

/**
 * @brief Parses YAML-formatted frontmatter from a Test262 test script.
 */
Test262Metadata ParseTest262Frontmatter(std::string_view source);

/**
 * @brief Status enumeration for a single Test262 execution.
 */
enum class Test262ResultKind {
    Pass,
    Fail,
    Skip
};

/**
 * @brief Detailed result of executing a Test262 test case.
 */
struct Test262Result {
    Test262ResultKind kind{Test262ResultKind::Pass};
    std::string test_name;
    std::string message;
    double duration_ms{0.0};
};

/**
 * @brief Harness runtime for running and evaluating TC39 Test262 compliance tests.
 */
class Test262Harness {
public:
    explicit Test262Harness(std::filesystem::path harness_dir = "test/test262/harness");

    /**
     * @brief Prepares a test script by bundling includes (assert.js, sta.js) and strict mode preamble.
     */
    std::string PrepareScript(std::string_view source, const Test262Metadata& meta);

    /**
     * @brief Executes a single test from source text.
     */
    Test262Result RunTest(std::string_view source, std::string_view test_name = "test.js");

    /**
     * @brief Executes a test file from disk.
     */
    Test262Result RunTestFile(const std::filesystem::path& file_path);

    /**
     * @brief Executes all test files recursively within a directory.
     */
    std::vector<Test262Result> RunTestDirectory(const std::filesystem::path& dir_path);

    const std::filesystem::path& GetHarnessDirectory() const noexcept { return harness_dir_; }
    void SetHarnessDirectory(std::filesystem::path dir) { harness_dir_ = std::move(dir); }

private:
    std::string LoadHarnessFile(std::string_view filename);

    std::filesystem::path harness_dir_;
    std::unordered_map<std::string, std::string> harness_cache_;
};

} // namespace aether

#endif // AETHER_TEST262_H
