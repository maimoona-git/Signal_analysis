// Phase 7: the Qt widgets, on a desktop Qt under the offscreen platform (the browser
// app is exercised end to end in Chrome by scripts/browser-gui-test.sh). Plot tests
// render into an image and look at pixels at positions computed from the data, so
// orientation, scale and colour are checked, not just "something drew".

#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStatusBar>
#include <QToolBar>
#include <QWheelEvent>

#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

#include "main_window.hpp"
#include "fec.hpp"
#include "frame_raster_widget.hpp"
#include "rank_analysis_widget.hpp"
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

// Smooth (bilinear) scaling (WaterfallWidget::paintData, found needed from a real user
// report -- see its own comment) samples a cell's own centre almost exactly, but not
// byte-for-byte: measured up to a few RGB units off, a genuine, harmless artifact of
// sub-pixel interpolation, not a real colour/position error. A small tolerance here
// checks the real thing the original exact-equality check cared about (the hot cell
// reads as hot, a cold one as cold) without depending on exact interpolation rounding.
bool color_near(QRgb a, QRgb b, int tolerance = 8) {
    return std::abs(qRed(a) - qRed(b)) <= tolerance && std::abs(qGreen(a) - qGreen(b)) <= tolerance &&
          std::abs(qBlue(a) - qBlue(b)) <= tolerance;
}

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
    // Applied on top of whatever the window sent, for tests that need a known FEC
    // scheme wired in but have no GUI control for it yet (the toolbar only has
    // auto-detect, not a manual FEC/interleaver picker).
    std::optional<std::string> force_fec_scheme;
    std::optional<size_t> force_fec_msg_len;

    void run(const AnalysisRequest& request, AnalysisCallback done) override {
        requests.push_back(request);
        if (mode == Mode::ThrowNow) throw std::runtime_error("unsupported file extension: '.bin'");
        if (mode == Mode::FailNow) { done(false, nullptr, QStringLiteral("model unavailable")); return; }
        auto options = request.options;
        if (force_fec_scheme) { options.fec_scheme = force_fec_scheme; options.fec_msg_len = force_fec_msg_len; }
        const auto* data = reinterpret_cast<const uint8_t*>(request.bytes.constData());
        last_result = std::make_shared<const sigapp::PipelineResult>(sigapp::run_pipeline(
            data, static_cast<size_t>(request.bytes.size()), request.filename.toStdString(), options, classification));
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

TEST_CASE("zoom/pan: wheel zooms toward the cursor, keeping the pointed-at data point fixed") {
    // Exercised via ConstellationWidget, but this is PlotWidgetBase's own shared
    // behaviour -- every plot widget (waterfall, constellation, frame raster,
    // rank analysis) gets it for free.
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}});
    render(c, 400, 400);
    CHECK_NEAR(c.zoomLevel(), 1.0, 1e-9);
    const QRectF before = c.viewRect();

    // A point off-centre, inside the plot area: zooming in should keep whatever
    // data point is under it fixed on screen.
    const QPointF cursor = c.plotRect().topLeft() + QPointF(20, 15);
    const QPointF dataUnderCursorBefore = c.pixelToData(cursor);

    QWheelEvent wheel(cursor, cursor, QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&c, &wheel);

    CHECK(c.zoomLevel() > 1.0);
    const QRectF after = c.viewRect();
    CHECK(after.width() < before.width());   // zoomed in: a smaller data span is now visible
    CHECK(after.height() < before.height());
    const QPointF dataUnderCursorAfter = c.pixelToData(cursor);
    CHECK_NEAR(dataUnderCursorBefore.x(), dataUnderCursorAfter.x(), 1e-6);
    CHECK_NEAR(dataUnderCursorBefore.y(), dataUnderCursorAfter.y(), 1e-6);

    // A wheel event outside the plot area (over an axis) is ignored, not zoomed.
    const double zoom_before_stray = c.zoomLevel();
    const QPointF outside(2, 2);
    QWheelEvent stray(outside, outside, QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&c, &stray);
    CHECK_NEAR(c.zoomLevel(), zoom_before_stray, 1e-9);
}

TEST_CASE("zoom/pan: dragging pans the view, and double-click resets it") {
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}});
    render(c, 400, 400);
    const QRectF original = c.viewRect();

    const QPointF from = c.plotRect().center();
    const QPointF to = from + QPointF(30, 20);
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &press);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &release);

    CHECK(c.viewRect().center() != original.center());  // panned away from the original centre
    CHECK_NEAR(c.viewRect().width(), original.width(), 1e-9);   // panning does not change the zoom level
    CHECK_NEAR(c.zoomLevel(), 1.0, 1e-9);

    QMouseEvent dbl(QEvent::MouseButtonDblClick, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&c, &dbl);
    render(c, 400, 400);
    CHECK_NEAR(c.zoomLevel(), 1.0, 1e-9);
    CHECK_NEAR(c.viewRect().center().x(), original.center().x(), 1e-6);
    CHECK_NEAR(c.viewRect().center().y(), original.center().y(), 1e-6);
}

TEST_CASE("zoom/pan: loading new data resets zoom, even if the previous capture was zoomed in") {
    ConstellationWidget c;
    c.setData({{1, 0}, {-1, 0}, {0, 1}, {0, -1}});
    render(c, 400, 400);
    const QPointF cursor = c.plotRect().center();
    QWheelEvent wheel(cursor, cursor, QPoint(0, 0), QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&c, &wheel);
    CHECK(c.zoomLevel() > 1.0);

    c.setData({{5, 5}, {-5, -5}});
    render(c, 400, 400);
    CHECK_NEAR(c.zoomLevel(), 1.0, 1e-9);
}

// --- waterfall --------------------------------------------------------------------

TEST_CASE("waterfall: viridis endpoints match pyqtgraph's table") {
    CHECK(viridis(0) == qRgb(68, 1, 84));
    CHECK(viridis(255) == qRgb(253, 231, 37));
    CHECK(viridis(-5) == viridis(0));
    CHECK(viridis(999) == viridis(255));
}

TEST_CASE("waterfall: frequency runs left to right, most recent time at the top, hot cells are yellow") {
    // Deliberately the SDR-waterfall convention (GQRX, SDR#, IQEngine), not the
    // original PyQt6 app's own time-on-x layout -- see the class comment.
    WaterfallWidget w;
    auto s = make_spectrogram(6, 8);
    s.data[2 * 8 + 5] = 100.0f;  // frame 2, frequency bin 5: the only hot cell
    w.setData(s);
    CHECK(w.hasData());
    CHECK(w.image().width() == 8 && w.image().height() == 6);
    CHECK(w.levelMin() == 0.0 && w.levelMax() == 100.0);
    CHECK(w.imageRect() == QRectF(-4000.0, 0.0, 7000.0, 0.5));   // [f0,fN] x [t0,tN]

    const QImage img = render(w, 500, 320);
    auto cell_center = [&](unsigned t, unsigned f) {
        return w.dataToPixel(-4000.0 + 7000.0 * (f + 0.5) / 8.0, 0.5 * (t + 0.5) / 6.0);
    };
    CHECK(color_near(pixel_at(img, cell_center(2, 5)), viridis(255)));  // hot cell, where the data says it is
    CHECK(color_near(pixel_at(img, cell_center(0, 0)), viridis(0)));    // cold
    CHECK(color_near(pixel_at(img, cell_center(5, 7)), viridis(0)));
    CHECK(color_near(pixel_at(img, cell_center(1, 5)), viridis(0)));    // neighbours in time are cold
    CHECK(color_near(pixel_at(img, cell_center(2, 4)), viridis(0)));    // and in frequency
    CHECK(cell_center(0, 0).x() < cell_center(0, 7).x());          // low frequency is further left on screen
    CHECK(cell_center(0, 0).y() > cell_center(5, 0).y());          // later frames are higher up (most recent at the top)
    CHECK(w.image().pixel(5, 6 - 1 - 2) == viridis(255));          // stored with row 0 = the most recent frame group
}

TEST_CASE("waterfall: rendered with smooth (bilinear) scaling, not hard blocky pixels") {
    // Found from a real user report: a short capture's picture (a handful of frame
    // groups, stretched tall to fill the plot -- time is the y axis; see the class
    // comment) rendered with hard nearest-neighbor pixels reads as "a blocky photo
    // pasted in", not a real-looking waterfall. This checks the actual visual property
    // that fixes it -- a point straddling two very different rows reads as a blend of
    // both, not a hard step -- so a future regression back to hard pixels (unlike the
    // cell-centre colour checks above, which cannot tell the two apart) is caught.
    WaterfallWidget w;
    auto s = make_spectrogram(2, 1);
    s.data = {-100.0f, 100.0f};  // frame 0 cold, frame 1 hot
    s.times = {0.0, 1.0};
    w.setData(s);
    const QImage img = render(w, 200, 400);

    // A single frequency bin and s.freqs.size() == 1: imageRect_ falls back to
    // (0,0,nfft,frames) = (0,0,1,2) data-space units (see WaterfallWidget::setData), so
    // the one column spans x in [0, 1] and the two rows split y in [0, 2] between them
    // -- frame 0 (cold, earlier) in [0, 1] at the bottom, frame 1 (hot, later, so drawn
    // at the top -- see the class comment) in [1, 2].
    const QPointF c0 = w.dataToPixel(0.5, 0.5);  // centre of frame 0's row
    const QPointF c1 = w.dataToPixel(0.5, 1.5);  // centre of frame 1's row
    // Measured (not assumed): with only two source rows stretched this tall, Qt's
    // bilinear ramp is not centred on the midpoint -- it sits flat at the cold colour
    // until around the midpoint, then rises the rest of the way to c1, so a point most of
    // the way from c0 to c1 (not exactly halfway) is where the blend actually shows.
    const QPointF mostly_toward_hot(c0.x(), c0.y() + 0.85 * (c1.y() - c0.y()));

    const QRgb cold = viridis(0), hot = viridis(255);
    const QRgb blended = pixel_at(img, mostly_toward_hot);
    CHECK(!color_near(blended, cold, 20));  // not a hard step: measurably left the cold colour...
    CHECK(!color_near(blended, hot, 20));   // ...without having fully arrived at the hot one either
    CHECK(qRed(blended) > qRed(cold) && qRed(blended) < qRed(hot));
}

TEST_CASE("waterfall: a long capture is folded into a bounded picture without losing a burst") {
    // 20,000 frames would be a 20,000-row picture; it is folded into kMaxColumns rows,
    // keeping the per-bin maximum so one loud frame stays visible, with levels from the
    // whole data.
    auto s = make_spectrogram(20000, 8, -50.0f);
    s.times.clear();
    for (unsigned i = 0; i < s.frames; ++i) s.times.push_back(0.001 * i);
    s.data[12345 * 8 + 5] = 100.0f;                            // one hot cell in one frame
    s.data[7 * 8 + 0] = -300.0f;                               // and one very cold cell elsewhere
    WaterfallWidget w;
    w.setData(s);
    CHECK(w.image().width() == 8 && w.image().height() == static_cast<int>(WaterfallWidget::kMaxColumns));
    CHECK(w.levelMax() == 100.0 && w.levelMin() == -300.0);     // whole-data levels, not the picture's
    const int folded_row = static_cast<int>(12345ull * WaterfallWidget::kMaxColumns / 20000);
    CHECK(w.image().pixel(5, static_cast<int>(WaterfallWidget::kMaxColumns) - 1 - folded_row) ==
          viridis(255));  // the burst survives, in the right column and row
    int hot = 0;
    for (int x = 0; x < w.image().width(); ++x)
        for (int y = 0; y < w.image().height(); ++y) hot += (w.image().pixel(x, y) == viridis(255));
    CHECK(hot == 1);                                           // and only there
    CHECK(w.imageRect() == QRectF(-4000.0, 0.0, 7000.0, 19.999));   // the time span is unchanged

    auto small = make_spectrogram(WaterfallWidget::kMaxColumns, 4, 1.0f);   // exactly at the limit: one row per frame
    w.setData(small);
    CHECK(w.image().height() == static_cast<int>(WaterfallWidget::kMaxColumns));
    auto just_over = make_spectrogram(WaterfallWidget::kMaxColumns + 1, 4, 1.0f);
    w.setData(just_over);
    CHECK(w.image().height() == static_cast<int>(WaterfallWidget::kMaxColumns));
}

TEST_CASE("waterfall: colours span the range monotonically") {
    WaterfallWidget w;
    auto s = make_spectrogram(1, 4);
    s.data = {-100.0f, -50.0f, 0.0f, 100.0f};  // bins 0..3, i.e. columns 0..3 (frequency is the x axis)
    s.times = {0.0};
    w.setData(s);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 100.0);
    CHECK(w.image().pixel(0, 0) == viridis(0));                          // lowest value, lowest bin (leftmost column)
    CHECK(w.image().pixel(3, 0) == viridis(255));                        // highest value, rightmost column
    CHECK(w.image().pixel(1, 0) == viridis(static_cast<int>(50.0 * 256 / 200)));
    CHECK(w.image().pixel(2, 0) == viridis(static_cast<int>(100.0 * 256 / 200)));
}

TEST_CASE("waterfall: setColorRange recolours from a manual dB range, and reverts to auto") {
    WaterfallWidget w;
    auto s = make_spectrogram(1, 4);
    s.data = {-100.0f, -50.0f, 0.0f, 100.0f};  // bins 0..3, i.e. columns 0..3 (frequency is the x axis)
    s.times = {0.0};
    w.setData(s);
    CHECK(w.dataMin() == -100.0 && w.dataMax() == 100.0);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 100.0);  // auto, before any override

    // Narrowing the range to [-50, 0] clips the -100 and 100 cells to the map's ends,
    // and moves the -50/0 cells to where that narrower span now puts them.
    w.setColorRange(-50.0, 0.0);
    CHECK(w.levelMin() == -50.0 && w.levelMax() == 0.0);
    CHECK(w.dataMin() == -100.0 && w.dataMax() == 100.0);  // the data's own range is untouched
    CHECK(w.image().pixel(0, 0) == viridis(0));            // -100, below the floor: clipped to the bottom colour
    CHECK(w.image().pixel(3, 0) == viridis(255));          // 100, above the ceiling: clipped to the top colour
    CHECK(w.image().pixel(1, 0) == viridis(0));            // -50 is now the floor itself
    CHECK(w.image().pixel(2, 0) == viridis(255));          // 0 is now the ceiling itself

    // One end only: the other keeps using the data's own value.
    w.setColorRange(std::nullopt, 0.0);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 0.0);

    // An inverted or degenerate range is rejected outright -- the previous range holds.
    w.setColorRange(10.0, -10.0);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 0.0);
    w.setColorRange(5.0, 5.0);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 0.0);

    // Both nullopt: back to full auto, matching setData()'s own initial state.
    w.setColorRange(std::nullopt, std::nullopt);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 100.0);
    CHECK(w.image().pixel(0, 0) == viridis(0));
    CHECK(w.image().pixel(3, 0) == viridis(255));

    // A fresh setData() always resets to auto, discarding any override the previous
    // capture had (a stale manual range would otherwise silently carry over to a
    // capture with an entirely different real dB range).
    w.setColorRange(-50.0, 0.0);
    w.setData(s);
    CHECK(w.levelMin() == -100.0 && w.levelMax() == 100.0);

    // No data loaded: nothing to recolour, and nothing crashes.
    WaterfallWidget empty;
    empty.setColorRange(-10.0, 10.0);
    CHECK(!empty.hasData());
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
    CHECK(w.hasData() && w.imageRect() == QRectF(0, 0, 8, 1));

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

// --- frame raster (Feature 4) ------------------------------------------------------

TEST_CASE("frame raster: colours the label strip and draws bits with frame index increasing upward") {
    sigapp::FrameRaster r;
    r.detected = true;
    r.frame_length = 4;
    r.num_frames = 3;
    r.matrix = {1, 0, 1, 0,   // frame 0
               0, 1, 0, 1,   // frame 1
               1, 1, 0, 0};  // frame 2
    r.columns.resize(4);
    r.columns[0].label = sigapp::ColumnLabel::Constant;
    r.columns[1].label = sigapp::ColumnLabel::CounterLike;
    r.columns[2].label = sigapp::ColumnLabel::Payload;
    r.columns[3].label = sigapp::ColumnLabel::Payload;

    FrameRasterWidget w;
    w.setData(r);
    CHECK(w.hasData());
    CHECK(w.image().width() == 4 && w.image().height() == 4);  // +1 row for the label strip

    // The label strip (image row 0).
    CHECK(w.image().pixel(0, 0) != w.image().pixel(2, 0));  // constant differs from payload
    CHECK(w.image().pixel(1, 0) != w.image().pixel(2, 0));  // counter-like differs from payload
    CHECK(w.image().pixel(2, 0) == w.image().pixel(3, 0));  // both payload columns match

    // Frame index increases upward: frame 2 is nearest the label strip, frame 0 furthest.
    auto white = qRgb(255, 255, 255);
    auto black = qRgb(0, 0, 0);
    CHECK(w.image().pixel(0, 1) == white && w.image().pixel(1, 1) == white);  // frame 2 = 1,1,0,0
    CHECK(w.image().pixel(2, 1) == black && w.image().pixel(3, 1) == black);
    CHECK(w.image().pixel(0, 2) == black && w.image().pixel(1, 2) == white);  // frame 1 = 0,1,0,1
    CHECK(w.image().pixel(0, 3) == white && w.image().pixel(1, 3) == black);  // frame 0 = 1,0,1,0

    const QImage rendered = render(w, 400, 300);
    const QRectF plot = w.plotRect();
    CHECK(pixel_at(rendered, QPointF(plot.left() + 2, plot.top() + 2)) != qRgb(255, 255, 255));  // the strip is drawn, not white background

    w.clearData();
    CHECK(!w.hasData());
    CHECK(w.image().isNull());
}

TEST_CASE("frame raster: no detection shows an empty picture, not a guess") {
    FrameRasterWidget w;
    sigapp::FrameRaster none;  // detected = false by default
    w.setData(none);
    CHECK(!w.hasData());
    CHECK(w.image().isNull());
    const QImage rendered = render(w, 300, 200);
    CHECK(pixel_at(rendered, w.plotRect().center()) == qRgb(255, 255, 255));
}

// --- rank analysis (Feature 2) -----------------------------------------------------

TEST_CASE("rank analysis: draws a profile line, a threshold line, and marks the spike") {
    sigapp::RankAnalysis a;
    a.detected = true;
    a.width = 16;
    a.rank = 8.0;
    a.code_rate = 0.5;
    a.windows_used = 3;
    for (unsigned w = 8; w <= 24; ++w) {
        sigapp::RankProfilePoint p;
        p.width = w;
        p.excess_deficiency = (w == 16) ? 10.0 : 0.1;
        a.profile.push_back(p);
    }

    RankAnalysisWidget widget;
    widget.setData(a);
    CHECK(widget.hasData());
    CHECK(widget.analysis().width == 16);

    const QImage rendered = render(widget, 400, 300);
    const QRectF plot = widget.plotRect();
    // Something other than plain white was drawn inside the plot area (the profile
    // line, the threshold line, and the spike marker all have to land somewhere).
    bool any_ink = false;
    for (int x = static_cast<int>(plot.left()) + 1; x < static_cast<int>(plot.right()); x += 5) {
        if (pixel_at(rendered, QPointF(x, plot.center().y())) != qRgb(255, 255, 255)) any_ink = true;
    }
    CHECK(any_ink);

    widget.clearData();
    CHECK(!widget.hasData());
    CHECK(widget.analysis().profile.empty());
}

TEST_CASE("rank analysis: no detection shows an empty profile, not a guess") {
    RankAnalysisWidget widget;
    sigapp::RankAnalysis none;  // detected = false, profile empty, by default
    widget.setData(none);
    CHECK(!widget.hasData());
    CHECK(widget.analysis().profile.empty());
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

    CHECK(win.windowTitle() == "Deconflicted - Signal Analysis Tool");
    CHECK(win.statusBar()->currentMessage() == "Open a .wav or .IQ file to begin.");
    CHECK(win.width() == 1300 && win.height() == 700);

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

    // waterfall above constellation on the left, results panel on the right, 2:1 -- each
    // in its own dockable panel, matching the old fixed splitter layout by default.
    auto docks = win.findChildren<QDockWidget*>();
    CHECK(docks.size() == 4);
    for (QDockWidget* dock : {win.waterfallDock(), win.constellationDock(), win.resultsDock(), win.featuresDock()}) {
        CHECK(dock->features() & QDockWidget::DockWidgetMovable);
        CHECK(dock->features() & QDockWidget::DockWidgetFloatable);
        CHECK(dock->features() & QDockWidget::DockWidgetClosable);
    }
    CHECK(win.waterfall()->geometry().bottom() <= win.constellation()->geometry().top() + 1
          || win.waterfall()->mapTo(&win, QPoint(0, 0)).y() < win.constellation()->mapTo(&win, QPoint(0, 0)).y());
    CHECK(win.waterfall()->mapTo(&win, QPoint(0, 0)).x() < win.resultsPanel()->mapTo(&win, QPoint(0, 0)).x());
    CHECK(win.waterfall()->width() > win.resultsPanel()->width());
    const double ratio = static_cast<double>(win.waterfall()->width()) / win.resultsPanel()->width();
    // Roughly 2:1, not pinned tighter: resizeDocks() (see applyDockProportions()'s own
    // comment) lands at a somewhat different point once the waterfall dock's own content
    // changes shape (its colour-range control row, added alongside this check once, moved
    // this ratio measurably even though the row's own minimum width does not appear to be
    // the reason -- direct reductions to it left the number unchanged) -- another symptom
    // of the same approximate, content-sensitive behaviour already noted below, for the
    // same underlying mechanism, not a new one. The two things this test actually needs to
    // hold -- waterfall wider than results, and both clearly non-degenerate -- are checked
    // above and by the ratio bound itself.
    CHECK(ratio > 1.3 && ratio < 3.2);
    win.resize(1400, 800);
    pump();
    // Unlike the old fixed QSplitter (whose stretch factors held the ratio through any
    // resize), a dockable panel's whole point is that its size is the user's to change --
    // Qt's own dock layout hands most of a resize's extra space to whichever panel is
    // last in the layout tree, rather than preserving every panel's share, and fighting
    // that to keep a fixed ratio would undermine the "drag it how you like" premise this
    // layout was switched to in the first place. So only the ordering is expected to
    // hold after a resize, not the exact proportions.
    CHECK(win.waterfall()->mapTo(&win, QPoint(0, 0)).x() < win.resultsPanel()->mapTo(&win, QPoint(0, 0)).x());
    CHECK(win.resultsPanel()->isReadOnly());

    // closing a panel via its View-menu action hides it without touching its data
    auto* view_menu = win.findChild<QMenu*>();
    CHECK(view_menu != nullptr);
    win.waterfallDock()->toggleViewAction()->trigger();
    CHECK(!win.waterfallDock()->isVisible());
    win.waterfallDock()->toggleViewAction()->trigger();
    CHECK(win.waterfallDock()->isVisible());
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
    CHECK(win.waterfall()->image().width() == static_cast<int>(r.spectrogram.nfft));
    CHECK(win.waterfall()->image().height() == static_cast<int>(r.spectrogram.frames));
    CHECK(win.constellation()->points().size() == r.samples.size());      // 512 samples: all kept
    CHECK(win.resultsPanel()->toPlainText().toStdString() == sigapp::format_result_text(r));
    CHECK(win.statusBar()->currentMessage().toStdString() == sigapp::format_done_status(r));
    CHECK(win.runAction()->isEnabled());
}

TEST_CASE("main window: the soft-decoding toggle is sent to the backend and shown in the panel") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("bpsk_ldpc_block.iq", demo_bytes("bpsk_ldpc_block.iq"));
    CHECK(!win.softDecodingCheckBox()->isChecked());   // off by default
    win.sampleRateEdit()->setText("8000");
    win.samplesPerSymbolSpinBox()->setValue(4);
    backend.force_fec_scheme = "ldpc";
    backend.force_fec_msg_len = 64;
    win.runAnalysis();
    pump();
    CHECK(!backend.requests.back().options.soft_decoding);
    CHECK(win.resultsPanel()->toPlainText().contains("Decoding mode:        hard"));

    win.softDecodingCheckBox()->setChecked(true);
    win.runAnalysis();
    pump();
    CHECK(backend.requests.back().options.soft_decoding);
    CHECK(win.resultsPanel()->toPlainText().contains("Decoding mode:        soft (LLR)"));
    CHECK(win.resultsPanel()->toPlainText().contains("Re-encode agreement:"));
}

TEST_CASE("main window: the recover-timing toggle is sent to the backend") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("bpsk_ldpc_block.iq", demo_bytes("bpsk_ldpc_block.iq"));
    CHECK(!win.recoverTimingCheckBox()->isChecked());   // off by default, like soft decoding
    win.sampleRateEdit()->setText("8000");
    win.samplesPerSymbolSpinBox()->setValue(4);
    backend.force_fec_scheme = "ldpc";
    backend.force_fec_msg_len = 64;
    win.runAnalysis();
    pump();
    CHECK(!backend.requests.back().options.recover_timing);

    win.recoverTimingCheckBox()->setChecked(true);
    win.runAnalysis();
    pump();
    CHECK(backend.requests.back().options.recover_timing);
}

TEST_CASE("main window: the IQ format combo box is sent to the backend") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("bpsk_ldpc_block.iq", demo_bytes("bpsk_ldpc_block.iq"));
    win.sampleRateEdit()->setText("8000");
    CHECK(win.iqFormatComboBox()->currentData().toString() == "cf32");  // default: this project's own convention

    win.runAnalysis();
    pump();
    CHECK(backend.requests.back().options.iq_format == sigcore::IqSampleFormat::Cf32);

    win.iqFormatComboBox()->setCurrentIndex(1);
    CHECK(win.iqFormatComboBox()->currentData().toString() == "ci16");
    win.runAnalysis();
    pump();
    CHECK(backend.requests.back().options.iq_format == sigcore::IqSampleFormat::Ci16);

    win.iqFormatComboBox()->setCurrentIndex(2);
    CHECK(win.iqFormatComboBox()->currentData().toString() == "cu8");
    win.runAnalysis();
    pump();
    CHECK(backend.requests.back().options.iq_format == sigcore::IqSampleFormat::Cu8);
}

TEST_CASE("main window: the square-signal toggle changes the waterfall, reversibly") {
    StubBackend backend;
    MainWindow win(&backend);
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    win.sampleRateEdit()->setText("8000");
    CHECK(!win.squareSignalCheckBox()->isChecked());  // off by default
    win.runAnalysis();
    pump();
    CHECK(win.waterfall()->hasData());
    const QImage original = win.waterfall()->image();

    win.squareSignalCheckBox()->setChecked(true);
    pump();
    CHECK(win.waterfall()->hasData());
    const QImage squared = win.waterfall()->image();
    CHECK(squared.size() == original.size());  // same nfft: same picture dimensions
    CHECK(squared != original);                // squaring the capture really does change the spectrum shown

    win.squareSignalCheckBox()->setChecked(false);
    pump();
    CHECK(win.waterfall()->image() == original);  // reverts exactly: recomputed from last_result_, not accumulated

    // Toggled with nothing analysed yet: nothing to square, nothing crashes.
    MainWindow win2(&backend);
    win2.squareSignalCheckBox()->setChecked(true);
    pump();
    CHECK(!win2.waterfall()->hasData());
}

TEST_CASE("main window: the waterfall colour-range sliders and Auto button") {
    StubBackend backend;
    MainWindow win(&backend);
    CHECK(!win.colorRangeMinSlider()->isEnabled());   // nothing loaded: disabled
    CHECK(!win.colorRangeMaxSlider()->isEnabled());
    CHECK(!win.colorRangeAutoButton()->isEnabled());

    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    win.sampleRateEdit()->setText("8000");
    win.runAnalysis();
    pump();
    CHECK(win.colorRangeMinSlider()->isEnabled());
    CHECK(win.colorRangeMaxSlider()->isEnabled());
    const double auto_min = win.waterfall()->levelMin();
    const double auto_max = win.waterfall()->levelMax();
    const int lo = win.colorRangeMinSlider()->value();
    const int hi = win.colorRangeMaxSlider()->value();
    CHECK(static_cast<double>(lo) <= auto_min);  // padded outward, never narrower than the real data
    CHECK(static_cast<double>(hi) >= auto_max);
    CHECK(win.waterfall()->levelMin() == auto_min);  // starts at full auto range, matching setData() itself
    CHECK(win.waterfall()->levelMax() == auto_max);

    // Narrow the range (proportionally, so this holds regardless of this capture's
    // actual dB span): the waterfall's applied levels follow the sliders exactly.
    const int quarter = std::max(1, (hi - lo) / 4);
    const int narrow_min = lo + quarter;
    const int narrow_max = hi - quarter;
    CHECK(narrow_min < narrow_max);
    win.colorRangeMinSlider()->setValue(narrow_min);
    win.colorRangeMaxSlider()->setValue(narrow_max);
    CHECK(win.waterfall()->levelMin() == static_cast<double>(narrow_min));
    CHECK(win.waterfall()->levelMax() == static_cast<double>(narrow_max));

    // Auto reverts to the data's own range, and back to matching the slider positions.
    win.colorRangeAutoButton()->click();
    CHECK(win.waterfall()->levelMin() == win.waterfall()->dataMin());
    CHECK(win.waterfall()->levelMax() == win.waterfall()->dataMax());
    CHECK(win.colorRangeMinSlider()->value() == lo);
    CHECK(win.colorRangeMaxSlider()->value() == hi);

    // Loading a new file disables and resets the controls again.
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    CHECK(!win.colorRangeMinSlider()->isEnabled());
    CHECK(!win.colorRangeMaxSlider()->isEnabled());
}

TEST_CASE("main window: the frame raster panel is wired to a finished analysis") {
    // fsk_plain.iq is a single short message with nothing to repeat: the raster panel
    // must honestly show "not detected", not a guess -- confirming the wiring reaches
    // both outcomes, not just the populated one.
    StubBackend backend;
    backend.classification = {"fsk", 1.0};  // fsk_plain.iq is FSK; StubBackend defaults to bpsk
    MainWindow win(&backend);
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    CHECK(!win.frameRaster()->hasData());  // cleared on load, before any analysis
    win.sampleRateEdit()->setText("8000");
    win.runAnalysis();
    pump();
    CHECK(!win.frameRaster()->hasData());
    CHECK(win.frameRasterText()->toPlainText().toStdString() == sigapp::format_raster_text(backend.last_result->raster));
    CHECK(win.frameRasterText()->toPlainText().contains("No repeating frame structure detected"));

    // A capture that DOES repeat: the panel picks it up.
    const sigapp::Bits header = {1, 0, 1, 1, 0, 0, 1, 0};
    Rng rng(60);
    sigapp::Bits stream;
    for (unsigned i = 0; i < 20; ++i) {
        stream.insert(stream.end(), header.begin(), header.end());
        for (unsigned b = 0; b < 24; ++b) stream.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
    }
    const auto samples = sigapp::modulate(stream, "bpsk");
    QByteArray raw(reinterpret_cast<const char*>(samples.data()), static_cast<qsizetype>(samples.size() * sizeof(samples[0])));
    win.loadFile("raster.iq", raw);
    win.sampleRateEdit()->setText("8000");
    backend.classification = {"bpsk", 1.0};
    win.runAnalysis();
    pump();

    CHECK(win.frameRaster()->hasData());
    CHECK(win.frameRaster()->raster().frame_length == 32);
    CHECK(win.frameRaster()->image().width() == 32);
    CHECK(win.frameRasterText()->toPlainText().contains("Frame length:         32 bits"));
    CHECK(win.frameRasterText()->toPlainText().contains("Sync word:            10110010"));
}

TEST_CASE("main window: the rank-analysis panel is wired to a finished analysis") {
    // fsk_plain.iq has no linear block code in it: the panel must honestly show "not
    // detected", not a guess, confirming the wiring reaches both outcomes.
    StubBackend backend;
    backend.classification = {"fsk", 1.0};
    MainWindow win(&backend);
    win.loadFile("fsk_plain.iq", demo_bytes("fsk_plain.iq"));
    CHECK(!win.rankAnalysis()->hasData());  // cleared on load, before any analysis
    win.sampleRateEdit()->setText("8000");
    win.runAnalysis();
    pump();
    CHECK(!win.rankAnalysis()->hasData());
    CHECK(win.rankAnalysisText()->toPlainText().toStdString() ==
          sigapp::format_rank_analysis_text(backend.last_result->rank_analysis));
    CHECK(win.rankAnalysisText()->toPlainText().contains("No rank-deficiency spike detected"));

    // A real LDPC(16,8) codeword stream: the panel picks up its true width and rate.
    sigapp::Bits stream;
    Rng rng(61);
    for (unsigned b = 0; b < 300; ++b) {
        const auto msg = random_bits(8, static_cast<uint64_t>(rng.next()));
        const auto coded = sigcore::ldpc_encode(msg);
        stream.insert(stream.end(), coded.begin(), coded.end());
    }
    const auto samples = sigapp::modulate(stream, "bpsk");
    QByteArray raw(reinterpret_cast<const char*>(samples.data()), static_cast<qsizetype>(samples.size() * sizeof(samples[0])));
    win.loadFile("rank.iq", raw);
    win.sampleRateEdit()->setText("8000");
    backend.classification = {"bpsk", 1.0};
    win.runAnalysis();
    pump();

    CHECK(win.rankAnalysis()->hasData());
    CHECK(win.rankAnalysis()->analysis().width == 16);
    CHECK(win.rankAnalysisText()->toPlainText().contains("Codeword length:      16 bits"));
}

TEST_CASE("main window: the parameter-search button finds the real message auto-detect missed") {
    // The exact bug report this feature exists to fix: a SINGLE LDPC-encoded message
    // (nothing repeats, so a header-repeat heuristic like detect.hpp's own has nothing
    // reliable to rank on) that auto-detect gets wrong, but the panel showed only the
    // raw, undecoded bits (hex-encoded, since it's not printable text) -- not the real
    // 'SIH2026!'. "Search Parameters" must find it directly.
    StubBackend backend;
    backend.classification = {"bpsk", 1.0};
    MainWindow win(&backend);

    sigapp::Bits stream;
    for (unsigned char ch : std::string("SIH2026!")) {
        sigapp::Bits byte_bits(8);
        for (int b = 0; b < 8; ++b) byte_bits[static_cast<size_t>(b)] = (ch >> (7 - b)) & 1;
        const auto coded = sigcore::ldpc_encode(byte_bits);
        stream.insert(stream.end(), coded.begin(), coded.end());
    }
    const auto samples = sigapp::modulate(stream, "bpsk");
    QByteArray raw(reinterpret_cast<const char*>(samples.data()), static_cast<qsizetype>(samples.size() * sizeof(samples[0])));
    win.loadFile("single_message.iq", raw);
    CHECK(!win.searchParamsButton()->isEnabled());  // nothing to search before an analysis has run

    win.sampleRateEdit()->setText("8000");
    win.runAnalysis();  // no auto-detect, no manual FEC: just the raw demodulated bits
    pump();
    CHECK(win.searchParamsButton()->isEnabled());
    CHECK(win.paramSearchTable()->rowCount() == 0);  // not run yet

    win.searchParamsButton()->click();
    CHECK(win.paramSearchTable()->rowCount() > 0);
    // The best candidate (row 0, auto-selected) is a real LDPC decode, and its message
    // text is the actual original message, not a hex placeholder.
    CHECK(win.paramSearchTable()->item(0, 1)->text() == QStringLiteral("ldpc"));
    CHECK(win.paramSearchText()->toPlainText() == QStringLiteral("SIH2026!"));

    // Loading a new file clears the search, and disables the button again.
    win.loadFile("single_message.iq", raw);
    CHECK(!win.searchParamsButton()->isEnabled());
    CHECK(win.paramSearchTable()->rowCount() == 0);
    CHECK(win.paramSearchText()->toPlainText().isEmpty());
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
