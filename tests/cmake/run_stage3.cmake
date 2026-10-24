execute_process(COMMAND "${SKYNET}" "${CONFIG}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT result STREQUAL "0" OR NOT output MATCHES "STAGE3_OK" OR output MATCHES "STAGE3_FAILED")
    message(FATAL_ERROR "stage3 failed (${result})\n${output}\n${errors}")
endif()
