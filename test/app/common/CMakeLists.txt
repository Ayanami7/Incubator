include(GoogleTest)
add_executable(app_common_cpu_test
    orbit_camera_test.cpp
    startup_config_test.cpp
)
target_link_libraries(app_common_cpu_test PRIVATE GTest::gtest_main IncubatorAppCommon)
set_target_properties(app_common_cpu_test PROPERTIES FOLDER "UnitTest/app/common")
gtest_discover_tests(app_common_cpu_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" PROPERTIES LABELS "cpu\\;common")
