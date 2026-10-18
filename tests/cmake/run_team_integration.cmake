if(NOT DEFINED TEAM_TEST_SKYNET OR NOT DEFINED TEAM_TEST_CONFIG)
    message(FATAL_ERROR "TEAM_TEST_SKYNET and TEAM_TEST_CONFIG are required")
endif()

execute_process(
    COMMAND "${TEAM_TEST_SKYNET}" "${TEAM_TEST_CONFIG}"
    RESULT_VARIABLE team_test_result
    OUTPUT_VARIABLE team_test_stdout
    ERROR_VARIABLE team_test_stderr
    TIMEOUT 30
)

set(team_test_log "${team_test_stdout}\n${team_test_stderr}")
if(team_test_log MATCHES "TEAM_INTEGRATION_PASS")
    message(STATUS "Team service integration test passed")
elseif(team_test_log MATCHES "TEAM_INTEGRATION_FAIL")
    message(FATAL_ERROR "Team service integration test failed:\n${team_test_log}")
else()
    message(FATAL_ERROR
        "Team service integration test produced no result (exit ${team_test_result}):\n${team_test_log}"
    )
endif()
