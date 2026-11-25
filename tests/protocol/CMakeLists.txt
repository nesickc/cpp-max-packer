add_test(NAME spectrapack_import_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/import_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
set_tests_properties(spectrapack_import_subprocess PROPERTIES TIMEOUT 180)

add_test(NAME spectrapack_desktop_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
# About 180 s with a Debug engine on a developer laptop; a hosted runner is slower.
set_tests_properties(spectrapack_desktop_subprocess PROPERTIES TIMEOUT 600)
add_test(NAME spectrapack_desktop_runtime_subprocess
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop_runtime_subprocess_test.py"
    "$<TARGET_FILE:spectrapack-engine>")
# About 42 s with a Debug engine on a developer laptop; a hosted runner is slower.
set_tests_properties(spectrapack_desktop_runtime_subprocess PROPERTIES TIMEOUT 120)

if(BUILD_TESTING)
  add_executable(spectrapack_result_contract_check result_contract_check.cpp)
  target_link_libraries(spectrapack_result_contract_check PRIVATE pack_io)
  target_compile_features(spectrapack_result_contract_check PRIVATE cxx_std_20)

  foreach(solve_case IN ITEMS
    solve_timed_memory_cap
    solve_timed_repacking
    solve_cube_64
    solve_box_tolerance
    solve_empty
    solve_unicode_relocation
    solve_transforms
    solve_resolved_catalog
    solve_rejections
    solve_continue
    solve_continue_rejections)
    add_test(NAME spectrapack_${solve_case}_subprocess
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/solve_subprocess_test.py"
        "$<TARGET_FILE:spectrapack-engine>"
        --contract-validator "$<TARGET_FILE:spectrapack_result_contract_check>"
        --case "${solve_case}")
    set_tests_properties(spectrapack_${solve_case}_subprocess PROPERTIES TIMEOUT 30)
  endforeach()
  # A dozen refusals: about 18 s with a Debug engine on a developer laptop.
  set_tests_properties(spectrapack_solve_continue_rejections_subprocess PROPERTIES TIMEOUT 90)
endif()

if(BUILD_TESTING)
  foreach(control_case IN ITEMS
    solve_deadline
    solve_console_stop)
    add_test(NAME spectrapack_${control_case}_subprocess
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/solve_controls_subprocess_test.py"
        "$<TARGET_FILE:spectrapack-engine>"
        --case "${control_case}")
    set_tests_properties(spectrapack_${control_case}_subprocess PROPERTIES TIMEOUT 30)
  endforeach()
endif()

