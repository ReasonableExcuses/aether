#include "aether/common.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

void PrintVersion() {
    std::cout << "AetherVM v1.0.0 (ECMAScript 2023+, ISO C++23, x86-64-v3 Multi-Tier JIT)\n";
}

void PrintHelp() {
    std::cout << "Usage: aether [options] [script.js] [arguments]\n\n"
              << "Options:\n"
              << "  -v, --version        Print AetherVM version and runtime metadata\n"
              << "  -h, --help           Display this help and usage summary\n"
              << "  -e, --eval <code>    Evaluate JavaScript script string directly\n\n"
              << "Examples:\n"
              << "  aether -e \"console.log('Hello from AetherVM!');\"\n"
              << "  aether index.js arg1 arg2\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintHelp();
        return 0;
    }

    std::string first_arg = argv[1];
    if (first_arg == "-v" || first_arg == "--version") {
        PrintVersion();
        return 0;
    }

    if (first_arg == "-h" || first_arg == "--help") {
        PrintHelp();
        return 0;
    }

    std::vector<std::string> cli_args;
    for (int i = 0; i < argc; ++i) {
        cli_args.emplace_back(argv[i]);
    }

    try {
        aether::Agent agent;
        agent.GetModuleLoader().InitializeModuleGlobals(cli_args);

        if (first_arg == "-e" || first_arg == "--eval") {
            if (argc < 3) {
                std::cerr << "Error: --eval requires a JavaScript code argument.\n";
                return 1;
            }
            std::string code = argv[2];
            agent.EvaluateScriptWithEventLoop(code);
        } else {
            std::string script_path = first_arg;
            if (!std::filesystem::exists(script_path)) {
                std::cerr << "Error: Cannot open file '" << script_path << "': No such file or directory.\n";
                return 1;
            }

            std::ifstream file(script_path, std::ios::in | std::ios::binary);
            if (!file.is_open()) {
                std::cerr << "Error: Cannot open file '" << script_path << "'.\n";
                return 1;
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            std::string source = ss.str();
            agent.EvaluateScriptWithEventLoop(source);
        }

        if (agent.GetInterpreter().HasPendingException()) {
            aether::TaggedValue exc = agent.GetInterpreter().PendingException();
            if (exc.IsHeapObject() && exc.AsHeapObject()->IsJSObject()) {
                auto* obj = static_cast<aether::JSObject*>(exc.AsHeapObject());
                std::string name = obj->HasNamedProperty("name") ? obj->GetNamedProperty("name").ToString() : "Error";
                std::string msg = obj->HasNamedProperty("message") ? obj->GetNamedProperty("message").ToString() : "";
                if (!msg.empty()) {
                    std::cerr << "Uncaught Exception: " << name << ": " << msg << "\n";
                } else {
                    std::cerr << "Uncaught Exception: " << exc.ToString() << "\n";
                }
            } else {
                std::cerr << "Uncaught Exception: " << exc.ToString() << "\n";
            }
            return 1;
        }

        if (agent.GetModuleLoader().HasExited()) {
            return agent.GetModuleLoader().GetExitCode();
        }

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Uncaught Exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Uncaught unknown exception occurred.\n";
        return 1;
    }
}
