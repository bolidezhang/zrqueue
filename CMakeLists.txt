cmake_minimum_required(VERSION 3.14)
project(Benchmark VERSION 1.0 LANGUAGES CXX)

# 速度优先的编译选项
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OPT_FLAGS "-O3 -DNDEBUG -march=native -mtune=native -flto=auto -fomit-frame-pointer -pthread")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OPT_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto=auto")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdata-sections -ffunction-sections")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /Ob2 /Oy- /GT /GL")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LTCG")
endif()

# 查找线程库（必须在使用 Threads::Threads 之前）
find_package(Threads REQUIRED)

# 可执行文件
add_executable(benchmark benchmark.cpp)

# 包含路径：当前目录 (zrqueue.h) + 子模块中的 rigtorp 头文件
target_include_directories(benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rigtorp/include
)

# 链接线程库
target_link_libraries(benchmark PRIVATE Threads::Threads)

# 静态链接 (可选)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(benchmark PRIVATE -static-libstdc++ -static-libgcc)
endif()