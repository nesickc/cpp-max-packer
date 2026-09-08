cmake_minimum_required(VERSION 3.25)

project(SpectraPack LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Catch2 3.11.0 CONFIG REQUIRED)

add_subdirectory(tests)
add_subdirectory(benchmarks)
