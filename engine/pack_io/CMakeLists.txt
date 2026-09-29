add_library(pack_io STATIC
  src/contracts.cpp
  src/inspection.cpp
  src/result_export.cpp
  src/result_builder.cpp
  src/embedded_schemas.hpp
)
target_include_directories(pack_io PUBLIC include PRIVATE ../core/include ../pack_geometry/include)
target_link_libraries(pack_io PUBLIC
  pack_geometry
  spectrapack_dependency_json
  nlohmann_json_schema_validator::validator
)
if(WIN32)
  target_link_libraries(pack_io PRIVATE bcrypt)
endif()
target_compile_features(pack_io PUBLIC cxx_std_20)
if(BUILD_TESTING)
  target_compile_definitions(pack_io PRIVATE SPECTRAPACK_ASSET_LOADER_TESTING=1)
endif()

add_executable(spectrapack_contract_tests
  tests/contracts_tests.cpp
)
target_link_libraries(spectrapack_contract_tests PRIVATE
  pack_io
  Catch2::Catch2WithMain
)
target_include_directories(spectrapack_contract_tests PRIVATE src)
target_compile_definitions(spectrapack_contract_tests PRIVATE
  SPECTRAPACK_CONTRACT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  SPECTRAPACK_SHARED_CONTRACT_FIXTURES="${CMAKE_SOURCE_DIR}/tests/contracts/schema-fixtures.json"
)

add_executable(spectrapack_result_builder_tests
  tests/result_builder_tests.cpp
)
target_link_libraries(spectrapack_result_builder_tests PRIVATE
  pack_io
  Catch2::Catch2WithMain
)
target_include_directories(spectrapack_result_builder_tests PRIVATE src)
target_compile_definitions(spectrapack_result_builder_tests PRIVATE
  SPECTRAPACK_CONTRACT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)

include(Catch)
catch_discover_tests(spectrapack_contract_tests)
catch_discover_tests(spectrapack_result_builder_tests)
