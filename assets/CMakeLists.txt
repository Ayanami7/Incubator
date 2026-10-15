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
