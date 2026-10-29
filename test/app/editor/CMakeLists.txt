include(GoogleTest)
add_executable(editor_test editor_test.cpp "${CMAKE_SOURCE_DIR}/app/editor/editor_app.cpp"
    "${CMAKE_SOURCE_DIR}/app/common/startup_config.cpp")
target_link_libraries(editor_test PRIVATE GTest::gtest_main IncubatorRuntime vulkan imgui)
target_include_directories(editor_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/test")
target_include_directories(editor_test SYSTEM PRIVATE "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")
target_compile_definitions(editor_test PRIVATE RUNTIME_TEST_ASSET_DIR="${CMAKE_BINARY_DIR}/test-fixtures/assets")
add_dependencies(editor_test runtime_test_fixtures runtime_test_shaders incubator_assets)
set_target_properties(editor_test PROPERTIES FOLDER "UnitTest/app/editor")
gtest_discover_tests(editor_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# 编辑器的独立进程恢复检查不作为 GoogleTest 程序发现。
add_executable(editor_scene_restart scene_restart_test.cpp "${CMAKE_SOURCE_DIR}/app/editor/editor_app.cpp")
target_link_libraries(editor_scene_restart PRIVATE IncubatorRuntime vulkan imgui)
target_include_directories(editor_scene_restart PRIVATE "${CMAKE_SOURCE_DIR}")
add_dependencies(editor_scene_restart incubator_assets)
set_target_properties(editor_scene_restart PROPERTIES FOLDER "UnitTest/app/editor")
