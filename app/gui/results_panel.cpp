#include "results_panel.hpp"

#include <QFontDatabase>

#include "result_text.hpp"

namespace sigui {

ResultsPanel::ResultsPanel(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));  // the panel is a column-aligned table
}

void ResultsPanel::showResult(const sigapp::PipelineResult& result) {
    setPlainText(QString::fromStdString(sigapp::format_result_text(result, kMaxMessageChars, /*show_soft_decoding_info=*/true)));
}

void ResultsPanel::showError(const QString& message) {
    setPlainText(QStringLiteral("Error: ") + message);
}

}  // namespace sigui
