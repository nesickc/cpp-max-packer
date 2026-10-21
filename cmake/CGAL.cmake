# CGAL Alpha Wrapping, pinned independently of vcpkg's GMP-requiring port.
# Boost.Multiprecision is a supported exact-arithmetic backend in CGAL 6.
include(FetchContent)
FetchContent_Declare(spectrapack_cgal
  URL https://github.com/CGAL/cgal/archive/refs/tags/v6.0.1.tar.gz
  URL_HASH SHA512=28963cdbf84d516290da69ca2ad5c90ec61dd9ca9cd97643ccf5b553c406e96a82621270214c0c81c32da7f8c388090a304d229925ed6e1548dbce3abcf1a259
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR spectrapack-no-subproject)
FetchContent_MakeAvailable(spectrapack_cgal)
set(CGAL_DISABLE_GMP TRUE CACHE BOOL "Use the pinned Boost arithmetic backend" FORCE)
find_package(CGAL 6.0.1 EXACT CONFIG REQUIRED PATHS "${spectrapack_cgal_SOURCE_DIR}" NO_DEFAULT_PATH)
foreach(component engine cpu-diagnostics)
  install(FILES "${spectrapack_cgal_SOURCE_DIR}/Installation/LICENSE"
    "${spectrapack_cgal_SOURCE_DIR}/Installation/LICENSE.GPL"
    "${spectrapack_cgal_SOURCE_DIR}/Installation/LICENSE.LGPL"
    "${spectrapack_cgal_SOURCE_DIR}/Installation/LICENSE.BSL"
    DESTINATION share/licenses/cgal COMPONENT ${component})
endforeach()
