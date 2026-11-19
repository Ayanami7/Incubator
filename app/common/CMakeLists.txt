add_library(IncubatorAppCommon STATIC
    app_session.h
    app_session.cpp
    lighting_controls.h
    orbit_camera.h
    session_options.h
    startup_config.h
    startup_config.cpp
    ui_input.h
    validation_mode.h
    viewport.h
)
target_link_libraries(IncubatorAppCommon PUBLIC IncubatorRuntime imgui)
target_include_directories(IncubatorAppCommon PUBLIC "${CMAKE_SOURCE_DIR}")
target_include_directories(IncubatorAppCommon SYSTEM PRIVATE
    "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include"
)
set_target_properties(IncubatorAppCommon PROPERTIES FOLDER "app/common")
