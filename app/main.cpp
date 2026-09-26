// The browser app: Qt Widgets compiled to WebAssembly (see BUILDING.md). The window is
// the same as the native app's; analyses run in this module, with the ML classification
// delegated to onnxruntime-web in the page through the bridge.

#include <QApplication>

#include "gui/main_window.hpp"
#include "gui/wasm_backend.hpp"
#include "gui/wasm_bindings.hpp"
#include "gui/wasm_file_picker.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    sigui::WasmBackend backend;
    sigui::MainWindow window(&backend);
    window.setFilePicker(&sigui::pick_file_in_browser);
    sigui::register_window_for_js(&window);
    window.show();

    // In the browser exec() returns to the event loop immediately without unwinding
    // this frame, so the stack objects above live as long as the page.
    return app.exec();
}
