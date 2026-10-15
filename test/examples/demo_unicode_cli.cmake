# Exercise the actual Windows Unicode entry point without creating a GPU window.
# Chinese, a space and a non-ACP character must reach both resource selection and diagnostics intact.
set(texture_path "textures/命令行 空格-😀-missing.png")
execute_process(
    COMMAND "${DEMO_EXE}" "${texture_path}"
    WORKING_DIRECTORY "${DEMO_WORKING_DIRECTORY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    ENCODING UTF-8
    TIMEOUT 15)

if(NOT result STREQUAL "1")
    message(FATAL_ERROR "Expected missing texture exit code 1, got ${result}\n${output}\n${errors}")
endif()

string(FIND "${output}" "Scene texture: ${texture_path}" selected_path)
string(FIND "${output}" "TextureLoader 'assets/${texture_path}': PNG file was not found" missing_path)
if(selected_path EQUAL -1 OR missing_path EQUAL -1)
    message(FATAL_ERROR "UTF-8 texture path was not preserved through the command line and loader\n${output}\n${errors}")
endif()
