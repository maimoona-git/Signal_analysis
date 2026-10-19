# The bridge between JS and the C++ core (see CLAUDE.md, "C++ <-> JS bridge").
#
#   sigbridge_portable  plain C++ (ML result validation), built on both targets
#                       so it is unit-tested natively as well as under WASM
#   sigbridge_objs      Embind bindings + the C++ -> JS ML call-out (WASM only);
#                       an OBJECT library so the Embind registrations are never
#                       dropped by the linker, and can be linked into the Qt app
#   sigbridge_module    standalone loadable module (sigbridge.js/.wasm) used by
#                       the JS test suite and any non-Qt JS host

add_library(sigbridge_portable STATIC src/ml_result.cpp)
target_include_directories(sigbridge_portable PUBLIC include)

if(EMSCRIPTEN)
  add_library(sigbridge_objs OBJECT src/ml_bridge.cpp src/pipeline_bridge.cpp src/bindings.cpp)
  target_include_directories(sigbridge_objs PUBLIC include)
  target_link_libraries(sigbridge_objs PUBLIC sigcore_core sigpipeline sigbridge_portable)

  add_executable(sigbridge_module $<TARGET_OBJECTS:sigbridge_objs>)
  set_target_properties(sigbridge_module PROPERTIES OUTPUT_NAME sigbridge)
  target_link_libraries(sigbridge_module PRIVATE sigbridge_objs embind)
  target_link_options(sigbridge_module PRIVATE
    --no-entry
    -sMODULARIZE=1
    -sEXPORT_NAME=createSigBridge
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=5MB
    -sENVIRONMENT=web,worker,node
  )

  find_program(SIG_NODE node NO_CMAKE_FIND_ROOT_PATH)
  if(NOT SIG_BUILD_TESTS)
    # no JS tests requested
  elseif(SIG_NODE)
    add_test(NAME test_bridge_js
      COMMAND ${SIG_NODE} --test ${PROJECT_SOURCE_DIR}/tests/bridge/bridge.test.mjs
              ${PROJECT_SOURCE_DIR}/tests/bridge/onnx.test.mjs
              ${PROJECT_SOURCE_DIR}/tests/bridge/pipeline.test.mjs
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(test_bridge_js PROPERTIES
      ENVIRONMENT "SIGBRIDGE_JS=$<TARGET_FILE:sigbridge_module>")
  else()
    message(WARNING "node not found: skipping the JS bridge tests")
  endif()
endif()
