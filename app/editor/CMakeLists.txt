add_executable(incubator_editor main.cpp editor_app.cpp editor_app.h editor_scene.h workspace.h
    "${CMAKE_SOURCE_DIR}/app/common/startup_config.cpp")
target_link_libraries(incubator_editor PRIVATE IncubatorRuntime imgui)
target_include_directories(incubator_editor PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(incubator_editor SYSTEM PRIVATE "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")
add_dependencies(incubator_editor incubator_assets)
set_target_properties(incubator_editor PROPERTIES FOLDER "app/editor"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
