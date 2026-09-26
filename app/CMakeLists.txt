qt_standard_project_setup()

# The GUI proper: widgets, main window, the backend interface. A plain static
# library so it also builds against a desktop Qt, where the widget tests run
# under the offscreen platform (tests/gui).
qt_add_library(siggui STATIC
  gui/plot_axes.cpp
  gui/waterfall_widget.cpp
  gui/constellation_widget.cpp
  gui/results_panel.cpp
  gui/main_window.cpp
)
target_include_directories(siggui PUBLIC gui)
target_link_libraries(siggui PUBLIC Qt6::Widgets sigpipeline)

if(EMSCRIPTEN)
  # The browser app: the GUI + the pipeline, with the bridge's ML call-out and the
  # Embind hooks linked in as objects so their registrations are kept.
  qt_add_executable(sigapp
    main.cpp
    gui/wasm_backend.cpp
    gui/wasm_bindings.cpp
    gui/wasm_file_picker.cpp
  )
  target_link_libraries(sigapp PRIVATE siggui sigbridge_objs embind)

  # Single-threaded by default (see CLAUDE.md): no SharedArrayBuffer, so the
  # output runs from any plain static host without COOP/COEP headers.
  target_link_options(sigapp PRIVATE -sALLOW_MEMORY_GROWTH=1)

  # Stage a servable copy under <build>/dist: our /web shell + the Qt output.
  # A custom target (always out of date) so edits to /web alone re-stage too.
  # scripts/stage-web.sh adds the JS modules, the model and the ORT runtime.
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

  # The end-to-end browser test (real Chrome, mouse + keyboard + file chooser): only if
  # node, the JS dependencies and a Chrome are present; skipped otherwise.
  find_program(SIG_NODE node NO_CMAKE_FIND_ROOT_PATH)
  find_program(SIG_CHROME NAMES "Google Chrome" google-chrome chromium chromium-browser
    PATHS "/Applications/Google Chrome.app/Contents/MacOS" NO_CMAKE_FIND_ROOT_PATH)
  if(SIG_BUILD_TESTS AND SIG_NODE AND SIG_CHROME AND EXISTS "${PROJECT_SOURCE_DIR}/node_modules/puppeteer-core")
    add_test(NAME test_browser_gui COMMAND "${PROJECT_SOURCE_DIR}/scripts/browser-gui-test.sh")
    set_tests_properties(test_browser_gui PROPERTIES ENVIRONMENT "CHROME=${SIG_CHROME}" TIMEOUT 300)
  else()
    message(STATUS "ps26147: skipping test_browser_gui (needs node, Chrome and `npm install`)")
  endif()
endif()
