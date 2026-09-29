#pragma once

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>

#include <functional>
#include <memory>
#include <optional>

#include "analysis_backend.hpp"
#include "constellation_widget.hpp"
#include "frame_raster_widget.hpp"
#include "param_search.hpp"
#include "rank_analysis_widget.hpp"
#include "results_panel.hpp"
#include "waterfall_widget.hpp"

namespace sigui {

// The application window: a toolbar (open file, run analysis, auto-detect, sample
// rate, samples/symbol) over four dockable panels -- Waterfall, Constellation,
// Results and Features (the frame raster / rank analysis / parameter search tabs)
// -- each of which can be dragged, floated into its own window, or closed, the way
// SDR#/GQRX/Wireshark arrange their analysis panels. A View menu brings closed
// panels back. The default arrangement matches the old fixed layout: waterfall
// above constellation on the left, results and features on the right.
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
    // The SIH26147 extra-feature panels, in a tab strip to the right of the results panel
    // (Feature 4, frame raster, is the first; Features 3/2/1 add further tabs).
    QTabWidget* featureTabs() const { return feature_tabs_; }
    FrameRasterWidget* frameRaster() const { return frame_raster_; }
    QPlainTextEdit* frameRasterText() const { return frame_raster_text_; }
    // Feature 2 (SIH26147 extra feature): the rank-analysis view's tab.
    RankAnalysisWidget* rankAnalysis() const { return rank_analysis_; }
    QPlainTextEdit* rankAnalysisText() const { return rank_analysis_text_; }
    // Feature 1 (SIH26147 extra feature, descoped): the decode-driven parameter
    // search's tab. Unlike Features 2 and 4, this does not run automatically after
    // every analysis (it is real extra work -- many interleaver x FEC combinations,
    // each with its own FEC decode attempt -- and is only useful when auto-detect's
    // single guess is wrong): the button runs it on demand, on the last analysis'
    // demodulated_bits.
    QPushButton* searchParamsButton() const { return search_params_button_; }
    QTableWidget* paramSearchTable() const { return param_search_table_; }
    QPlainTextEdit* paramSearchText() const { return param_search_text_; }
    QAction* openAction() const { return open_action_; }
    QAction* runAction() const { return run_action_; }
    QCheckBox* autoDetectCheckBox() const { return auto_detect_; }
    // Feature 3 (SIH26147 extra feature): the Hard/Soft decode toggle.
    QCheckBox* softDecodingCheckBox() const { return soft_decoding_; }
    // Real-world signal support: matched-filter timing recovery + carrier tracking.
    QCheckBox* recoverTimingCheckBox() const { return recover_timing_; }
    // Real-world signal support: which transmit pulse shape recoverTimingCheckBox
    // should assume ("rrc" or "rect" -- see QComboBox::currentData()). Only matters
    // when recover timing is on.
    QComboBox* pulseShapeComboBox() const { return pulse_shape_; }
    // Real-world signal support: blind wideband channel detection -- see
    // sigcore::find_channels's own comment. Unlike recoverTiming/pulseShape (which
    // only change how demodulation runs), turning this on and picking a channel
    // replaces what the whole rest of the analysis sees (spectrogram, symbol-rate
    // suggestion, demodulation) with that one extracted channel -- so, like Feature
    // 1's Parameter Search, this is a real, explicit two-step: run once to see
    // candidates, select one, then "Extract Selected Channel" to actually apply it.
    QCheckBox* findChannelsCheckBox() const { return find_channels_; }
    QTableWidget* channelTable() const { return channel_table_; }
    QPushButton* extractChannelButton() const { return extract_channel_button_; }
    // Squares the capture before computing the waterfall (not the demodulated bits --
    // display only): collapses an M-fold symmetric modulation down to reveal an
    // otherwise-invisible carrier tone, the same M=2 trick recover_linear's own coarse
    // carrier-offset estimate uses internally (core/include/recover.hpp) -- this lets
    // a user see why/whether that estimate should find anything, not just trust it did.
    QCheckBox* squareSignalCheckBox() const { return square_signal_; }
    // The waterfall's colour-range controls: two sliders (dB), a live-value label, and
    // a button back to full auto-range. Disabled (and reset to auto) whenever the
    // waterfall has no data.
    QSlider* colorRangeMinSlider() const { return color_min_slider_; }
    QSlider* colorRangeMaxSlider() const { return color_max_slider_; }
    QPushButton* colorRangeAutoButton() const { return color_range_auto_button_; }
    QLineEdit* sampleRateEdit() const { return sample_rate_; }
    QSpinBox* samplesPerSymbolSpinBox() const { return samples_per_symbol_; }
    // Real-world signal support: the blind samples/symbol suggestion label next to the
    // spin box above, filled in after an analysis finishes -- empty text until then.
    QLabel* symbolRateSuggestionLabel() const { return symbol_rate_suggestion_; }
    // Real-world signal support: how a raw .IQ/.sigmf-data file's samples are stored on
    // disk ("cf32", "ci16" or "cu8" -- see QComboBox::currentData()).
    QComboBox* iqFormatComboBox() const { return iq_format_; }
    QString loadedFilename() const { return filename_; }
    bool analysisRunning() const { return running_; }

    // The four dockable panels, for tests and for the View menu's toggle actions.
    QDockWidget* waterfallDock() const { return waterfall_dock_; }
    QDockWidget* constellationDock() const { return constellation_dock_; }
    QDockWidget* resultsDock() const { return results_dock_; }
    QDockWidget* featuresDock() const { return features_dock_; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void buildToolbar();
    void buildViewMenu();
    void applyDockProportions();
    QDockWidget* makePanelDock(const QString& title, QWidget* content);
    void finishAnalysis(quint64 run_id, bool ok, std::shared_ptr<const sigapp::PipelineResult> result, const QString& error);
    void failAnalysis(const QString& message);
    void runParamSearch();
    void showParamSearchCandidate(int row);
    // Applies the currently-selected row in channel_table_ (real-world signal
    // support): re-runs analysis with that candidate's index as select_channel.
    void extractSelectedChannel();
    // Recomputes the waterfall from last_result_ -- squared or not, per
    // squareSignalCheckBox() -- then resets the colour-range controls to match (a
    // fresh WaterfallWidget::setData() always reverts to auto-range; see its comment).
    void applySquareSignal();
    // Syncs the colour-range sliders/label to the waterfall's current data (auto
    // range), and enables/disables them with whether it has data at all.
    void resetColorRangeControls();
    void updateColorRangeFromSliders();

    AnalysisBackend* backend_;
    std::function<void()> file_picker_;

    WaterfallWidget* waterfall_ = nullptr;
    ConstellationWidget* constellation_ = nullptr;
    ResultsPanel* results_ = nullptr;
    QTabWidget* feature_tabs_ = nullptr;
    QDockWidget* waterfall_dock_ = nullptr;
    QDockWidget* constellation_dock_ = nullptr;
    QDockWidget* results_dock_ = nullptr;
    QDockWidget* features_dock_ = nullptr;
    FrameRasterWidget* frame_raster_ = nullptr;
    QPlainTextEdit* frame_raster_text_ = nullptr;
    RankAnalysisWidget* rank_analysis_ = nullptr;
    QPlainTextEdit* rank_analysis_text_ = nullptr;
    QPushButton* search_params_button_ = nullptr;
    QTableWidget* param_search_table_ = nullptr;
    QPlainTextEdit* param_search_text_ = nullptr;
    sigapp::ParamSearchResult param_search_result_;      // the last search, for row selection
    std::shared_ptr<const sigapp::PipelineResult> last_result_;  // for "Search Parameters"; see its own comment
    QAction* open_action_ = nullptr;
    QAction* run_action_ = nullptr;
    QCheckBox* auto_detect_ = nullptr;
    QCheckBox* soft_decoding_ = nullptr;
    QCheckBox* recover_timing_ = nullptr;
    QComboBox* pulse_shape_ = nullptr;
    QCheckBox* find_channels_ = nullptr;
    QTableWidget* channel_table_ = nullptr;
    QPushButton* extract_channel_button_ = nullptr;
    std::optional<size_t> selected_channel_;  // persists across re-runs of the same file
    QCheckBox* square_signal_ = nullptr;
    QSlider* color_min_slider_ = nullptr;
    QSlider* color_max_slider_ = nullptr;
    QLabel* color_range_label_ = nullptr;
    QPushButton* color_range_auto_button_ = nullptr;
    QLineEdit* sample_rate_ = nullptr;
    QSpinBox* samples_per_symbol_ = nullptr;
    QLabel* symbol_rate_suggestion_ = nullptr;
    QComboBox* iq_format_ = nullptr;

    QString filename_;
    QByteArray bytes_;
    bool running_ = false;
    quint64 run_id_ = 0;  // bumped by every run and every file load, so a stale result is ignored
    bool docks_sized_ = false;  // resizeDocks() applied once, on the window's first showEvent -- see its comment
};

}  // namespace sigui
