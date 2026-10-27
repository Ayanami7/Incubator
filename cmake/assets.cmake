# 部署 Demo 使用的模型、场景和默认启动配置；精确测试样本由 test/runtime 单独部署。
set(DEMO_MODEL_OUTPUTS)
foreach(MODEL_FILE IN ITEMS
    README.md
    utah_teapot/utah_teapot.obj
    avocado/glTF/Avocado.gltf
    avocado/glTF/Avocado.bin
    avocado/glTF/Avocado_baseColor.png
    avocado/glTF/Avocado_roughnessMetallic.png
    avocado/glTF/Avocado_normal.png)
    set(MODEL_SOURCE "${ASSETS_DIR}/models/${MODEL_FILE}")
    set(MODEL_OUTPUT "${CMAKE_BINARY_DIR}/assets/models/${MODEL_FILE}")
    get_filename_component(MODEL_OUTPUT_DIRECTORY "${MODEL_OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${MODEL_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MODEL_OUTPUT_DIRECTORY}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MODEL_SOURCE}" "${MODEL_OUTPUT}"
        DEPENDS "${MODEL_SOURCE}"
        COMMENT "部署示例模型 ${MODEL_FILE}"
        VERBATIM
    )
    list(APPEND DEMO_MODEL_OUTPUTS "${MODEL_OUTPUT}")
endforeach()
add_custom_target(demo_models DEPENDS ${DEMO_MODEL_OUTPUTS})

set(DEMO_SCENE_OUTPUT "${CMAKE_BINARY_DIR}/assets/scenes/mixed.json")
add_custom_command(OUTPUT "${DEMO_SCENE_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/assets/scenes"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ASSETS_DIR}/scenes/mixed.json" "${DEMO_SCENE_OUTPUT}"
    DEPENDS "${ASSETS_DIR}/scenes/mixed.json" VERBATIM)
add_custom_target(demo_scenes DEPENDS "${DEMO_SCENE_OUTPUT}")

set(DEMO_CONFIG_OUTPUT "${CMAKE_BINARY_DIR}/incubator.json")
add_custom_command(OUTPUT "${DEMO_CONFIG_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${INCUBATOR_ROOT_DIR}/incubator.json" "${DEMO_CONFIG_OUTPUT}"
    DEPENDS "${INCUBATOR_ROOT_DIR}/incubator.json"
    COMMENT "部署默认启动配置"
    VERBATIM)
add_custom_target(demo_config DEPENDS "${DEMO_CONFIG_OUTPUT}")
