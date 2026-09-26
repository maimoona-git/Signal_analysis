#pragma once

#include <QAction>
#include <QCheckBox>
#include <QLineEdit>
#include <QMainWindow>
#include <QSpinBox>

#include <functional>
#include <memory>

#include "analysis_backend.hpp"
#include "constellation_widget.hpp"
#include "results_panel.hpp"
#include "waterfall_widget.hpp"

namespace sigui {

// The application window, laid out like the native app's: a toolbar (open file,
// run analysis, auto-detect, sample rate, samples/symbol) over a splitter with the
// waterfall above the constellation on the left and the results panel on the right.
//
// Differences forced by the browser: there is no file path (the capture arrives
// as bytes plus a filename), and the analysis is asynchronous, so the window
// disables "Run Analysis" while one is in flight.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `backend` must outlive the window.
    explicit MainWindow(AnalysisBackend* backend, QWidget* parent = nullptr);

    // "Open File...": runs the file picker (see setFilePicker) and loads what it yields.
    void openFile();
    // Replaces how "Open File..." asks for a file. The picker must arrange for loadFile()
    // to be called with the chosen file's name and bytes (or do nothing if cancelled).
    // The default is Qt's QFileDialog::getOpenFileContent; the browser app installs an
    // <input type="file"> picker (gui/wasm_file_picker.hpp), which behaves the same in
    // every browser (Qt's own uses the Chromium-only File System Access picker).
    void setFilePicker(std::function<void()> picker) { file_picker_ = std::move(picker); }
    // What opening a file does once its bytes are in hand (also the drag-and-drop path).
    void loadFile(const QString& filename, const QByteArray& bytes);
    // "Run Analysis" on the loaded file, with the toolbar's settings.
    void runAnalysis();

    WaterfallWidget* waterfall() const { return waterfall_; }
    ConstellationWidget* constellation() const { return constellation_; }
    ResultsPanel* resultsPanel() const { return results_; }
    QAction* openAction() const { return open_action_; }
    QAction* runAction() const { return run_action_; }
    QCheckBox* autoDetectCheckBox() const { return auto_detect_; }
    QLineEdit* sampleRateEdit() const { return sample_rate_; }
    QSpinBox* samplesPerSymbolSpinBox() const { return samples_per_symbol_; }
    QString loadedFilename() const { return filename_; }
    bool analysisRunning() const { return running_; }

private:
    void buildUi();
    void buildToolbar();
    void finishAnalysis(quint64 run_id, bool ok, std::shared_ptr<const sigapp::PipelineResult> result, const QString& error);
    void failAnalysis(const QString& message);

    AnalysisBackend* backend_;
    std::function<void()> file_picker_;

    WaterfallWidget* waterfall_ = nullptr;
    ConstellationWidget* constellation_ = nullptr;
    ResultsPanel* results_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* run_action_ = nullptr;
    QCheckBox* auto_detect_ = nullptr;
    QLineEdit* sample_rate_ = nullptr;
    QSpinBox* samples_per_symbol_ = nullptr;

    QString filename_;
    QByteArray bytes_;
    bool running_ = false;
    quint64 run_id_ = 0;  // bumped by every run and every file load, so a stale result is ignored
};

}  // namespace sigui
