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
    core/math/aabb.h
)
set(CORE_SOURCES
    core/log/logger.cpp
    core/error/error.cpp
    core/utils/hash.cpp
    core/utils/utf8.cpp
    core/path/path.cpp
    core/math/aabb.cpp
)

# platform 模块头文件/源文件
set(PLATFORM_HEADERS
    platform/fileio/fileio.h
    platform/fileio/file_handle.h
    platform/input/input.h
    platform/input/input_state.h
    platform/window/window.h
)
set(PLATFORM_SOURCES
    platform/fileio/fileio.cpp
    platform/input/input_state.cpp
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
    resource/mesh_data.h
    resource/loader/model_resource.h
    resource/loader/model_loader.h
    resource/loader/detail/assimp_fileio.h
    resource/loader/detail/model_source.h
    resource/loader/loader_base.h
    resource/loader/text_loader.h
    resource/loader/text_resource.h
    resource/loader/spirv_reader.h
    resource/loader/shader_resource.h
    resource/loader/shader_loader.h
    resource/loader/texture_resource.h
    resource/loader/texture_loader.h
)
set(RESOURCE_SOURCES
    resource/resource_id.cpp
    resource/resource_registry.cpp
    resource/resource_object.cpp
    resource/resource_manager.cpp
    resource/loader/model_resource.cpp
    resource/loader/model_loader.cpp
    resource/loader/detail/assimp_fileio.cpp
    resource/loader/detail/model_source.cpp
    resource/loader/text_loader.cpp
    resource/loader/text_resource.cpp
    resource/loader/spirv_reader.cpp
    resource/loader/shader_resource.cpp
    resource/loader/shader_loader.cpp
    resource/loader/texture_resource.cpp
    resource/loader/texture_loader.cpp
)

# render 模块头文件、源文件
set(RENDER_HEADERS
    render/renderer.h
    render/detail/renderer_review.h
    render/frame_input.h
    render/lighting.h
    render/mesh.h
    render/texture.h
    render/material.h
    render/sampler.h
    render/integration/imgui_renderer.h
    render/vulkan/vulkan_swapchain.h
    render/vulkan/vulkan_frame_scheduler.h
    render/vulkan/vulkan_pipeline.h
    render/vulkan/vulkan_command_list.h
    render/vulkan/vulkan_buffer.h
    render/vulkan/vulkan_image.h
    render/vulkan/vulkan_binding_set.h
    render/vulkan/vulkan_material_binding.h
    render/vulkan/vulkan_sampler.h
    render/vulkan/vulkan_instance.h
    render/vulkan/glfw_surface.h
    render/vulkan/vulkan_device.h
)
set(RENDER_SOURCES
    render/renderer.cpp
    render/lighting.cpp
    render/integration/imgui_renderer.cpp
    render/vulkan/vulkan_swapchain.cpp
    render/vulkan/vulkan_frame_scheduler.cpp
    render/vulkan/vulkan_pipeline.cpp
    render/vulkan/vulkan_command_list.cpp
    render/vulkan/vulkan_buffer.cpp
    render/vulkan/vulkan_image.cpp
    render/vulkan/vulkan_binding_set.cpp
    render/vulkan/vulkan_material_binding.cpp
    render/vulkan/vulkan_sampler.cpp
    render/vulkan/vulkan_instance.cpp
    render/vulkan/glfw_surface.cpp
    render/vulkan/vulkan_device.cpp
)


# 合并所有头文件和源文件到一个变量
set(SCENE_HEADERS
    scene/entity.h
    scene/scene.h
    scene/scene_description.h
    scene/scene_rebuild.h
    scene/scene_file.h
    scene/scene_frame.h
    scene/transform.h
    scene/camera.h
    scene/frustum.h
    scene/detail/pose_validation.h
)
set(SCENE_SOURCES
    scene/scene.cpp
    scene/scene_description.cpp
    scene/scene_rebuild.cpp
    scene/scene_file.cpp
    scene/scene_frame.cpp
    scene/transform.cpp
    scene/camera.cpp
    scene/frustum.cpp
)

set(ALL_HEADERS
    model/model_reference.h
    model/prepared_model.h
    model/model_instance.h
    ${CORE_HEADERS}
    ${PLATFORM_HEADERS}
    ${RESOURCE_HEADERS}
    ${RENDER_HEADERS}
    ${SCENE_HEADERS}
)
set(ALL_SOURCES
    model/model_reference.cpp
    model/prepared_model.cpp
    model/model_instance.cpp
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${RESOURCE_SOURCES}
    ${RENDER_SOURCES}
    ${SCENE_SOURCES}
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
target_link_libraries(${TARGET_NAME} PRIVATE stb_image)
target_link_libraries(${TARGET_NAME} PRIVATE assimp::assimp)

# Model input checks and scene files use Assimp's bundled JSON parser privately.
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")

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
