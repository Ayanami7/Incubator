# utfcpp CMakeLists.txt
# 仅头文件的 UTF-8 处理库，提供 utf8::utf8cpp 接口目标

cmake_minimum_required(VERSION 3.15)

project(utf8cpp VERSION 4.1.1 LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)

# 头文件目录
target_include_directories(
    ${PROJECT_NAME} INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source>"
)

# 命名空间别名：utf8::utf8cpp
add_library(utf8::utf8cpp ALIAS ${PROJECT_NAME})