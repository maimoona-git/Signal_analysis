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

    // The data is (frames, nfft) row-major; the picture has time along x and
    // frequency up y, so bin f of frame t lands at pixel (t, nfft - 1 - f).
    image_ = QImage(static_cast<int>(s.frames), static_cast<int>(s.nfft), QImage::Format_RGB32);
    for (unsigned t = 0; t < s.frames; ++t) {
        for (unsigned f = 0; f < s.nfft; ++f) {
            const double v = s.data[static_cast<size_t>(t) * s.nfft + f];
            const int index = span > 0.0 ? static_cast<int>((v - levelMin_) * 256.0 / span) : 0;  // pyqtgraph's LUT scaling
            image_.setPixel(static_cast<int>(t), static_cast<int>(s.nfft - 1 - f), viridis(index));
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
