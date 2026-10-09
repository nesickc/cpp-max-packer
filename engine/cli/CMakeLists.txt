add_executable(spectrapack-engine src/main.cpp src/solve.cpp src/desktop.cpp src/desktop_session.cpp)
target_link_libraries(spectrapack-engine PRIVATE pack_service pack_solver)
target_include_directories(spectrapack-engine PRIVATE ../pack_io/src)
if(WIN32)
  target_link_libraries(spectrapack-engine PRIVATE Psapi bcrypt)
endif()
target_compile_features(spectrapack-engine PRIVATE cxx_std_20)
target_compile_definitions(spectrapack-engine PRIVATE
  SPECTRAPACK_ENGINE_VERSION="${SPECTRAPACK_ENGINE_VERSION}"
  SPECTRAPACK_ENGINE_COMMIT="${SPECTRAPACK_ENGINE_COMMIT}")
install(TARGETS spectrapack-engine RUNTIME DESTINATION bin COMPONENT engine)

if(BUILD_TESTING)
  add_executable(spectrapack_runtime_memory_tests tests/runtime_memory_tests.cpp)
  target_include_directories(spectrapack_runtime_memory_tests PRIVATE src ../pack_io/src ../pack_io/include ../pack_io/tests)
  target_link_libraries(spectrapack_runtime_memory_tests PRIVATE spectrapack_dependency_json spectrapack_runtime_values Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(spectrapack_runtime_memory_tests)
  add_executable(spectrapack_runtime_wire_tests tests/runtime_wire_tests.cpp)
  target_include_directories(spectrapack_runtime_wire_tests PRIVATE src)
  target_link_libraries(spectrapack_runtime_wire_tests PRIVATE pack_io Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(spectrapack_runtime_wire_tests)
  add_executable(spectrapack_cli_memory_check
    tests/solve_memory_test_main.cpp
    src/solve.cpp src/desktop.cpp src/desktop_session.cpp)
  target_include_directories(spectrapack_cli_memory_check PRIVATE src ../pack_io/tests ../pack_io/src)
  target_link_libraries(spectrapack_cli_memory_check PRIVATE pack_service pack_solver)
  if(WIN32)
    target_link_libraries(spectrapack_cli_memory_check PRIVATE Psapi bcrypt)
  endif()
  target_compile_features(spectrapack_cli_memory_check PRIVATE cxx_std_20)
  target_compile_definitions(spectrapack_cli_memory_check PRIVATE SPECTRAPACK_CLI_TESTING)
  add_test(NAME spectrapack_finalization_deadline_subprocess
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/protocol/desktop_runtime_subprocess_test.py"
      "$<TARGET_FILE:spectrapack-engine>" --memory-helper "$<TARGET_FILE:spectrapack_cli_memory_check>"
      --case test_finalization_deadline_preserves_nonempty_authority_and_previous_result)
  set_tests_properties(spectrapack_finalization_deadline_subprocess PROPERTIES TIMEOUT 30)
endif()
