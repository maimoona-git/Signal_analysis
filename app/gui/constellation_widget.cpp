#include "constellation_widget.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace sigui {

ConstellationWidget::ConstellationWidget(QWidget* parent) : PlotWidgetBase(parent) {
    xLabel_ = QStringLiteral("In-phase (I)");
    yLabel_ = QStringLiteral("Quadrature (Q)");
    grid_ = true;
    bounds_ = QRectF(-1.0, -1.0, 2.0, 2.0);
    view_ = bounds_;
}

void ConstellationWidget::setData(const std::vector<std::complex<float>>& samples, size_t max_points) {
    points_.clear();
    const size_t n = samples.size();
    const size_t count = std::min(n, max_points);
    points_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& s = samples[count == n ? i : i * n / count];
        points_.emplace_back(s.real(), s.imag());
    }

    if (points_.empty()) {
        bounds_ = QRectF(-1.0, -1.0, 2.0, 2.0);
    } else {
        double x0 = points_[0].x(), x1 = x0, y0 = points_[0].y(), y1 = y0;
        for (const auto& p : points_) {
            x0 = std::min(x0, p.x()); x1 = std::max(x1, p.x());
            y0 = std::min(y0, p.y()); y1 = std::max(y1, p.y());
        }
        double w = x1 - x0, h = y1 - y0;
        if (w <= 0.0 && h <= 0.0) { w = h = 2.0; x0 -= 1.0; y0 -= 1.0; }    // a single point: show it centred
        else { const double pad = 0.02; x0 -= w * pad; y0 -= h * pad; w *= 1 + 2 * pad; h *= 1 + 2 * pad; }
        bounds_ = QRectF(x0, y0, std::max(w, 1e-12), std::max(h, 1e-12));
    }
    update();
}

void ConstellationWidget::clearData() {
    points_.clear();
    bounds_ = QRectF(-1.0, -1.0, 2.0, 2.0);
    update();
}

// Aspect ratio locked to 1:1 (like setAspectLocked(True)): the same number of data
// units per pixel on both axes, centred on the data.
void ConstellationWidget::prepareView(const QRectF& plot) {
    const double w = std::max(bounds_.width(), 1e-12), h = std::max(bounds_.height(), 1e-12);
    const double units_per_pixel = std::max(w / plot.width(), h / plot.height());
    const double vw = units_per_pixel * plot.width(), vh = units_per_pixel * plot.height();
    view_ = QRectF(bounds_.center().x() - vw / 2, bounds_.center().y() - vh / 2, vw, vh);
}

void ConstellationWidget::paintData(QPainter& painter) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(30, 100, 200, 150));
    for (const auto& p : points_) painter.drawEllipse(dataToPixel(p.x(), p.y()), 3.0, 3.0);  // size 6 px
}

}  // namespace sigui
