# runtime CMakeLists.txt

set(TARGET_NAME IncubatorRuntime)

# core 模块头文件/源文件
set(CORE_HEADERS
    core/log/logger.h
    core/error/error.h
    core/utils/hash.h
    core/clock/clock.h
    core/path/path.h
)
set(CORE_SOURCES
    core/log/logger.cpp
    core/error/error.cpp
    core/utils/hash.cpp
    core/path/path.cpp
)

# platform 模块头文件/源文件
set(PLATFORM_HEADERS
    platform/fileio/fileio.h
    platform/fileio/file_handle.h
    platform/input/input.h
    platform/window/window.h
)
set(PLATFORM_SOURCES
    platform/fileio/fileio.cpp
    platform/window/window.cpp
)

# 合并所有头文件和源文件到一个变量
set(ALL_HEADERS
    ${CORE_HEADERS}
    ${PLATFORM_HEADERS}
)
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
)

# 创建静态库目标
add_library(
    ${TARGET_NAME} STATIC
    ${ALL_HEADERS}
    ${ALL_SOURCES}
)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Engine")

if(MSVC)
    target_compile_options(${TARGET_NAME} PUBLIC /utf-8) # MSVC UTF-8 编码支持
endif()

# 添加依赖项
add_dependencies(${TARGET_NAME} spdlog::spdlog)
add_dependencies(${TARGET_NAME} fmt::fmt)

# 链接依赖库
target_link_libraries(${TARGET_NAME} PUBLIC spdlog::spdlog)
target_link_libraries(${TARGET_NAME} PUBLIC fmt::fmt)
target_link_libraries(${TARGET_NAME} PUBLIC glfw)

# 设置包含目录
target_include_directories(
    ${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${SRC_DIR}>
    $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/glm>
)
