#include "main_window.hpp"

#include <QDoubleValidator>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "error_text.hpp"
#include "result_text.hpp"

namespace sigui {

MainWindow::MainWindow(AnalysisBackend* backend, QWidget* parent) : QMainWindow(parent), backend_(backend) {
    setWindowTitle(QStringLiteral("PS 26147 - Signal Analysis Tool"));
    resize(1100, 700);
    buildUi();
    buildToolbar();
    statusBar()->showMessage(QStringLiteral("Open a .wav or .IQ file to begin."));
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* splitter = new QSplitter(Qt::Horizontal);

    auto* plots = new QSplitter(Qt::Vertical);
    waterfall_ = new WaterfallWidget;
    constellation_ = new ConstellationWidget;
    plots->addWidget(waterfall_);
    plots->addWidget(constellation_);

    results_ = new ResultsPanel;

    // The SIH26147 extra-feature panels: a tab strip so each new feature (frame raster,
    // then soft decoding, rank analysis, parameter search) gets its own page without
    // reshaping the rest of the window as they are added.
    feature_tabs_ = new QTabWidget;
    auto* raster_page = new QWidget;
    auto* raster_layout = new QVBoxLayout(raster_page);
    raster_layout->setContentsMargins(0, 0, 0, 0);
    frame_raster_ = new FrameRasterWidget;
    frame_raster_text_ = new QPlainTextEdit;
    frame_raster_text_->setReadOnly(true);
    frame_raster_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    frame_raster_text_->setMaximumHeight(120);
    raster_layout->addWidget(frame_raster_, /*stretch=*/1);
    raster_layout->addWidget(frame_raster_text_);
    feature_tabs_->addTab(raster_page, QStringLiteral("Frame Raster"));

    auto* rank_page = new QWidget;
    auto* rank_layout = new QVBoxLayout(rank_page);
    rank_layout->setContentsMargins(0, 0, 0, 0);
    rank_analysis_ = new RankAnalysisWidget;
    rank_analysis_text_ = new QPlainTextEdit;
    rank_analysis_text_->setReadOnly(true);
    rank_analysis_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    rank_analysis_text_->setMaximumHeight(120);
    rank_layout->addWidget(rank_analysis_, /*stretch=*/1);
    rank_layout->addWidget(rank_analysis_text_);
    feature_tabs_->addTab(rank_page, QStringLiteral("Rank Analysis"));

    splitter->addWidget(plots);
    splitter->addWidget(results_);
    splitter->addWidget(feature_tabs_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 2);
    // Qt sizes an untouched splitter from the panes' size hints (the text panel's is
    // small), which gives roughly 4:1 and wraps the panel's aligned lines; the layout
    // the stretch factors describe is 2:1:2, so start there (proportions survive resizes).
    splitter->setSizes({2000, 1000, 2000});

    layout->addWidget(splitter);
    setCentralWidget(central);
}

void MainWindow::buildToolbar() {
    QToolBar* toolbar = addToolBar(QStringLiteral("Main"));

    open_action_ = new QAction(QStringLiteral("Open File..."), this);
    connect(open_action_, &QAction::triggered, this, [this] { openFile(); });
    toolbar->addAction(open_action_);

    run_action_ = new QAction(QStringLiteral("Run Analysis"), this);
    connect(run_action_, &QAction::triggered, this, [this] { runAnalysis(); });
    run_action_->setEnabled(false);
    toolbar->addAction(run_action_);

    auto_detect_ = new QCheckBox(QStringLiteral("Auto-detect FEC/Interleaving"));
    toolbar->addWidget(auto_detect_);

    // Feature 3 (SIH26147 extra feature): Hard/Soft decode toggle.
    soft_decoding_ = new QCheckBox(QStringLiteral("Soft decoding"));
    toolbar->addWidget(soft_decoding_);

    toolbar->addWidget(new QLabel(QStringLiteral(" Sample rate (Hz, .IQ only): ")));
    sample_rate_ = new QLineEdit;
    sample_rate_->setPlaceholderText(QStringLiteral("default"));
    sample_rate_->setValidator(new QDoubleValidator(0.0, 1e12, 6, sample_rate_));
    sample_rate_->setFixedWidth(90);
    toolbar->addWidget(sample_rate_);

    toolbar->addWidget(new QLabel(QStringLiteral(" Samples/symbol: ")));
    samples_per_symbol_ = new QSpinBox;
    samples_per_symbol_->setRange(1, 64);
    samples_per_symbol_->setValue(1);
    toolbar->addWidget(samples_per_symbol_);
}

void MainWindow::openFile() {
    if (file_picker_) {
        file_picker_();
        return;
    }
    QPointer<MainWindow> self(this);
    QFileDialog::getOpenFileContent(QStringLiteral("Signal captures (*.wav *.iq *.IQ);;All files (*)"),
                                    [self](const QString& name, const QByteArray& content) {
                                        if (self && !name.isEmpty()) self->loadFile(name, content);  // cancelled: empty name
                                    });
}

void MainWindow::loadFile(const QString& filename, const QByteArray& bytes) {
    ++run_id_;  // an analysis still in flight belongs to the previous file
    running_ = false;
    filename_ = filename;
    bytes_ = bytes;
    run_action_->setEnabled(true);
    statusBar()->showMessage(QStringLiteral("Loaded %1. Click 'Run Analysis' to proceed.").arg(filename));
    results_->clearText();
    waterfall_->clearData();
    constellation_->clearData();
    frame_raster_->clearData();
    frame_raster_text_->clear();
    rank_analysis_->clearData();
    rank_analysis_text_->clear();
}

void MainWindow::runAnalysis() {
    if (filename_.isEmpty() || running_) return;

    AnalysisRequest request;
    request.bytes = bytes_;
    request.filename = filename_;

    const QString rate_text = sample_rate_->text().trimmed();
    if (!rate_text.isEmpty()) {
        bool ok = false;
        const double rate = QLocale().toDouble(rate_text, &ok);
        if (ok) request.options.sample_rate = rate;
    }
    request.options.samples_per_symbol = static_cast<unsigned>(samples_per_symbol_->value());
    request.options.auto_detect = auto_detect_->isChecked();
    request.options.soft_decoding = soft_decoding_->isChecked();

    statusBar()->showMessage(QStringLiteral("Running analysis..."));
    running_ = true;
    run_action_->setEnabled(false);
    const quint64 id = ++run_id_;

    QPointer<MainWindow> self(this);
    try {
        backend_->run(request, [self, id](bool ok, std::shared_ptr<const sigapp::PipelineResult> result, const QString& error) {
            if (!self) return;
            // The backend may call back from a JS microtask (browser) or before run()
            // returns (tests): always finish on the next turn of the Qt event loop.
            QMetaObject::invokeMethod(self, [self, id, ok, result, error] {
                if (self) self->finishAnalysis(id, ok, result, error);
            }, Qt::QueuedConnection);
        });
    } catch (const std::exception& e) {
        running_ = false;
        run_action_->setEnabled(true);
        failAnalysis(QString::fromStdString(sigbridge::describe_exception(e)));
    }
}

void MainWindow::finishAnalysis(quint64 run_id, bool ok, std::shared_ptr<const sigapp::PipelineResult> result, const QString& error) {
    if (run_id != run_id_) return;  // a different file was loaded meanwhile
    running_ = false;
    run_action_->setEnabled(true);

    if (!ok || !result) {
        failAnalysis(error);
        return;
    }
    waterfall_->setData(result->spectrogram);
    constellation_->setData(result->samples);
    results_->showResult(*result);
    frame_raster_->setData(result->raster);
    frame_raster_text_->setPlainText(QString::fromStdString(sigapp::format_raster_text(result->raster)));
    rank_analysis_->setData(result->rank_analysis);
    rank_analysis_text_->setPlainText(QString::fromStdString(sigapp::format_rank_analysis_text(result->rank_analysis)));
    statusBar()->showMessage(QString::fromStdString(sigapp::format_done_status(*result)));
}

void MainWindow::failAnalysis(const QString& message) {
    results_->showError(message);
    // Non-modal: a modal exec() cannot block in the browser.
    auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Analysis failed"), message, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
    statusBar()->showMessage(QStringLiteral("Analysis failed."));
}

}  // namespace sigui
