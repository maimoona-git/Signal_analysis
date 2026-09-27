#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

#include <vector>

namespace sigui {

// "Nice" tick positions covering [lo, hi]: multiples of 1, 2 or 5 x 10^k, roughly
// `target_count` of them. Empty for an empty or non-finite range.
std::vector<double> nice_ticks(double lo, double hi, int target_count = 6);

// A tick label with just enough decimals for the tick spacing ("0.5", "1200", "1e+06").
QString format_tick(double value, double step);

// A widget that draws a white plot area with numbered axes, axis titles and an
// optional grid, and leaves the data to subclasses. The data-space rectangle
// `view_` (x0, y0 at the bottom-left, y pointing up) maps onto `plotRect()`;
// subclasses set `baseView_` (via prepareView, or whenever their data changes) to
// the fully-zoomed-out rectangle that should show everything, and this base class
// derives the actually-displayed `view_` from it by applying the current
// zoom/pan -- mouse wheel zooms toward the cursor, dragging pans, and double-
// clicking resets back to `baseView_`. Every subclass gets this for free.
class PlotWidgetBase : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidgetBase(QWidget* parent = nullptr);

    // The pixel rectangle of the data area (inside the axes), for the current size.
    QRectF plotRect() const;
    // The data-space rectangle currently shown: x, y = lower-left corner, width, height.
    QRectF viewRect() const { return view_; }
    QPointF dataToPixel(double x, double y) const;
    // The inverse of dataToPixel: what data-space point a pixel corresponds to,
    // given the view currently on screen.
    QPointF pixelToData(const QPointF& pixel) const;

    // How far in the view currently is: 1.0 is fully zoomed out (view_ == baseView_).
    double zoomLevel() const { return zoom_; }
    // Resets zoom/pan back to baseView_ (also called whenever a subclass's data
    // changes -- see resetZoom()).
    void resetZoom();

    QSize sizeHint() const override { return {520, 300}; }
    QSize minimumSizeHint() const override { return {260, 180}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    // Called at the start of every paint with the current plot area, so a
    // subclass can derive `baseView_` from its data (e.g. to keep an aspect
    // ratio). Called on every repaint (including a resize), NOT just when data
    // changes, so it must not reset the user's current zoom/pan itself -- call
    // resetZoom() explicitly from setData()/clearData() instead.
    virtual void prepareView(const QRectF& /*plotPixels*/) {}
    virtual void paintData(QPainter& painter) = 0;

    // The fully-zoomed-out view a subclass wants shown at zoom level 1: set this
    // (not view_) from data-loading code and from prepareView().
    QRectF baseView_{0.0, 0.0, 1.0, 1.0};
    // The view actually on screen right now: baseView_ with the current zoom/pan
    // applied. Recomputed every paint; read this (via dataToPixel) to draw.
    QRectF view_{0.0, 0.0, 1.0, 1.0};
    QString xLabel_, yLabel_;
    bool grid_ = false;

private:
    void applyZoom();  // recomputes view_ from baseView_, zoom_ and center_

    double zoom_ = 1.0;
    QPointF center_;
    bool centerValid_ = false;  // center_ is lazily initialised from baseView_'s first center
    bool panning_ = false;
    QPoint lastPanPixel_;

    static constexpr double kMinZoom = 1.0;
    static constexpr double kMaxZoom = 64.0;
    static constexpr double kWheelZoomStep = 1.15;  // per wheel notch (120 units of angleDelta)
};

}  // namespace sigui
