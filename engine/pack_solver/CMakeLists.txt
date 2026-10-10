add_library(pack_solver STATIC
  src/incumbent.cpp
  src/baseline.cpp
  src/orientations.cpp
  src/spectral.cpp
  src/spectral_pipeline.cpp
)
target_include_directories(pack_solver PUBLIC include)
target_link_libraries(pack_solver PUBLIC pack_geometry pack_compute)
target_compile_features(pack_solver PUBLIC cxx_std_20)
target_compile_options(pack_solver PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)

add_executable(spectrapack_solver_tests
  tests/incumbent_tests.cpp
  tests/baseline_tests.cpp
  tests/orientations_tests.cpp
  tests/spectral_tests.cpp
  tests/t009_tests.cpp
  tests/t010_tests.cpp
)
target_compile_definitions(spectrapack_solver_tests PRIVATE SPECTRAPACK_TEST_ROOT="${PROJECT_SOURCE_DIR}")
target_link_libraries(spectrapack_solver_tests PRIVATE
  pack_solver spectrapack_test_support Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(spectrapack_solver_tests)

set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${PROJECT_SOURCE_DIR}/tests/cmake/QualificationTimeouts.cmake")
