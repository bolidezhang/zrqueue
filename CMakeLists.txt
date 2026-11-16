cmake_minimum_required(VERSION 3.14)
project(Benchmark VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimisation flags are configuration-specific: they must NOT be appended to
# CMAKE_CXX_FLAGS unconditionally, or a Debug build would still be compiled with
# -O3 -DNDEBUG (assertions off, unoptimisable for debugging).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Flags shared by every configuration.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdata-sections -ffunction-sections")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Oy- /GT")
endif()

# Flags applied only to optimised configurations. -DNDEBUG is what MSVC's Release
# configuration already defines; -march=native makes binaries non-portable, so it
# belongs to the performance build, not to Debug.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        string(TOUPPER "${cfg}" CFG_UPPER)
        set(CMAKE_CXX_FLAGS_${CFG_UPPER}
            "${CMAKE_CXX_FLAGS_${CFG_UPPER}} -O3 -DNDEBUG -march=native -mtune=native -flto=auto -fomit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS_${CFG_UPPER}
            "${CMAKE_EXE_LINKER_FLAGS_${CFG_UPPER}} -flto=auto")
    endforeach()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # /O2 /Ob2 /GL replace CMake's default /Od for optimised configurations.
    # /DNDEBUG must be listed explicitly: assigning to CMAKE_CXX_FLAGS_<CFG>
    # discards CMake's default value, and losing NDEBUG re-enables every assert
    # in the measured code (rigtorp's pop() then pays a cross-core acquire load
    # per element, which understated it by ~10x in an earlier run).
    foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
        string(TOUPPER "${cfg}" CFG_UPPER)
        set(CMAKE_CXX_FLAGS_${CFG_UPPER}
            "/O2 /Ob2 /GL /DNDEBUG")
        set(CMAKE_EXE_LINKER_FLAGS_${CFG_UPPER} "/LTCG")
    endforeach()
endif()

find_package(Threads REQUIRED)

# Executable target
add_executable(benchmark benchmark.cpp)

# Include paths: Current directory (zrqueue.h) + rigtorp headers in submodule
target_include_directories(benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rigtorp/include
)

target_link_libraries(benchmark PRIVATE Threads::Threads)

# Static linking (optional)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(benchmark PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Correctness lives in its own target, independent of the benchmark.
# zrqueue_tests.cpp undefines NDEBUG so claim/commit misuse assertions stay live
# even in an optimised build.
enable_testing()
add_executable(zrqueue_tests tests/zrqueue_tests.cpp)
target_include_directories(zrqueue_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zrqueue_tests PRIVATE Threads::Threads)
add_test(NAME zrqueue_tests COMMAND zrqueue_tests)
