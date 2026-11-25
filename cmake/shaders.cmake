# 编译两个应用共用的生产 Shader，不依赖应用可执行目标。
if(WIN32)
    set(GLSLANG_VALIDATOR "${THIRD_PARTY_DIR}/vulkan/bin/glslangValidator.exe")
    if(NOT EXISTS "${GLSLANG_VALIDATOR}")
        message(FATAL_ERROR "Bundled GLSL compiler not found: ${GLSLANG_VALIDATOR}")
    endif()

    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
    set(CUBE_VERTEX_SPIRV "${SHADER_OUTPUT_DIR}/cube.vert.spv")
    set(CUBE_FRAGMENT_SPIRV "${SHADER_OUTPUT_DIR}/cube.frag.spv")
    add_custom_command(
        OUTPUT "${CUBE_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S vert -o "${CUBE_VERTEX_SPIRV}" "${ASSETS_DIR}/shaders/cube.vert"
        DEPENDS "${ASSETS_DIR}/shaders/cube.vert"
        COMMENT "编译 Vulkan 1.3 / SPIR-V 1.6 顶点 Shader"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${CUBE_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S frag -o "${CUBE_FRAGMENT_SPIRV}" "${ASSETS_DIR}/shaders/cube.frag"
        DEPENDS "${ASSETS_DIR}/shaders/cube.frag"
        COMMENT "编译 Vulkan 1.3 / SPIR-V 1.6 片元 Shader"
        VERBATIM
    )
    set(SHADOW_VERTEX_SPIRV "${SHADER_OUTPUT_DIR}/shadow.vert.spv")
    add_custom_command(
        OUTPUT "${SHADOW_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S vert -o "${SHADOW_VERTEX_SPIRV}" "${ASSETS_DIR}/shaders/shadow.vert"
        DEPENDS "${ASSETS_DIR}/shaders/shadow.vert"
        COMMENT "编译阴影深度顶点 Shader"
        VERBATIM
    )
    foreach(MASK_SHADER_STAGE IN ITEMS vert frag)
        set(MASK_SHADER_SOURCE "${ASSETS_DIR}/shaders/shadow_mask.${MASK_SHADER_STAGE}")
        set(MASK_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/shadow_mask.${MASK_SHADER_STAGE}.spv")
        add_custom_command(
            OUTPUT "${MASK_SHADER_OUTPUT}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
            COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                    -S ${MASK_SHADER_STAGE} -o "${MASK_SHADER_OUTPUT}" "${MASK_SHADER_SOURCE}"
            DEPENDS "${MASK_SHADER_SOURCE}"
            COMMENT "编译阴影镂空 Shader"
            VERBATIM
        )
        list(APPEND MASK_SHADER_OUTPUTS "${MASK_SHADER_OUTPUT}")
    endforeach()
    set(DISPLAY_VERTEX_SPIRV "${SHADER_OUTPUT_DIR}/display.vert.spv")
    set(DISPLAY_FRAGMENT_SPIRV "${SHADER_OUTPUT_DIR}/display.frag.spv")
    set(FXAA_FRAGMENT_SPIRV "${SHADER_OUTPUT_DIR}/fxaa.frag.spv")
    add_custom_command(
        OUTPUT "${DISPLAY_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S vert -o "${DISPLAY_VERTEX_SPIRV}" "${ASSETS_DIR}/shaders/display.vert"
        DEPENDS "${ASSETS_DIR}/shaders/display.vert"
        COMMENT "编译显示 Pass 顶点 Shader"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${DISPLAY_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S frag -o "${DISPLAY_FRAGMENT_SPIRV}" "${ASSETS_DIR}/shaders/display.frag"
        DEPENDS "${ASSETS_DIR}/shaders/display.frag"
        COMMENT "编译显示 Pass 片元 Shader"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${FXAA_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S frag -o "${FXAA_FRAGMENT_SPIRV}" "${ASSETS_DIR}/shaders/fxaa.frag"
        DEPENDS "${ASSETS_DIR}/shaders/fxaa.frag"
        COMMENT "编译 FXAA 片元 Shader"
        VERBATIM
    )
    add_custom_target(incubator_shaders
        DEPENDS "${CUBE_VERTEX_SPIRV}" "${CUBE_FRAGMENT_SPIRV}" "${SHADOW_VERTEX_SPIRV}"
                ${MASK_SHADER_OUTPUTS} "${DISPLAY_VERTEX_SPIRV}" "${DISPLAY_FRAGMENT_SPIRV}" "${FXAA_FRAGMENT_SPIRV}")

endif()

