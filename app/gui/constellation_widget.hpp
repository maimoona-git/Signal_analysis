#pragma once

#include <QPointF>

#include <complex>
#include <vector>

#include "plot_axes.hpp"

namespace sigui {

// I/Q scatter, like the native app's pyqtgraph plot: equal scale on both axes,
// light grid, translucent blue dots, axes "In-phase (I)" / "Quadrature (Q)".
class ConstellationWidget : public PlotWidgetBase {
    Q_OBJECT
public:
    static constexpr size_t kDefaultMaxPoints = 4000;

    explicit ConstellationWidget(QWidget* parent = nullptr);

    // Plots at most `max_points` samples. A longer capture is thinned by taking
    // evenly spaced samples (the native app draws a random subset; this is
    // deterministic, so the picture is reproducible).
    void setData(const std::vector<std::complex<float>>& samples, size_t max_points = kDefaultMaxPoints);
    void clearData();

    const std::vector<QPointF>& points() const { return points_; }

protected:
    void prepareView(const QRectF& plotPixels) override;
    void paintData(QPainter& painter) override;

private:
    std::vector<QPointF> points_;
    QRectF bounds_;  // data bounds, padded
};

}  // namespace sigui
