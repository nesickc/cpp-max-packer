add_library(pack_io STATIC
  src/contracts.cpp
  src/embedded_schemas.hpp
)
target_include_directories(pack_io PUBLIC include PRIVATE ../core/include)
target_link_libraries(pack_io PUBLIC
  spectrapack_dependency_json
  nlohmann_json_schema_validator::validator
)
target_compile_features(pack_io PUBLIC cxx_std_20)

add_executable(spectrapack_contract_tests
  tests/contracts_tests.cpp
)
target_link_libraries(spectrapack_contract_tests PRIVATE
  pack_io
  Catch2::Catch2WithMain
)
target_compile_definitions(spectrapack_contract_tests PRIVATE
  SPECTRAPACK_CONTRACT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  SPECTRAPACK_SHARED_CONTRACT_FIXTURES="${CMAKE_SOURCE_DIR}/tests/contracts/schema-fixtures.json"
)

include(Catch)
catch_discover_tests(spectrapack_contract_tests)
