add_executable(spectrapack-engine src/main.cpp src/solve.cpp src/desktop.cpp src/desktop_session.cpp)
target_link_libraries(spectrapack-engine PRIVATE pack_io pack_solver)
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
  add_executable(spectrapack_runtime_wire_tests tests/runtime_wire_tests.cpp tests/publication_tests.cpp)
  target_include_directories(spectrapack_runtime_wire_tests PRIVATE src)
  target_link_libraries(spectrapack_runtime_wire_tests PRIVATE pack_io pack_solver Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(spectrapack_runtime_wire_tests)
endif()
