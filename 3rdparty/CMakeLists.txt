# 3rdparty CMakelists.txt
set(THIRD_PARTY_FOLDER "3rdparty")

# fmt (placed before spdlog so external fmt is available)
if(NOT TARGET fmt)
    add_subdirectory(fmt-12.1.0)
    set_target_properties(fmt PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/fmt)
endif()

# spdlog (force using external fmt)
if(NOT TARGET spdlog)
    option(SPDLOG_FMT_EXTERNAL "Use external fmt" ON)
    option(SPDLOG_FMT_EXTERNAL_HO "" OFF)
    option(SPDLOG_BUILD_EXAMPLE "" OFF)
    option(SPDLOG_INSTALL "" OFF)
    add_subdirectory(spdlog-1.16.0)
    set_target_properties(spdlog PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/spdlog)
endif()

# google test
if(NOT TARGET gtest AND NOT TARGET gmock)
    option(BUILD_GTEST "" ON)
    option(BUILD_GMOCK "" ON)
    add_subdirectory(googletest-1.15.0)
    set_target_properties(gtest PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/gtest)
    set_target_properties(gmock PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/gtest)
    set_target_properties(gtest_main PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/gtest)
    set_target_properties(gmock_main PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/gtest)
endif()

# glfw
if(NOT TARGET glfw)
    option(GLFW_BUILD_EXAMPLES "" OFF)
    option(GLFW_BUILD_TESTS "" OFF)
    option(GLFW_BUILD_DOCS "" OFF)
    add_subdirectory(glfw)
    set_target_properties(glfw PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/glfw)
endif()

# imgui
if(NOT TARGET imgui)
    add_subdirectory(imgui-1.91.9b)
    set_target_properties(imgui PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/imgui)
endif()

# vulkan
if(WIN32)
    add_subdirectory(vulkan)
    set_target_properties(vulkan PROPERTIES FOLDER ${THIRD_PARTY_FOLDER}/vulkan)
endif()
