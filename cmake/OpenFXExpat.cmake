# SPDX-License-Identifier: BSD-3-Clause
# Find the system expat for HostSupport (used by openfx's CMakeLists.txt and by the installed
# openfxConfig.cmake): CMake's FindEXPAT, or conan's expat::expat. On a universal macOS build
# (more than one CMAKE_OSX_ARCHITECTURES), the SDK's expat, which is universal (MacPorts' and
# Homebrew's are not), unless EXPAT_INCLUDE_DIR is already set.
#
#   ofx_find_expat([<min version>] [REQUIRED])
macro(ofx_find_expat)
  if(NOT TARGET expat::expat)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _ofx_n_archs)
    if(APPLE AND _ofx_n_archs GREATER 1 AND NOT EXPAT_INCLUDE_DIR)
      # the SDK path: CMAKE_OSX_SYSROOT if it is a path (CMake 4 leaves it empty by default, and it
      # may be an SDK name), else CMake's internal _CMAKE_OSX_SYSROOT_PATH, else xcrun
      set(_ofx_sdk "")
      if(IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}" AND IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
        set(_ofx_sdk "${CMAKE_OSX_SYSROOT}")
      elseif(IS_DIRECTORY "${_CMAKE_OSX_SYSROOT_PATH}")
        set(_ofx_sdk "${_CMAKE_OSX_SYSROOT_PATH}")
      else()
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                        OUTPUT_VARIABLE _ofx_sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      endif()
      if(_ofx_sdk AND EXISTS "${_ofx_sdk}/usr/lib/libexpat.tbd")
        # (EXPAT_LIBRARY_RELEASE: FindEXPAT's cache variable since CMake 3.27)
        set(EXPAT_INCLUDE_DIR "${_ofx_sdk}/usr/include" CACHE PATH "expat include directory")
        set(EXPAT_LIBRARY "${_ofx_sdk}/usr/lib/libexpat.tbd" CACHE FILEPATH "expat library")
        set(EXPAT_LIBRARY_RELEASE "${EXPAT_LIBRARY}" CACHE FILEPATH "expat library")
      endif()
    endif()
    find_package(EXPAT ${ARGN})
    if(EXPAT_FOUND)
      # CMake's FindEXPAT provides EXPAT::EXPAT; conan's CMakeDeps provides expat::expat
      add_library(expat::expat INTERFACE IMPORTED)
      target_link_libraries(expat::expat INTERFACE EXPAT::EXPAT)
    endif()
  endif()
endmacro()
