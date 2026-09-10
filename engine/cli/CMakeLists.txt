add_executable(spectrapack-engine src/main.cpp)
target_link_libraries(spectrapack-engine PRIVATE pack_service)
target_compile_features(spectrapack-engine PRIVATE cxx_std_20)
target_compile_definitions(spectrapack-engine PRIVATE
  SPECTRAPACK_ENGINE_VERSION="${SPECTRAPACK_ENGINE_VERSION}"
  SPECTRAPACK_ENGINE_COMMIT="${SPECTRAPACK_ENGINE_COMMIT}")
install(TARGETS spectrapack-engine RUNTIME DESTINATION bin COMPONENT engine)
