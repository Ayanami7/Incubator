# Demo 消费层测试与独立的轨道相机 CPU 测试。
include(GoogleTest)

add_executable(demo_test demo_test.cpp "${CMAKE_SOURCE_DIR}/examples/demo/demo_app.cpp"
    "${CMAKE_SOURCE_DIR}/examples/demo/startup_config.cpp")
target_link_libraries(demo_test PRIVATE GTest::gtest_main IncubatorRuntime vulkan imgui)
target_include_directories(demo_test PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/test")
target_compile_definitions(demo_test PRIVATE
    RUNTIME_TEST_ASSET_DIR="${CMAKE_BINARY_DIR}/test-fixtures/assets")
target_include_directories(demo_test SYSTEM PRIVATE "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")
add_dependencies(demo_test runtime_test_fixtures runtime_test_shaders demo_models demo_scenes)
set_target_properties(demo_test PROPERTIES FOLDER "UnitTest/examples/demo")
gtest_discover_tests(demo_test
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# 独立进程写入与读取同一场景文件，用于阶段重启验收。
add_executable(scene_restart_test scene_restart_test.cpp "${CMAKE_SOURCE_DIR}/examples/demo/demo_app.cpp")
target_link_libraries(scene_restart_test PRIVATE IncubatorRuntime vulkan imgui)
target_include_directories(scene_restart_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
add_dependencies(scene_restart_test demo_models demo_scenes demo_shaders runtime_ui_fonts)
set_target_properties(scene_restart_test PROPERTIES FOLDER "UnitTest/examples/demo")

add_executable(orbit_camera_test orbit_camera_test.cpp)
target_link_libraries(orbit_camera_test PRIVATE GTest::gtest_main IncubatorRuntime imgui)
target_include_directories(orbit_camera_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
set_target_properties(orbit_camera_test PROPERTIES FOLDER "UnitTest/examples/demo")
gtest_discover_tests(orbit_camera_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(startup_config_test startup_config_test.cpp "${CMAKE_SOURCE_DIR}/examples/demo/startup_config.cpp")
target_link_libraries(startup_config_test PRIVATE GTest::gtest_main IncubatorRuntime)
target_include_directories(startup_config_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_include_directories(startup_config_test SYSTEM PRIVATE "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include")
set_target_properties(startup_config_test PROPERTIES FOLDER "UnitTest/examples/demo")
gtest_discover_tests(startup_config_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
