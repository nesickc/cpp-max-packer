add_library(pack_geometry STATIC
  src/import.cpp src/solid_analysis.cpp src/exact_predicates.cpp
  src/validation.cpp src/validation_kernel.cpp
  src/display_lod.cpp src/conservative_fields.cpp)
target_include_directories(pack_geometry PUBLIC include)
target_compile_features(pack_geometry PUBLIC cxx_std_20)
target_compile_options(pack_geometry PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/fp:strict>)
# The bounded meshoptimizer callback may throw across its C-linkage API.
# MSVC's default /EHsc assumes such calls cannot throw; clear that assumption
# for this adapter so Release retains its catch/cleanup paths.
set_source_files_properties(src/display_lod.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/EHsc->")
target_link_libraries(pack_geometry PRIVATE
  spectrapack_dependency_import_geometry spectrapack_dependency_display_geometry)

add_executable(spectrapack_import_tests tests/import_tests.cpp)
target_link_libraries(spectrapack_import_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(spectrapack_import_tests)

add_executable(spectrapack_solid_analysis_tests
  tests/solid_analysis_tests.cpp
  tests/predicate_tests.cpp)
target_link_libraries(spectrapack_solid_analysis_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
catch_discover_tests(spectrapack_solid_analysis_tests)

add_executable(spectrapack_validation_tests
  tests/validation_tests.cpp
  tests/validation_kernel_tests.cpp)
target_link_libraries(spectrapack_validation_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
catch_discover_tests(spectrapack_validation_tests)

add_executable(spectrapack_display_lod_tests tests/display_lod_tests.cpp)
target_link_libraries(spectrapack_display_lod_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
catch_discover_tests(spectrapack_display_lod_tests)

add_executable(spectrapack_conservative_fields_tests tests/conservative_fields_tests.cpp)
target_link_libraries(spectrapack_conservative_fields_tests PRIVATE pack_geometry Catch2::Catch2WithMain)
catch_discover_tests(spectrapack_conservative_fields_tests)
