include(GoogleTest)

add_executable(editor_cpu_test
    editor_operations_cpu_test.cpp
    editor_controller_cpu_test.cpp
    editor_viewport_tools_cpu_test.cpp
)
target_link_libraries(editor_cpu_test PRIVATE GTest::gtest_main IncubatorEditorSupport)
target_include_directories(editor_cpu_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/test")
set_target_properties(editor_cpu_test PROPERTIES FOLDER "UnitTest/app/editor")
gtest_discover_tests(editor_cpu_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" PROPERTIES LABELS "cpu\\;editor")

add_executable(editor_test
    editor_workspace_docs_test.cpp
    editor_interaction_test.cpp
    editor_render_wiring_test.cpp
)
target_link_libraries(editor_test PRIVATE GTest::gtest_main IncubatorEditorSupport vulkan)
target_include_directories(editor_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/test")
target_compile_definitions(editor_test PRIVATE RUNTIME_TEST_ASSET_DIR="${CMAKE_BINARY_DIR}/test-fixtures/assets")
add_dependencies(editor_test runtime_test_fixtures runtime_test_shaders incubator_assets)
set_target_properties(editor_test PROPERTIES FOLDER "UnitTest/app/editor")
gtest_discover_tests(editor_test WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "gpu\\;editor")

# 独立隔离内置展示场景端到端对照与截图导出回归，支持日常按标签跳过。
add_executable(editor_showcase_test editor_showcase_test.cpp)
target_link_libraries(editor_showcase_test PRIVATE GTest::gtest_main IncubatorEditorSupport vulkan)
target_include_directories(editor_showcase_test PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/test")
target_compile_definitions(editor_showcase_test PRIVATE RUNTIME_TEST_ASSET_DIR="${CMAKE_BINARY_DIR}/test-fixtures/assets")
add_dependencies(editor_showcase_test runtime_test_fixtures runtime_test_shaders incubator_assets)
set_target_properties(editor_showcase_test PROPERTIES FOLDER "UnitTest/app/editor")
gtest_discover_tests(editor_showcase_test
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "gpu\\;editor\\;showcase")

# 编辑器的独立进程恢复检查不作为 GoogleTest 程序发现。
add_executable(editor_scene_restart scene_restart_test.cpp)
target_link_libraries(editor_scene_restart PRIVATE IncubatorEditorSupport vulkan)
target_include_directories(editor_scene_restart PRIVATE "${CMAKE_SOURCE_DIR}")
add_dependencies(editor_scene_restart incubator_assets)
set_target_properties(editor_scene_restart PROPERTIES FOLDER "UnitTest/app/editor")
