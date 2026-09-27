#include "plot_axes.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sigui {

namespace {

constexpr int kLeftMargin = 68;
constexpr int kBottomMargin = 46;
constexpr int kTopMargin = 10;
constexpr int kRightMargin = 14;

const QColor kAxisColor(90, 90, 90);
const QColor kTextColor(60, 60, 60);

}  // namespace

std::vector<double> nice_ticks(double lo, double hi, int target_count) {
    std::vector<double> ticks;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo || target_count < 1) return ticks;

    const double raw = (hi - lo) / target_count;
    const double exponent = std::floor(std::log10(raw));
    const double magnitude = std::pow(10.0, exponent);
    const double fraction = raw / magnitude;
    const double nice = fraction < 1.5 ? 1.0 : fraction < 3.0 ? 2.0 : fraction < 7.0 ? 5.0 : 10.0;
    const double step = nice * magnitude;

    const double first = std::ceil(lo / step - 1e-9) * step;
    for (double v = first; v <= hi + step * 1e-9; v += step) {
        ticks.push_back(std::fabs(v) < step * 1e-9 ? 0.0 : v);  // no "-0"
        if (ticks.size() > 100) break;
    }
    return ticks;
}

QString format_tick(double value, double step) {
    if (value == 0.0) return QStringLiteral("0");
    const double magnitude = std::max(std::fabs(value), std::fabs(step));
    if (magnitude >= 1e6 || step < 1e-4) return QString::number(value, 'g', 6);
    const int decimals = step >= 1.0 ? 0 : static_cast<int>(std::ceil(-std::log10(step) - 1e-9));
    return QString::number(value, 'f', std::min(decimals, 8));
}

PlotWidgetBase::PlotWidgetBase(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false);
    setMouseTracking(false);
    setToolTip(QStringLiteral("Scroll to zoom, drag to pan, double-click to reset"));
    setFocusPolicy(Qt::StrongFocus);
}

QRectF PlotWidgetBase::plotRect() const {
    return QRectF(kLeftMargin, kTopMargin, std::max(1, width() - kLeftMargin - kRightMargin),
                  std::max(1, height() - kTopMargin - kBottomMargin));
}

QPointF PlotWidgetBase::dataToPixel(double x, double y) const {
    const QRectF plot = plotRect();
    return QPointF(plot.left() + (x - view_.x()) / view_.width() * plot.width(),
                   plot.bottom() - (y - view_.y()) / view_.height() * plot.height());
}

QPointF PlotWidgetBase::pixelToData(const QPointF& pixel) const {
    const QRectF plot = plotRect();
    return QPointF(view_.x() + (pixel.x() - plot.left()) / plot.width() * view_.width(),
                   view_.y() + (plot.bottom() - pixel.y()) / plot.height() * view_.height());
}

void PlotWidgetBase::applyZoom() {
    if (!centerValid_) {
        center_ = baseView_.center();
        centerValid_ = true;
    }
    const double w = std::max(baseView_.width(), 1e-12) / zoom_;
    const double h = std::max(baseView_.height(), 1e-12) / zoom_;
    view_ = QRectF(center_.x() - w / 2.0, center_.y() - h / 2.0, w, h);
}

void PlotWidgetBase::resetZoom() {
    zoom_ = 1.0;
    centerValid_ = false;  // re-derived from baseView_ on the next applyZoom()
    panning_ = false;
    update();
}

void PlotWidgetBase::wheelEvent(QWheelEvent* event) {
    const QPointF cursor = event->position();
    if (!plotRect().contains(cursor)) {
        event->ignore();
        return;
    }

    const QPointF before = pixelToData(cursor);
    const double notches = event->angleDelta().y() / 120.0;
    zoom_ = std::clamp(zoom_ * std::pow(kWheelZoomStep, notches), kMinZoom, kMaxZoom);
    applyZoom();
    const QPointF after = pixelToData(cursor);
    center_ += before - after;  // keep the point under the cursor fixed
    applyZoom();

    update();
    event->accept();
}

void PlotWidgetBase::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && plotRect().contains(event->pos())) {
        panning_ = true;
        lastPanPixel_ = event->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PlotWidgetBase::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPointF before = pixelToData(QPointF(lastPanPixel_));
        const QPointF after = pixelToData(QPointF(event->pos()));
        center_ += before - after;
        lastPanPixel_ = event->pos();
        applyZoom();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PlotWidgetBase::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && panning_) {
        panning_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PlotWidgetBase::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        resetZoom();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PlotWidgetBase::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);

    const QRectF plot = plotRect();
    prepareView(plot);
    applyZoom();

    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    p.setFont(small);
    const QFontMetrics fm(small);

    // ticks (and grid)
    const auto xticks = nice_ticks(view_.left(), view_.right(), std::max(2, static_cast<int>(plot.width() / 90)));
    const auto yticks = nice_ticks(view_.top(), view_.bottom(), std::max(2, static_cast<int>(plot.height() / 50)));
    const double xstep = xticks.size() > 1 ? xticks[1] - xticks[0] : 1.0;
    const double ystep = yticks.size() > 1 ? yticks[1] - yticks[0] : 1.0;

    QColor grid = kAxisColor;
    grid.setAlphaF(0.3);
    for (double xv : xticks) {
        const double px = dataToPixel(xv, view_.y()).x();
        if (grid_) { p.setPen(QPen(grid, 1)); p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom())); }
        p.setPen(kAxisColor);
        p.drawLine(QPointF(px, plot.bottom()), QPointF(px, plot.bottom() + 4));
        p.setPen(kTextColor);
        p.drawText(QRectF(px - 40, plot.bottom() + 5, 80, fm.height()), Qt::AlignHCenter | Qt::AlignTop, format_tick(xv, xstep));
    }
    for (double yv : yticks) {
        const double py = dataToPixel(view_.x(), yv).y();
        if (grid_) { p.setPen(QPen(grid, 1)); p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py)); }
        p.setPen(kAxisColor);
        p.drawLine(QPointF(plot.left() - 4, py), QPointF(plot.left(), py));
        p.setPen(kTextColor);
        p.drawText(QRectF(2, py - fm.height() / 2.0, plot.left() - 8, fm.height()), Qt::AlignRight | Qt::AlignVCenter, format_tick(yv, ystep));
    }

    // data, clipped to the plot area
    p.save();
    p.setClipRect(plot);
    paintData(p);
    p.restore();

    // frame and axis titles
    p.setPen(QPen(kAxisColor, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot);
    p.setPen(kTextColor);
    p.setFont(font());
    p.drawText(QRectF(plot.left(), height() - fm.height() - 4, plot.width(), fm.height() + 2), Qt::AlignHCenter | Qt::AlignVCenter, xLabel_);
    p.save();
    p.translate(14, plot.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-plot.height() / 2, -10, plot.height(), 20), Qt::AlignHCenter | Qt::AlignVCenter, yLabel_);
    p.restore();
}

}  // namespace sigui
