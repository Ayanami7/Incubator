# runtime CMakeLists.txt

set(TARGET_NAME IncubatorRuntime)

# core 模块头文件/源文件
set(CORE_HEADERS
    core/log/logger.h
    core/error/error.h
    core/utils/hash.h
    core/utils/utf8.h
    core/clock/clock.h
    core/path/path.h
)
set(CORE_SOURCES
    core/log/logger.cpp
    core/error/error.cpp
    core/utils/hash.cpp
    core/utils/utf8.cpp
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

# resource 模块头文件、源文件
set(RESOURCE_HEADERS
    resource/resource_type.h
    resource/resource_id.h
    resource/resource_registry.h
    resource/resource_state.h
    resource/asset_info.h
    resource/resource_object.h
    resource/resource_manager.h
    resource/loader/loader_base.h
    resource/loader/text_loader.h
    resource/loader/text_resource.h
    resource/loader/spirv_reader.h
    resource/loader/shader_resource.h
    resource/loader/shader_loader.h
)
set(RESOURCE_SOURCES
    resource/resource_id.cpp
    resource/resource_registry.cpp
    resource/resource_object.cpp
    resource/resource_manager.cpp
    resource/loader/text_loader.cpp
    resource/loader/text_resource.cpp
    resource/loader/spirv_reader.cpp
    resource/loader/shader_resource.cpp
    resource/loader/shader_loader.cpp
)

# render 模块头文件、源文件
set(RENDER_HEADERS
    render/renderer.h
    render/integration/imgui_renderer.h
    render/vulkan/vulkan_swapchain.h
    render/vulkan/vulkan_frame_scheduler.h
    render/vulkan/vulkan_pipeline.h
    render/vulkan/vulkan_command_list.h
    render/vulkan/vulkan_buffer.h
    render/vulkan/vulkan_image.h
    render/vulkan/vulkan_binding_set.h
    render/vulkan/vulkan_sampler.h
    render/vulkan/vulkan_instance.h
    render/vulkan/glfw_surface.h
    render/vulkan/vulkan_device.h
)
set(RENDER_SOURCES
    render/renderer.cpp
    render/integration/imgui_renderer.cpp
    render/vulkan/vulkan_swapchain.cpp
    render/vulkan/vulkan_frame_scheduler.cpp
    render/vulkan/vulkan_pipeline.cpp
    render/vulkan/vulkan_command_list.cpp
    render/vulkan/vulkan_buffer.cpp
    render/vulkan/vulkan_image.cpp
    render/vulkan/vulkan_binding_set.cpp
    render/vulkan/vulkan_sampler.cpp
    render/vulkan/vulkan_instance.cpp
    render/vulkan/glfw_surface.cpp
    render/vulkan/vulkan_device.cpp
)


# 合并所有头文件和源文件到一个变量
set(ALL_HEADERS
    ${CORE_HEADERS}
    ${PLATFORM_HEADERS}
    ${RESOURCE_HEADERS}
    ${RENDER_HEADERS}
)
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${RESOURCE_SOURCES}
    ${RENDER_SOURCES}
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
target_link_libraries(${TARGET_NAME} PUBLIC utf8::utf8cpp)
target_link_libraries(${TARGET_NAME} PRIVATE imgui)

# Render owns the Vulkan integration. Keep it private because public Runtime headers
# do not expose Vulkan types. The bundled Vulkan target is currently Windows-only.
if(WIN32)
    target_link_libraries(${TARGET_NAME} PRIVATE vulkan)
endif()

# 设置包含目录
target_include_directories(
    ${TARGET_NAME} PUBLIC
    $<BUILD_INTERFACE:${SRC_DIR}>
    $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/glm>
)
