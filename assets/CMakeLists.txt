# Demo assets contain only the fixed showcase models. Test fixtures are deployed by test/runtime.
set(DEMO_MODEL_OUTPUTS)
foreach(MODEL_FILE IN ITEMS
    utah_teapot/utah_teapot.obj
    avocado/glTF/Avocado.gltf
    avocado/glTF/Avocado.bin
    avocado/glTF/Avocado_baseColor.png
    avocado/glTF/Avocado_roughnessMetallic.png
    avocado/glTF/Avocado_normal.png)
    set(MODEL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/models/${MODEL_FILE}")
    set(MODEL_OUTPUT "${CMAKE_BINARY_DIR}/assets/models/${MODEL_FILE}")
    get_filename_component(MODEL_OUTPUT_DIRECTORY "${MODEL_OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${MODEL_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MODEL_OUTPUT_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MODEL_SOURCE}" "${MODEL_OUTPUT}"
        DEPENDS "${MODEL_SOURCE}"
        COMMENT "Deploying showcase model ${MODEL_FILE}"
        VERBATIM
    )
    list(APPEND DEMO_MODEL_OUTPUTS "${MODEL_OUTPUT}")
endforeach()
add_custom_target(demo_models DEPENDS ${DEMO_MODEL_OUTPUTS})
