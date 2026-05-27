cmake_minimum_required(VERSION 3.18)
project(tts_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release by default — the perf flags below only apply in that config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Shared modules need PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Probe for LTO; we enable it below only if the toolchain supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_OK OUTPUT IPO_ERR)

set(ROOT_DIR  "${CMAKE_SOURCE_DIR}/..")
set(DEPS_DIR  "${ROOT_DIR}/dependencies")
set(SRC_DIR   "${ROOT_DIR}/src")

set(INCLUDE_DIR "${DEPS_DIR}/include")
set(PIPER_SRC   "${DEPS_DIR}/piper/src/cpp")

# pybind11
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 REQUIRED CONFIG)

# opus / ogg
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(OGG  REQUIRED IMPORTED_TARGET ogg)

# The module itself
pybind11_add_module(tts_engine
  ${SRC_DIR}/tts_module.cpp
  ${PIPER_SRC}/piper.cpp
)

target_include_directories(tts_engine PRIVATE
  ${PIPER_SRC}
  ${INCLUDE_DIR}
)

target_link_directories(tts_engine PRIVATE
  ${DEPS_DIR}
  ${DEPS_DIR}/lib
)

target_link_libraries(tts_engine PRIVATE
  piper_phonemize
  espeak-ng
  onnxruntime
  pthread
  PkgConfig::OPUS
  PkgConfig::OGG
)

# Compile flags
target_compile_options(tts_engine PRIVATE
  -Wall -Wno-unused-function
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-ffast-math>
  $<$<CONFIG:Release>:-fvisibility=hidden>
  $<$<CONFIG:Release>:-fvisibility-inlines-hidden>
  $<$<CONFIG:Release>:-DNDEBUG>
)

if(IPO_OK)
  set_target_properties(tts_engine PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
  message(STATUS "LTO not supported: ${IPO_ERR}")
endif()

# Drop the .so next to main.py so `import tts_engine` works without PYTHONPATH.
set_target_properties(tts_engine PROPERTIES
  BUILD_RPATH              "$ORIGIN/dependencies:$ORIGIN/dependencies/lib"
  INSTALL_RPATH            "$ORIGIN/dependencies:$ORIGIN/dependencies/lib"
  LIBRARY_OUTPUT_DIRECTORY "${ROOT_DIR}"
)

# Strip Release builds — shaves a few MB off the .so.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
  add_custom_command(TARGET tts_engine POST_BUILD
    COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:tts_engine>
    COMMENT "Stripping tts_engine"
  )
endif()
