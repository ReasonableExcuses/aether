#include "aether/module.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/parser.h"
#include "aether/lexer.h"
#include "aether/bytecode_emitter.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace aether {

// ============================================================================
// SourceTextModuleRecord Implementation
// ============================================================================

SourceTextModuleRecord::SourceTextModuleRecord(std::string_view specifier, Realm& realm)
    : specifier_(specifier), realm_(realm) {
    environment_ = new JSObject(realm_.GetRootShape());
}

SourceTextModuleRecord::~SourceTextModuleRecord() = default;

void SourceTextModuleRecord::Parse(std::string_view source) {
    source_code_ = std::string(source);
    Lexer lexer(source_code_);
    Parser parser(lexer);
    ast_ = parser.ParseProgram();

    requested_modules_.clear();
    import_entries_.clear();
    export_entries_.clear();
    module_variables_.clear();
    has_top_level_await_ = false;

    // Top-Level Await Detection Visitor
    class AwaitScanner : public ASTVisitor {
    public:
        bool has_await{false};
        void VisitLiteral(LiteralExpr*) override {}
        void VisitIdentifier(IdentifierExpr*) override {}
        void VisitUnary(UnaryExpr* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
        void VisitBinary(BinaryExpr* n) override {
            if (n->Lhs()) n->Lhs()->Accept(this);
            if (n->Rhs()) n->Rhs()->Accept(this);
        }
        void VisitAssign(AssignExpr* n) override { if (n->Value()) n->Value()->Accept(this); }
        void VisitArrayDestructureAssign(ArrayDestructureAssignExpr* n) override { if (n->Value()) n->Value()->Accept(this); }
        void VisitUpdate(UpdateExpr*) override {}
        void VisitPropertyAccess(PropertyAccessExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
        void VisitPropertyAssign(PropertyAssignExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Value()) n->Value()->Accept(this);
        }
        void VisitCall(CallExpr* n) override {
            if (n->Callee()) n->Callee()->Accept(this);
            for (const auto& a : n->Args()) a->Accept(this);
        }
        void VisitNew(NewExpr* n) override {
            if (n->Callee()) n->Callee()->Accept(this);
            for (const auto& a : n->Args()) a->Accept(this);
        }
        void VisitFunctionExpr(FunctionExpr*) override {} // Don't descend into nested functions!
        void VisitObjectLiteral(ObjectLiteralExpr* n) override {
            for (const auto& p : n->Properties()) {
                if (p.computed_key) p.computed_key->Accept(this);
                if (p.value) p.value->Accept(this);
            }
        }
        void VisitArrayLiteral(ArrayLiteralExpr* n) override {
            for (const auto& e : n->Elements()) e->Accept(this);
        }
        void VisitIndexAccess(IndexAccessExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Index()) n->Index()->Accept(this);
        }
        void VisitIndexAssign(IndexAssignExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Index()) n->Index()->Accept(this);
            if (n->Value()) n->Value()->Accept(this);
        }
        void VisitCompoundPropertyAssign(CompoundPropertyAssignExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Rhs()) n->Rhs()->Accept(this);
        }
        void VisitCompoundIndexAssign(CompoundIndexAssignExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Index()) n->Index()->Accept(this);
            if (n->Rhs()) n->Rhs()->Accept(this);
        }
        void VisitConditional(ConditionalExpr* n) override {
            if (n->Condition()) n->Condition()->Accept(this);
            if (n->ThenBranch()) n->ThenBranch()->Accept(this);
            if (n->ElseBranch()) n->ElseBranch()->Accept(this);
        }
        void VisitTemplateLiteral(TemplateLiteralExpr* n) override {
            for (const auto& e : n->Expressions()) e->Accept(this);
        }
        void VisitOptionalPropertyAccess(OptionalPropertyAccessExpr* n) override { if (n->Object()) n->Object()->Accept(this); }
        void VisitOptionalIndexAccess(OptionalIndexAccessExpr* n) override {
            if (n->Object()) n->Object()->Accept(this);
            if (n->Index()) n->Index()->Accept(this);
        }
        void VisitOptionalCall(OptionalCallExpr* n) override {
            if (n->Callee()) n->Callee()->Accept(this);
            for (const auto& a : n->Args()) a->Accept(this);
        }
        void VisitSpread(SpreadExpr* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
        void VisitYield(YieldExpr*) override {}
        void VisitAwait(AwaitExpr*) override { has_await = true; }
        void VisitImportMeta(ImportMetaExpr*) override {}
        void VisitDynamicImport(DynamicImportExpr* n) override { if (n->Specifier()) n->Specifier()->Accept(this); }

        void VisitExprStmt(ExprStmt* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
        void VisitVarDecl(VarDeclStmt* n) override { if (n->Initializer()) n->Initializer()->Accept(this); }
        void VisitBlock(BlockStmt* n) override { for (const auto& s : n->Statements()) s->Accept(this); }
        void VisitIf(IfStmt* n) override {
            if (n->Condition()) n->Condition()->Accept(this);
            if (n->ThenBranch()) n->ThenBranch()->Accept(this);
            if (n->ElseBranch()) n->ElseBranch()->Accept(this);
        }
        void VisitWhile(WhileStmt* n) override {
            if (n->Condition()) n->Condition()->Accept(this);
            if (n->Body()) n->Body()->Accept(this);
        }
        void VisitDoWhile(DoWhileStmt* n) override {
            if (n->Body()) n->Body()->Accept(this);
            if (n->Condition()) n->Condition()->Accept(this);
        }
        void VisitFor(ForStmt* n) override {
            if (n->Init()) n->Init()->Accept(this);
            if (n->Cond()) n->Cond()->Accept(this);
            if (n->Update()) n->Update()->Accept(this);
            if (n->Body()) n->Body()->Accept(this);
        }
        void VisitBreak(BreakStmt*) override {}
        void VisitContinue(ContinueStmt*) override {}
        void VisitSwitch(SwitchStmt* n) override {
            if (n->Discriminant()) n->Discriminant()->Accept(this);
            for (const auto& c : n->Cases()) {
                if (c.test) c.test->Accept(this);
                for (const auto& s : c.statements) s->Accept(this);
            }
        }
        void VisitDebugger(DebuggerStmt*) override {}
        void VisitFunctionDecl(FunctionDeclStmt*) override {} // Don't descend into nested functions!
        void VisitClassDecl(ClassDeclStmt*) override {}
        void VisitImportDecl(ImportDeclStmt*) override {}
        void VisitExportDecl(ExportDeclStmt* n) override {
            if (n->Declaration()) n->Declaration()->Accept(this);
            if (n->DefaultExpr()) n->DefaultExpr()->Accept(this);
        }
        void VisitThrow(ThrowStmt* n) override { if (n->GetExpr()) n->GetExpr()->Accept(this); }
        void VisitTryCatch(TryCatchStmt* n) override {
            if (n->TryBlock()) n->TryBlock()->Accept(this);
            if (n->CatchBlock()) n->CatchBlock()->Accept(this);
            if (n->FinallyBlock()) n->FinallyBlock()->Accept(this);
        }
        void VisitReturn(ReturnStmt* n) override { if (n->Value()) n->Value()->Accept(this); }
        void VisitProgram(Program* n) override { for (const auto& s : n->Statements()) s->Accept(this); }
    };

    AwaitScanner scanner;
    ast_->Accept(&scanner);
    has_top_level_await_ = scanner.has_await;

    // Scan AST declarations
    for (const auto& stmt : ast_->Statements()) {
        if (auto* imp = dynamic_cast<ImportDeclStmt*>(stmt.get())) {
            requested_modules_.push_back(imp->ModuleSpecifier());
            if (!imp->DefaultImport().empty()) {
                import_entries_.push_back(ImportEntry{imp->ModuleSpecifier(), "default", imp->DefaultImport()});
            }
            if (!imp->NamespaceImport().empty()) {
                import_entries_.push_back(ImportEntry{imp->ModuleSpecifier(), "*", imp->NamespaceImport()});
            }
            for (const auto& n : imp->NamedImports()) {
                import_entries_.push_back(ImportEntry{imp->ModuleSpecifier(), n.imported_name, n.local_name});
            }
        } else if (auto* exp = dynamic_cast<ExportDeclStmt*>(stmt.get())) {
            if (exp->IsDefault()) {
                export_entries_.push_back(ExportEntry{"default", "default", "", ""});
                if (auto* fn = dynamic_cast<FunctionDeclStmt*>(exp->Declaration())) {
                    if (!fn->Name().empty()) module_variables_.insert(fn->Name());
                } else if (auto* cls = dynamic_cast<ClassDeclStmt*>(exp->Declaration())) {
                    if (!cls->Name().empty()) module_variables_.insert(cls->Name());
                }
            } else if (exp->IsExportAll()) {
                requested_modules_.push_back(exp->FromSpecifier());
                export_entries_.push_back(ExportEntry{
                    exp->ExportAllAs().empty() ? "*" : exp->ExportAllAs(),
                    "",
                    exp->FromSpecifier(),
                    "*"
                });
            } else if (!exp->NamedExports().empty()) {
                if (!exp->FromSpecifier().empty()) {
                    requested_modules_.push_back(exp->FromSpecifier());
                    for (const auto& n : exp->NamedExports()) {
                        export_entries_.push_back(ExportEntry{n.exported_name, "", exp->FromSpecifier(), n.local_name});
                    }
                } else {
                    for (const auto& n : exp->NamedExports()) {
                        export_entries_.push_back(ExportEntry{n.exported_name, n.local_name, "", ""});
                    }
                }
            } else if (exp->Declaration()) {
                if (auto* v = dynamic_cast<VarDeclStmt*>(exp->Declaration())) {
                    module_variables_.insert(v->Name());
                    export_entries_.push_back(ExportEntry{v->Name(), v->Name(), "", ""});
                } else if (auto* fn = dynamic_cast<FunctionDeclStmt*>(exp->Declaration())) {
                    module_variables_.insert(fn->Name());
                    export_entries_.push_back(ExportEntry{fn->Name(), fn->Name(), "", ""});
                } else if (auto* cls = dynamic_cast<ClassDeclStmt*>(exp->Declaration())) {
                    module_variables_.insert(cls->Name());
                    export_entries_.push_back(ExportEntry{cls->Name(), cls->Name(), "", ""});
                } else if (auto* blk = dynamic_cast<BlockStmt*>(exp->Declaration())) {
                    for (const auto& sub_stmt : blk->Statements()) {
                        if (auto* sub_v = dynamic_cast<VarDeclStmt*>(sub_stmt.get())) {
                            module_variables_.insert(sub_v->Name());
                            export_entries_.push_back(ExportEntry{sub_v->Name(), sub_v->Name(), "", ""});
                        }
                    }
                }
            }
        } else if (auto* v = dynamic_cast<VarDeclStmt*>(stmt.get())) {
            module_variables_.insert(v->Name());
        } else if (auto* fn = dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
            module_variables_.insert(fn->Name());
        } else if (auto* cls = dynamic_cast<ClassDeclStmt*>(stmt.get())) {
            module_variables_.insert(cls->Name());
        } else if (auto* blk = dynamic_cast<BlockStmt*>(stmt.get())) {
            for (const auto& sub_stmt : blk->Statements()) {
                if (auto* blk_v = dynamic_cast<VarDeclStmt*>(sub_stmt.get())) {
                    module_variables_.insert(blk_v->Name());
                }
            }
        }
    }
}

SourceTextModuleRecord::ResolvedBinding SourceTextModuleRecord::ResolveExport(
    std::string_view name, std::unordered_set<SourceTextModuleRecord*>& seen) {
    if (seen.count(this)) {
        return ResolvedBinding{nullptr, "", false, false}; // Cycle
    }
    seen.insert(this);

    // 1. Check direct and indirect export entries
    for (const auto& e : export_entries_) {
        if (e.export_name == name) {
            if (e.module_request.empty()) {
                // Local export
                std::string target_name = e.local_name.empty() ? std::string(name) : e.local_name;
                return ResolvedBinding{this, target_name, false, false};
            } else {
                // Re-export: export { x as y } from "mod"
                auto it = loaded_dependencies_.find(e.module_request);
                if (it != loaded_dependencies_.end() && it->second) {
                    if (e.import_name == "*") {
                        // export * as ns from "mod"
                        return ResolvedBinding{it->second.get(), "", true, false};
                    }
                    return it->second->ResolveExport(e.import_name, seen);
                }
            }
        }
    }

    // 2. Check star re-exports: export * from "mod" (skips "default")
    if (name != "default") {
        for (const auto& e : export_entries_) {
            if (e.export_name == "*" && e.import_name == "*" && !e.module_request.empty()) {
                auto it = loaded_dependencies_.find(e.module_request);
                if (it != loaded_dependencies_.end() && it->second) {
                    auto res = it->second->ResolveExport(name, seen);
                    if (res.module != nullptr) {
                        return res;
                    }
                }
            }
        }
    }

    // 3. Fallback for dynamic/CommonJS synthetic properties
    if (environment_->HasNamedProperty(name)) {
        return ResolvedBinding{this, std::string(name), false, false};
    }

    return ResolvedBinding{nullptr, "", false, false};
}

void SourceTextModuleRecord::Link(ModuleLoader& loader) {
    if (status_ == ModuleStatus::Linked || status_ == ModuleStatus::Evaluating || status_ == ModuleStatus::Evaluated) {
        return;
    }
    status_ = ModuleStatus::Linking;

    std::string referrer_dir = std::filesystem::path(specifier_).parent_path().string();

    // 1. Recursively load and link all requested modules
    for (const auto& req : requested_modules_) {
        if (!loaded_dependencies_.count(req)) {
            auto dep = loader.LoadModule(req, referrer_dir);
            loaded_dependencies_[req] = dep;
            loader.LinkModule(dep.get());
        }
    }

    // 2. Resolve all imported bindings
    resolved_imports_.clear();
    for (const auto& imp : import_entries_) {
        auto dep = loaded_dependencies_[imp.module_request];
        if (!dep) {
            throw std::runtime_error("Cannot find module '" + imp.module_request + "'");
        }
        if (imp.import_name == "*") {
            // Namespace import: import * as ns from "mod"
            resolved_imports_[imp.local_name] = ResolvedImport{
                nullptr,
                "",
                true,
                dep->GetNamespaceObject(loader.GetAgent())
            };
        } else {
            std::unordered_set<SourceTextModuleRecord*> seen;
            auto binding = dep->ResolveExport(imp.import_name, seen);
            if (!binding.module) {
                throw std::runtime_error("SyntaxError: The requested module '" + imp.module_request + 
                                         "' does not provide an export named '" + imp.import_name + "'");
            }
            if (binding.is_namespace) {
                resolved_imports_[imp.local_name] = ResolvedImport{
                    nullptr,
                    "",
                    true,
                    binding.module->GetNamespaceObject(loader.GetAgent())
                };
            } else {
                resolved_imports_[imp.local_name] = ResolvedImport{
                    binding.module->GetEnvironment(),
                    binding.export_name,
                    false,
                    nullptr
                };
            }
        }
    }

    // 3. Compile bytecode for this module
    BytecodeEmitter emitter;
    emitter.SetModuleRecord(this);
    bytecode_ = emitter.Emit(ast_.get());

    status_ = ModuleStatus::Linked;
}

TaggedValue SourceTextModuleRecord::Evaluate(Agent& agent) {
    if (status_ == ModuleStatus::Evaluated) {
        return TaggedValue::Undefined();
    }
    if (status_ == ModuleStatus::Evaluating) {
        return TaggedValue::Undefined(); // Cyclic dependency skip
    }
    status_ = ModuleStatus::Evaluating;

    // 1. Post-order DFS: evaluate dependencies first
    for (const auto& req : requested_modules_) {
        auto it = loaded_dependencies_.find(req);
        if (it != loaded_dependencies_.end() && it->second) {
            if (it->second->Status() != ModuleStatus::Evaluated) {
                it->second->Evaluate(agent);
            }
        }
    }

    // 2. Hoist top-level function declarations
    if (ast_) {
        uint32_t dummy_id = 999;
        for (const auto& stmt : ast_->Statements()) {
            if (auto* fn_decl = dynamic_cast<FunctionDeclStmt*>(stmt.get())) {
                BytecodeEmitter fn_emitter;
                fn_emitter.SetModuleRecord(this);
                auto fn_bc = fn_emitter.EmitFunction(fn_decl);
                auto* fn_obj = new JSFunction(fn_decl->Name(), std::move(fn_bc), nullptr, realm_.GetRootShape());
                fn_obj->SetAsync(fn_decl->IsAsync());
                fn_obj->SetGenerator(fn_decl->IsGenerator());
                environment_->SetNamedProperty(fn_decl->Name(), TaggedValue::FromHeapObject(fn_obj), dummy_id);
            } else if (auto* exp = dynamic_cast<ExportDeclStmt*>(stmt.get())) {
                if (exp->Declaration()) {
                    if (auto* exp_fn_decl = dynamic_cast<FunctionDeclStmt*>(exp->Declaration())) {
                        BytecodeEmitter fn_emitter;
                        fn_emitter.SetModuleRecord(this);
                        auto fn_bc = fn_emitter.EmitFunction(exp_fn_decl);
                        auto* fn_obj = new JSFunction(exp_fn_decl->Name(), std::move(fn_bc), nullptr, realm_.GetRootShape());
                        fn_obj->SetAsync(exp_fn_decl->IsAsync());
                        fn_obj->SetGenerator(exp_fn_decl->IsGenerator());
                        environment_->SetNamedProperty(exp_fn_decl->Name(), TaggedValue::FromHeapObject(fn_obj), dummy_id);
                        if (exp->IsDefault()) {
                            environment_->SetNamedProperty("default", TaggedValue::FromHeapObject(fn_obj), dummy_id);
                        }
                    }
                }
            }
        }
    }

    // 3. Execute bytecode
    TaggedValue result = TaggedValue::Undefined();
    if (bytecode_) {
        if (has_top_level_await_) {
            top_level_promise_ = AllocateRuntime<JSPromise>(realm_.GetRootShape());
            if (realm_.GetPromisePrototype()) top_level_promise_->SetPrototype(realm_.GetPromisePrototype());
            auto* fn_obj = new JSFunction(specifier_, bytecode_.get(), nullptr, realm_.GetRootShape());
            fn_obj->SetAsync(true);
            auto* gen = new JSGeneratorObject(fn_obj, {}, TaggedValue::Undefined(), nullptr, realm_.GetRootShape());
            gen->SetAsyncPromise(top_level_promise_);
            agent.GetInterpreter().RunAsyncStep(gen, TaggedValue::Undefined(), false);
            result = TaggedValue::FromHeapObject(top_level_promise_);
        } else {
            result = agent.GetInterpreter().Execute(*bytecode_);
        }
    }

    status_ = ModuleStatus::Evaluated;
    if (namespace_object_) {
        GetNamespaceObject(agent);
    }
    return result;
}

JSObject* SourceTextModuleRecord::GetNamespaceObject(Agent& agent) {
    if (!namespace_object_) {
        uint32_t shape_id = 9500;
        namespace_object_ = new JSObject(realm_.GetRootShape());
        namespace_object_->SetPrototype(environment_);
        TaggedValue tag_sym = realm_.GetSymbolToStringTag();
        if (!tag_sym.IsUndefined() && tag_sym.IsSymbol()) {
            std::string sym_key = static_cast<JSSymbol*>(tag_sym.AsHeapObject())->Key();
            namespace_object_->SetNamedProperty(sym_key, TaggedValue::FromHeapObject(new JSString("Module")), shape_id);
        }
    }

    uint32_t sid = 9501;
    for (const auto& e : export_entries_) {
        if (e.export_name == "*") continue;
        std::unordered_set<SourceTextModuleRecord*> seen;
        auto binding = ResolveExport(e.export_name, seen);
        if (binding.module) {
            if (binding.is_namespace) {
                namespace_object_->SetNamedProperty(e.export_name, TaggedValue::FromHeapObject(binding.module->GetNamespaceObject(agent)), sid);
            } else {
                TaggedValue val = binding.module->GetEnvironment()->GetNamedProperty(binding.export_name);
                if (!val.IsUndefined()) {
                    namespace_object_->SetNamedProperty(e.export_name, val, sid);
                }
            }
        }
    }

    return namespace_object_;
}

JSObject* SourceTextModuleRecord::GetImportMeta(Realm* realm) {
    if (!import_meta_) {
        import_meta_ = new JSObject(realm ? realm->GetRootShape() : realm_.GetRootShape());
        uint32_t sid = 9600;
        import_meta_->SetNamedProperty("url", TaggedValue::FromHeapObject(new JSString("file:///" + specifier_)), sid);
        import_meta_->SetNamedProperty("filename", TaggedValue::FromHeapObject(new JSString(specifier_)), sid);
        std::string dirname = std::filesystem::path(specifier_).parent_path().string();
        import_meta_->SetNamedProperty("dirname", TaggedValue::FromHeapObject(new JSString(dirname)), sid);
    }
    return import_meta_;
}

// ============================================================================
// ModuleLoader Implementation
// ============================================================================

ModuleLoader::ModuleLoader(Agent& agent)
    : agent_(agent) {}

void ModuleLoader::RegisterVirtualModule(std::string_view specifier, std::string_view source) {
    virtual_modules_[std::string(specifier)] = std::string(source);
}

std::shared_ptr<SourceTextModuleRecord> ModuleLoader::LoadModule(std::string_view specifier, std::string_view referrer_dir) {
    s_current_loader_ = this;
    std::string spec(specifier);
    std::string resolved_id = spec;
    std::string source_code;
    bool is_virtual = false;

    // 1. Virtual modules
    auto v_it = virtual_modules_.find(spec);
    if (v_it != virtual_modules_.end()) {
        resolved_id = spec;
        source_code = v_it->second;
        is_virtual = true;
    } else {
        // 2. Filesystem
        std::filesystem::path target_path(spec);
        if (target_path.is_relative() && !referrer_dir.empty()) {
            target_path = std::filesystem::path(std::string(referrer_dir)) / target_path;
        }

        if (!std::filesystem::exists(target_path) && !target_path.has_extension()) {
            std::filesystem::path with_js = target_path; with_js.replace_extension(".js");
            std::filesystem::path with_mjs = target_path; with_mjs.replace_extension(".mjs");
            if (std::filesystem::exists(with_mjs)) target_path = with_mjs;
            else if (std::filesystem::exists(with_js)) target_path = with_js;
        }

        if (std::filesystem::exists(target_path)) {
            std::error_code ec;
            std::filesystem::path canonical_p = std::filesystem::canonical(target_path, ec);
            resolved_id = ec ? target_path.string() : canonical_p.string();

            std::ifstream file(resolved_id, std::ios::in | std::ios::binary);
            if (!file) {
                throw std::runtime_error("Cannot read module file: " + resolved_id);
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            source_code = ss.str();
        } else {
            throw std::runtime_error("Cannot find module '" + spec + "'");
        }
    }

    // 3. Cache lookup
    auto cache_it = esm_cache_.find(resolved_id);
    if (cache_it != esm_cache_.end()) {
        return cache_it->second;
    }

    // 4. CommonJS imported into ESM
    if (resolved_id.ends_with(".cjs") || (source_code.find("module.exports") != std::string::npos && source_code.find("export ") == std::string::npos && source_code.find("import ") == std::string::npos)) {
        TaggedValue cjs_exports = Require(resolved_id, referrer_dir);
        auto record = std::make_shared<SourceTextModuleRecord>(resolved_id, agent_.GetRealm());
        esm_cache_[resolved_id] = record;

        uint32_t sid = 9100;
        record->GetEnvironment()->SetNamedProperty("default", cjs_exports, sid);
        if (cjs_exports.IsHeapObject() && cjs_exports.AsHeapObject()->IsJSObject()) {
            auto* obj = static_cast<JSObject*>(cjs_exports.AsHeapObject());
            TaggedValue keys_arr = GetOwnKeys(cjs_exports);
            if (keys_arr.IsHeapObject() && keys_arr.AsHeapObject()->IsArray()) {
                auto* arr = static_cast<JSArray*>(keys_arr.AsHeapObject());
                for (size_t i = 0; i < arr->Length(); ++i) {
                    std::string k = arr->GetElement(i).ToString();
                    record->GetEnvironment()->SetNamedProperty(k, obj->GetNamedProperty(k), sid);
                }
            }
        }
        record->SetStatus(ModuleStatus::Evaluated);
        return record;
    }

    // 5. Create new module record, insert early into cache for cycle handling
    auto record = std::make_shared<SourceTextModuleRecord>(resolved_id, agent_.GetRealm());
    esm_cache_[resolved_id] = record;
    record->Parse(source_code);
    return record;
}

void ModuleLoader::LinkModule(SourceTextModuleRecord* module) {
    if (module) {
        module->Link(*this);
    }
}

TaggedValue ModuleLoader::EvaluateModule(SourceTextModuleRecord* module) {
    if (!module) return TaggedValue::Undefined();
    return module->Evaluate(agent_);
}

TaggedValue ModuleLoader::ImportDynamic(std::string_view specifier, std::string_view referrer_dir) {
    auto* promise = AllocateRuntime<JSPromise>(agent_.GetRealm().GetRootShape());
    if (agent_.GetRealm().GetPromisePrototype()) {
        promise->SetPrototype(agent_.GetRealm().GetPromisePrototype());
    }

    std::string spec(specifier);
    std::string ref_dir(referrer_dir);

    agent_.GetMicrotasks().Enqueue([this, promise, spec, ref_dir]() {
        try {
            auto mod = this->LoadModule(spec, ref_dir);
            this->LinkModule(mod.get());
            TaggedValue eval_res = this->EvaluateModule(mod.get());

            if (eval_res.IsHeapObject() && eval_res.AsHeapObject()->IsPromise()) {
                auto* t_promise = static_cast<JSPromise*>(eval_res.AsHeapObject());
                auto* on_ok = AllocateRuntime<JSNativeFunction>("", [this, promise, mod](const std::vector<TaggedValue>&) -> TaggedValue {
                    promise->Fulfill(TaggedValue::FromHeapObject(mod->GetNamespaceObject(this->agent_)), this->agent_.GetMicrotasks());
                    return TaggedValue::Undefined();
                });
                auto* on_err = AllocateRuntime<JSNativeFunction>("", [this, promise](const std::vector<TaggedValue>& args) -> TaggedValue {
                    TaggedValue err = args.empty() ? TaggedValue::Undefined() : args[0];
                    promise->Reject(err, this->agent_.GetMicrotasks());
                    return TaggedValue::Undefined();
                });
                t_promise->AddReaction(TaggedValue::FromHeapObject(on_ok), TaggedValue::FromHeapObject(on_err), nullptr, this->agent_.GetMicrotasks());
            } else {
                promise->Fulfill(TaggedValue::FromHeapObject(mod->GetNamespaceObject(this->agent_)), this->agent_.GetMicrotasks());
            }
        } catch (const std::exception& e) {
            promise->Reject(TaggedValue::FromHeapObject(new JSError("Error", e.what())), this->agent_.GetMicrotasks());
        }
    });

    return TaggedValue::FromHeapObject(promise);
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
            } else {
                std::filesystem::path with_mjs = target_path;
                with_mjs.replace_extension(".mjs");
                if (std::filesystem::exists(with_mjs)) target_path = with_mjs;
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

    // 3. Check ESM cache or if module contains ESM syntax / is .mjs
    if (resolved_filename.ends_with(".mjs") || source_code.find("export ") != std::string::npos || source_code.find("import ") != std::string::npos) {
        auto esm_mod = LoadModule(resolved_id, referrer_dir);
        LinkModule(esm_mod.get());
        EvaluateModule(esm_mod.get());
        return TaggedValue::FromHeapObject(esm_mod->GetNamespaceObject(agent_));
    }

    // 4. Check CommonJS module cache
    auto cache_it = module_cache_.find(resolved_id);
    if (cache_it != module_cache_.end()) {
        return cache_it->second->module_object->GetNamedProperty("exports");
    }

    // 5. Create ModuleRecord and insert into cache early (supports circular dependencies)
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

    // 6. Wrap source code in CommonJS wrapper function
    std::string wrapped = "(function(exports, require, module, __filename, __dirname) {\n" + source_code + "\n})";
    TaggedValue wrapper_fn = agent_.EvaluateScript(wrapped);

    // 7. Create module-specific require function
    std::string cur_dir = resolved_dirname;
    auto* local_require = new JSNativeFunction("require", [this, cur_dir](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::Undefined();
        std::string s = args[0].ToString();
        return this->Require(s, cur_dir);
    });

    // 8. Execute module wrapper
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
