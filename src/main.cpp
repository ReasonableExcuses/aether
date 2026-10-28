#include "aether/common.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"
#include "aether/options.h"

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
              << "  -v, --version           Print AetherVM version and runtime metadata\n"
              << "  -h, --help              Display this help and usage summary\n"
              << "  -e, --eval <code>       Evaluate JavaScript script string directly\n"
              << "  --gc-stress[=N]         Trigger GC every N allocations (default: 1)\n"
              << "  --gc-verify             Verify heap invariants before and after GC\n"
              << "  --trace-gc              Print GC traces and allocation metrics\n"
              << "  --gc-at-safepoints      Trigger GC at backward branch safepoints\n"
              << "  --max-heap=MB           Set maximum heap budget in MB (default: 512)\n"
              << "  --interpreter-only      Disable JIT compilation and tiering\n"
              << "  --always-baseline       Compile functions to Baseline JIT immediately\n"
              << "  --always-optimize       Compile functions to Optimizing JIT immediately\n"
              << "  --tier-threshold=N      Invocation count threshold for Tier 1 JIT\n"
              << "  --stack-size=KB         Native execution stack budget in KB\n\n"
              << "Examples:\n"
              << "  aether -e \"console.log('Hello from AetherVM!');\"\n"
              << "  aether --gc-stress=100 index.js arg1 arg2\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintHelp();
        return 0;
    }

    std::vector<std::string> cli_args;
    for (int i = 0; i < argc; ++i) {
        cli_args.emplace_back(argv[i]);
    }

    aether::Options::Get().ParseEnv();
    aether::Options::Parse(cli_args);

    if (cli_args.size() < 2) {
        PrintHelp();
        return 0;
    }

    std::string first_arg = cli_args[1];
    if (first_arg == "-v" || first_arg == "--version") {
        PrintVersion();
        return 0;
    }

    if (first_arg == "-h" || first_arg == "--help") {
        PrintHelp();
        return 0;
    }

    try {
        aether::Agent agent;

        if (first_arg == "-e" || first_arg == "--eval") {
            if (cli_args.size() < 3) {
                std::cerr << "Error: --eval requires a JavaScript code argument.\n";
                return 1;
            }
            std::string code = cli_args[2];
            std::vector<std::string> eval_argv;
            eval_argv.push_back(cli_args[0]);
            eval_argv.push_back("[eval]");
            for (size_t i = 3; i < cli_args.size(); ++i) {
                eval_argv.push_back(cli_args[i]);
            }
            agent.GetModuleLoader().InitializeModuleGlobals(eval_argv);
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
            agent.GetModuleLoader().InitializeModuleGlobals(cli_args);
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
