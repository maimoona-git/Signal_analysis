#include "main_window.hpp"

#include <QDoubleValidator>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
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

#include <cmath>
#include <optional>

#include "correlate.hpp"
#include "error_text.hpp"
#include "result_text.hpp"

namespace sigui {

MainWindow::MainWindow(AnalysisBackend* backend, QWidget* parent) : QMainWindow(parent), backend_(backend) {
    setWindowTitle(QStringLiteral("Deconflicted - Signal Analysis Tool"));
    resize(1300, 700);
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

    // Real-world signal support: blind wideband channel detection. Like Parameter
    // Search above, an explicit two-step -- "Find channels" (toolbar) populates this
    // table on the next "Run Analysis", picking a row and pressing the button here
    // re-runs analysis with that channel extracted and applied.
    auto* channels_page = new QWidget;
    auto* channels_layout = new QVBoxLayout(channels_page);
    channels_layout->setContentsMargins(0, 0, 0, 0);
    channel_table_ = new QTableWidget(0, 3);
    channel_table_->setHorizontalHeaderLabels(
        {QStringLiteral("Center Freq (Hz)"), QStringLiteral("Bandwidth (Hz)"), QStringLiteral("Confidence")});
    channel_table_->horizontalHeader()->setStretchLastSection(true);
    channel_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    channel_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    channel_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    extract_channel_button_ = new QPushButton(QStringLiteral("Extract Selected Channel"));
    extract_channel_button_->setEnabled(false);
    connect(channel_table_, &QTableWidget::currentCellChanged, this,
           [this](int row, int, int, int) { extract_channel_button_->setEnabled(row >= 0); });
    connect(extract_channel_button_, &QPushButton::clicked, this, [this] { extractSelectedChannel(); });
    channels_layout->addWidget(channel_table_, /*stretch=*/1);
    channels_layout->addWidget(extract_channel_button_);
    feature_tabs_->addTab(channels_page, QStringLiteral("Channels"));

    // The waterfall's own colour-range controls (two dB sliders, a live-value label,
    // and a button back to auto-range) sit in a thin strip above the plot itself, so
    // they travel with it if the panel is dragged out or floated.
    auto* waterfall_container = new QWidget;
    auto* waterfall_layout = new QVBoxLayout(waterfall_container);
    waterfall_layout->setContentsMargins(0, 0, 0, 0);
    waterfall_layout->setSpacing(2);

    auto* range_row = new QWidget;
    auto* range_layout = new QHBoxLayout(range_row);
    range_layout->setContentsMargins(4, 2, 4, 2);
    // A short caption, not the original "Colour range (dB):" -- a fixed-text label
    // can't shrink the way the sliders can, and that longer wording was measurably
    // pushing this dock past its intended ~2:1 share of a freely user-resizable window
    // (see MainWindow's own class comment); this one is short enough not to repeat
    // that. The full wording is still in the tooltip for anyone who wants it spelled out.
    auto* range_caption = new QLabel(QStringLiteral("dB:"));
    range_caption->setToolTip(QStringLiteral("Colour range (dB): drag to change contrast, Auto to reset"));
    range_row->setToolTip(range_caption->toolTip());
    range_layout->addWidget(range_caption);
    color_min_slider_ = new QSlider(Qt::Horizontal);
    color_max_slider_ = new QSlider(Qt::Horizontal);
    color_min_slider_->setMinimumWidth(30);
    color_max_slider_->setMinimumWidth(30);
    color_range_label_ = new QLabel;
    color_range_label_->setMinimumWidth(0);
    color_range_auto_button_ = new QPushButton(QStringLiteral("Auto"));
    connect(color_min_slider_, &QSlider::valueChanged, this, [this] { updateColorRangeFromSliders(); });
    connect(color_max_slider_, &QSlider::valueChanged, this, [this] { updateColorRangeFromSliders(); });
    connect(color_range_auto_button_, &QPushButton::clicked, this, [this] {
        if (waterfall_->hasData()) waterfall_->setColorRange(std::nullopt, std::nullopt);
        resetColorRangeControls();
    });
    range_layout->addWidget(color_min_slider_, /*stretch=*/1);
    range_layout->addWidget(color_max_slider_, /*stretch=*/1);
    range_layout->addWidget(color_range_label_);
    range_layout->addWidget(color_range_auto_button_);

    waterfall_layout->addWidget(range_row);
    waterfall_layout->addWidget(waterfall_, /*stretch=*/1);
    resetColorRangeControls();  // starts with no data: sliders disabled, label blank

    // Default arrangement matches the old fixed layout -- waterfall above constellation
    // on the left, results and features on the right -- but every panel here can be
    // dragged to a new spot, floated into its own window, or closed (buildViewMenu()
    // adds a way to bring a closed one back).
    waterfall_dock_ = makePanelDock(QStringLiteral("Waterfall"), waterfall_container);
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

    toolbar->addWidget(new QLabel(QStringLiteral(" Sample rate (Hz, .IQ only): ")));
    sample_rate_ = new QLineEdit;
    sample_rate_->setPlaceholderText(QStringLiteral("default"));
    sample_rate_->setValidator(new QDoubleValidator(0.0, 1e12, 6, sample_rate_));
    sample_rate_->setFixedWidth(90);
    toolbar->addWidget(sample_rate_);

    // Real-world signal support: how a raw .IQ/.sigmf-data file's samples are stored on
    // disk -- ignored for .wav. Cf32 (this project's own convention, and the common
    // case for a SigMF .sigmf-data file with no conversion needed) stays the default,
    // so every existing capture opens exactly as before unless this is changed.
    // See sigcore::IqSampleFormat's own comment for what Ci16/Cu8 are and which real
    // hardware actually produces them.
    toolbar->addWidget(new QLabel(QStringLiteral(" IQ format: ")));
    iq_format_ = new QComboBox;
    iq_format_->addItem(QStringLiteral("cf32 (float32)"), QStringLiteral("cf32"));
    iq_format_->addItem(QStringLiteral("ci16 (int16, e.g. USRP)"), QStringLiteral("ci16"));
    iq_format_->addItem(QStringLiteral("cu8 (uint8, e.g. RTL-SDR)"), QStringLiteral("cu8"));
    iq_format_->setToolTip(QStringLiteral("How a raw .IQ or .sigmf-data file's samples are stored on disk."));
    toolbar->addWidget(iq_format_);

    toolbar->addWidget(new QLabel(QStringLiteral(" Samples/symbol: ")));
    samples_per_symbol_ = new QSpinBox;
    samples_per_symbol_->setRange(1, 64);
    samples_per_symbol_->setValue(1);
    toolbar->addWidget(samples_per_symbol_);

    // Real-world signal support: a blind suggestion for the field above, filled in
    // once an analysis finishes (see PipelineResult::symbol_rate_estimate's own
    // comment) -- advisory only, never applied automatically, since the field stays
    // a required, exact input for demodulation. Empty until then, and cleared again
    // on the next file load.
    symbol_rate_suggestion_ = new QLabel;
    symbol_rate_suggestion_->setToolTip(
        QStringLiteral("A blind estimate of samples/symbol from the raw capture alone, for reference -- "
                       "not applied automatically."));
    toolbar->addWidget(symbol_rate_suggestion_);

    // A second row: decoding-mode toggles. Split out from the row above (rather than
    // one long toolbar) because that row alone already matches the original app's
    // width, and each toggle added here since has made a single row overflow a
    // realistic browser window -- clipping "Sample rate"/"Samples/symbol" off the
    // visible edge entirely, found from a real (repeated) browser-test failure, not
    // guessed at.
    addToolBarBreak();
    QToolBar* mode_toolbar = addToolBar(QStringLiteral("Decoding mode"));

    // Feature 3 (SIH26147 extra feature): Hard/Soft decode toggle.
    soft_decoding_ = new QCheckBox(QStringLiteral("Soft decoding"));
    mode_toolbar->addWidget(soft_decoding_);

    // Real-world signal support: matched-filter timing recovery + carrier tracking,
    // for a linear scheme with a real (unknown) timing offset and carrier error --
    // off by default since it changes what gets decoded (see PipelineOptions::
    // recover_timing's own comment) and is only useful for samples_per_symbol > 1.
    recover_timing_ = new QCheckBox(QStringLiteral("Recover timing/carrier"));
    recover_timing_->setToolTip(
        QStringLiteral("For a real capture (not one built for this app): matched-filter timing "
                       "recovery and carrier tracking instead of naive decimation."));
    mode_toolbar->addWidget(recover_timing_);

    // Which transmit pulse shape recover_timing should assume -- only matters when
    // that checkbox is on. See core/include/recover.hpp's own comment on PulseShape
    // for why "rrc" and "rect" are not interchangeable: the wrong one measurably
    // hurts the lock on a real capture, not just leaves it unimproved.
    pulse_shape_ = new QComboBox;
    pulse_shape_->addItem(QStringLiteral("RRC pulse shape"), QStringLiteral("rrc"));
    pulse_shape_->addItem(QStringLiteral("Rect (unshaped) pulse"), QStringLiteral("rect"));
    pulse_shape_->setToolTip(
        QStringLiteral("Which transmit pulse shape 'Recover timing/carrier' should assume. RRC is "
                       "the right choice for a real transmitter that pulse-shapes its output; Rect "
                       "assumes no shaping at all (simple NRZ/on-off pulses)."));
    mode_toolbar->addWidget(pulse_shape_);

    // Real-world signal support: blind wideband channel detection. Off by default,
    // like recover_timing -- a real, separate mode, not attempted on every capture.
    // See the "Channels" feature tab for picking and applying a found candidate.
    find_channels_ = new QCheckBox(QStringLiteral("Find channels"));
    find_channels_->setToolTip(
        QStringLiteral("For a wideband capture where the signal of interest is not at baseband: "
                       "blind-detect candidate channels (see the Channels tab) before anything "
                       "else runs."));
    mode_toolbar->addWidget(find_channels_);

    // Squares the capture before computing the waterfall -- display only, does not touch
    // the demodulated bits. See squareSignalCheckBox()'s own comment.
    square_signal_ = new QCheckBox(QStringLiteral("Square signal"));
    square_signal_->setToolTip(
        QStringLiteral("Waterfall only: squares the capture, which collapses an M-fold symmetric "
                       "modulation down to a single tone at any residual carrier offset -- the "
                       "same trick the real-world recovery above uses to find one."));
    connect(square_signal_, &QCheckBox::toggled, this, [this] { applySquareSignal(); });
    mode_toolbar->addWidget(square_signal_);
}

void MainWindow::openFile() {
    if (file_picker_) {
        file_picker_();
        return;
    }
    QPointer<MainWindow> self(this);
    QFileDialog::getOpenFileContent(
        QStringLiteral("Signal captures (*.wav *.iq *.IQ *.sigmf-data);;All files (*)"),
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
    resetColorRangeControls();
    constellation_->clearData();
    frame_raster_->clearData();
    frame_raster_text_->clear();
    rank_analysis_->clearData();
    rank_analysis_text_->clear();
    symbol_rate_suggestion_->clear();
    last_result_.reset();
    param_search_result_ = sigapp::ParamSearchResult{};
    param_search_table_->setRowCount(0);
    param_search_text_->clear();
    search_params_button_->setEnabled(false);
    selected_channel_.reset();
    channel_table_->setRowCount(0);
    extract_channel_button_->setEnabled(false);
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
    const QString iq_format = iq_format_->currentData().toString();
    if (iq_format == QStringLiteral("ci16")) request.options.iq_format = sigcore::IqSampleFormat::Ci16;
    else if (iq_format == QStringLiteral("cu8")) request.options.iq_format = sigcore::IqSampleFormat::Cu8;
    else request.options.iq_format = sigcore::IqSampleFormat::Cf32;
    request.options.samples_per_symbol = static_cast<unsigned>(samples_per_symbol_->value());
    request.options.auto_detect = auto_detect_->isChecked();
    request.options.soft_decoding = soft_decoding_->isChecked();
    request.options.recover_timing = recover_timing_->isChecked();
    request.options.pulse_shape = (pulse_shape_->currentData().toString() == QStringLiteral("rect"))
        ? sigcore::PulseShape::Rect : sigcore::PulseShape::Rrc;
    request.options.find_channels_enabled = find_channels_->isChecked();
    if (selected_channel_) request.options.select_channel = *selected_channel_;

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
    last_result_ = result;
    applySquareSignal();  // sets the waterfall from result->spectrogram (or its squared version) and resets the range controls
    constellation_->setData(result->samples);
    results_->showResult(*result);
    frame_raster_->setData(result->raster);
    frame_raster_text_->setPlainText(QString::fromStdString(sigapp::format_raster_text(result->raster)));
    rank_analysis_->setData(result->rank_analysis);
    rank_analysis_text_->setPlainText(QString::fromStdString(sigapp::format_rank_analysis_text(result->rank_analysis)));
    if (result->symbol_rate_estimate) {
        symbol_rate_suggestion_->setText(
            QStringLiteral(" (suggested: %1, confidence %2%)")
                .arg(result->symbol_rate_estimate->samples_per_symbol, 0, 'f', 1)
                .arg(static_cast<int>(result->symbol_rate_estimate->confidence * 100.0 + 0.5)));
    } else {
        symbol_rate_suggestion_->clear();
    }
    // Real-world signal support: repopulate the Channels tab from this result's own
    // candidates (empty when find_channels_ was off). Re-selecting the applied row
    // (when channel_extracted) is purely a visual "this is what you're looking at"
    // cue -- it does not itself trigger another run (extractSelectedChannel only runs
    // from the button, not from a selection change).
    {
        const auto& channels = result->channel_candidates;
        channel_table_->setRowCount(static_cast<int>(channels.size()));
        for (int row = 0; row < static_cast<int>(channels.size()); ++row) {
            const auto& c = channels[static_cast<size_t>(row)];
            channel_table_->setItem(row, 0, new QTableWidgetItem(QString::number(c.center_freq_hz, 'f', 1)));
            channel_table_->setItem(row, 1, new QTableWidgetItem(QString::number(c.bandwidth_hz, 'f', 1)));
            channel_table_->setItem(row, 2, new QTableWidgetItem(QString::number(c.confidence, 'f', 3)));
        }
        if (result->channel_extracted && selected_channel_ && *selected_channel_ < channels.size()) {
            channel_table_->selectRow(static_cast<int>(*selected_channel_));
        }
    }
    param_search_result_ = sigapp::ParamSearchResult{};
    param_search_table_->setRowCount(0);
    param_search_text_->clear();
    search_params_button_->setEnabled(true);
    statusBar()->showMessage(QString::fromStdString(sigapp::format_done_status(*result)));
}

void MainWindow::applySquareSignal() {
    if (!last_result_) return;
    if (square_signal_->isChecked()) {
        std::vector<std::complex<float>> squared(last_result_->samples.size());
        for (size_t i = 0; i < squared.size(); ++i) squared[i] = last_result_->samples[i] * last_result_->samples[i];
        // Same nfft as the original spectrogram (generate_spectrogram's default hop),
        // so the squared view is at the same time/frequency resolution, not a
        // surprising change alongside the surprising content.
        const auto spectrogram =
            sigapp::generate_spectrogram(squared, last_result_->sample_rate, last_result_->spectrogram.nfft);
        waterfall_->setData(spectrogram);
    } else {
        waterfall_->setData(last_result_->spectrogram);
    }
    resetColorRangeControls();  // setData() always reverts to auto-range; keep the UI in sync
}

void MainWindow::resetColorRangeControls() {
    const bool has_data = waterfall_->hasData();
    color_min_slider_->setEnabled(has_data);
    color_max_slider_->setEnabled(has_data);
    color_range_auto_button_->setEnabled(has_data);
    if (!has_data) {
        color_range_label_->setText(QStringLiteral("no data"));
        return;
    }
    // Sliders are whole dB; dataMin_/dataMax_ padded outward so both ends stay reachable
    // (a slider's own value can't land strictly beyond its range).
    const int lo = static_cast<int>(std::floor(waterfall_->dataMin())) - 1;
    const int hi = static_cast<int>(std::ceil(waterfall_->dataMax())) + 1;
    const QSignalBlocker block_min(color_min_slider_), block_max(color_max_slider_);
    color_min_slider_->setRange(lo, hi);
    color_max_slider_->setRange(lo, hi);
    color_min_slider_->setValue(lo);
    color_max_slider_->setValue(hi);
    color_range_label_->setText(QStringLiteral("%1 .. %2 dB (auto)").arg(lo).arg(hi));
}

void MainWindow::updateColorRangeFromSliders() {
    if (!waterfall_->hasData()) return;
    const int lo = color_min_slider_->value();
    const int hi = color_max_slider_->value();
    if (lo >= hi) return;  // dragging through each other mid-gesture; WaterfallWidget rejects this anyway
    waterfall_->setColorRange(static_cast<double>(lo), static_cast<double>(hi));
    color_range_label_->setText(QStringLiteral("%1 .. %2 dB").arg(lo).arg(hi));
}

void MainWindow::extractSelectedChannel() {
    const int row = channel_table_->currentRow();
    if (row < 0 || running_ || filename_.isEmpty()) return;
    // Unlike showParamSearchCandidate (a pure local recompute from bits already in
    // hand), applying a channel changes what the raw capture itself feeds into
    // ingest/demod -- see PipelineOptions::select_channel's own comment -- so this
    // has to re-run the whole analysis, not just redraw from last_result_.
    selected_channel_ = static_cast<size_t>(row);
    runAnalysis();
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
