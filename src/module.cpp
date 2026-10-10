#include "aether/module.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace aether {

ModuleLoader::ModuleLoader(Agent& agent)
    : agent_(agent) {}

void ModuleLoader::RegisterVirtualModule(std::string_view specifier, std::string_view source) {
    virtual_modules_[std::string(specifier)] = std::string(source);
}

TaggedValue ModuleLoader::Require(std::string_view specifier, std::string_view referrer_dir) {
    s_current_loader_ = this;
    std::string spec(specifier);
    std::string resolved_id = spec;
    std::string resolved_filename = spec;
    std::string resolved_dirname = "";
    std::string source_code;

    // 1. Check virtual modules registry
    auto v_it = virtual_modules_.find(spec);
    if (v_it != virtual_modules_.end()) {
        resolved_id = spec;
        source_code = v_it->second;
    } else {
        // 2. Check filesystem resolution
        std::filesystem::path target_path(spec);
        if (target_path.is_relative() && !referrer_dir.empty()) {
            target_path = std::filesystem::path(std::string(referrer_dir)) / target_path;
        }

        if (!std::filesystem::exists(target_path) && !target_path.has_extension()) {
            std::filesystem::path with_ext = target_path;
            with_ext.replace_extension(".js");
            if (std::filesystem::exists(with_ext)) {
                target_path = with_ext;
            }
        }

        if (std::filesystem::exists(target_path)) {
            std::error_code ec;
            std::filesystem::path canonical_p = std::filesystem::canonical(target_path, ec);
            resolved_filename = ec ? target_path.string() : canonical_p.string();
            resolved_dirname = std::filesystem::path(resolved_filename).parent_path().string();
            resolved_id = resolved_filename;

            std::ifstream file(resolved_filename, std::ios::in | std::ios::binary);
            if (!file) {
                throw std::runtime_error("Cannot read module file: " + resolved_filename);
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            source_code = ss.str();
        } else {
            throw std::runtime_error("Cannot find module '" + spec + "'");
        }
    }

    // 3. Check module cache
    auto cache_it = module_cache_.find(resolved_id);
    if (cache_it != module_cache_.end()) {
        return cache_it->second->module_object->GetNamedProperty("exports");
    }

    // 4. Create ModuleRecord and insert into cache early (supports circular dependencies)
    uint32_t shape_id = 1000;
    auto record = std::make_shared<ModuleRecord>();
    record->id = resolved_id;
    record->filename = resolved_filename;
    record->dirname = resolved_dirname;
    record->module_object = new JSObject(agent_.GetRealm().GetRootShape());
    auto* exports_obj = new JSObject(agent_.GetRealm().GetRootShape());
    record->module_object->SetNamedProperty("exports", TaggedValue::FromHeapObject(exports_obj), shape_id);
    record->module_object->SetNamedProperty("id", TaggedValue::FromHeapObject(new JSString(resolved_id)), shape_id);
    record->module_object->SetNamedProperty("loaded", TaggedValue::FromBoolean(false), shape_id);
    module_cache_[resolved_id] = record;

    // 5. Wrap source code in CommonJS wrapper function
    std::string wrapped = "(function(exports, require, module, __filename, __dirname) {\n" + source_code + "\n})";
    TaggedValue wrapper_fn = agent_.EvaluateScript(wrapped);

    // 6. Create module-specific require function
    std::string cur_dir = resolved_dirname;
    auto* local_require = new JSNativeFunction("require", [this, cur_dir](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::Undefined();
        std::string s = args[0].ToString();
        return this->Require(s, cur_dir);
    });

    // 7. Execute module wrapper
    TaggedValue exports_val = record->module_object->GetNamedProperty("exports");
    TaggedValue require_val = TaggedValue::FromHeapObject(local_require);
    TaggedValue module_val = TaggedValue::FromHeapObject(record->module_object);
    TaggedValue filename_val = TaggedValue::FromHeapObject(new JSString(resolved_filename));
    TaggedValue dirname_val = TaggedValue::FromHeapObject(new JSString(resolved_dirname));

    CallFunction(wrapper_fn, TaggedValue::Undefined(), {
        exports_val,
        require_val,
        module_val,
        filename_val,
        dirname_val
    });

    record->loaded = true;
    record->module_object->SetNamedProperty("loaded", TaggedValue::FromBoolean(true), shape_id);
    return record->module_object->GetNamedProperty("exports");
}

void ModuleLoader::InitializeModuleGlobals(const std::vector<std::string>& cli_args) {
    s_current_loader_ = this;
    JSObject* global = agent_.GetRealm().GetGlobalObject();
    uint32_t shape_id = 2000;

    // 1. global require
    auto* global_require = new JSNativeFunction("require", [this](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::Undefined();
        std::string spec = args[0].ToString();
        return this->Require(spec, "");
    });
    global->SetNamedProperty("require", TaggedValue::FromHeapObject(global_require), shape_id);

    // 2. process object
    auto* process_obj = new JSObject(agent_.GetRealm().GetRootShape());

    // process.argv
    auto* argv_arr = new JSArray(agent_.GetRealm().GetRootShape(), cli_args.size());
    if (agent_.GetRealm().GetArrayPrototype()) {
        argv_arr->SetPrototype(agent_.GetRealm().GetArrayPrototype());
    }
    for (size_t i = 0; i < cli_args.size(); ++i) {
        argv_arr->SetElement(static_cast<uint32_t>(i), TaggedValue::FromHeapObject(new JSString(cli_args[i])));
    }
    process_obj->SetNamedProperty("argv", TaggedValue::FromHeapObject(argv_arr), shape_id);

    // process.version
    process_obj->SetNamedProperty("version", TaggedValue::FromHeapObject(new JSString("v22.0.0-aether")), shape_id);

    // process.platform
#if defined(_WIN32)
    process_obj->SetNamedProperty("platform", TaggedValue::FromHeapObject(new JSString("win32")), shape_id);
#elif defined(__APPLE__)
    process_obj->SetNamedProperty("platform", TaggedValue::FromHeapObject(new JSString("darwin")), shape_id);
#else
    process_obj->SetNamedProperty("platform", TaggedValue::FromHeapObject(new JSString("linux")), shape_id);
#endif

    // process.arch
#if defined(_M_X64) || defined(__x86_64__)
    process_obj->SetNamedProperty("arch", TaggedValue::FromHeapObject(new JSString("x64")), shape_id);
#elif defined(_M_ARM64) || defined(__aarch64__)
    process_obj->SetNamedProperty("arch", TaggedValue::FromHeapObject(new JSString("arm64")), shape_id);
#else
    process_obj->SetNamedProperty("arch", TaggedValue::FromHeapObject(new JSString("ia32")), shape_id);
#endif

    // process.cwd()
    auto* cwd_fn = new JSNativeFunction("cwd", [](const std::vector<TaggedValue>&) -> TaggedValue {
        std::string cwd = std::filesystem::current_path().string();
        return TaggedValue::FromHeapObject(new JSString(cwd));
    });
    process_obj->SetNamedProperty("cwd", TaggedValue::FromHeapObject(cwd_fn), shape_id);

    // process.exit(code)
    auto* exit_fn = new JSNativeFunction("exit", [this](const std::vector<TaggedValue>& args) -> TaggedValue {
        int code = 0;
        if (!args.empty()) code = static_cast<int>(args[0].AsNumber());
        this->SetExit(code);
        this->agent_.GetEventLoop().Stop();
        return TaggedValue::Undefined();
    });
    process_obj->SetNamedProperty("exit", TaggedValue::FromHeapObject(exit_fn), shape_id);

    // process.nextTick(callback)
    auto* nextTick_fn = new JSNativeFunction("nextTick", [this](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!args.empty()) {
            TaggedValue cb = args[0];
            this->agent_.GetMicrotasks().Enqueue([cb]() {
                CallFunction(cb, TaggedValue::Undefined(), {});
            });
        }
        return TaggedValue::Undefined();
    });
    process_obj->SetNamedProperty("nextTick", TaggedValue::FromHeapObject(nextTick_fn), shape_id);

    global->SetNamedProperty("process", TaggedValue::FromHeapObject(process_obj), shape_id);
}

} // namespace aether
