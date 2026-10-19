# sigcore: the DSP/FEC engine shared by the native and WASM builds.
#
# `sigcore_core` is a plain static library with no Python and no Embind in
# it; the pybind11 module (native only) and the Embind bridge (/bridge, WASM
# only) are thin layers on top of it.

# Sources with no third-party dependency: compile identically on both targets.
set(SIGCORE_PORTABLE_SOURCES
  src/wav_io.cpp
  src/ingest.cpp
  src/iq_io.cpp
  src/sigmf_meta.cpp
  src/fsk_estimate.cpp
  src/fec_extra.cpp
  src/fec_ccsds.cpp
  src/modulation_blind.cpp
  src/interleave.cpp
  src/gf256.cpp
  src/reed_solomon.cpp
  src/viterbi.cpp
  src/fec.cpp
)

# Sources that call into liquid-dsp.
set(SIGCORE_LIQUID_SOURCES
  src/dsp.cpp
  src/demod.cpp
  src/ml_preprocess.cpp
  src/soft_demod.cpp
  src/recover.cpp
  src/symbol_rate.cpp
  src/channel_finder.cpp
)

add_library(sigcore_core STATIC ${SIGCORE_PORTABLE_SOURCES})
target_include_directories(sigcore_core PUBLIC include)
set_target_properties(sigcore_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(EMSCRIPTEN)
  # liquid-dsp cross-compiled by scripts/build-liquid-wasm.sh (static, internal
  # FFT, no fftw/SIMD/threads).
  set(SIG_LIQUID_WASM_PREFIX "$ENV{HOME}/wasm-deps/install" CACHE PATH
      "Install prefix of the Emscripten-built liquid-dsp")
  find_path(LIQUID_INCLUDE_DIR liquid/liquid.h
    PATHS "${SIG_LIQUID_WASM_PREFIX}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_library(LIQUID_LIBRARY NAMES liquid
    PATHS "${SIG_LIQUID_WASM_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT LIQUID_INCLUDE_DIR OR NOT LIQUID_LIBRARY)
    message(FATAL_ERROR
      "WASM liquid-dsp not found under ${SIG_LIQUID_WASM_PREFIX}. Run scripts/build-liquid-wasm.sh.")
  endif()
  target_sources(sigcore_core PRIVATE ${SIGCORE_LIQUID_SOURCES})
  target_include_directories(sigcore_core PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(sigcore_core PUBLIC ${LIQUID_LIBRARY})
else()
  find_path(LIQUID_INCLUDE_DIR liquid/liquid.h
    HINTS /opt/homebrew/opt/liquid-dsp/include /usr/local/opt/liquid-dsp/include
  )
  find_library(LIQUID_LIBRARY NAMES liquid
    HINTS /opt/homebrew/opt/liquid-dsp/lib /usr/local/opt/liquid-dsp/lib
  )
  if(NOT LIQUID_INCLUDE_DIR OR NOT LIQUID_LIBRARY)
    message(FATAL_ERROR "liquid-dsp not found. Install it, e.g. `brew install liquid-dsp`.")
  endif()

  target_sources(sigcore_core PRIVATE ${SIGCORE_LIQUID_SOURCES})
  target_include_directories(sigcore_core PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(sigcore_core PUBLIC ${LIQUID_LIBRARY})

  # Optional pybind11 module, same `sigcore` Python API as the native app.
  # Built only when the configuring Python has pybind11 installed, e.g.
  #   cmake -B build -DPython3_EXECUTABLE=/path/to/venv/bin/python
  option(SIG_BUILD_PYTHON "Build the native pybind11 `sigcore` module" ON)
  if(SIG_BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development)
    if(Python3_FOUND)
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE pybind11_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
    endif()
    find_package(pybind11 CONFIG QUIET)
    if(pybind11_FOUND)
      pybind11_add_module(sigcore src/bindings.cpp)
      target_link_libraries(sigcore PRIVATE sigcore_core)
    else()
      message(STATUS "sigcore: pybind11 not found for ${Python3_EXECUTABLE}, skipping Python module")
    endif()
  endif()
endif()
