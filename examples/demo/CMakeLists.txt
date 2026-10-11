# examples/demo CMakelists.txt

set(TARGET_NAME demo)

add_executable(
    ${TARGET_NAME}
    main.cpp
)

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
        COMMENT "Compiling cube vertex shader for Vulkan 1.3 / SPIR-V 1.6"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${CUBE_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --target-env spirv1.6 --glsl-version 450
                -S frag -o "${CUBE_FRAGMENT_SPIRV}" "${ASSETS_DIR}/shaders/cube.frag"
        DEPENDS "${ASSETS_DIR}/shaders/cube.frag"
        COMMENT "Compiling cube fragment shader for Vulkan 1.3 / SPIR-V 1.6"
        VERBATIM
    )
    add_custom_target(demo_shaders DEPENDS "${CUBE_VERTEX_SPIRV}" "${CUBE_FRAGMENT_SPIRV}")
    add_dependencies(${TARGET_NAME} demo_shaders)
    set_target_properties(${TARGET_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

if(MSVC)
    target_compile_options(${TARGET_NAME} PUBLIC /utf-8) # MSVC unicode support
endif()

add_dependencies(${TARGET_NAME} IncubatorRuntime)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "examples")

target_link_libraries(
    ${TARGET_NAME} PUBLIC
    IncubatorRuntime
)
target_include_directories(
    ${TARGET_NAME} PUBLIC
)
