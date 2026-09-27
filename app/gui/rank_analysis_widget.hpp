#pragma once

#include "plot_axes.hpp"
#include "rank_analysis.hpp"

namespace sigui {

// Feature 2 (SIH26147 extra feature): the rank-analysis view. Plots the excess rank
// deficiency (RankProfilePoint::excess_deficiency) against candidate codeword width: a
// flat, near-zero line everywhere except a spike at the true codeword length, if the
// capture actually carries a linear block code the analysis can see (see
// rank_analysis.hpp's "honest limits"). A dashed horizontal line marks the detection
// floor (RankAnalysisOptions::excess_deficiency_threshold); the winning width, if any,
// is marked with a filled dot and its own vertical guide line.
class RankAnalysisWidget : public PlotWidgetBase {
    Q_OBJECT
public:
    explicit RankAnalysisWidget(QWidget* parent = nullptr);

    void setData(const sigapp::RankAnalysis& analysis);
    void clearData();

    bool hasData() const { return analysis_.detected; }
    const sigapp::RankAnalysis& analysis() const { return analysis_; }

protected:
    void paintData(QPainter& painter) override;

private:
    sigapp::RankAnalysis analysis_;
};

}  // namespace sigui
