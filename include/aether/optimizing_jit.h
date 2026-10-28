#ifndef AETHER_OPTIMIZING_JIT_H
#define AETHER_OPTIMIZING_JIT_H

#include "aether/common.h"
#include "aether/bytecode.h"
#include "aether/assembler.h"
#include "aether/sea_of_nodes.h"
#include "aether/baseline_jit.h"
#include <memory>

namespace aether {

/**
 * @brief Tier 2 Optimizing JIT Compiler powered by Sea-of-Nodes SSA Graph.
 */
class OptimizingJIT {
public:
    static std::unique_ptr<ExecutableMemory> Compile(const BytecodeArray& bytecode, TypeFeedbackVector* feedback = nullptr);

#if defined(__GNUC__) || defined(__clang__)
    __attribute__((no_sanitize("function", "undefined")))
#endif
    static TaggedValue Invoke(const ExecutableMemory& exec, const TaggedValue* args = nullptr, uint32_t count = 0) {
        auto fn = exec.AsFunction<JITEntrypoint>();
        uint64_t raw = fn(reinterpret_cast<const uint64_t*>(args), count);
        return TaggedValue(raw);
    }

    // Threshold invocation counter for triggering Tier 2 compilation
    static constexpr uint32_t kInvocationThreshold = 100;

    static bool CanCompile(const BytecodeArray& bytecode);

private:
    static int32_t GetRegisterOffset(uint8_t reg) noexcept {
        return -(56 + (static_cast<int32_t>(reg) + 1) * 8);
    }
};

} // namespace aether

#endif // AETHER_OPTIMIZING_JIT_H
