# File dependencies keep texture-only edits independent of executable relinking.
set(DEMO_TEXTURE_OUTPUTS)
foreach(TEXTURE_NAME IN ITEMS reference_rgba.png direction_rgba.png)
    set(TEXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/textures/${TEXTURE_NAME}")
    set(TEXTURE_OUTPUT "${CMAKE_BINARY_DIR}/assets/textures/${TEXTURE_NAME}")
    add_custom_command(
        OUTPUT "${TEXTURE_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/assets/textures"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TEXTURE_SOURCE}" "${TEXTURE_OUTPUT}"
        DEPENDS "${TEXTURE_SOURCE}"
        COMMENT "Deploying ${TEXTURE_NAME}"
        VERBATIM
    )
    list(APPEND DEMO_TEXTURE_OUTPUTS "${TEXTURE_OUTPUT}")
endforeach()
add_custom_target(demo_textures DEPENDS ${DEMO_TEXTURE_OUTPUTS})

# Original model fixtures, also consumed by CPU import tests and later model integration.
set(DEMO_MODEL_OUTPUTS)
foreach(MODEL_FILE IN ITEMS
    minimal_gltf/model.gltf
    minimal_gltf/geometry/mesh.bin
    minimal_obj/model.obj
    minimal_obj/materials/model.mtl)
    set(MODEL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/models/${MODEL_FILE}")
    set(MODEL_OUTPUT "${CMAKE_BINARY_DIR}/assets/models/${MODEL_FILE}")
    get_filename_component(MODEL_OUTPUT_DIRECTORY "${MODEL_OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${MODEL_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MODEL_OUTPUT_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MODEL_SOURCE}" "${MODEL_OUTPUT}"
        DEPENDS "${MODEL_SOURCE}"
        COMMENT "Deploying model ${MODEL_FILE}"
        VERBATIM
    )
    list(APPEND DEMO_MODEL_OUTPUTS "${MODEL_OUTPUT}")
endforeach()
add_custom_target(demo_models DEPENDS ${DEMO_MODEL_OUTPUTS})
