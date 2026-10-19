// The browser app: Qt Widgets compiled to WebAssembly (see BUILDING.md). The window is
// the same as the native app's; analyses run in this module, with the ML classification
// delegated to onnxruntime-web in the page through the bridge.

#include <QApplication>

#include <emscripten.h>

#include "gui/main_window.hpp"
#include "gui/wasm_worker_backend.hpp"
#include "gui/wasm_bindings.hpp"
#include "gui/wasm_file_picker.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    sigui::WorkerBackend backend;  // the analysis runs in a Web Worker (falls back to this module)
    sigui::MainWindow window(&backend);
    window.setFilePicker(&sigui::pick_file_in_browser);
    // "Previous/Next window" for a streamed file: the page owns the File and re-reads a slice.
    window.setWindowStepper([](int direction) { EM_ASM({ if (window.sigStepWindow) window.sigStepWindow($0); }, direction); });
    sigui::register_window_for_js(&window);
    window.show();

    // In the browser exec() returns to the event loop immediately without unwinding
    // this frame, so the stack objects above live as long as the page.
    return app.exec();
}
