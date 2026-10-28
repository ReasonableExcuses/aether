#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace aether {

struct Options {
    int gc_stress{0};                   // Collect every N allocations (0 = disabled, 1 if flag given without =N)
    bool gc_verify{false};              // Run heap verifier before/after every GC
    bool trace_gc{false};               // Print GC traces to stdout/stderr
    bool gc_at_safepoints{false};       // Trigger GC at safepoints
    size_t max_heap_mb{0};              // Maximum heap budget in MB (0 = default 4GB)
    bool interpreter_only{false};       // Disable all JIT compilation and tiering
    bool always_baseline{false};        // Immediately compile to Tier 1 Baseline JIT
    bool always_optimize{false};        // Immediately compile to Tier 2 Optimizing JIT
    int tier_threshold{0};              // Invocations before Tier 1/2 compilation (0 = default 5/100)
    size_t stack_size_kb{0};            // Native stack limit in KB (0 = default)

    void ParseEnv() noexcept;

    static Options& Get() noexcept {
        static Options s_instance;
        return s_instance;
    }

    static void Reset() noexcept {
        Get() = Options();
#if defined(AETHER_GC_STRESS_DEFAULT) && AETHER_GC_STRESS_DEFAULT
        Get().gc_stress = 1;
#endif
    }

    // Parses engine flags from the args vector, removing matched flags.
    // Leaves argv[0] (binary), script path, and script arguments in args.
    static void Parse(std::vector<std::string>& args);
};

} // namespace aether
