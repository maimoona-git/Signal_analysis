#pragma once

#include <QPlainTextEdit>
#include <QString>

#include "pipeline.hpp"

namespace sigui {

// Read-only text panel with the identified signal parameters and correlation
// output, formatted exactly like the native app's (see sigapp::format_result_text).
class ResultsPanel : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ResultsPanel(QWidget* parent = nullptr);

    void showResult(const sigapp::PipelineResult& result);
    void showError(const QString& message);
    void clearText() { setPlainText(QString()); }
};

}  // namespace sigui
