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

# Tests that read tests/data: a plain path natively; under WASM the directory is
# preloaded into the module's virtual filesystem at /testdata.
function(sig_test_uses_data name)
  if(EMSCRIPTEN)
    target_compile_definitions(${name} PRIVATE SIG_TEST_DATA_DIR="/testdata")
    target_link_options(${name} PRIVATE --preload-file ${PROJECT_SOURCE_DIR}/tests/data@/testdata)
  else()
    target_compile_definitions(${name} PRIVATE SIG_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  endif()
endfunction()

sig_add_test(test_toolchain_smoke core/test_toolchain_smoke.cpp)
sig_add_test(test_dsp core/test_dsp.cpp)
sig_add_test(test_demod core/test_demod.cpp)
sig_add_test(test_interleave core/test_interleave.cpp)
sig_add_test(test_fec core/test_fec.cpp)
sig_add_test(test_ingest core/test_ingest.cpp)
sig_test_uses_data(test_ingest)
sig_add_test(test_ml_result bridge/test_ml_result.cpp)
target_link_libraries(test_ml_result PRIVATE sigbridge_portable)
sig_add_test(test_ml_preprocess core/test_ml_preprocess.cpp)
sig_test_uses_data(test_ml_preprocess)
sig_add_test(test_correlate core/test_correlate.cpp)
target_link_libraries(test_correlate PRIVATE sigpipeline)
sig_test_uses_data(test_correlate)
sig_add_test(test_dispatch core/test_dispatch.cpp)
target_link_libraries(test_dispatch PRIVATE sigpipeline)
sig_test_uses_data(test_dispatch)
sig_add_test(test_detect core/test_detect.cpp)
target_link_libraries(test_detect PRIVATE sigpipeline)
sig_test_uses_data(test_detect)
sig_add_test(test_pipeline core/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE sigpipeline)
sig_test_uses_data(test_pipeline)
sig_add_test(test_result_text core/test_result_text.cpp)
target_link_libraries(test_result_text PRIVATE sigpipeline)
sig_test_uses_data(test_result_text)

# GUI widget tests: only where a desktop Qt is available (configure with
# -DCMAKE_PREFIX_PATH=<Qt>); they run under the offscreen platform. The browser app
# itself is tested end to end in Chrome (scripts/browser-gui-test.sh).
if(TARGET siggui AND NOT EMSCRIPTEN)
  sig_add_test(test_gui gui/test_gui.cpp)
  target_link_libraries(test_gui PRIVATE siggui Qt6::Widgets)
  sig_test_uses_data(test_gui)
  set_tests_properties(test_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
