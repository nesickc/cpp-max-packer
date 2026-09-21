add_library(pack_compute STATIC src/correlation.cpp)
target_include_directories(pack_compute PUBLIC include)
target_compile_features(pack_compute PUBLIC cxx_std_20)
target_compile_options(pack_compute PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)
target_link_libraries(pack_compute PRIVATE spectrapack_dependency_cpu_compute)

add_executable(spectrapack_compute_tests tests/correlation_tests.cpp)
target_link_libraries(spectrapack_compute_tests PRIVATE
  pack_compute spectrapack_test_support Catch2::Catch2WithMain)
target_include_directories(spectrapack_compute_tests PRIVATE src)
include(Catch)
catch_discover_tests(spectrapack_compute_tests)
