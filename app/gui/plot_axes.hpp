#pragma once

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
// subclasses set it (see prepareView) and draw in paintData().
class PlotWidgetBase : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidgetBase(QWidget* parent = nullptr);

    // The pixel rectangle of the data area (inside the axes), for the current size.
    QRectF plotRect() const;
    // The data-space rectangle currently shown: x, y = lower-left corner, width, height.
    QRectF viewRect() const { return view_; }
    QPointF dataToPixel(double x, double y) const;

    QSize sizeHint() const override { return {520, 300}; }
    QSize minimumSizeHint() const override { return {260, 180}; }

protected:
    void paintEvent(QPaintEvent* event) override;

    // Called at the start of every paint with the current plot area, so a
    // subclass can derive `view_` from its data (e.g. to keep an aspect ratio).
    virtual void prepareView(const QRectF& /*plotPixels*/) {}
    virtual void paintData(QPainter& painter) = 0;

    QRectF view_{0.0, 0.0, 1.0, 1.0};
    QString xLabel_, yLabel_;
    bool grid_ = false;
};

}  // namespace sigui
