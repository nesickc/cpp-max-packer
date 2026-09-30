add_test(NAME spectrapack_protocol_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/service_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
set_tests_properties(spectrapack_protocol_subprocess PROPERTIES TIMEOUT 10)

add_test(NAME spectrapack_import_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/import_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
set_tests_properties(spectrapack_import_subprocess PROPERTIES TIMEOUT 15)

add_test(NAME spectrapack_desktop_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
set_tests_properties(spectrapack_desktop_subprocess PROPERTIES TIMEOUT 300)

if(BUILD_TESTING)
  add_executable(spectrapack_result_contract_check result_contract_check.cpp)
  target_link_libraries(spectrapack_result_contract_check PRIVATE pack_io)
  target_compile_features(spectrapack_result_contract_check PRIVATE cxx_std_20)

  foreach(solve_case IN ITEMS
    solve_cube_64
    solve_empty
    solve_unicode_relocation
    solve_transforms
    solve_resolved_catalog
    solve_rejections)
    add_test(NAME spectrapack_${solve_case}_subprocess
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/solve_subprocess_test.py"
        "$<TARGET_FILE:spectrapack-engine>"
        --contract-validator "$<TARGET_FILE:spectrapack_result_contract_check>"
        --case "${solve_case}")
    set_tests_properties(spectrapack_${solve_case}_subprocess PROPERTIES TIMEOUT 30)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(control_case IN ITEMS
    solve_retained_resource
    solve_deadline
    solve_console_stop
    solve_caller_reserve)
    add_test(NAME spectrapack_${control_case}_subprocess
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/solve_controls_subprocess_test.py"
        "$<TARGET_FILE:spectrapack-engine>"
        --memory-helper "$<TARGET_FILE:spectrapack_cli_memory_check>"
        --case "${control_case}")
    set_tests_properties(spectrapack_${control_case}_subprocess PROPERTIES TIMEOUT 30)
  endforeach()
endif()


add_custom_target(spectrapack_service_replay_perf
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/service_replay_perf.py"
    "$<TARGET_FILE:spectrapack-engine>" --samples 5 --requests 1000
  DEPENDS spectrapack-engine
  USES_TERMINAL)
