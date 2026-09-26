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

    // The decoded message is shown up to kMaxMessageChars characters: Qt lays out a
    // wrapped line of millions of characters in minutes (measured: 2 MB, 220 s).
    static constexpr size_t kMaxMessageChars = 10000;

    void showResult(const sigapp::PipelineResult& result);
    void showError(const QString& message);
    void clearText() { setPlainText(QString()); }
};

}  // namespace sigui
