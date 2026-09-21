add_library(pack_solver STATIC
  src/incumbent.cpp
  src/baseline.cpp
)
target_include_directories(pack_solver PUBLIC include)
target_link_libraries(pack_solver PUBLIC pack_geometry)
target_compile_features(pack_solver PUBLIC cxx_std_20)
target_compile_options(pack_solver PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)

add_executable(spectrapack_solver_tests
  tests/incumbent_tests.cpp
  tests/baseline_tests.cpp
)
target_link_libraries(spectrapack_solver_tests PRIVATE
  pack_solver spectrapack_test_support Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(spectrapack_solver_tests)
