#ifndef AETHER_MODULE_H
#define AETHER_MODULE_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <vector>

namespace aether {

class Agent;

/**
 * @brief CommonJS Module metadata and cached export record.
 */
struct ModuleRecord {
    std::string id;
    std::string filename;
    std::string dirname;
    JSObject* module_object{nullptr};
    bool loaded{false};
};

/**
 * @brief CommonJS & ESM Module resolution, caching, and Node.js-compatible process environment.
 */
class ModuleLoader {
public:
    explicit ModuleLoader(Agent& agent);
    ~ModuleLoader() = default;

    static ModuleLoader* Current() noexcept { return s_current_loader_; }
    static void SetCurrent(ModuleLoader* loader) noexcept { s_current_loader_ = loader; }

    /**
     * @brief Registers an in-memory virtual module source for embedded execution or testing.
     */
    void RegisterVirtualModule(std::string_view specifier, std::string_view source);

    /**
     * @brief CommonJS require implementation resolving specifiers from virtual registry or filesystem.
     */
    TaggedValue Require(std::string_view specifier, std::string_view referrer_dir = "");

    /**
     * @brief Injects Node.js-compatible environment globals (process, require) into the Realm.
     */
    void InitializeModuleGlobals(const std::vector<std::string>& args = {});

    int GetExitCode() const noexcept { return exit_code_; }
    bool HasExited() const noexcept { return has_exited_; }
    void SetExit(int code) noexcept { has_exited_ = true; exit_code_ = code; }

private:
    static inline thread_local ModuleLoader* s_current_loader_{nullptr};

    Agent& agent_;
    std::unordered_map<std::string, std::string> virtual_modules_;
    std::unordered_map<std::string, std::shared_ptr<ModuleRecord>> module_cache_;
    bool has_exited_{false};
    int exit_code_{0};
};

} // namespace aether

#endif // AETHER_MODULE_H
