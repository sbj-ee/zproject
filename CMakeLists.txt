cmake_minimum_required(VERSION 3.21)

# zproject supports Apple Silicon only on macOS. Set before project() so the
# compiler checks use it; an explicit -DCMAKE_OSX_ARCHITECTURES=... still wins.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures for macOS" FORCE)
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Single source of truth for the version (SPEC.md §22): bump ONLY the VERSION
# here. It reaches all three binaries through generated/zproject/version.hpp,
# and CPack names the packages from PROJECT_VERSION.
project(zproject VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ZPROJECT_BUILD_TESTS "Build the Qt Test suite" ON)
# Off for the server-only build (no Qt Widgets needed): zprojectd + CLI.
option(ZPROJECT_BUILD_GUI "Build the zproject desktop app (Qt Widgets)" ON)

# Minimum Qt is 6.4 (Ubuntu 24.04 noble / Pop!_OS 24.04 ship 6.4.2; SPEC.md
# §4.3). Use no Qt API newer than 6.4.
set(ZPROJECT_QT_COMPONENTS Core Network)
if(ZPROJECT_BUILD_GUI)
  list(APPEND ZPROJECT_QT_COMPONENTS Gui Widgets)
endif()
if(ZPROJECT_BUILD_TESTS)
  list(APPEND ZPROJECT_QT_COMPONENTS Test)
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${ZPROJECT_QT_COMPONENTS})

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

set(ZPROJECT_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/core/version.hpp.in"
  "${ZPROJECT_GENERATED_DIR}/zproject/version.hpp"
  @ONLY)

# macOS bundle id and minimum (Info.plist; SPEC.md §20).
set(ZPROJECT_MACOS_MIN "${CMAKE_OSX_DEPLOYMENT_TARGET}")

include(cmake/SQLite.cmake)
add_subdirectory(core)
add_subdirectory(store)
add_subdirectory(server)
add_subdirectory(client)
add_subdirectory(cli)
if(ZPROJECT_BUILD_GUI)
  add_subdirectory(app)
  if(APPLE)
    include(cmake/MacBundle.cmake)
  endif()
endif()

if(ZPROJECT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
