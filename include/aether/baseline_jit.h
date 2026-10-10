#ifndef AETHER_BASELINE_JIT_H
#define AETHER_BASELINE_JIT_H

#include "aether/common.h"
#include "aether/bytecode.h"
#include "aether/assembler.h"
#include <memory>

namespace aether {

using JITEntrypoint = uint64_t (*)(const uint64_t* args, uint32_t arg_count);

/**
 * @brief Tier 1 Baseline JIT Compiler generating native machine code directly from Bytecode.
 */
class BaselineJIT {
public:
    static bool CanCompile(const BytecodeArray& bytecode);
    static std::unique_ptr<ExecutableMemory> Compile(const BytecodeArray& bytecode);

    static TaggedValue Invoke(const ExecutableMemory& exec, const TaggedValue* args = nullptr, uint32_t count = 0) {
        auto fn = exec.AsFunction<JITEntrypoint>();
        uint64_t raw = fn(reinterpret_cast<const uint64_t*>(args), count);
        return TaggedValue(raw);
    }

    // Threshold invocation counter for triggering Tier 1 compilation
    static constexpr uint32_t kInvocationThreshold = 5;

private:
    static int32_t GetRegisterOffset(uint8_t reg) noexcept {
        // Base callee-saved space = 56 bytes; each virtual register is 8 bytes
        return -(56 + (static_cast<int32_t>(reg) + 1) * 8);
    }
};

} // namespace aether

#endif // AETHER_BASELINE_JIT_H
