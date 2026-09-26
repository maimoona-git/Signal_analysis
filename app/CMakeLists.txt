qt_standard_project_setup()

qt_add_executable(sigapp main.cpp)
target_link_libraries(sigapp PRIVATE Qt6::Widgets sigcore_core)

if(EMSCRIPTEN)
  # Single-threaded by default (see CLAUDE.md): no SharedArrayBuffer, so the
  # output runs from any plain static host without COOP/COEP headers.
  target_link_options(sigapp PRIVATE -sALLOW_MEMORY_GROWTH=1)

  # Stage a servable copy under <build>/dist: our /web shell + the Qt output.
  # A custom target (always out of date) so edits to /web alone re-stage too.
  set(SIG_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
  add_custom_target(sigapp_dist ALL
    DEPENDS sigapp
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SIG_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE_DIR:sigapp>/sigapp.js"
      "$<TARGET_FILE_DIR:sigapp>/sigapp.wasm"
      "$<TARGET_FILE_DIR:sigapp>/qtloader.js"
      "${PROJECT_SOURCE_DIR}/web/index.html"
      "${SIG_DIST_DIR}"
    COMMENT "Staging web app into ${SIG_DIST_DIR}"
  )
endif()
