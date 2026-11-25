# 编译两个应用共用的生产 Shader，不依赖应用可执行目标。
set(INCUBATOR_SHADER_SOURCES
    cube.vert
    cube.frag
    shadow.vert
    shadow_mask.vert
    shadow_mask.frag
    display.vert
    display.frag
    fxaa.frag
    skybox.vert
    skybox.frag
    bloom_prefilter.frag
    bloom_downsample.frag
    bloom_upsample.frag
    debug_line.vert
    debug_line.frag
)

set(INCUBATOR_SHADER_SPIRV_FILES)
foreach(SHADER_NAME IN LISTS INCUBATOR_SHADER_SOURCES)
    list(APPEND INCUBATOR_SHADER_SPIRV_FILES "${SHADER_NAME}.spv")
endforeach()

if(WIN32)
    set(GLSLANG_VALIDATOR "${THIRD_PARTY_DIR}/vulkan/bin/glslangValidator.exe")
    if(NOT EXISTS "${GLSLANG_VALIDATOR}")
        message(FATAL_ERROR "Bundled GLSL compiler not found: ${GLSLANG_VALIDATOR}")
    endif()

    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
    set(SHADER_FRAME_DATA_INCLUDE "${ASSETS_DIR}/shaders/frame_data.glsl")
    set(INCUBATOR_SHADER_OUTPUTS)

    foreach(SHADER_NAME IN LISTS INCUBATOR_SHADER_SOURCES)
        get_filename_component(SHADER_EXT "${SHADER_NAME}" LAST_EXT)
        string(SUBSTRING "${SHADER_EXT}" 1 -1 SHADER_STAGE)
        set(SHADER_SOURCE "${ASSETS_DIR}/shaders/${SHADER_NAME}")
        set(SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")
        add_custom_command(
            OUTPUT "${SHADER_OUTPUT}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
            COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                    -I"${ASSETS_DIR}/shaders" -S "${SHADER_STAGE}" -o "${SHADER_OUTPUT}" "${SHADER_SOURCE}"
            DEPENDS "${SHADER_SOURCE}" "${SHADER_FRAME_DATA_INCLUDE}"
            COMMENT "编译 Vulkan 1.3 / SPIR-V 1.6 Shader ${SHADER_NAME}"
            VERBATIM
        )
        list(APPEND INCUBATOR_SHADER_OUTPUTS "${SHADER_OUTPUT}")
    endforeach()

    add_custom_target(incubator_shaders DEPENDS ${INCUBATOR_SHADER_OUTPUTS})
endif()
