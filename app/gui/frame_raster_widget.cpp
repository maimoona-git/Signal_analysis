#include "frame_raster_widget.hpp"

#include <QPainter>

namespace sigui {

namespace {

constexpr QRgb kConstantColor = qRgb(46, 160, 67);   // green: header / sync
constexpr QRgb kCounterColor = qRgb(219, 154, 4);     // amber: counter-like
constexpr QRgb kPayloadColor = qRgb(225, 225, 225);   // light grey: payload
constexpr QRgb kBitOne = qRgb(255, 255, 255);
constexpr QRgb kBitZero = qRgb(0, 0, 0);

}  // namespace

FrameRasterWidget::FrameRasterWidget(QWidget* parent) : PlotWidgetBase(parent) {
    xLabel_ = QStringLiteral("Bit position");
    yLabel_ = QStringLiteral("Frame index");
    baseView_ = QRectF(0.0, 0.0, 1.0, 1.0);
}

void FrameRasterWidget::setData(const sigapp::FrameRaster& raster) {
    if (!raster.detected || raster.frame_length == 0 || raster.num_frames == 0) {
        clearData();
        raster_ = raster;  // keep the (not-detected) raster visible via raster()
        return;
    }

    raster_ = raster;
    const unsigned f = raster.frame_length, r = raster.num_frames;

    // +1 row at the very top for the column-label strip.
    image_ = QImage(static_cast<int>(f), static_cast<int>(r) + 1, QImage::Format_RGB32);
    for (unsigned j = 0; j < f; ++j) {
        const auto label = raster.columns[j].label;
        const QRgb strip = label == sigapp::ColumnLabel::Constant     ? kConstantColor
                          : label == sigapp::ColumnLabel::CounterLike ? kCounterColor
                                                                      : kPayloadColor;
        image_.setPixel(static_cast<int>(j), 0, strip);
    }
    for (unsigned row = 0; row < r; ++row) {
        // Frame index increases upward, so frame (r - 1 - row) is drawn at image row (row + 1).
        const unsigned frame = r - 1 - row;
        const uint8_t* frame_bits = &raster.matrix[static_cast<size_t>(frame) * f];
        for (unsigned j = 0; j < f; ++j) {
            image_.setPixel(static_cast<int>(j), static_cast<int>(row) + 1, frame_bits[j] ? kBitOne : kBitZero);
        }
    }

    // Matches image_'s own pixel size exactly (r+1: the +1 is the label-strip row),
    // so at zoom level 1 this reproduces the original "image fills the plot area"
    // appearance pixel for pixel.
    baseView_ = QRectF(0.0, 0.0, static_cast<double>(f), static_cast<double>(r) + 1.0);
    resetZoom();  // a newly loaded capture starts fully zoomed out, not wherever the last one was left
    update();
}

void FrameRasterWidget::clearData() {
    raster_ = sigapp::FrameRaster{};
    image_ = QImage();
    baseView_ = QRectF(0.0, 0.0, 1.0, 1.0);
    resetZoom();
    update();
}

void FrameRasterWidget::paintData(QPainter& painter) {
    if (image_.isNull()) return;
    // Map the image's own data-space rect (0,0)-(f,r) through the current
    // view/zoom, exactly like WaterfallWidget's paintData, so zooming in
    // actually magnifies the bits instead of just relabelling the axes.
    const QPointF top_left = dataToPixel(0.0, static_cast<double>(image_.height()));
    const QPointF bottom_right = dataToPixel(static_cast<double>(image_.width()), 0.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);  // crisp bit blocks, not blurred
    painter.drawImage(QRectF(top_left, bottom_right), image_);
}

}  // namespace sigui
