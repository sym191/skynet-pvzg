execute_process(COMMAND "${SKYNET}" "${CONFIG}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 60)
if(NOT result STREQUAL "0" OR NOT output MATCHES "STAGE4_OK" OR output MATCHES "STAGE4_FAILED")
    message(FATAL_ERROR "stage4 failed (${result})\n${output}\n${errors}")
endif()
