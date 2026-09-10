add_library(pack_geometry STATIC src/import.cpp src/solid_analysis.cpp src/exact_predicates.cpp)
target_include_directories(pack_geometry PUBLIC include)
target_compile_features(pack_geometry PUBLIC cxx_std_20)
target_compile_options(pack_geometry PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)
target_link_libraries(pack_geometry PRIVATE spectrapack_dependency_import_geometry)

add_executable(spectrapack_import_tests tests/import_tests.cpp)
target_link_libraries(spectrapack_import_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(spectrapack_import_tests)

add_executable(spectrapack_solid_analysis_tests
  tests/solid_analysis_tests.cpp
  tests/predicate_tests.cpp)
target_link_libraries(spectrapack_solid_analysis_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
catch_discover_tests(spectrapack_solid_analysis_tests)
