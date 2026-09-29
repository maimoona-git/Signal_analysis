#include "waterfall_widget.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace sigui {

namespace {

constexpr unsigned char kViridis[256][3] = {
#include "viridis_table.inc"
};

}  // namespace

QRgb viridis(int index) {
    const int i = std::clamp(index, 0, 255);
    return qRgb(kViridis[i][0], kViridis[i][1], kViridis[i][2]);
}

WaterfallWidget::WaterfallWidget(QWidget* parent) : PlotWidgetBase(parent) {
    xLabel_ = QStringLiteral("Frequency (Hz)");
    yLabel_ = QStringLiteral("Time (s)");
    baseView_ = QRectF(0.0, 0.0, 1.0, 1.0);
}

void WaterfallWidget::setData(const sigapp::Spectrogram& s) {
    if (s.frames == 0 || s.nfft == 0 || s.data.size() < static_cast<size_t>(s.frames) * s.nfft) {
        clearData();
        return;
    }

    const auto [lo_it, hi_it] = std::minmax_element(s.data.begin(), s.data.end());
    dataMin_ = *lo_it;
    dataMax_ = *hi_it;
    overrideMin_.reset();
    overrideMax_.reset();

    // The data is (frames, nfft) row-major. A long capture is folded into kMaxColumns
    // groups of frames ("columns" by name, still -- see kMaxColumns' own comment),
    // each the per-bin maximum over its group. Kept (not just consumed into image_) so
    // setColorRange() can recolour later without the spectrogram.
    columns_ = std::min(s.frames, kMaxColumns);
    nfft_ = s.nfft;
    foldedData_.assign(static_cast<size_t>(columns_) * nfft_, 0.0f);
    std::vector<float> column(s.nfft);
    for (unsigned c = 0; c < columns_; ++c) {
        const size_t first = static_cast<size_t>(c) * s.frames / columns_;
        const size_t last = static_cast<size_t>(c + 1) * s.frames / columns_;   // exclusive; every group is non-empty
        std::copy_n(s.data.begin() + static_cast<long>(first * s.nfft), s.nfft, column.begin());
        for (size_t t = first + 1; t < last; ++t) {
            const float* row = &s.data[t * s.nfft];
            for (unsigned f = 0; f < s.nfft; ++f) column[f] = std::max(column[f], row[f]);
        }
        std::copy_n(column.begin(), s.nfft, foldedData_.begin() + static_cast<long>(c) * s.nfft);
    }
    regenerateImage();

    // Like the original, just with the axes swapped (see the class comment): the image
    // is stretched over [freqs[0], freqs[-1]] x [times[0], times[-1]] (a zero span
    // becomes 1); with a single frame or bin it is left at one data unit per pixel.
    if (s.times.size() > 1 && s.freqs.size() > 1) {
        const double w = s.freqs.back() - s.freqs.front();
        const double h = s.times.back() - s.times.front();
        imageRect_ = QRectF(s.freqs.front(), s.times.front(), w != 0.0 ? w : 1.0, h != 0.0 ? h : 1.0);
    } else {
        imageRect_ = QRectF(0.0, 0.0, s.nfft, s.frames);
    }

    // Auto-range with pyqtgraph's default 2% padding.
    const double px = imageRect_.width() * 0.02, py = imageRect_.height() * 0.02;
    baseView_ = QRectF(imageRect_.x() - px, imageRect_.y() - py, imageRect_.width() + 2 * px, imageRect_.height() + 2 * py);
    resetZoom();  // a newly loaded capture starts fully zoomed out, not wherever the last one was left
    update();
}

void WaterfallWidget::clearData() {
    image_ = QImage();
    imageRect_ = QRectF();
    baseView_ = QRectF(0.0, 0.0, 1.0, 1.0);
    foldedData_.clear();
    columns_ = 0;
    nfft_ = 0;
    dataMin_ = levelMin_ = 0.0;
    dataMax_ = levelMax_ = 1.0;
    overrideMin_.reset();
    overrideMax_.reset();
    resetZoom();
    update();
}

void WaterfallWidget::regenerateImage() {
    levelMin_ = overrideMin_.value_or(dataMin_);
    levelMax_ = overrideMax_.value_or(dataMax_);
    const double span = levelMax_ - levelMin_;

    // Frequency bin f is column x = f directly (ascending left to right, like a
    // spectrum analyzer -- freqs is already fft-shifted ascending, so no flip needed).
    // Frame group c is row (columns_ - 1 - c): c = 0 is the earliest group, and the
    // most recent time belongs at the top (row 0), matching a real SDR waterfall's own
    // scroll direction -- see the class comment.
    image_ = QImage(static_cast<int>(nfft_), static_cast<int>(columns_), QImage::Format_RGB32);
    for (unsigned c = 0; c < columns_; ++c) {
        for (unsigned f = 0; f < nfft_; ++f) {
            const float value = foldedData_[static_cast<size_t>(c) * nfft_ + f];
            const int index = span > 0.0 ? static_cast<int>((value - levelMin_) * 256.0 / span) : 0;  // pyqtgraph's LUT scaling
            image_.setPixel(static_cast<int>(f), static_cast<int>(columns_ - 1 - c), viridis(index));
        }
    }
}

void WaterfallWidget::setColorRange(std::optional<double> min_db, std::optional<double> max_db) {
    if (foldedData_.empty()) return;  // nothing loaded to recolour
    const double new_min = min_db.value_or(dataMin_);
    const double new_max = max_db.value_or(dataMax_);
    if (new_min >= new_max) return;  // would divide by zero or invert the map -- reject, keep the old range
    overrideMin_ = min_db;
    overrideMax_ = max_db;
    regenerateImage();
    update();
}

void WaterfallWidget::paintData(QPainter& painter) {
    if (image_.isNull()) return;
    const QPointF top_left = dataToPixel(imageRect_.left(), imageRect_.bottom());
    const QPointF bottom_right = dataToPixel(imageRect_.right(), imageRect_.top());
    // Smooth (bilinear), not the original pyqtgraph's own hard nearest-neighbor: found from
    // a real user report that a short capture's picture (a handful of columns, stretched
    // wide) reads as "a blocky photo pasted in", not a real-looking waterfall -- pyqtgraph's
    // own hard-pixel rendering has the exact same problem, this just no longer matches it
    // pixel-for-pixel. Sampling at a source cell's own centre (not its edge) still lands on
    // that cell's exact colour either way, so this does not disturb the colour-fidelity
    // tests (test_gui.cpp's own "hot cells are yellow" etc. sample cell centres for exactly
    // this reason) -- verified by running them with this change, not assumed.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(top_left, bottom_right), image_);
}

}  // namespace sigui
