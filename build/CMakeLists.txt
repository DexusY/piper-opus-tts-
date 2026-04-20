cmake_minimum_required(VERSION 3.18)
project(tts_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ROOT_DIR  "${CMAKE_SOURCE_DIR}/..")
set(DEPS_DIR  "${ROOT_DIR}/dependencies")
set(SRC_DIR   "${ROOT_DIR}/src")

set(INCLUDE_DIR "${DEPS_DIR}/include")
set(PIPER_SRC   "${DEPS_DIR}/piper/src/cpp")

# ── pybind11 ──────────────────────────────────────────────────────────────────
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 REQUIRED CONFIG)

# ── opus / ogg ────────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(OGG  REQUIRED ogg)

# ── tts_engine module ─────────────────────────────────────────────────────────
pybind11_add_module(tts_engine
  ${SRC_DIR}/tts_module.cpp
  ${PIPER_SRC}/piper.cpp
)

target_include_directories(tts_engine PRIVATE
  ${PIPER_SRC}
  ${INCLUDE_DIR}
  ${OPUS_INCLUDE_DIRS}
  ${OGG_INCLUDE_DIRS}
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
  ${OPUS_LIBRARIES}
  ${OGG_LIBRARIES}
)

# .so goes to project root so Python can import it directly
set_target_properties(tts_engine PROPERTIES
  BUILD_RPATH              "$ORIGIN/dependencies:$ORIGIN/dependencies/lib"
  INSTALL_RPATH            "$ORIGIN/dependencies:$ORIGIN/dependencies/lib"
  LIBRARY_OUTPUT_DIRECTORY "${ROOT_DIR}"
)
