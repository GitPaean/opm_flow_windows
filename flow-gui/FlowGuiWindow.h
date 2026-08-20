/*
  FlowGuiWindow - main window of flow-gui: job queue table with live
  progress, run options, log pane, and (when built with summary support)
  the results plotting tab.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialog;
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;
class QJsonArray;
class QJsonObject;
class QTableWidget;
class QTabWidget;
class QTimer;
class DeckEditorWidget;
class SummaryPlotWidget;
class Viewer3DWidget;
namespace flowgui { class RestartComparePanel; class StructurePanel; }

class FlowGuiWindow : public QMainWindow
{
public:
    FlowGuiWindow();

protected:
    void closeEvent(QCloseEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;

private:
    // one queued/running/finished simulation job
    struct Job {
        QString deck;                 // absolute path to the *.DATA file
        QString outdir;               // resolved output directory (set at start)
        enum State { Queued, Running, Done, Failed, Stopped } state = Queued;
        double  progressDays = 0.0;   // simulated days so far
        double  totalDays    = 0.0;   // total days in the schedule (0 = unknown)
        int     reportStep   = 0;
        int     reportTotal  = 0;
        qint64  elapsedMs    = 0;
        int     exitCode     = 0;
        // part of the batch the current Run press started: "Run selected"
        // marks only the selected rows, so the runner skips the rest.
        bool    inRun        = false;
    };

    // widgets
    QTabWidget*     tabs_        = nullptr;
    QComboBox*      simBox_      = nullptr;   // which flow to run, plus the
                                              // builds used before; empty = auto
    QTableWidget*   jobTable_    = nullptr;
    QSpinBox*       ranksSpin_   = nullptr;
    QSpinBox*       threadsSpin_ = nullptr;
    QComboBox*      outdirMode_  = nullptr;
    QLineEdit*      outdirEdit_  = nullptr;
    QLineEdit*      extraEdit_   = nullptr;
    QCheckBox*      tuningChk_   = nullptr;   // --enable-tuning (off = flow's default)
    QPushButton*    runBtn_      = nullptr;
    QPushButton*    runSelBtn_   = nullptr;   // run only the selected rows
    QPushButton*    stopBtn_     = nullptr;
    QPushButton*    skipBtn_     = nullptr;
    QPlainTextEdit* logView_     = nullptr;
    QSystemTrayIcon* tray_       = nullptr;
    SummaryPlotWidget* summary_  = nullptr;   // null when built without summary
    Viewer3DWidget*    viewer3D_ = nullptr;   // null when built without 3D
    // The restart comparison tab; null when built without summary support.
    flowgui::RestartComparePanel* compare_ = nullptr;
    // The group tree / network view; null when built without the deck model.
    flowgui::StructurePanel* structure_ = nullptr;
    DeckEditorWidget*  deckEd_   = nullptr;

    // run state
    QString       exePath_;       // the flow executable shipped with this GUI
    QProcess*     proc_    = nullptr;
    QProcess*     vproc_   = nullptr;   // one-off deck validation run
    QVector<Job>  jobs_;
    int           current_ = -1;  // index into jobs_ of the running job
    bool          aborted_ = false;
    QElapsedTimer jobTimer_;
    QString       lineBuf_;       // partial last line of process output
    QString       lastFinishedSmspec_;  // for the notification click
    QString       logPend_;       // batched log text (flushed every 100 ms)
    QTimer*       logTimer_ = nullptr;
    QString       projectPath_;   // current .opmproj file ("" = unsaved)
    // Open PRT/DBG viewers by normalised file path, so a second View press
    // raises the window already showing that report rather than cloning it.
    QHash<QString, QDialog*> reportViewers_;

    // helpers
    static QString findFlowExe();
    QString resolveSimulator() const;   // override when set, else auto-detect
    // The simulator box: what it says now, the builds it remembers, and
    // how a build gets in (browsing to one, typing one, or a project).
    QString     currentSimulator() const;
    QStringList simulators() const;
    void setSimulators(const QStringList& list, const QString& current);
    void rememberSimulator(const QString& path);
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& text);
    void setRunning(bool on);
    // Register a case picked in any tab, through whichever list owns naming.
    void openCaseEverywhere(const QString& smspecPath);
    void addDecks(const QStringList& files);
    void refreshRow(int i);
    void parseProgress(const QString& chunk);
    QString jobEta(const Job& j) const;
    void startNextJob();
    void killCurrentTree();
    void stopCurrentJob();      // kill current job AND abort the queue
    void skipCurrentJob();      // kill current job, continue with the next
    void validateSelectedDeck();
    void flushLog();

    // projects (.opmproj)
    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    bool writeProject(const QString& path);
    bool readProject(const QString& path);
    // The state of the tabs themselves (plot, 3D view, deck editor) and of
    // the queue, shared by the project file and the between-sessions
    // settings so both recover the same working setup.
    QJsonObject collectUiState() const;
    void restoreUiState(const QJsonObject& ui);
    QJsonArray  jobsState() const;
    void restoreJobs(const QJsonArray& jobs);
    void updateWindowTitle();
    void openJobFolder(int row);
    void viewJobFile(int row, const QString& ext);   // "PRT" or "DBG"

    // The selected queue rows, or the current one when nothing is selected.
    QList<int> selectedJobRows() const;
    // Put the selected jobs' deck paths - or their output directories - on the
    // clipboard, one per line. The Deck column shows the full path but a table
    // cell is not selectable text, and that path is what people want to paste
    // into a terminal or a bug report.
    void copySelectedJobPaths(bool outputDir);
    void notifyQueueDone(int okCount, int failCount);

    void onAddDecks();
    void onBrowseOutdir();
    void onRun(bool selectedOnly);
    // Offer to save deck-editor changes so the run reads the edited files
    // (flow re-reads the decks from disk on every run). False = user cancelled.
    bool flushDeckEdits();
};
