cmake_minimum_required(VERSION 3.25)

project(SpectraPack LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  set(SPECTRAPACK_REQUIRED_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND
     NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL SPECTRAPACK_REQUIRED_MSVC_RUNTIME)
    message(FATAL_ERROR
      "CMAKE_MSVC_RUNTIME_LIBRARY must be ${SPECTRAPACK_REQUIRED_MSVC_RUNTIME} for the static CPU build.")
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "${SPECTRAPACK_REQUIRED_MSVC_RUNTIME}" CACHE STRING
    "SpectraPack's required static MSVC runtime")
endif()

option(SPECTRAPACK_ENABLE_VULKAN "Enable the Vulkan compute backend" OFF)
if(SPECTRAPACK_ENABLE_VULKAN)
  message(FATAL_ERROR
    "SPECTRAPACK_ENABLE_VULKAN=ON is unsupported: the Vulkan compute backend is not implemented.")
endif()

find_package(Catch2 3.11.0 CONFIG REQUIRED)

add_subdirectory(engine/dependencies)
add_subdirectory(tests)
add_subdirectory(benchmarks)
