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
