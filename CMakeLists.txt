cmake_minimum_required(VERSION 3.21)
project(ps26147 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# One source tree, two targets:
#   native: cmake -B build && cmake --build build
#   WASM:   scripts/build-wasm.sh   (qt-cmake chainloads the Emscripten toolchain)
if(EMSCRIPTEN)
  message(STATUS "ps26147: configuring WASM target (Emscripten ${EMSCRIPTEN_VERSION})")
else()
  message(STATUS "ps26147: configuring native target")
endif()

option(SIG_BUILD_TESTS "Build the C++ test suite" ON)
option(SIG_BUILD_APP "Build the Qt Widgets GUI (needs Qt6 Widgets)" ON)

add_subdirectory(core)

if(SIG_BUILD_APP)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    add_subdirectory(app)
  else()
    message(STATUS "ps26147: Qt6 Widgets not found, skipping /app")
  endif()
endif()

if(SIG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
