#include "main_window.hpp"

#include <QDoubleValidator>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

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

    splitter->addWidget(plots);
    splitter->addWidget(results_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    // Qt sizes an untouched splitter from the panes' size hints (the text panel's is
    // small), which gives roughly 4:1 and wraps the panel's aligned lines; the layout
    // the stretch factors describe is 2:1, so start there (proportions survive resizes).
    splitter->setSizes({2000, 1000});

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
        failAnalysis(QString::fromUtf8(e.what()));
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
