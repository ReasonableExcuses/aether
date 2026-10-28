cmake_minimum_required(VERSION 3.20)
project(AetherVM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

option(AETHER_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AETHER_TSAN "Enable ThreadSanitizer" OFF)
option(AETHER_GC_STRESS_DEFAULT "Enable GC stress by default" OFF)

if(MSVC)
    add_compile_options(/std:c++latest /W4 /permissive- /Zc:preprocessor)
    if(AETHER_SANITIZE)
        add_compile_definitions(AETHER_SANITIZE_ACTIVE=1)
        add_compile_options(/fsanitize=address)
    endif()
    if(AETHER_GC_STRESS_DEFAULT)
        add_compile_definitions(AETHER_GC_STRESS_DEFAULT=1)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(AETHER_SANITIZE)
        add_compile_definitions(AETHER_SANITIZE_ACTIVE=1)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize=function -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined -fno-sanitize=function)
    endif()
    if(AETHER_TSAN)
        add_compile_definitions(AETHER_SANITIZE_ACTIVE=1)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
    if(AETHER_GC_STRESS_DEFAULT)
        add_compile_definitions(AETHER_GC_STRESS_DEFAULT=1)
    endif()
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
    src/advanced_intrinsics.cpp
    src/bounded_regex.cpp
    src/options.cpp
    src/heap_verifier.cpp
)

add_library(aether_core STATIC ${AETHER_SOURCES})
target_link_libraries(aether_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(aether_core PRIVATE /Zi)
endif()

add_executable(aether src/main.cpp)
target_link_libraries(aether PRIVATE aether_core)

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/test_*.cpp")
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE aether_core)
    if(MSVC)
        target_link_options(${TEST_NAME} PRIVATE /DEBUG)
    endif()
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 60
    )
endforeach()
