find_package(manifold CONFIG REQUIRED)
find_package(fcl CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(nlohmann_json 3.12.0 CONFIG REQUIRED)

find_path(SPECTRAPACK_POCKETFFT_INCLUDE_DIR
  NAMES pocketfft_hdronly.h
  REQUIRED
)

add_library(spectrapack_dependency_geometry INTERFACE)
target_link_libraries(spectrapack_dependency_geometry INTERFACE
  manifold::manifold
  fcl
  meshoptimizer::meshoptimizer
)

add_library(spectrapack_dependency_cpu_compute INTERFACE)
target_include_directories(spectrapack_dependency_cpu_compute INTERFACE
  "${SPECTRAPACK_POCKETFFT_INCLUDE_DIR}"
)

add_library(spectrapack_dependency_json INTERFACE)
target_link_libraries(spectrapack_dependency_json INTERFACE
  nlohmann_json::nlohmann_json
)

set(SPECTRAPACK_DISTRIBUTION_LICENSE_PORTS
  catch2
  ccd
  clipper2
  eigen3
  fcl
  json-schema-validator
  manifold
  meshoptimizer
  nlohmann-json
  octomap
  pocketfft
  tbb
)

if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
  message(FATAL_ERROR "The CPU diagnostic install requires the vcpkg installed directory and target triplet.")
endif()

foreach(port IN LISTS SPECTRAPACK_DISTRIBUTION_LICENSE_PORTS)
  set(copyright_file "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright")
  if(NOT EXISTS "${copyright_file}")
    message(FATAL_ERROR "Required third-party copyright file is missing: ${copyright_file}")
  endif()
  install(FILES "${copyright_file}"
    DESTINATION "share/licenses/${port}"
    COMPONENT cpu-diagnostics)
endforeach()

install(FILES
  "${CMAKE_SOURCE_DIR}/cmake/dependencies.lock.json"
  "${CMAKE_SOURCE_DIR}/cmake/toolchains.lock.json"
  "${CMAKE_BINARY_DIR}/build-metadata.json"
  DESTINATION "share/spectrapack"
  COMPONENT cpu-diagnostics)

# The standalone engine component carries the notices for its actual linked
# runtime libraries, even when the optional CPU diagnostic is not staged.
foreach(port IN ITEMS nlohmann-json json-schema-validator)
  install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright"
    DESTINATION "share/licenses/${port}" COMPONENT engine)
endforeach()
install(FILES
  "${CMAKE_SOURCE_DIR}/cmake/dependencies.lock.json"
  "${CMAKE_SOURCE_DIR}/cmake/toolchains.lock.json"
  "${CMAKE_BINARY_DIR}/build-metadata.json"
  DESTINATION "share/spectrapack" COMPONENT engine)
