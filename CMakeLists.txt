cmake_minimum_required(VERSION 3.20)
project(AetherVM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/std:c++latest /W4 /permissive- /Zc:preprocessor)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include_directories(include)

set(AETHER_SOURCES
    src/value.cpp
    src/heap_object.cpp
    src/bytecode.cpp
    src/interpreter.cpp
    src/bytecode_emitter.cpp
    src/lexer.cpp
    src/parser.cpp
    src/gc.cpp
    src/heap.cpp
    src/assembler.cpp
    src/baseline_jit.cpp
    src/sea_of_nodes.cpp
    src/optimizing_jit.cpp
    src/runtime.cpp
    src/event_loop.cpp
    src/module.cpp
    src/intl.cpp
    src/wasm.cpp
    src/background_compiler.cpp
    src/concurrent_marker.cpp
    src/test262.cpp
)

add_library(aether_core STATIC ${AETHER_SOURCES})
target_compile_options(aether_core PRIVATE /Zi)


add_executable(test_milestone1 tests/test_milestone1.cpp)
target_link_libraries(test_milestone1 PRIVATE aether_core)

add_executable(test_milestone2 tests/test_milestone2.cpp)
target_link_libraries(test_milestone2 PRIVATE aether_core)

add_executable(test_milestone3 tests/test_milestone3.cpp)
target_link_libraries(test_milestone3 PRIVATE aether_core)

add_executable(test_milestone4_jit tests/test_milestone4_jit.cpp)
target_link_libraries(test_milestone4_jit PRIVATE aether_core)

add_executable(test_milestone5_son_jit tests/test_milestone5_son_jit.cpp)
target_link_libraries(test_milestone5_son_jit PRIVATE aether_core)

add_executable(test_milestone6_runtime tests/test_milestone6_runtime.cpp)
target_link_libraries(test_milestone6_runtime PRIVATE aether_core)

add_executable(test_milestone7_closures tests/test_milestone7_closures.cpp)
target_link_libraries(test_milestone7_closures PRIVATE aether_core)

add_executable(test_milestone8_intrinsics tests/test_milestone8_intrinsics.cpp)
target_link_libraries(test_milestone8_intrinsics PRIVATE aether_core)

add_executable(test_milestone9_event_loop tests/test_milestone9_event_loop.cpp)
target_link_libraries(test_milestone9_event_loop PRIVATE aether_core)

add_executable(test_milestone10_modules tests/test_milestone10_modules.cpp)
target_link_libraries(test_milestone10_modules PRIVATE aether_core)

add_executable(test_milestone11_typed_arrays tests/test_milestone11_typed_arrays.cpp)
target_link_libraries(test_milestone11_typed_arrays PRIVATE aether_core)

add_executable(test_test262_harness tests/test_test262_harness.cpp)
target_link_libraries(test_test262_harness PRIVATE aether_core)
target_link_options(test_test262_harness PRIVATE /DEBUG)

add_executable(test_milestone12_modern_syntax tests/test_milestone12_modern_syntax.cpp)
target_link_libraries(test_milestone12_modern_syntax PRIVATE aether_core)

add_executable(aether src/main.cpp)
target_link_libraries(aether PRIVATE aether_core)

add_executable(test_milestone13_iteration_symbols_generators tests/test_milestone13_iteration_symbols_generators.cpp)
target_link_libraries(test_milestone13_iteration_symbols_generators PRIVATE aether_core)

add_executable(test_milestone14_async_await tests/test_milestone14_async_await.cpp)
target_link_libraries(test_milestone14_async_await PRIVATE aether_core)

add_executable(test_milestone15_proxy_reflect tests/test_milestone15_proxy_reflect.cpp)
target_link_libraries(test_milestone15_proxy_reflect PRIVATE aether_core)

add_executable(test_milestone16_async_iterators tests/test_milestone16_async_iterators.cpp)
target_link_libraries(test_milestone16_async_iterators PRIVATE aether_core)

add_executable(test_milestone17_esm tests/test_milestone17_esm.cpp)
target_link_libraries(test_milestone17_esm PRIVATE aether_core)

add_executable(test_milestone18_intl tests/test_milestone18_intl.cpp)
target_link_libraries(test_milestone18_intl PRIVATE aether_core)

add_executable(test_milestone19_wasm tests/test_milestone19_wasm.cpp)
target_link_libraries(test_milestone19_wasm PRIVATE aether_core)

add_executable(test_milestone20_parallel tests/test_milestone20_parallel.cpp)
target_link_libraries(test_milestone20_parallel PRIVATE aether_core)
