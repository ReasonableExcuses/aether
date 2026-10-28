#include "aether/options.h"
#include <string_view>
#include <algorithm>
#include <stdexcept>

namespace aether {

void Options::Parse(std::vector<std::string>& args) {
    if (args.empty()) return;

    std::vector<std::string> remaining;
    remaining.push_back(args[0]); // binary name

    bool stop_flags = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (stop_flags || arg.empty() || arg[0] != '-') {
            remaining.push_back(arg);
            if (arg != "-e" && arg != "--eval") {
                stop_flags = true;
            }
            continue;
        }

        if (arg == "--") {
            stop_flags = true;
            continue;
        }

        if (arg == "--gc-stress") {
            Get().gc_stress = 1;
        } else if (arg.rfind("--gc-stress=", 0) == 0) {
            try {
                Get().gc_stress = std::max(1, std::stoi(arg.substr(12)));
            } catch (...) {
                Get().gc_stress = 1;
            }
        } else if (arg == "--gc-verify") {
            Get().gc_verify = true;
        } else if (arg == "--trace-gc") {
            Get().trace_gc = true;
        } else if (arg == "--gc-at-safepoints") {
            Get().gc_at_safepoints = true;
        } else if (arg.rfind("--max-heap=", 0) == 0) {
            try {
                Get().max_heap_mb = static_cast<size_t>(std::stoull(arg.substr(11)));
            } catch (...) {}
        } else if (arg == "--interpreter-only") {
            Get().interpreter_only = true;
        } else if (arg == "--always-baseline") {
            Get().always_baseline = true;
        } else if (arg == "--always-optimize") {
            Get().always_optimize = true;
        } else if (arg.rfind("--tier-threshold=", 0) == 0) {
            try {
                Get().tier_threshold = std::max(0, std::stoi(arg.substr(17)));
            } catch (...) {}
        } else if (arg.rfind("--stack-size=", 0) == 0) {
            try {
                Get().stack_size_kb = static_cast<size_t>(std::stoull(arg.substr(13)));
            } catch (...) {}
        } else {
            // Unrecognized or standard option (-v, -h, -e)
            remaining.push_back(arg);
            if (arg == "-e" || arg == "--eval") {
                if (i + 1 < args.size()) {
                    remaining.push_back(args[++i]);
                }
            }
        }
    }

    args = std::move(remaining);
}

void Options::ParseEnv() noexcept {
    const char* env = std::getenv("AETHER_FLAGS");
    if (!env || !*env) return;
    try {
        std::string env_str(env);
        std::vector<std::string> args = {"aether"};
        std::string token;
        for (char c : env_str) {
            if (c == ' ' || c == '\t') {
                if (!token.empty()) {
                    args.push_back(token);
                    token.clear();
                }
            } else {
                token.push_back(c);
            }
        }
        if (!token.empty()) args.push_back(token);
        Parse(args);
    } catch (...) {}
}

} // namespace aether

