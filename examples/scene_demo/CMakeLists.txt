add_executable(scene_demo main.cpp scene_demo.h)
target_link_libraries(scene_demo PRIVATE IncubatorRuntime imgui)
add_dependencies(scene_demo demo_shaders demo_textures demo_models)
set_target_properties(scene_demo PROPERTIES FOLDER "examples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
    target_compile_options(scene_demo PRIVATE /utf-8)
endif()
