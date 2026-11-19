# 最小展示程序通过共享应用目标使用会话和启动配置。
add_executable(demo main.cpp demo_app.cpp demo_app.h showcase.h)
target_link_libraries(demo PRIVATE IncubatorAppCommon)
add_dependencies(demo incubator_assets)
set_target_properties(demo PROPERTIES FOLDER "examples" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
