# 编辑工具构建目标，同时作为输出程序名称。
set(TARGET_NAME Editor)

# 编辑工具自身的头文件与源文件。
set(EDITOR_HEADERS
    editor_app.h
    editor_controller.h
    editor_scene.h
    editor_operations.h
    editor_selection.h
    editor_theme.h
    gizmo.h
    selection_overlay.h
    viewport_debug_lines.h
    viewport_screenshot.h
    workspace.h
)
set(EDITOR_SOURCES
    editor_app.cpp
    editor_controller.cpp
    editor_controller_assets.cpp
    editor_controller_viewport.cpp
    editor_scene.cpp
    editor_scene_panels.cpp
    editor_scene_properties.cpp
    editor_operations.cpp
    editor_theme.cpp
    gizmo.cpp
    selection_overlay.cpp
    viewport_debug_lines.cpp
    viewport_screenshot.cpp
)

# 让编辑器及其测试共用同一份编译后的应用实现。
add_library(IncubatorEditorSupport STATIC
    ${EDITOR_HEADERS}
    ${EDITOR_SOURCES}
)
target_link_libraries(IncubatorEditorSupport PUBLIC IncubatorAppCommon PRIVATE comdlg32)
set_target_properties(IncubatorEditorSupport PROPERTIES FOLDER "app/editor")

add_executable(${TARGET_NAME} main.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES
    FOLDER "app/editor"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(${TARGET_NAME} PRIVATE IncubatorEditorSupport)
add_dependencies(${TARGET_NAME} incubator_assets)
