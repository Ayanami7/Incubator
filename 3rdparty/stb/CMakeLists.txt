add_library(stb_image STATIC stb_image_impl.cpp)
target_include_directories(stb_image SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(stb_image PRIVATE
    STBI_ONLY_PNG
    STBI_NO_STDIO
    STBI_MAX_DIMENSIONS=4096
)
set_target_properties(stb_image PROPERTIES FOLDER "3rdparty/stb")
