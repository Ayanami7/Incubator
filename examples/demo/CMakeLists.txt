# 最小展示程序，只消费 Runtime 与实际共用的小组件。
add_executable(demo main.cpp demo_app.cpp demo_app.h showcase.h "${CMAKE_SOURCE_DIR}/app/common/startup_config.cpp")
target_link_libraries(demo PRIVATE IncubatorRuntime imgui)
target_include_directories(demo PRIVATE "${CMAKE_SOURCE_DIR}")
target_include_directories(demo SYSTEM PRIVATE "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")
add_dependencies(demo incubator_assets)
set_target_properties(demo PROPERTIES FOLDER "examples" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
