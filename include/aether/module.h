#ifndef AETHER_MODULE_H
#define AETHER_MODULE_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

namespace aether {

class Agent;
class Realm;
class ModuleLoader;
class Program;
class BytecodeArray;
class NativeRootSet;

enum class ModuleStatus {
    Unlinked,
    Linking,
    Linked,
    Evaluating,
    Evaluated,
    Errored
};

struct ImportEntry {
    std::string module_request;
    std::string import_name;    // "name", "default", or "*"
    std::string local_name;
};

struct ExportEntry {
    std::string export_name;    // exported symbol
    std::string local_name;     // local symbol in this module
    std::string module_request; // re-export source module if any
    std::string import_name;    // re-export import name if any
};

/**
 * @brief ECMAScript 2024 Source Text Module Record.
 */
class SourceTextModuleRecord {
public:
    SourceTextModuleRecord(std::string_view specifier, Realm& realm);
    ~SourceTextModuleRecord();

    const std::string& Specifier() const noexcept { return specifier_; }
    ModuleStatus Status() const noexcept { return status_; }
    void SetStatus(ModuleStatus s) noexcept { status_ = s; }

    Program* GetAST() const noexcept { return ast_.get(); }
    BytecodeArray* GetBytecode() const noexcept { return bytecode_.get(); }
    JSObject* GetEnvironment() const noexcept { return environment_; }
    JSObject* GetNamespaceObject(Agent& agent);
    JSObject* GetImportMeta(Realm* realm);

    const std::vector<std::string>& RequestedModules() const noexcept { return requested_modules_; }
    const std::vector<ImportEntry>& ImportEntries() const noexcept { return import_entries_; }
    const std::vector<ExportEntry>& ExportEntries() const noexcept { return export_entries_; }

    void Parse(std::string_view source);
    void Link(ModuleLoader& loader);
    TaggedValue Evaluate(Agent& agent);

    struct ResolvedBinding {
        SourceTextModuleRecord* module{nullptr};
        std::string export_name{};
        bool is_namespace{false};
        bool is_ambiguous{false};

        ResolvedBinding() = default;
        ResolvedBinding(SourceTextModuleRecord* mod, std::string name, bool is_ns = false, bool is_amb = false)
            : module(mod), export_name(std::move(name)), is_namespace(is_ns), is_ambiguous(is_amb) {}
    };
    ResolvedBinding ResolveExport(std::string_view name, std::unordered_set<SourceTextModuleRecord*>& seen);

    struct ResolvedImport {
        JSObject* target_env{nullptr};
        std::string target_property;
        bool is_namespace{false};
        JSObject* namespace_obj{nullptr};
    };
    const std::unordered_map<std::string, ResolvedImport>& ResolvedImports() const { return resolved_imports_; }
    const std::unordered_set<std::string>& ModuleVariables() const { return module_variables_; }

    bool HasTopLevelAwait() const noexcept { return has_top_level_await_; }
    JSPromise* TopLevelPromise() const noexcept { return top_level_promise_; }

private:
    std::string specifier_;
    Realm& realm_;
    ModuleStatus status_{ModuleStatus::Unlinked};
    std::string source_code_;
    std::unique_ptr<Program> ast_;
    std::unique_ptr<BytecodeArray> bytecode_;

    std::vector<std::string> requested_modules_;
    std::vector<ImportEntry> import_entries_;
    std::vector<ExportEntry> export_entries_;

    // The loader owns records; dependency cycles must not retain host roots.
    std::unordered_map<std::string, std::weak_ptr<SourceTextModuleRecord>> loaded_dependencies_;
    std::unordered_map<std::string, ResolvedImport> resolved_imports_;
    std::unordered_set<std::string> module_variables_;

    JSObject* environment_{nullptr};
    JSObject* namespace_object_{nullptr};
    JSObject* import_meta_{nullptr};
    JSPromise* top_level_promise_{nullptr};
    bool has_top_level_await_{false};
    std::unique_ptr<NativeRootSet> roots_;
    std::unique_ptr<NativeRootSet> ast_roots_;
    std::unique_ptr<NativeRootSet> bytecode_roots_;
};

/**
 * @brief CommonJS Module metadata and cached export record.
 */
struct ModuleRecord {
    ModuleRecord();
    ~ModuleRecord();
    std::string id;
    std::string filename;
    std::string dirname;
    JSObject* module_object{nullptr};
    bool loaded{false};
    std::unique_ptr<NativeRootSet> roots;
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
     * @brief Loads an ECMAScript module by specifier (resolving virtual or filesystem).
     */
    std::shared_ptr<SourceTextModuleRecord> LoadModule(std::string_view specifier, std::string_view referrer_dir = "");

    /**
     * @brief Recursively links an ECMAScript module and its dependencies.
     */
    void LinkModule(SourceTextModuleRecord* module);

    /**
     * @brief Evaluates an ECMAScript module graph.
     */
    TaggedValue EvaluateModule(SourceTextModuleRecord* module);

    /**
     * @brief Asynchronous dynamic import(specifier) returning a JSPromise.
     */
    TaggedValue ImportDynamic(std::string_view specifier, std::string_view referrer_dir = "");

    /**
     * @brief Injects Node.js-compatible environment globals (process, require) into the Realm.
     */
    void InitializeModuleGlobals(const std::vector<std::string>& args = {});

    int GetExitCode() const noexcept { return exit_code_; }
    bool HasExited() const noexcept { return has_exited_; }
    void SetExit(int code) noexcept { has_exited_ = true; exit_code_ = code; }

    Agent& GetAgent() noexcept { return agent_; }

private:
    static inline thread_local ModuleLoader* s_current_loader_{nullptr};

    Agent& agent_;
    std::unordered_map<std::string, std::string> virtual_modules_;
    std::unordered_map<std::string, std::shared_ptr<ModuleRecord>> module_cache_;
    std::unordered_map<std::string, std::shared_ptr<SourceTextModuleRecord>> esm_cache_;
    bool has_exited_{false};
    int exit_code_{0};
};

} // namespace aether

#endif // AETHER_MODULE_H
