# project root CMakeLists.txt
cmake_minimum_required(VERSION 3.19 FATAL_ERROR)
project(Incubator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

file(MAKE_DIRECTORY bin/Debug)
file(MAKE_DIRECTORY bin/Release)

# # set generate output directory
# set(publish_bin_debug ${PROJECT_SOURCE_DIR}/bin/Debug)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${publish_bin_debug})
# set(publish_bin_release ${PROJECT_SOURCE_DIR}/bin/Release)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${publish_bin_release})

# ---- Include guards ----
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
    )
endif()


# first directory
set(INCUBATOR_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(BINARY_ROOT_DIR "${INCUBATOR_ROOT_DIR}/")


# second directory
set(SRC_DIR "${INCUBATOR_ROOT_DIR}/src")
set(ASSETS_DIR "${INCUBATOR_ROOT_DIR}/assets")
set(THIRD_PARTY_DIR "${INCUBATOR_ROOT_DIR}/3rdparty")


add_subdirectory(3rdparty)
add_subdirectory(src)