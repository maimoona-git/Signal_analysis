qt_standard_project_setup()

# The GUI proper: widgets, main window, the backend interface. A plain static
# library so it also builds against a desktop Qt, where the widget tests run
# under the offscreen platform (tests/gui).
qt_add_library(siggui STATIC
  gui/plot_axes.cpp
  gui/waterfall_widget.cpp
  gui/constellation_widget.cpp
  gui/frame_raster_widget.cpp
  gui/rank_analysis_widget.cpp
  gui/results_panel.cpp
  gui/main_window.cpp
)
target_include_directories(siggui PUBLIC gui)
target_link_libraries(siggui PUBLIC Qt6::Widgets sigpipeline sigbridge_portable)

if(EMSCRIPTEN)
  # The browser app: the GUI + the pipeline, with the bridge's ML call-out and the
  # Embind hooks linked in as objects so their registrations are kept.
  qt_add_executable(sigapp
    main.cpp
    gui/wasm_backend.cpp
    gui/wasm_worker_backend.cpp
    gui/wasm_bindings.cpp
    gui/wasm_file_picker.cpp
  )
  target_link_libraries(sigapp PRIVATE siggui sigbridge_objs embind)

  # Single-threaded by default (see CLAUDE.md): no SharedArrayBuffer, so the
  # output runs from any plain static host without COOP/COEP headers.
  # The heap grows on demand up to 4 GB (the default cap is 2 GB): a large capture is held
  # several times over while it is analysed (measured: a 512 MB file fits in 2 GB, 1 GB does not).
  target_link_options(sigapp PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=5MB)

  # The static site: <build>/dist is rebuilt from scratch on every build (so stale files
  # cannot accumulate) and holds exactly what a plain static file server needs, nothing else:
  #
  #   index.html                 the page shell (web/index.html)
  #   css/tokens.css, css/fonts.css   the shared design system (Phase 5: copied verbatim
  #                               from site/src/styles/tokens.css) and self-hosted font faces
  #   fonts/*.woff2               IBM Plex Mono/Sans, self-hosted (no external requests)
  #   sigapp.js, sigapp.wasm     the Qt app (the WASM binary and its Emscripten loader)
  #   qtloader.js                Qt's loader
  #   js/onnx-classifier.mjs     runs the ML model with onnxruntime-web (web/js)
  #   js/analysis-worker.js, sigbridge.js, sigbridge.wasm
  #                              the analysis Web Worker and the second WASM instance it runs
  #   models/*.onnx, *.json      the exported classifier and its description (ml/models)
  #   vendor/ort/*               the onnxruntime-web runtime (from node_modules)
  #
  # All references are relative, so it can be served from any path. Check it with
  # scripts/verify-dist.sh.
  set(SIG_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
  set(SIG_ORT_DIR "${PROJECT_SOURCE_DIR}/node_modules/onnxruntime-web/dist")
  set(SIG_STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${SIG_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SIG_DIST_DIR}/js" "${SIG_DIST_DIR}/models" "${SIG_DIST_DIR}/vendor/ort" "${SIG_DIST_DIR}/css" "${SIG_DIST_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/web/css" "${SIG_DIST_DIR}/css"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/web/fonts" "${SIG_DIST_DIR}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy
      "$<TARGET_FILE_DIR:sigapp>/sigapp.js"
      "$<TARGET_FILE_DIR:sigapp>/sigapp.wasm"
      "$<TARGET_FILE_DIR:sigapp>/qtloader.js"
      "${PROJECT_SOURCE_DIR}/web/index.html"
      "${SIG_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/web/js/onnx-classifier.mjs"
      "${PROJECT_SOURCE_DIR}/web/js/analysis-worker.js" "${SIG_DIST_DIR}/js"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:sigbridge_module>/sigbridge.js"
      "$<TARGET_FILE_DIR:sigbridge_module>/sigbridge.wasm" "${SIG_DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
      "${PROJECT_SOURCE_DIR}/ml/models/modulation_classifier.onnx"
      "${PROJECT_SOURCE_DIR}/ml/models/modulation_classifier.json"
      "${SIG_DIST_DIR}/models"
  )
  if(EXISTS "${SIG_ORT_DIR}/ort.wasm.min.mjs")
    # Only the plain CPU (wasm) runtime; the webgl/webgpu/jsep/asyncify variants are not used.
    list(APPEND SIG_STAGE_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy
        "${SIG_ORT_DIR}/ort.wasm.min.mjs"
        "${SIG_ORT_DIR}/ort-wasm-simd-threaded.mjs"
        "${SIG_ORT_DIR}/ort-wasm-simd-threaded.wasm"
        "${SIG_DIST_DIR}/vendor/ort")
  else()
    message(WARNING "onnxruntime-web is not installed (run `npm install`): the staged app "
                    "will load but cannot classify signals")
  endif()
  add_custom_target(sigapp_dist ALL
    DEPENDS sigapp sigbridge_module
    ${SIG_STAGE_COMMANDS}
    COMMENT "Staging the static site into ${SIG_DIST_DIR}"
  )

  # The end-to-end browser test (real Chrome, mouse + keyboard + file chooser): only if
  # node, the JS dependencies and a Chrome are present; skipped otherwise.
  find_program(SIG_NODE node NO_CMAKE_FIND_ROOT_PATH)
  find_program(SIG_CHROME NAMES "Google Chrome" google-chrome chromium chromium-browser
    PATHS "/Applications/Google Chrome.app/Contents/MacOS" NO_CMAKE_FIND_ROOT_PATH)
  if(SIG_BUILD_TESTS AND SIG_NODE AND SIG_CHROME AND EXISTS "${PROJECT_SOURCE_DIR}/node_modules/puppeteer-core")
    add_test(NAME test_browser_gui COMMAND "${PROJECT_SOURCE_DIR}/scripts/browser-gui-test.sh")
    set_tests_properties(test_browser_gui PROPERTIES ENVIRONMENT "CHROME=${SIG_CHROME}" TIMEOUT 300)
    # The static site as a deployer meets it: exact file set, no leaked paths, and the browser
    # test again on a relocated copy served by a stock python http.server.
    add_test(NAME test_static_site COMMAND "${PROJECT_SOURCE_DIR}/scripts/verify-dist.sh" "${SIG_DIST_DIR}")
    set_tests_properties(test_static_site PROPERTIES ENVIRONMENT "CHROME=${SIG_CHROME}" TIMEOUT 600)
  else()
    message(STATUS "ps26147: skipping test_browser_gui (needs node, Chrome and `npm install`)")
  endif()
endif()
