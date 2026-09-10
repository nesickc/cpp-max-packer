add_library(spectrapack_test_support STATIC
  support/src/analytic_fixtures.cpp
  support/src/integer_correlation.cpp
)
target_include_directories(spectrapack_test_support PUBLIC support/include)
target_compile_features(spectrapack_test_support PUBLIC cxx_std_20)

add_executable(spectrapack_native_tests
  native/analytic_fixtures_tests.cpp
  native/integer_correlation_tests.cpp
)
target_link_libraries(spectrapack_native_tests PRIVATE
  spectrapack_test_support
  Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(spectrapack_native_tests)

add_executable(spectrapack_cpu_dependency_smoke
  integration/cpu_dependencies_main.cpp
  integration/cpu_dependencies_tests.cpp
)
target_link_libraries(spectrapack_cpu_dependency_smoke PRIVATE
  spectrapack_dependency_geometry
  spectrapack_dependency_cpu_compute
  spectrapack_dependency_json
  Catch2::Catch2
)
if(WIN32)
  target_link_libraries(spectrapack_cpu_dependency_smoke PRIVATE Psapi)
endif()
install(TARGETS spectrapack_cpu_dependency_smoke
  RUNTIME DESTINATION diagnostics
  COMPONENT cpu-diagnostics)

catch_discover_tests(spectrapack_cpu_dependency_smoke)

add_subdirectory(protocol)
