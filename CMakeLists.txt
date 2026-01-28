# project root CMakeLists.txt
cmake_minimum_required(VERSION 3.15 FATAL_ERROR)
project(Incubator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

file(MAKE_DIRECTORY bin/debug)
file(MAKE_DIRECTORY bin/release)
file(MAKE_DIRECTORY assets)

# 头文件包含保护
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
    )
endif()

# 一级目录
set(INCUBATOR_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BINARY_ROOT_DIR "${INCUBATOR_ROOT_DIR}/")

# 二级目录
set(SRC_DIR "${INCUBATOR_ROOT_DIR}/src")
set(ASSETS_DIR "${INCUBATOR_ROOT_DIR}/assets")
set(THIRD_PARTY_DIR "${INCUBATOR_ROOT_DIR}/3rdparty")

add_subdirectory(3rdparty)

add_subdirectory(src)

# 单元测试和示例
add_subdirectory(test)
add_subdirectory(examples)