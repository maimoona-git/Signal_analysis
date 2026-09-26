// Phase 7: the Qt widgets, on a desktop Qt under the offscreen platform (the browser
// app is exercised end to end in Chrome by scripts/browser-gui-test.sh). Plot tests
// render into an image and look at pixels at positions computed from the data, so
// orientation, scale and colour are checked, not just "something drew".

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>

#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include "main_window.hpp"
#include "result_text.hpp"
#include "../harness.hpp"
#include "../record_file.hpp"
#include "../testutil.hpp"

using namespace sigui;
using namespace sigtest;

#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

std::string data_path(const std::string& rel) { return std::string(SIG_TEST_DATA_DIR) + "/pipeline/" + rel; }

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

QByteArray demo_bytes(const char* name) {
    const auto b = read_file(data_path(name));
    return QByteArray(reinterpret_cast<const char*>(b.data()), static_cast<qsizetype>(b.size()));
}

std::string from_hex(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

QImage render(QWidget& w, int width, int height) {
    w.resize(width, height);
    return w.grab().toImage();
}

QRgb pixel_at(const QImage& img, QPointF p) { return img.pixel(static_cast<int>(std::lround(p.x())), static_cast<int>(std::lround(p.y()))); }

// A spectrogram on a known grid: `frames` x `nfft`, times 0..0.5 s, freqs -4000..3000 Hz.
sigapp::Spectrogram make_spectrogram(unsigned frames, unsigned nfft, float fill = 0.0f) {
    sigapp::Spectrogram s;
    s.frames = frames;
    s.nfft = nfft;
    s.data.assign(static_cast<size_t>(frames) * nfft, fill);
    for (unsigned i = 0; i < nfft; ++i) s.freqs.push_back((static_cast<double>(i) - nfft / 2.0) * 1000.0);
    for (unsigned i = 0; i < frames; ++i) s.times.push_back(0.1 * i);
    return s;
}

// A backend whose behaviour the test controls.
class StubBackend : public AnalysisBackend {
public:
    enum class Mode { RespondNow, Hold, FailNow, ThrowNow } mode = Mode::RespondNow;
    sigapp::Classification classification{"bpsk", 1.0};
    std::vector<AnalysisRequest> requests;
    AnalysisCallback held;
    std::shared_ptr<const sigapp::PipelineResult> last_result;

    void run(const AnalysisRequest& request, AnalysisCallback done) override {
        requests.push_back(request);
        if (mode == Mode::ThrowNow) throw std::runtime_error("unsupported file extension: '.bin'");
        if (mode == Mode::FailNow) { done(false, nullptr, QStringLiteral("model unavailable")); return; }
        const auto* data = reinterpret_cast<const uint8_t*>(request.bytes.constData());
        last_result = std::make_shared<const sigapp::PipelineResult>(sigapp::run_pipeline(
            data, static_cast<size_t>(request.bytes.size()), request.filename.toStdString(), request.options, classification));
        if (mode == Mode::Hold) { held = std::move(done); return; }
        done(true, last_result, QString());
    }
    void deliver_held() { held(true, last_result, QString()); }
};

void pump() { QCoreApplication::processEvents(); }

}  // namespace

// --- plot axes ------------------------------------------------------------------

TEST_CASE("nice_ticks picks round numbers and never -0") {
    CHECK(nice_ticks(0, 10, 5) == (std::vector<double>{0, 2, 4, 6, 8, 10}));
    CHECK(nice_ticks(-1, 1, 4) == (std::vector<double>{-1, -0.5, 0, 0.5, 1}));
    const auto t = nice_ticks(-4000, 3000, 6);
    CHECK(t.front() >= -4000 && t.back() <= 3000 && t.size() >= 4 && t.size() <= 12);
    for (double v : nice_ticks(-1, 1, 4)) CHECK(!std::signbit(v) || v != 0.0);
    CHECK(nice_ticks(5, 5).empty() && nice_ticks(2, 1).empty());
    CHECK(nice_ticks(0, std::nan("")).empty());
    CHECK(nice_ticks(0, 1e-9, 5).size() >= 2);
}

TEST_CASE("format_tick uses just enough decimals") {
    CHECK(format_tick(0.0, 0.5) == "0");
    CHECK(format_tick(0.5, 0.5) == "0.5");
    CHECK(format_tick(1200, 200) == "1200");
    CHECK(format_tick(0.25, 0.05) == "0.25");
    CHECK(format_tick(2.5e6, 5e5) == "2.5e+06");
}

// --- waterfall --------------------------------------------------------------------

TEST_CASE("waterfall: viridis endpoints match pyqtgraph's table") {
    CHECK(viridis(0) == qRgb(68, 1, 84));
    CHECK(viridis(255) == qRgb(253, 231, 37));
    CHECK(viridis(-5) == viridis(0));
    CHECK(viridis(999) == viridis(255));
}

TEST_CASE("waterfall: time runs left to right, low frequencies at the bottom, hot cells are yellow") {
    WaterfallWidget w;
    auto s = make_spectrogram(6, 8);
    s.data[2 * 8 + 5] = 100.0f;  // frame 2, frequency bin 5: the only hot cell
    w.setData(s);
    CHECK(w.hasData());
    CHECK(w.image().width() == 6 && w.image().height() == 8);
    CHECK(w.levelMin() == 0.0 && w.levelMax() == 100.0);
    CHECK(w.imageRect() == QRectF(0.0, -4000.0, 0.5, 7000.0));   // [t0,tN] x [f0,fN], like setRect

    const QImage img = render(w, 500, 320);
    auto cell_center = [&](unsigned t, unsigned f) {
        return w.dataToPixel(0.5 * (t + 0.5) / 6.0, -4000.0 + 7000.0 * (f + 0.5) / 8.0);
    };
    CHECK(pixel_at(img, cell_center(2, 5)) == viridis(255));      // hot cell, where the data says it is
    CHECK(pixel_at(img, cell_center(0, 0)) == viridis(0));         // cold
    CHECK(pixel_at(img, cell_center(5, 7)) == viridis(0));
    CHECK(pixel_at(img, cell_center(1, 5)) == viridis(0));         // neighbours in time are cold
    CHECK(pixel_at(img, cell_center(2, 4)) == viridis(0));         // and in frequency
    CHECK(cell_center(0, 0).y() > cell_center(0, 7).y());          // low frequency is lower on screen
    CHECK(cell_center(0, 0).x() < cell_center(5, 0).x());          // later frames are further right
    CHECK(w.image().pixel(2, 8 - 1 - 5) == viridis(255));          // stored with row 0 = highest frequency
}

TEST_CASE("waterfall: a long capture is folded into a bounded picture without losing a burst") {
    // 20,000 frames would be a 20,000-column picture; it is folded into kMaxColumns, keeping
    // the per-bin maximum so one loud frame stays visible, with levels from the whole data.
    auto s = make_spectrogram(20000, 8, -50.0f);
    s.times.clear();
    for (unsigned i = 0; i < s.frames; ++i) s.times.push_back(0.001 * i);
    s.data[12345 * 8 + 5] = 100.0f;                            // one hot cell in one frame
    s.data[7 * 8 + 0] = -300.0f;                               // and one very cold cell elsewhere
    WaterfallWidget w;
    w.setData(s);
    CHECK(w.image().width() == static_cast<int>(WaterfallWidget::kMaxColumns) && w.image().height() == 8);
    CHECK(w.levelMax() == 100.0 && w.levelMin() == -300.0);     // whole-data levels, not the picture's
    const int column = static_cast<int>(12345ull * WaterfallWidget::kMaxColumns / 20000);
    CHECK(w.image().pixel(column, 8 - 1 - 5) == viridis(255));  // the burst survives, in the right column and row
    int hot = 0;
    for (int x = 0; x < w.image().width(); ++x)
        for (int y = 0; y < 8; ++y) hot += (w.image().pixel(x, y) == viridis(255));
    CHECK(hot == 1);                                           // and only there
    CHECK(w.imageRect() == QRectF(0.0, -4000.0, 19.999, 7000.0));   // the time span is unchanged

    auto small = make_spectrogram(WaterfallWidget::kMaxColumns, 4, 1.0f);   // exactly at the limit: one column per frame
    w.setData(small);
    CHECK(w.image().width() == static_cast<int>(WaterfallWidget::kMaxColumns));
    auto just_over = make_spectrogram(WaterfallWidget::kMaxColumns + 1, 4, 1.0f);
    w.setData(just_over);
    CHECK(w.image().width() == static_cast<int>(WaterfallWidget::kMaxColumns));
}

TEST_CASE("waterfall: colours span the range monotonically") {
    WaterfallWidget w;
    auto s = make_spectrogram(1, 4);
    s.data = {-100.0f, -50.0f, 0.0f, 100.0f};
    s.times = {0.0};
    w.setData(s);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 100.0);
    CHECK(w.image().pixel(0, 3) == viridis(0));                          // lowest value, lowest bin (bottom row)
    CHECK(w.image().pixel(0, 0) == viridis(255));                        // highest value, top row
    CHECK(w.image().pixel(0, 2) == viridis(static_cast<int>(50.0 * 256 / 200)));
    CHECK(w.image().pixel(0, 1) == viridis(static_cast<int>(100.0 * 256 / 200)));
}

TEST_CASE("waterfall: empty, cleared, degenerate and invalid data") {
    WaterfallWidget w;
    QImage img = render(w, 500, 320);
    const QPointF center = w.plotRect().center();
    CHECK(pixel_at(img, center) == qRgb(255, 255, 255));            // nothing to show: white
    CHECK(pixel_at(img, QPointF(w.plotRect().left(), w.plotRect().center().y())) != qRgb(255, 255, 255));  // but the frame is drawn

    w.setData(make_spectrogram(6, 8, 5.0f));                        // flat data: all one colour, no division by zero
    img = render(w, 500, 320);
    CHECK(pixel_at(img, center) == viridis(0));
    w.clearData();
    CHECK(!w.hasData());
    img = render(w, 500, 320);
    CHECK(pixel_at(img, center) == qRgb(255, 255, 255));

    auto single = make_spectrogram(1, 8, 1.0f);                     // one frame: left at one unit per pixel
    single.times = {0.0};
    w.setData(single);
    CHECK(w.hasData() && w.imageRect() == QRectF(0, 0, 1, 8));

    sigapp::Spectrogram bad;
    bad.frames = 4; bad.nfft = 4; bad.data.assign(3, 0.0f);         // fewer values than frames*nfft
    w.setData(bad);
    CHECK(!w.hasData());
    w.setData(sigapp::Spectrogram{});
    CHECK(!w.hasData());
}

// --- constellation ------------------------------------------------------------------

TEST_CASE("constellation: equal scale on both axes at any window shape") {
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.5f, 0.5f}});
    for (auto [w, h] : {std::pair{600, 200}, std::pair{200, 500}, std::pair{400, 400}}) {
        render(c, w, h);
        const double per_i = c.dataToPixel(1, 0).x() - c.dataToPixel(0, 0).x();
        const double per_q = c.dataToPixel(0, 0).y() - c.dataToPixel(0, 1).y();   // up on screen is +Q
        CHECK_NEAR(per_i, per_q, 1e-6);
        CHECK(per_i > 0);
        // the constellation is centred in the plot
        CHECK_NEAR(c.dataToPixel(0, 0).x(), c.plotRect().center().x(), 1.0);
        CHECK_NEAR(c.dataToPixel(0, 0).y(), c.plotRect().center().y(), 1.0);
    }
}

TEST_CASE("constellation: points are drawn where the data says, in translucent blue") {
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}});
    const QImage img = render(c, 400, 400);
    for (auto [i, q] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
        const QRgb px = pixel_at(img, c.dataToPixel(i, q));
        CHECK(qBlue(px) > qRed(px) + 40);                    // bluish
        CHECK(qRed(px) < 255 && qGreen(px) < 255);            // not white
        CHECK(qBlue(px) > 170);                               // translucent (alpha 150/255): still light, even over a grid line
    }
    // a place with no point (and no grid line) stays white
    CHECK(pixel_at(img, c.dataToPixel(0.5, 0.5) + QPointF(20, 0)) != qRgb(0, 0, 0));
}

TEST_CASE("constellation: a light grid is drawn under the points") {
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}});
    const QImage img = render(c, 400, 400);
    const QRectF plot = c.plotRect();
    // the vertical grid line through I = 0, well away from any point
    const QRgb on_line = img.pixel(static_cast<int>(std::lround(c.dataToPixel(0, 0).x())), static_cast<int>(plot.top() + 10));
    CHECK(qRed(on_line) < 255 && qRed(on_line) > 150);      // grey at alpha 0.3, not black, not white
}

TEST_CASE("constellation: long captures are thinned deterministically to 4000 evenly spaced samples") {
    std::vector<std::complex<float>> samples(10000);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = {static_cast<float>(i), 0.0f};
    ConstellationWidget c;
    c.setData(samples);
    CHECK(c.points().size() == ConstellationWidget::kDefaultMaxPoints);
    CHECK(c.points()[0].x() == 0.0);
    CHECK(c.points()[1].x() == 2.0);                         // i * n / count = 1 * 10000 / 4000 = 2
    CHECK(c.points()[3999].x() == 9997.0);                   // 3999 * 10000 / 4000, rounded down
    for (size_t i = 1; i < c.points().size(); ++i) CHECK(c.points()[i].x() > c.points()[i - 1].x());
    const auto first = c.points();
    c.setData(samples);
    CHECK(c.points() == first);                              // reproducible

    c.setData(samples, 100);
    CHECK(c.points().size() == 100);
    samples.resize(50);
    c.setData(samples);
    CHECK(c.points().size() == 50 && c.points()[49].x() == 49.0);   // short captures are kept whole
}

TEST_CASE("constellation: empty, single-point and identical-point data do not break the view") {
    ConstellationWidget c;
    render(c, 300, 300);
    CHECK(c.points().empty());
    c.setData({{2.0f, -3.0f}});
    QImage img = render(c, 300, 300);
    CHECK_NEAR(c.dataToPixel(2, -3).x(), c.plotRect().center().x(), 1.0);
    CHECK(qBlue(pixel_at(img, c.dataToPixel(2, -3))) > qRed(pixel_at(img, c.dataToPixel(2, -3))));
    c.setData(std::vector<std::complex<float>>(100, {1.0f, 1.0f}));
    render(c, 300, 300);
    CHECK(std::isfinite(c.dataToPixel(1, 1).x()));
    c.setData({{0, 0}, {1e-9f, 1e-9f}});                     // a vanishingly small spread
    render(c, 300, 300);
    CHECK(std::isfinite(c.dataToPixel(0, 0).x()));
    c.clearData();
    CHECK(c.points().empty());
}

// --- results panel ----------------------------------------------------------------

TEST_CASE("results panel is read-only and shows results, errors and nothing") {
    ResultsPanel panel;
    CHECK(panel.isReadOnly());
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    sigapp::PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto result = sigapp::run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", o, {"fsk", 1.0});
    panel.showResult(result);
    CHECK(panel.toPlainText().toStdString() == sigapp::format_result_text(result));
    CHECK(panel.toPlainText().contains("Decoded message:      'SIH2026!'"));
    panel.showError(QStringLiteral("boom"));
    CHECK(panel.toPlainText() == QStringLiteral("Error: boom"));
    panel.clearText();
    CHECK(panel.toPlainText().isEmpty());
}

TEST_CASE("results panel: a decoded message of millions of bits is cut, and shows instantly") {
    // Shown whole on one wrapped line, a message of millions of characters makes Qt's layout
    // take minutes (measured in the browser: 8M bits -> a 2 MB hex string -> 220 s). 4M bits
    // (a million characters) is enough to see it: ~16 s uncapped natively, instant capped.
    sigapp::PipelineResult r;
    r.predicted_modulation = "bpsk";
    r.demodulated_bits = random_bits(4'000'000, 5);
    r.boundaries.header_pattern = {1, 0, 1};
    ResultsPanel panel;
    QElapsedTimer timer;
    timer.start();
    panel.showResult(r);
    panel.resize(400, 400);
    panel.grab();                                            // force the text layout too
    CHECK(timer.elapsed() < 3000);
    const QString text = panel.toPlainText();
    CHECK(text.size() < 12000);
    CHECK(text.contains("characters in all, the first 10000 shown"));
    CHECK(text.contains("Frame period:"));                   // the rest of the panel is intact
    CHECK(text.contains("Payload spans found:"));
}

// --- main window ---------------------------------------------------------------------

TEST_CASE("main window: layout and controls match the native app") {
    StubBackend backend;
    MainWindow win(&backend);
    win.show();
    pump();

    CHECK(win.windowTitle() == "PS 26147 - Signal Analysis Tool");
    CHECK(win.statusBar()->currentMessage() == "Open a .wav or .IQ file to begin.");
    CHECK(win.width() == 1100 && win.height() == 700);

    CHECK(win.openAction()->text() == "Open File...");
    CHECK(win.runAction()->text() == "Run Analysis");
    CHECK(!win.runAction()->isEnabled());                                  // nothing loaded yet
    CHECK(win.autoDetectCheckBox()->text() == "Auto-detect FEC/Interleaving");
    CHECK(!win.autoDetectCheckBox()->isChecked());
    CHECK(win.sampleRateEdit()->placeholderText() == "default");
    CHECK(win.sampleRateEdit()->maximumWidth() == 90 && win.sampleRateEdit()->minimumWidth() == 90);
    CHECK(win.samplesPerSymbolSpinBox()->minimum() == 1 && win.samplesPerSymbolSpinBox()->maximum() == 64);
    CHECK(win.samplesPerSymbolSpinBox()->value() == 1);

    // the sample-rate field only takes numbers in 0..1e12
    int pos = 0;
    QString text = "8000";
    CHECK(win.sampleRateEdit()->validator()->validate(text, pos) == QValidator::Acceptable);
    text = "abc";
    CHECK(win.sampleRateEdit()->validator()->validate(text, pos) == QValidator::Invalid);
    text = "-5";
    CHECK(win.sampleRateEdit()->validator()->validate(text, pos) == QValidator::Invalid);

    // toolbar captions
    QStringList labels;
    for (auto* l : win.findChildren<QLabel*>()) labels << l->text();
    CHECK(labels.contains(" Sample rate (Hz, .IQ only): "));
    CHECK(labels.contains(" Samples/symbol: "));

    // waterfall above constellation on the left, results panel on the right, 2:1
    auto splitters = win.findChildren<QSplitter*>();
    CHECK(splitters.size() == 2);
    CHECK(win.waterfall()->geometry().bottom() <= win.constellation()->geometry().top() + 1
          || win.waterfall()->mapTo(&win, QPoint(0, 0)).y() < win.constellation()->mapTo(&win, QPoint(0, 0)).y());
    CHECK(win.waterfall()->mapTo(&win, QPoint(0, 0)).x() < win.resultsPanel()->mapTo(&win, QPoint(0, 0)).x());
    CHECK(win.waterfall()->width() > win.resultsPanel()->width());
    const double ratio = static_cast<double>(win.waterfall()->width()) / win.resultsPanel()->width();
    CHECK(ratio > 1.6 && ratio < 2.4);                                    // 2:1, not the ~4:1 Qt picks from size hints
    win.resize(1400, 800);
    pump();
    const double after = static_cast<double>(win.waterfall()->width()) / win.resultsPanel()->width();
    CHECK(after > 1.6 && after < 2.4);                                    // and it holds when the window is resized
    CHECK(win.resultsPanel()->isReadOnly());
}

TEST_CASE("main window: loading a file enables the run action, reports it, and clears the views") {
    StubBackend backend;
    MainWindow win(&backend);
    win.waterfall()->setData(make_spectrogram(4, 4, 1.0f));
    win.constellation()->setData({{1, 1}});
    win.resultsPanel()->setPlainText("stale");

    win.loadFile("capture.iq", demo_bytes("fsk_plain.iq"));
    CHECK(win.runAction()->isEnabled());
    CHECK(win.loadedFilename() == "capture.iq");
    CHECK(win.statusBar()->currentMessage() == "Loaded capture.iq. Click 'Run Analysis' to proceed.");
    CHECK(win.resultsPanel()->toPlainText().isEmpty());
    CHECK(!win.waterfall()->hasData());
    CHECK(win.constellation()->points().empty());
}

TEST_CASE("main window: run analysis sends the toolbar settings to the backend") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));

    win.runAnalysis();                                        // defaults: blank rate, 1 sample/symbol, no auto-detect
    pump();
    CHECK(backend.requests.size() == 1);
    CHECK(backend.requests[0].filename == "fsk_plain.iq");
    CHECK(backend.requests[0].bytes == demo_bytes("fsk_plain.iq"));
    CHECK(!backend.requests[0].options.sample_rate.has_value());
    CHECK(backend.requests[0].options.samples_per_symbol == 1);
    CHECK(!backend.requests[0].options.auto_detect);
    CHECK(!backend.requests[0].options.interleaver && !backend.requests[0].options.fec_scheme);   // the GUI never sets these

    win.sampleRateEdit()->setText("8000");
    win.samplesPerSymbolSpinBox()->setValue(4);
    win.autoDetectCheckBox()->setChecked(true);
    win.runAnalysis();
    pump();
    CHECK(backend.requests.size() == 2);
    CHECK(backend.requests[1].options.sample_rate == std::optional<double>(8000.0));
    CHECK(backend.requests[1].options.samples_per_symbol == 4);
    CHECK(backend.requests[1].options.auto_detect);

    win.sampleRateEdit()->setText("  2400000 ");                // surrounding spaces are ignored
    win.runAnalysis();
    pump();
    CHECK(backend.requests[2].options.sample_rate == std::optional<double>(2400000.0));
}

TEST_CASE("main window: a finished analysis fills the plots, the panel and the status line") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    win.sampleRateEdit()->setText("8000");
    win.runAnalysis();
    CHECK(win.statusBar()->currentMessage() == "Running analysis...");
    CHECK(!win.runAction()->isEnabled());                     // one analysis at a time
    pump();

    const auto& r = *backend.last_result;
    CHECK(win.waterfall()->hasData());
    CHECK(win.waterfall()->image().width() == static_cast<int>(r.spectrogram.frames));
    CHECK(win.waterfall()->image().height() == static_cast<int>(r.spectrogram.nfft));
    CHECK(win.constellation()->points().size() == r.samples.size());      // 512 samples: all kept
    CHECK(win.resultsPanel()->toPlainText().toStdString() == sigapp::format_result_text(r));
    CHECK(win.statusBar()->currentMessage().toStdString() == sigapp::format_done_status(r));
    CHECK(win.runAction()->isEnabled());
}

TEST_CASE("main window: the panel shows exactly what the ORIGINAL GUI showed for the same run") {
    // demo_ldpc_block_autodetect in pipeline.rec: the original pipeline run the way the
    // GUI runs it (auto-detect on, 4 samples/symbol, 8 kHz, no explicit parameters).
    const auto recs = read_records(data_path("pipeline.rec"));
    for (const auto& rec : recs) {
        if (field(rec, "name") != "demo_ldpc_block_autodetect") continue;
        StubBackend backend;
        backend.classification = {field(rec, "label"), to_double(field(rec, "confidence"))};
        MainWindow win(&backend);
        win.loadFile("bpsk_ldpc_block.iq", demo_bytes("bpsk_ldpc_block.iq"));
        win.autoDetectCheckBox()->setChecked(true);
        win.samplesPerSymbolSpinBox()->setValue(static_cast<int>(to_int(field(rec, "samples_per_symbol"))));
        win.sampleRateEdit()->setText(QString::fromStdString(field(rec, "sample_rate")));
        win.runAnalysis();
        pump();
        CHECK(win.resultsPanel()->toPlainText().toStdString() == from_hex(field(rec, "panel_text_hex")));
        CHECK(win.statusBar()->currentMessage().toStdString() == from_hex(field(rec, "status_hex")));
        return;
    }
    CHECK(false);  // scenario not found
}

TEST_CASE("main window: failures are reported in the panel, status line and a non-blocking dialog") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("x.iq", demo_bytes("fsk_plain.iq"));

    backend.mode = StubBackend::Mode::FailNow;
    win.runAnalysis();
    pump();
    CHECK(win.resultsPanel()->toPlainText() == "Error: model unavailable");
    CHECK(win.statusBar()->currentMessage() == "Analysis failed.");
    CHECK(win.runAction()->isEnabled());
    auto* box = win.findChild<QMessageBox*>();
    CHECK(box != nullptr);
    CHECK(box->text() == "model unavailable" && box->icon() == QMessageBox::Warning);   // (macOS ignores dialog titles)
    box->close();
    pump();

    backend.mode = StubBackend::Mode::ThrowNow;                // an immediate failure from run()
    win.runAnalysis();
    pump();
    CHECK(win.resultsPanel()->toPlainText() == "Error: unsupported file extension: '.bin'");
    CHECK(win.statusBar()->currentMessage() == "Analysis failed.");
    CHECK(win.runAction()->isEnabled());

    backend.mode = StubBackend::Mode::RespondNow;              // and it recovers
    win.runAnalysis();
    pump();
    CHECK(win.statusBar()->currentMessage().startsWith("Done."));
}

TEST_CASE("main window: a second run is ignored while one is in flight; a stale result is dropped") {
    StubBackend backend;
    backend.mode = StubBackend::Mode::Hold;
    MainWindow win(&backend);
    win.loadFile("a.iq", demo_bytes("fsk_plain.iq"));
    win.runAnalysis();
    win.runAnalysis();                                          // ignored: already running
    CHECK(backend.requests.size() == 1);
    CHECK(win.analysisRunning());

    win.loadFile("b.iq", demo_bytes("fsk_plain.iq"));           // a new file supersedes the pending analysis
    CHECK(!win.analysisRunning());
    backend.deliver_held();                                     // the old analysis finally comes back
    pump();
    CHECK(win.resultsPanel()->toPlainText().isEmpty());
    CHECK(!win.waterfall()->hasData());
    CHECK(win.statusBar()->currentMessage() == "Loaded b.iq. Click 'Run Analysis' to proceed.");

    win.runAnalysis();                                          // the new file analyses normally
    backend.deliver_held();
    pump();
    CHECK(win.waterfall()->hasData());
}

TEST_CASE("main window: 'Open File...' runs the installed file picker and loads what it delivers") {
    StubBackend backend;
    MainWindow win(&backend);
    int picks = 0;
    win.setFilePicker([&] { ++picks; win.loadFile("picked.wav", QByteArray("xyz")); });   // what a picker does on success
    win.openAction()->trigger();
    CHECK(picks == 1);
    CHECK(win.loadedFilename() == "picked.wav");
    CHECK(win.runAction()->isEnabled());
    win.setFilePicker([&] { ++picks; });                                                  // a cancelled pick loads nothing
    win.openAction()->trigger();
    CHECK(picks == 2 && win.loadedFilename() == "picked.wav");
}

TEST_CASE("main window: running with nothing loaded does nothing") {
    StubBackend backend;
    MainWindow win(&backend);
    win.runAnalysis();
    pump();
    CHECK(backend.requests.empty());
    CHECK(win.statusBar()->currentMessage() == "Open a .wav or .IQ file to begin.");
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    return sigtest::run_all();
}
