add_test(NAME spectrapack_protocol_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/service_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
set_tests_properties(spectrapack_protocol_subprocess PROPERTIES TIMEOUT 10)

add_custom_target(spectrapack_service_replay_perf
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/service_replay_perf.py"
    "$<TARGET_FILE:spectrapack-engine>" --samples 5 --requests 1000
  DEPENDS spectrapack-engine
  USES_TERMINAL)
