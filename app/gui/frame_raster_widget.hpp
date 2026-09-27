#pragma once

#include <QImage>

#include "plot_axes.hpp"
#include "raster.hpp"

namespace sigui {

// Feature 4 (SIH26147 extra feature): the frame raster view. Each row is one frame, each
// column one bit position; black = 0, white = 1. A colour strip along the top marks how
// each column was labelled: green = constant (header/sync), amber = counter-like, light
// grey = payload. Frame index increases UP the y-axis, the same convention this app's
// other plots use for their own y-axis (e.g. WaterfallWidget's frequency); the axis label
// and its tick numbers make the direction unambiguous either way.
class FrameRasterWidget : public PlotWidgetBase {
    Q_OBJECT
public:
    explicit FrameRasterWidget(QWidget* parent = nullptr);

    void setData(const sigapp::FrameRaster& raster);
    void clearData();

    bool hasData() const { return raster_.detected; }
    const sigapp::FrameRaster& raster() const { return raster_; }
    // The bitmap actually drawn: frame_length columns, num_frames + 1 rows (the extra row
    // is the label strip at the top, y = 0).
    const QImage& image() const { return image_; }

protected:
    void paintData(QPainter& painter) override;

private:
    sigapp::FrameRaster raster_;
    QImage image_;
};

}  // namespace sigui
