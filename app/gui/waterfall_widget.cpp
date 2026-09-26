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
    xLabel_ = QStringLiteral("Time (s)");
    yLabel_ = QStringLiteral("Frequency (Hz)");
    view_ = QRectF(0.0, 0.0, 1.0, 1.0);
}

void WaterfallWidget::setData(const sigapp::Spectrogram& s) {
    if (s.frames == 0 || s.nfft == 0 || s.data.size() < static_cast<size_t>(s.frames) * s.nfft) {
        clearData();
        return;
    }

    const auto [lo_it, hi_it] = std::minmax_element(s.data.begin(), s.data.end());
    levelMin_ = *lo_it;
    levelMax_ = *hi_it;
    const double span = levelMax_ - levelMin_;

    // The data is (frames, nfft) row-major; the picture has time along x and frequency up y,
    // so bin f of column c lands at pixel (c, nfft - 1 - f). A long capture is folded into
    // kMaxColumns columns, each the per-bin maximum over its group of frames.
    const unsigned columns = std::min(s.frames, kMaxColumns);
    image_ = QImage(static_cast<int>(columns), static_cast<int>(s.nfft), QImage::Format_RGB32);
    std::vector<float> column(s.nfft);
    for (unsigned c = 0; c < columns; ++c) {
        const size_t first = static_cast<size_t>(c) * s.frames / columns;
        const size_t last = static_cast<size_t>(c + 1) * s.frames / columns;   // exclusive; every group is non-empty
        std::copy_n(s.data.begin() + static_cast<long>(first * s.nfft), s.nfft, column.begin());
        for (size_t t = first + 1; t < last; ++t) {
            const float* row = &s.data[t * s.nfft];
            for (unsigned f = 0; f < s.nfft; ++f) column[f] = std::max(column[f], row[f]);
        }
        for (unsigned f = 0; f < s.nfft; ++f) {
            const int index = span > 0.0 ? static_cast<int>((column[f] - levelMin_) * 256.0 / span) : 0;  // pyqtgraph's LUT scaling
            image_.setPixel(static_cast<int>(c), static_cast<int>(s.nfft - 1 - f), viridis(index));
        }
    }

    // Like the original: the image is stretched over [times[0], times[-1]] x
    // [freqs[0], freqs[-1]] (a zero span becomes 1); with a single frame or bin it is
    // left at one data unit per pixel.
    if (s.times.size() > 1 && s.freqs.size() > 1) {
        const double w = s.times.back() - s.times.front();
        const double h = s.freqs.back() - s.freqs.front();
        imageRect_ = QRectF(s.times.front(), s.freqs.front(), w != 0.0 ? w : 1.0, h != 0.0 ? h : 1.0);
    } else {
        imageRect_ = QRectF(0.0, 0.0, s.frames, s.nfft);
    }

    // Auto-range with pyqtgraph's default 2% padding.
    const double px = imageRect_.width() * 0.02, py = imageRect_.height() * 0.02;
    view_ = QRectF(imageRect_.x() - px, imageRect_.y() - py, imageRect_.width() + 2 * px, imageRect_.height() + 2 * py);
    update();
}

void WaterfallWidget::clearData() {
    image_ = QImage();
    imageRect_ = QRectF();
    view_ = QRectF(0.0, 0.0, 1.0, 1.0);
    update();
}

void WaterfallWidget::paintData(QPainter& painter) {
    if (image_.isNull()) return;
    const QPointF top_left = dataToPixel(imageRect_.left(), imageRect_.bottom());
    const QPointF bottom_right = dataToPixel(imageRect_.right(), imageRect_.top());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);  // hard pixels, like pyqtgraph's image item
    painter.drawImage(QRectF(top_left, bottom_right), image_);
}

}  // namespace sigui
