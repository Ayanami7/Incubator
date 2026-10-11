# examples/demo CMakelists.txt

set(TARGET_NAME demo)

add_executable(
    ${TARGET_NAME}
    main.cpp
)

if(MSVC)
    target_compile_options(${TARGET_NAME} PUBLIC /utf-8) # MSVC unicode support
endif()

add_dependencies(${TARGET_NAME} IncubatorRuntime)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "examples")

target_link_libraries(
    ${TARGET_NAME} PUBLIC
    IncubatorRuntime
)
target_include_directories(
    ${TARGET_NAME} PUBLIC
)
