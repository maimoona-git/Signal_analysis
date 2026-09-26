# Each test is a standalone executable registered with ctest. Under the
# Emscripten toolchain, ctest runs it through node automatically
# (CMAKE_CROSSCOMPILING_EMULATOR), so the same `ctest` drives both targets.

function(sig_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sigcore_core)
  if(EMSCRIPTEN)
    # NODERAWFS off: tests use Emscripten's in-memory FS, like the browser.
    target_link_options(${name} PRIVATE -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

sig_add_test(test_toolchain_smoke core/test_toolchain_smoke.cpp)
sig_add_test(test_dsp core/test_dsp.cpp)
sig_add_test(test_demod core/test_demod.cpp)
sig_add_test(test_interleave core/test_interleave.cpp)
sig_add_test(test_fec core/test_fec.cpp)
