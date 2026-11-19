# 编辑工具构建目标，同时作为输出程序名称。
set(TARGET_NAME Editor)

# 编辑工具自身的头文件与源文件。
set(EDITOR_HEADERS
    editor_app.h
    editor_scene.h
    editor_operations.h
    editor_selection.h
    selection_overlay.h
    workspace.h
)
set(EDITOR_SOURCES
    editor_app.cpp
    editor_scene.cpp
    editor_operations.cpp
    selection_overlay.cpp
)

# 让编辑器及其测试共用同一份编译后的应用实现。
add_library(IncubatorEditorSupport STATIC
    ${EDITOR_HEADERS}
    ${EDITOR_SOURCES}
)
target_link_libraries(IncubatorEditorSupport PUBLIC IncubatorAppCommon)
set_target_properties(IncubatorEditorSupport PROPERTIES FOLDER "app/editor")

add_executable(${TARGET_NAME} main.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES
    FOLDER "app/editor"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_link_libraries(${TARGET_NAME} PRIVATE IncubatorEditorSupport)
add_dependencies(${TARGET_NAME} incubator_assets)
