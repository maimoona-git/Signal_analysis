#include "rank_analysis_widget.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace sigui {

namespace {

const QColor kLineColor(31, 111, 235);       // blue: the profile itself
const QColor kThresholdColor(150, 150, 150);  // grey dashed: the detection floor
const QColor kSpikeColor(200, 40, 40);        // red: the winning width

}  // namespace

RankAnalysisWidget::RankAnalysisWidget(QWidget* parent) : PlotWidgetBase(parent) {
    xLabel_ = QStringLiteral("Candidate codeword width (bits)");
    yLabel_ = QStringLiteral("Excess rank deficiency (bits)");
    grid_ = true;
    view_ = QRectF(0.0, -1.0, 1.0, 2.0);
}

void RankAnalysisWidget::setData(const sigapp::RankAnalysis& analysis) {
    analysis_ = analysis;
    if (analysis_.profile.empty()) {
        clearData();
        return;
    }

    double lo = 0.0, hi = 0.0;
    for (const auto& p : analysis_.profile) {
        lo = std::min(lo, p.excess_deficiency);
        hi = std::max(hi, p.excess_deficiency);
    }
    // A little headroom above/below the data so the line never touches the frame.
    const double pad = std::max(1.0, (hi - lo) * 0.1);
    const double x0 = static_cast<double>(analysis_.profile.front().width);
    const double x1 = static_cast<double>(analysis_.profile.back().width);
    view_ = QRectF(x0, lo - pad, std::max(1.0, x1 - x0), (hi - lo) + 2 * pad);
    update();
}

void RankAnalysisWidget::clearData() {
    analysis_ = sigapp::RankAnalysis{};
    view_ = QRectF(0.0, -1.0, 1.0, 2.0);
    update();
}

void RankAnalysisWidget::paintData(QPainter& painter) {
    if (analysis_.profile.empty()) return;

    // The detection floor: read back from the gap between a flagged spike's excess and
    // its own "would still count" boundary is not available here, so this draws the
    // widely-used default (RankAnalysisOptions::excess_deficiency_threshold) rather than
    // plumbing the option through -- documented on the constant below, not silently
    // assumed to always match a caller's own custom threshold.
    constexpr double kDefaultThreshold = 3.0;
    painter.setPen(QPen(kThresholdColor, 1, Qt::DashLine));
    const QPointF floor_left = dataToPixel(view_.left(), kDefaultThreshold);
    const QPointF floor_right = dataToPixel(view_.right(), kDefaultThreshold);
    painter.drawLine(floor_left, floor_right);

    painter.setPen(QPen(kLineColor, 2));
    QPointF prev;
    bool have_prev = false;
    for (const auto& p : analysis_.profile) {
        const QPointF here = dataToPixel(p.width, p.excess_deficiency);
        if (have_prev) painter.drawLine(prev, here);
        prev = here;
        have_prev = true;
    }

    if (analysis_.detected) {
        const QPointF spike = dataToPixel(analysis_.width, view_.y());
        const QPointF spike_top = dataToPixel(analysis_.width, view_.y() + view_.height());
        painter.setPen(QPen(kSpikeColor, 1, Qt::DashLine));
        painter.drawLine(spike, spike_top);

        for (const auto& p : analysis_.profile) {
            if (p.width == analysis_.width) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(kSpikeColor);
                painter.drawEllipse(dataToPixel(p.width, p.excess_deficiency), 4, 4);
                break;
            }
        }
    }
}

}  // namespace sigui
