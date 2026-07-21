/*
  FlowGuiWindow - main window of flow-gui-qt: job queue table with live
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
class SummaryPlotWidget;

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
    QTableWidget*   jobTable_    = nullptr;
    QSpinBox*       ranksSpin_   = nullptr;
    QSpinBox*       threadsSpin_ = nullptr;
    QComboBox*      outdirMode_  = nullptr;
    QLineEdit*      outdirEdit_  = nullptr;
    QLineEdit*      extraEdit_   = nullptr;
    QPushButton*    runBtn_      = nullptr;
    QPushButton*    stopBtn_     = nullptr;
    QPlainTextEdit* logView_     = nullptr;
    QSystemTrayIcon* tray_       = nullptr;
    SummaryPlotWidget* summary_  = nullptr;   // null when built without summary

    // run state
    QString       exePath_;       // the flow executable shipped with this GUI
    QProcess*     proc_    = nullptr;
    QVector<Job>  jobs_;
    int           current_ = -1;  // index into jobs_ of the running job
    bool          aborted_ = false;
    QElapsedTimer jobTimer_;
    QString       lineBuf_;       // partial last line of process output

    // helpers
    static QString findFlowExe();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& text);
    void setRunning(bool on);
    void addDecks(const QStringList& files);
    void refreshRow(int i);
    void parseProgress(const QString& chunk);
    QString jobEta(const Job& j) const;
    void startNextJob();
    void stopCurrentJob();
    void openJobFolder(int row);
    void viewJobPrt(int row);
    void notifyQueueDone(int okCount, int failCount);

    void onAddDecks();
    void onBrowseOutdir();
    void onRun();
};
