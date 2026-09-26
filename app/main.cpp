// Phase 1 placeholder GUI: proves Qt Widgets compiles to WASM and renders
// into the browser canvas. Replaced by the real main window in Phase 7.

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QVBoxLayout>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("PS 26147 Signal Analysis");

    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);
    auto* label = new QLabel("Hello from Qt Widgets on WebAssembly", central);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    window.setCentralWidget(central);

    window.resize(640, 400);
    window.show();
    return app.exec();
}
