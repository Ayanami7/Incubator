# File dependencies keep texture-only edits independent of executable relinking.
set(REFERENCE_TEXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/textures/reference_rgba.png")
set(REFERENCE_TEXTURE_OUTPUT "${CMAKE_BINARY_DIR}/assets/textures/reference_rgba.png")
add_custom_command(
    OUTPUT "${REFERENCE_TEXTURE_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/assets/textures"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REFERENCE_TEXTURE_SOURCE}" "${REFERENCE_TEXTURE_OUTPUT}"
    DEPENDS "${REFERENCE_TEXTURE_SOURCE}"
    COMMENT "Deploying reference RGBA texture"
    VERBATIM
)
add_custom_target(demo_textures DEPENDS "${REFERENCE_TEXTURE_OUTPUT}")
