# 编辑工具构建目标，同时作为输出程序名称。
set(TARGET_NAME Editor)

# 编辑工具自身的头文件与源文件。
set(EDITOR_HEADERS
    editor_app.h
    editor_scene.h
    workspace.h
)
set(EDITOR_SOURCES
    main.cpp
    editor_app.cpp
)

# 两个应用实际共用的小组件，按需编译到当前目标。
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(COMMON_HEADERS
    "${COMMON_DIR}/lighting_controls.h"
    "${COMMON_DIR}/orbit_camera.h"
    "${COMMON_DIR}/session_options.h"
    "${COMMON_DIR}/startup_config.h"
    "${COMMON_DIR}/ui_input.h"
    "${COMMON_DIR}/validation_mode.h"
    "${COMMON_DIR}/viewport.h"
)
set(COMMON_SOURCES
    "${COMMON_DIR}/startup_config.cpp"
)

# 创建程序并设置开发运行目录。
add_executable(${TARGET_NAME}
    ${EDITOR_HEADERS}
    ${EDITOR_SOURCES}
    ${COMMON_HEADERS}
    ${COMMON_SOURCES}
)
set_target_properties(${TARGET_NAME} PROPERTIES
    FOLDER "app/editor"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# 链接运行时与界面依赖，构建时部署所需资产。
target_link_libraries(${TARGET_NAME} PRIVATE
    IncubatorRuntime
    imgui
)
add_dependencies(${TARGET_NAME} incubator_assets)

# 项目头文件与仅供启动配置解析使用的第三方头文件。
target_include_directories(${TARGET_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}"
)
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    "${THIRD_PARTY_DIR}/assimp/contrib/rapidjson/include"
)
