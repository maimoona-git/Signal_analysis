// What the page's JavaScript can do with the running app (WASM only):
//   Module.sigAppOpenFile(bytes, filename)  load a capture as if it had been picked
//                                           with "Open File..." (drag-and-drop)
//   Module.sigAppState()                    a snapshot of the UI: status text, results
//                                           text, plot contents and where the toolbar
//                                           controls are on the canvas, which the
//                                           automated browser test uses to click them

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

#include "error_text.hpp"
#include "js_util.hpp"
#include "wasm_bindings.hpp"

using emscripten::val;

namespace sigui {

namespace {

MainWindow* g_window = nullptr;

val point(const QWidget* window, const QWidget* widget) {
    const QPoint p = widget->mapTo(window, widget->rect().center());
    val o = val::object();
    o.set("x", p.x());
    o.set("y", p.y());
    return o;
}

val rect_in_window(const QWidget* window, const QWidget* widget, const QRectF& local) {
    const QPoint origin = widget->mapTo(window, QPoint(0, 0));
    val o = val::object();
    o.set("x", origin.x() + local.x());
    o.set("y", origin.y() + local.y());
    o.set("width", local.width());
    o.set("height", local.height());
    return o;
}

void open_file(val bytes, std::string filename) {
    std::string error;
    try {
        if (!g_window) throw std::runtime_error("the window is not ready yet");
        const std::vector<uint8_t> data = sigbridge::bytes_from_js(bytes);
        g_window->loadFile(QString::fromStdString(filename),
                           QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size())));
        return;
    } catch (const std::exception& e) {
        error = sigbridge::describe_exception(e);
    }
    sigbridge::throw_js_error(error);
}

val state() {
    val s = val::object();
    if (!g_window) return s;
    const QWidget* w = g_window;
    QToolBar* toolbar = g_window->findChild<QToolBar*>();

    s.set("status", g_window->statusBar()->currentMessage().toStdString());
    s.set("resultText", g_window->resultsPanel()->toPlainText().toStdString());
    s.set("filename", g_window->loadedFilename().toStdString());
    s.set("canRun", g_window->runAction()->isEnabled());
    s.set("running", g_window->analysisRunning());
    s.set("dialogOpen", g_window->findChild<QMessageBox*>() != nullptr);
    s.set("sampleRateText", g_window->sampleRateEdit()->text().toStdString());
    s.set("samplesPerSymbol", g_window->samplesPerSymbolSpinBox()->value());
    s.set("autoDetect", g_window->autoDetectCheckBox()->isChecked());

    val size = val::object();
    size.set("width", g_window->width());
    size.set("height", g_window->height());
    s.set("window", size);

    val waterfall = val::object();
    waterfall.set("hasData", g_window->waterfall()->hasData());
    waterfall.set("frames", g_window->waterfall()->hasData() ? g_window->waterfall()->image().width() : 0);
    waterfall.set("nfft", g_window->waterfall()->hasData() ? g_window->waterfall()->image().height() : 0);
    waterfall.set("plot", rect_in_window(w, g_window->waterfall(), g_window->waterfall()->plotRect()));
    s.set("waterfall", waterfall);

    val constellation = val::object();
    constellation.set("points", static_cast<double>(g_window->constellation()->points().size()));
    constellation.set("plot", rect_in_window(w, g_window->constellation(), g_window->constellation()->plotRect()));
    s.set("constellation", constellation);

    val controls = val::object();
    if (toolbar) {
        controls.set("open", point(w, toolbar->widgetForAction(g_window->openAction())));
        controls.set("run", point(w, toolbar->widgetForAction(g_window->runAction())));
    }
    controls.set("autoDetect", point(w, g_window->autoDetectCheckBox()));
    controls.set("sampleRate", point(w, g_window->sampleRateEdit()));
    controls.set("samplesPerSymbol", point(w, g_window->samplesPerSymbolSpinBox()));
    s.set("controls", controls);
    return s;
}

}  // namespace

void register_window_for_js(MainWindow* window) { g_window = window; }

EMSCRIPTEN_BINDINGS(sig_app) {
    emscripten::function("sigAppOpenFile", &open_file);
    emscripten::function("sigAppState", &state);
}

}  // namespace sigui
