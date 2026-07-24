/*
  FlowGuiWindow - main window of flow-gui: job queue table with live
  progress, run options, log pane, and (when built with summary support)
  the results plotting tab.

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QSystemTrayIcon;
class QTableWidget;
class QTabWidget;
class QTimer;
class DeckEditorWidget;
class SummaryPlotWidget;
class Viewer3DWidget;

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
    };

    // widgets
    QTabWidget*     tabs_        = nullptr;
    QLineEdit*      simEdit_     = nullptr;   // developer override; empty = auto
    QTableWidget*   jobTable_    = nullptr;
    QSpinBox*       ranksSpin_   = nullptr;
    QSpinBox*       threadsSpin_ = nullptr;
    QComboBox*      outdirMode_  = nullptr;
    QLineEdit*      outdirEdit_  = nullptr;
    QLineEdit*      extraEdit_   = nullptr;
    QPushButton*    runBtn_      = nullptr;
    QPushButton*    stopBtn_     = nullptr;
    QPushButton*    skipBtn_     = nullptr;
    QPlainTextEdit* logView_     = nullptr;
    QSystemTrayIcon* tray_       = nullptr;
    SummaryPlotWidget* summary_  = nullptr;   // null when built without summary
    Viewer3DWidget*    viewer3D_ = nullptr;   // null when built without 3D
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

    // helpers
    static QString findFlowExe();
    QString resolveSimulator() const;   // override when set, else auto-detect
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& text);
    void setRunning(bool on);
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
    void updateWindowTitle();
    void openJobFolder(int row);
    void viewJobFile(int row, const QString& ext);   // "PRT" or "DBG"
    void notifyQueueDone(int okCount, int failCount);

    void onAddDecks();
    void onBrowseOutdir();
    void onRun();
};
