#include "main_window.hpp"

#include <QDoubleValidator>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QTableWidgetItem>
#include <QToolBar>
#include <QVBoxLayout>

#include "correlate.hpp"
#include "error_text.hpp"
#include "result_text.hpp"

namespace sigui {

MainWindow::MainWindow(AnalysisBackend* backend, QWidget* parent) : QMainWindow(parent), backend_(backend) {
    setWindowTitle(QStringLiteral("Deconflicted - Signal Analysis Tool"));
    resize(1100, 700);
    setDockNestingEnabled(true);
    buildUi();
    buildToolbar();
    buildViewMenu();
    statusBar()->showMessage(QStringLiteral("Open a .wav or .IQ file to begin."));
}

// Builds a dock widget but does not place it in the layout -- buildUi() does that
// itself, via addDockWidget or splitDockWidget, so that a dock is only ever inserted
// into the layout tree once (inserting it once with addDockWidget and again with
// splitDockWidget left the layout's internal tree confused about which node governed
// each panel's width, and resizeDocks' proportions came out wrong as a result).
QDockWidget* MainWindow::makePanelDock(const QString& title, QWidget* content) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(title);  // so state can be saved/restored by name, not just position
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    dock->setWidget(content);
    return dock;
}

void MainWindow::buildUi() {
    waterfall_ = new WaterfallWidget;
    constellation_ = new ConstellationWidget;
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

    // Feature 1 (SIH26147 extra feature, descoped): decode-driven parameter search.
    // Unlike the tabs above, nothing here runs automatically -- see searchParamsButton's
    // own doc comment on why -- so this tab starts with just a button and fills in once
    // it is pressed.
    auto* search_page = new QWidget;
    auto* search_layout = new QVBoxLayout(search_page);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_params_button_ = new QPushButton(QStringLiteral("Search Parameters"));
    search_params_button_->setEnabled(false);
    connect(search_params_button_, &QPushButton::clicked, this, [this] { runParamSearch(); });
    param_search_table_ = new QTableWidget(0, 4);
    param_search_table_->setHorizontalHeaderLabels(
        {QStringLiteral("Confidence"), QStringLiteral("FEC"), QStringLiteral("Interleaver"), QStringLiteral("Offset")});
    param_search_table_->horizontalHeader()->setStretchLastSection(true);
    param_search_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    param_search_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    param_search_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(param_search_table_, &QTableWidget::currentCellChanged, this,
           [this](int row, int, int, int) { showParamSearchCandidate(row); });
    param_search_text_ = new QPlainTextEdit;
    param_search_text_->setReadOnly(true);
    param_search_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    param_search_text_->setMaximumHeight(120);
    search_layout->addWidget(search_params_button_);
    search_layout->addWidget(param_search_table_, /*stretch=*/1);
    search_layout->addWidget(param_search_text_);
    feature_tabs_->addTab(search_page, QStringLiteral("Parameter Search"));

    // Default arrangement matches the old fixed layout -- waterfall above constellation
    // on the left, results and features on the right -- but every panel here can be
    // dragged to a new spot, floated into its own window, or closed (buildViewMenu()
    // adds a way to bring a closed one back).
    waterfall_dock_ = makePanelDock(QStringLiteral("Waterfall"), waterfall_);
    constellation_dock_ = makePanelDock(QStringLiteral("Constellation"), constellation_);
    results_dock_ = makePanelDock(QStringLiteral("Results"), results_);
    features_dock_ = makePanelDock(QStringLiteral("Features"), feature_tabs_);

    addDockWidget(Qt::LeftDockWidgetArea, waterfall_dock_);
    splitDockWidget(waterfall_dock_, constellation_dock_, Qt::Vertical);
    addDockWidget(Qt::RightDockWidgetArea, results_dock_);
    splitDockWidget(results_dock_, features_dock_, Qt::Horizontal);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (docks_sized_) return;
    docks_sized_ = true;
    // resizeDocks() only sticks once the window has an established layout -- called
    // any earlier (e.g. from buildUi(), during construction), Qt's initial show
    // discards the hint and lays panels out from their size hints instead, which is
    // roughly 4:1 rather than the 2:1 this mirrors from the old QSplitter code's own
    // {2000, 1000, 2000} (proportions that large, not just "2, 1, 2", to dominate
    // each panel's own minimum size hint).
    // Queued, not applied directly here: the window's very first show triggers its own
    // internal layout pass (from each panel's size hint) shortly after showEvent fires,
    // which otherwise wins the race and overwrites a hint set synchronously here.
    QPointer<MainWindow> self(this);
    QMetaObject::invokeMethod(this, [self] {
        if (self) self->applyDockProportions();
    }, Qt::QueuedConnection);
}

void MainWindow::applyDockProportions() {
    resizeDocks({waterfall_dock_, results_dock_, features_dock_}, {2000, 1000, 2000}, Qt::Horizontal);
    resizeDocks({waterfall_dock_, constellation_dock_}, {2000, 1000}, Qt::Vertical);
}

void MainWindow::buildViewMenu() {
    QMenu* view = menuBar()->addMenu(QStringLiteral("View"));
    for (QDockWidget* dock : {waterfall_dock_, constellation_dock_, results_dock_, features_dock_}) {
        view->addAction(dock->toggleViewAction());
    }
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
    last_result_.reset();
    param_search_result_ = sigapp::ParamSearchResult{};
    param_search_table_->setRowCount(0);
    param_search_text_->clear();
    search_params_button_->setEnabled(false);
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
    last_result_ = result;
    param_search_result_ = sigapp::ParamSearchResult{};
    param_search_table_->setRowCount(0);
    param_search_text_->clear();
    search_params_button_->setEnabled(true);
    statusBar()->showMessage(QString::fromStdString(sigapp::format_done_status(*result)));
}

void MainWindow::runParamSearch() {
    if (!last_result_) return;
    statusBar()->showMessage(QStringLiteral("Searching parameters..."));
    // Synchronous: search_params is plain C++ with no ML call-out, but it is real work
    // (many interleaver x FEC combinations, each with its own FEC decode attempt), so
    // this can take a noticeable moment on a large capture -- see param_search.hpp's
    // own performance note. It runs only when this button is pressed, never
    // automatically, for exactly that reason.
    param_search_result_ = sigapp::search_params(last_result_->demodulated_bits);

    const auto& candidates = param_search_result_.candidates;
    param_search_table_->setRowCount(static_cast<int>(candidates.size()));
    for (int row = 0; row < static_cast<int>(candidates.size()); ++row) {
        const auto& c = candidates[static_cast<size_t>(row)];
        param_search_table_->setItem(row, 0, new QTableWidgetItem(QString::number(c.confidence, 'f', 3)));
        param_search_table_->setItem(row, 1, new QTableWidgetItem(c.fec ? QString::fromStdString(*c.fec) : QStringLiteral("none")));
        param_search_table_->setItem(row, 2, new QTableWidgetItem(c.interleaver ? QString::fromStdString(c.interleaver->kind) : QStringLiteral("none")));
        param_search_table_->setItem(row, 3, new QTableWidgetItem(QString::number(c.bit_offset)));
    }
    if (!candidates.empty()) {
        param_search_table_->selectRow(static_cast<int>(param_search_result_.best_index));
    } else {
        param_search_text_->clear();
    }
    statusBar()->showMessage(param_search_result_.detected
                                 ? QStringLiteral("Parameter search: %1 candidates, best confidence %2.")
                                       .arg(candidates.size())
                                       .arg(QString::number(candidates[param_search_result_.best_index].confidence, 'f', 3))
                                 : QStringLiteral("Parameter search: no candidate cleared the confidence floor."));
}

void MainWindow::showParamSearchCandidate(int row) {
    if (!last_result_ || row < 0 || row >= static_cast<int>(param_search_result_.candidates.size())) {
        param_search_text_->clear();
        return;
    }
    const auto& c = param_search_result_.candidates[static_cast<size_t>(row)];
    try {
        const auto decoded = sigapp::decode_candidate(last_result_->demodulated_bits, c);
        param_search_text_->setPlainText(QString::fromStdString(sigapp::bits_to_text(decoded)));
    } catch (const std::exception& e) {
        param_search_text_->setPlainText(QStringLiteral("(could not decode this candidate: %1)")
                                             .arg(QString::fromStdString(sigbridge::describe_exception(e))));
    }
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
