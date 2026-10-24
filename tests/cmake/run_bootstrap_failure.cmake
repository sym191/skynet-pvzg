file(READ "${CONFIG}" config)
file(WRITE "${CONFIG}.failure" "${config}\nbootstrap = 'snlua stage3_missing_bootstrap'\n")
execute_process(COMMAND "${SKYNET}" "${CONFIG}.failure"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 5)
if(NOT result STREQUAL "1")
    message(FATAL_ERROR "bootstrap failure must exit 1, got ${result}\n${output}\n${errors}")
endif()
