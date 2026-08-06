/*
  Copyright (C) 2026 SINTEF Digital

  FlowGuiWindow implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "FlowGuiWindow.h"
#ifdef FLOWGUI_HAVE_SUMMARY
  #include "SummaryPlotWidget.h"
#endif
#ifdef FLOWGUI_HAVE_3D
  #include "Viewer3D.h"
#endif
#include "DeckEditor.h"
#include "GuiPaths.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSaveFile>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#if !defined(Q_OS_WIN)
  #include <signal.h>
  #include <unistd.h>
#else
  #include <windows.h>
#endif

// Exempt a spawned simulator process from Windows power throttling
// ("EcoQoS"): Windows clocks background processes down substantially on
// modern laptops (a serial Norne measured 1.6x slower than the same binary
// with the exemption). This reaches the direct child (serial flow, or
// mpiexec); newer flow builds also request the exemption themselves, which
// covers the smpd-spawned MPI ranks. No-op on other platforms.
static void exemptFromPowerThrottling(qint64 pid)
{
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, DWORD(pid));
    if (!h) return;
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = 0;   // never throttle this process
    SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof(s));
    CloseHandle(h);
#else
    Q_UNUSED(pid);
#endif
}

static const char* kAppName = "flow-gui";
// First entry of the simulator box: the empty choice, spelled out.
static const char* kShippedSimulator = "(the flow shipped with the GUI)";
static const char* kVersion = FLOWGUI_VERSION;

namespace {
enum Col { ColDeck = 0, ColStatus, ColProgress, ColElapsed, ColEta, ColCount };

QString fmtDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

// The session state is the same JSON the project file carries, stored in the
// settings as text: readable with a text editor, and one key per section
// instead of a key per control.
template <class T> QString settingFromJson(const T& j)
{
    return QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact));
}

QJsonObject jsonFromSetting(const QVariant& v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).object();
}

QJsonArray jsonArrayFromSetting(const QVariant& v)
{
    return QJsonDocument::fromJson(v.toString().toUtf8()).array();
}
} // namespace

// What the simulator box says, with the "shipped with the GUI" entry read as
// the empty choice it stands for.
QString FlowGuiWindow::currentSimulator() const
{
    if (!simBox_) return QString();
    const QString text = simBox_->currentText().trimmed();
    return text == QLatin1String(kShippedSimulator) ? QString() : text;
}

// The builds used before, most recent first (the "shipped" entry is not one).
QStringList FlowGuiWindow::simulators() const
{
    QStringList list;
    if (!simBox_) return list;
    for (int i = 0; i < simBox_->count(); ++i) {
        const QString p = simBox_->itemText(i).trimmed();
        if (!p.isEmpty() && p != QLatin1String(kShippedSimulator)) list << p;
    }
    return list;
}

void FlowGuiWindow::setSimulators(const QStringList& list, const QString& current)
{
    if (!simBox_) return;
    const QSignalBlocker block(simBox_);
    simBox_->clear();
    simBox_->addItem(QLatin1String(kShippedSimulator));
    for (const QString& p : list)
        if (!p.trimmed().isEmpty()) simBox_->addItem(QDir::toNativeSeparators(p));
    if (current.trimmed().isEmpty()) simBox_->setCurrentIndex(0);
    else                             simBox_->setCurrentText(QDir::toNativeSeparators(current));
}

// Put a build at the top of the list, so the two or three being compared stay
// within reach however long the session runs.
void FlowGuiWindow::rememberSimulator(const QString& path)
{
    const QString p = QDir::toNativeSeparators(path.trimmed());
    if (p.isEmpty() || p == QLatin1String(kShippedSimulator)) return;
    QStringList list = simulators();
    list.removeAll(p);
    list.prepend(p);
    while (list.size() > 10) list.removeLast();
    setSimulators(list, p);
}

QString FlowGuiWindow::resolveSimulator() const
{
    const QString custom = currentSimulator();
    if (!custom.isEmpty()) return custom;
    return findFlowExe();
}

QString FlowGuiWindow::findFlowExe()
{
#ifdef Q_OS_WIN
    const QString exeName = QStringLiteral("flow.exe");
#else
    const QString exeName = QStringLiteral("flow");
#endif
    const char* candidates[] = {
        ".", "build-mpi/opm-simulators/bin", "build/opm-simulators/bin"
    };
    QDir base(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 4; ++up) {
        for (const char* c : candidates) {
            const QString p = base.filePath(QString::fromLatin1(c) + '/' + exeName);
            if (QFileInfo::exists(p)) return QDir::cleanPath(p);
        }
        if (!base.cdUp()) break;
    }
    return QString();
}

FlowGuiWindow::FlowGuiWindow()
{
    updateWindowTitle();
    resize(1000, 720);
    setAcceptDrops(true);

    // --- Project menu: save/restore the whole session setup ----------------
    {
        QMenu* m = menuBar()->addMenu(QStringLiteral("&Project"));
        auto* aNew  = m->addAction(QStringLiteral("&New"));
        auto* aOpen = m->addAction(QStringLiteral("&Open..."));
        aOpen->setShortcut(QKeySequence::Open);
        m->addSeparator();
        auto* aSave = m->addAction(QStringLiteral("&Save"));
        aSave->setShortcut(QKeySequence::Save);
        auto* aSaveAs = m->addAction(QStringLiteral("Save &As..."));
        connect(aNew,    &QAction::triggered, this, [this] { newProject(); });
        connect(aOpen,   &QAction::triggered, this, [this] { openProject(); });
        connect(aSave,   &QAction::triggered, this, [this] { saveProject(); });
        connect(aSaveAs, &QAction::triggered, this, [this] { saveProjectAs(); });
    }

    tabs_ = new QTabWidget(this);

    // ================= Run tab ==============================================
    auto* runPage = new QWidget;
    auto* top = new QVBoxLayout(runPage);

    // --- simulator override (for developers testing fresh flow builds) ------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Simulator:")));
        // A list, not a plain field: comparing a release build against one's
        // own is switching back and forth between two paths, and typing the
        // second one again each time is the annoying part.
        simBox_ = new QComboBox;
        simBox_->setEditable(true);
        simBox_->setInsertPolicy(QComboBox::NoInsert);   // the history is ours
        simBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        simBox_->lineEdit()->setPlaceholderText(
            QStringLiteral("auto - the flow executable shipped next to this program"));
        simBox_->setToolTip(QStringLiteral(
            "which flow to run: pick a build you have used before, type a path, "
            "or choose the first entry to use the flow shipped with the GUI"));
        setSimulators(QStringList(), QString());     // the "shipped" entry
        row->addWidget(simBox_, 1);
        auto* bsim = new QPushButton(QStringLiteral("Browse..."));
        row->addWidget(bsim);
        top->addLayout(row);
        connect(bsim, &QPushButton::clicked, this, [this] {
#ifdef Q_OS_WIN
            const QString filter = QStringLiteral("Programs (*.exe);;All files (*)");
#else
            const QString filter = QStringLiteral("All files (*)");
#endif
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Select flow executable"),
                flowgui::startDir(QStringLiteral("simulator"), currentSimulator()),
                filter);
            if (f.isEmpty()) return;
            flowgui::rememberDir(QStringLiteral("simulator"), f);
            rememberSimulator(f);                    // to the top of the list
        });
        // Typing a path or picking one from the list: both report what will
        // run, and a typed one joins the list.
        connect(simBox_->lineEdit(), &QLineEdit::editingFinished, this, [this] {
            rememberSimulator(currentSimulator());
            appendLog(QStringLiteral("simulator: %1\n")
                          .arg(QDir::toNativeSeparators(resolveSimulator())));
        });
        connect(simBox_, &QComboBox::activated, this, [this](int) {
            appendLog(QStringLiteral("simulator: %1\n")
                          .arg(QDir::toNativeSeparators(resolveSimulator())));
        });
    }

    // --- job table -----------------------------------------------------------
    {
        auto* box = new QGroupBox(QStringLiteral("Job queue  (drop *.DATA files anywhere)"));
        auto* row = new QHBoxLayout(box);
        jobTable_ = new QTableWidget(0, ColCount);
        jobTable_->setHorizontalHeaderLabels(
            { QStringLiteral("Deck"), QStringLiteral("Status"),
              QStringLiteral("Progress"), QStringLiteral("Elapsed"),
              QStringLiteral("ETA") });
        jobTable_->horizontalHeader()->setSectionResizeMode(ColDeck, QHeaderView::Stretch);
        jobTable_->horizontalHeader()->setSectionResizeMode(ColProgress, QHeaderView::Fixed);
        jobTable_->setColumnWidth(ColProgress, 140);
        jobTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        jobTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        jobTable_->verticalHeader()->setVisible(false);
        row->addWidget(jobTable_, 1);

        auto* col = new QVBoxLayout;
        auto* badd  = new QPushButton(QStringLiteral("Add deck..."));
        auto* brem  = new QPushButton(QStringLiteral("Remove"));
        auto* bclr  = new QPushButton(QStringLiteral("Clear"));
        auto* bopen = new QPushButton(QStringLiteral("Open result folder"));
        auto* bedit = new QPushButton(QStringLiteral("View/Edit deck"));
        auto* bprt  = new QPushButton(QStringLiteral("View PRT"));
        auto* bdbg  = new QPushButton(QStringLiteral("View DBG"));
        bprt->setToolTip(QStringLiteral("the run's print file (results, warnings, errors)"));
        bdbg->setToolTip(QStringLiteral("the run's debug file (developer diagnostics)"));
        bopen->setToolTip(QStringLiteral("open the run's output directory in the file manager"));
        connect(badd, &QPushButton::clicked, this, [this] { onAddDecks(); });
        connect(brem, &QPushButton::clicked, this, [this] {
            const int r = jobTable_->currentRow();
            if (r < 0 || r >= jobs_.size()) return;
            if (r == current_) { QMessageBox::information(this, QLatin1String(kAppName),
                QStringLiteral("Stop the job before removing it.")); return; }
            jobs_.remove(r);
            if (current_ > r) --current_;
            jobTable_->removeRow(r);
        });
        connect(bclr, &QPushButton::clicked, this, [this] {
            if (proc_) { QMessageBox::information(this, QLatin1String(kAppName),
                QStringLiteral("Stop the running job first.")); return; }
            jobs_.clear(); current_ = -1;
            jobTable_->setRowCount(0);
        });
        connect(bopen, &QPushButton::clicked, this, [this] { openJobFolder(jobTable_->currentRow()); });
        connect(bprt,  &QPushButton::clicked, this,
                [this] { viewJobFile(jobTable_->currentRow(), QStringLiteral("PRT")); });
        connect(bdbg,  &QPushButton::clicked, this,
                [this] { viewJobFile(jobTable_->currentRow(), QStringLiteral("DBG")); });
        connect(jobTable_, &QTableWidget::cellDoubleClicked, this,
                [this](int row, int) { openJobFolder(row); });
        connect(bedit, &QPushButton::clicked, this, [this] {
            const int r = jobTable_->currentRow();
            if (r < 0 || r >= jobs_.size() || !deckEd_) return;
            deckEd_->openDeck(jobs_[r].deck);
            tabs_->setCurrentWidget(deckEd_);
        });
        // queue management on top; the result viewers (used after a run
        // finishes) grouped below
        for (auto* b : { badd, bedit, brem, bclr }) col->addWidget(b);
        col->addSpacing(16);
        for (auto* b : { bprt, bdbg, bopen }) col->addWidget(b);
        col->addStretch(1);
        row->addLayout(col);
        top->addWidget(box, 2);
    }

    // --- options -------------------------------------------------------------
    {
        auto* box  = new QGroupBox(QStringLiteral("Run options"));
        auto* grid = new QGridLayout(box);

        grid->addWidget(new QLabel(QStringLiteral("MPI ranks:")), 0, 0);
        ranksSpin_ = new QSpinBox;  ranksSpin_->setRange(1, 256);
        grid->addWidget(ranksSpin_, 0, 1);

        grid->addWidget(new QLabel(QStringLiteral("OMP threads:")), 0, 2);
        threadsSpin_ = new QSpinBox; threadsSpin_->setRange(1, 64);
        grid->addWidget(threadsSpin_, 0, 3);

        grid->addWidget(new QLabel(QStringLiteral("Output:")), 0, 4);
        outdirMode_ = new QComboBox;
        outdirMode_->addItem(QStringLiteral("next to deck (<deck>_run)"));
        outdirMode_->addItem(QStringLiteral("custom directory"));
        grid->addWidget(outdirMode_, 0, 5);
        grid->setColumnStretch(5, 1);

        grid->addWidget(new QLabel(QStringLiteral("Out dir:")), 1, 0);
        outdirEdit_ = new QLineEdit; outdirEdit_->setEnabled(false);
        grid->addWidget(outdirEdit_, 1, 1, 1, 4);
        auto* bout = new QPushButton(QStringLiteral("Browse..."));
        grid->addWidget(bout, 1, 5);
        connect(outdirMode_, &QComboBox::currentIndexChanged, this,
                [this](int i) { outdirEdit_->setEnabled(i == 1); });
        connect(bout, &QPushButton::clicked, this, [this] { onBrowseOutdir(); });

        grid->addWidget(new QLabel(QStringLiteral("Extra options:")), 2, 0);
        extraEdit_ = new QLineEdit;
        extraEdit_->setToolTip(QStringLiteral(
            "additional flow command line options, e.g. --linear-solver=ilu0"));
        grid->addWidget(extraEdit_, 2, 1, 1, 4);
        tuningChk_ = new QCheckBox(QStringLiteral("TUNING"));
        tuningChk_->setToolTip(QStringLiteral(
            "--enable-tuning : honor the deck's TUNING keyword (time-step "
            "control etc. set in the schedule); off by default (flow's own "
            "default) - the deck's initial time-stepping is used throughout"));
        grid->addWidget(tuningChk_, 2, 5);
        top->addWidget(box);
    }

    // --- run / stop / clear-log ---------------------------------------------
    {
        auto* row = new QHBoxLayout;
        runBtn_    = new QPushButton(QStringLiteral("Run queue"));
        runSelBtn_ = new QPushButton(QStringLiteral("Run selected"));
        stopBtn_ = new QPushButton(QStringLiteral("Stop queue"));
        auto* skipBtn = new QPushButton(QStringLiteral("Skip job"));
        auto* valBtn  = new QPushButton(QStringLiteral("Validate deck"));
        auto* bclear  = new QPushButton(QStringLiteral("Clear log"));
        stopBtn_->setEnabled(false);
        skipBtn->setEnabled(false);
        runSelBtn_->setToolTip(QStringLiteral(
            "run only the decks selected in the queue above (Ctrl/Shift-click "
            "for several), leaving the rest untouched"));
        stopBtn_->setToolTip(QStringLiteral("kill the running job and abort the remaining queue"));
        skipBtn->setToolTip(QStringLiteral("kill the running job and continue with the next one"));
        valBtn->setToolTip(QStringLiteral("parse-and-initialize the selected deck without running "
                                          "the simulation (flow --enable-dry-run)"));
        connect(runBtn_,    &QPushButton::clicked, this, [this] { onRun(false); });
        connect(runSelBtn_, &QPushButton::clicked, this, [this] { onRun(true); });
        connect(stopBtn_, &QPushButton::clicked, this, [this] { stopCurrentJob(); });
        connect(skipBtn,  &QPushButton::clicked, this, [this] { skipCurrentJob(); });
        connect(valBtn,   &QPushButton::clicked, this, [this] { validateSelectedDeck(); });
        connect(bclear,   &QPushButton::clicked, this, [this] { logView_->clear(); });
        skipBtn_ = skipBtn;   // enabled/disabled together with Stop in setRunning()
        row->addWidget(runSelBtn_); row->addWidget(runBtn_);
        row->addWidget(stopBtn_); row->addWidget(skipBtn);
        row->addWidget(valBtn);  row->addWidget(bclear);
        row->addStretch(1);
        top->addLayout(row);
    }

    // --- log -----------------------------------------------------------------
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(200000);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    logView_->setFont(mono);
    top->addWidget(logView_, 3);

    tabs_->addTab(runPage, QStringLiteral("Run"));

    // ================= Summary plots tab ====================================
#ifdef FLOWGUI_HAVE_SUMMARY
    summary_ = new SummaryPlotWidget;
    tabs_->addTab(summary_, QStringLiteral("Summary Plots"));
#endif

    // ================= 3D view tab ==========================================
#ifdef FLOWGUI_HAVE_3D
    viewer3D_ = new Viewer3DWidget;
    tabs_->addTab(viewer3D_, QStringLiteral("3D View"));
#ifdef FLOWGUI_HAVE_SUMMARY
    if (summary_) {
        connect(summary_, &SummaryPlotWidget::caseAdded, viewer3D_,
                [this](const QString& label, const QString& path) {
                    viewer3D_->addCase(label, path);
                });
        connect(summary_, &SummaryPlotWidget::caseRenamed, viewer3D_,
                [this](const QString& path, const QString& label) {
                    viewer3D_->renameCase(path, label);
                });
        connect(summary_, &SummaryPlotWidget::caseRemoved, viewer3D_,
                [this](const QString& path) { viewer3D_->removeCase(path); });
        connect(summary_, &SummaryPlotWidget::caseOrder, viewer3D_,
                [this](const QStringList& paths) { viewer3D_->reorderCases(paths); });
    }
#endif
#endif

    // ================= Deck editor tab ======================================
    deckEd_ = new DeckEditorWidget;
    tabs_->addTab(deckEd_, QStringLiteral("Deck Editor"));

    setCentralWidget(tabs_);

    // Tick the running job's Elapsed/ETA once a second so they advance even
    // while flow is quiet between report steps.
    auto* tick = new QTimer(this);
    tick->setInterval(1000);
    connect(tick, &QTimer::timeout, this, [this] {
        if (current_ >= 0 && current_ < jobs_.size() &&
            jobs_[current_].state == Job::Running)
            refreshRow(current_);
    });
    tick->start();

    // A working directory that no longer exists (the GUI was started from a
    // run folder a later run cleaned out) makes opm-common refuse every
    // summary and grid file with "cannot get current path", which reads like
    // the cases are broken rather than the directory. Step out of it once,
    // here, and say so - the tabs re-check before each read.
    const QString moved = flowgui::ensureWorkingDirectory();

    loadSettings();
    exePath_ = resolveSimulator();

    appendLog(QString::fromLatin1("%1 %2 - queue OPM Flow simulations and watch them run.\n")
                  .arg(QLatin1String(kAppName), QLatin1String(kVersion)));
    if (!moved.isEmpty())
        appendLog(QStringLiteral("NOTE: the directory this was started from is gone; "
                                 "working from %1 instead\n")
                      .arg(QDir::toNativeSeparators(moved)));
    if (exePath_.isEmpty())
        appendLog(QStringLiteral("WARNING: no flow executable found next to this "
                                 "program - simulations cannot be started.\n"));
    else
        appendLog(QStringLiteral("simulator: %1%2\n")
                      .arg(QDir::toNativeSeparators(exePath_),
                           currentSimulator().isEmpty()
                               ? QString() : QStringLiteral("  (override)")));
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::loadSettings()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    setSimulators(s.value(QStringLiteral("simulators")).toStringList(),
                  s.value(QStringLiteral("simulator")).toString());
    ranksSpin_->setValue(s.value(QStringLiteral("ranks"), 1).toInt());
    threadsSpin_->setValue(s.value(QStringLiteral("threads"), 1).toInt());
    outdirMode_->setCurrentIndex(s.value(QStringLiteral("outmode"), 0).toInt());
    outdirEdit_->setText(s.value(QStringLiteral("outdir")).toString());
    outdirEdit_->setEnabled(outdirMode_->currentIndex() == 1);
    extraEdit_->setText(s.value(QStringLiteral("extra")).toString());
    tuningChk_->setChecked(s.value(QStringLiteral("tuning"), false).toBool());
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());

    // Restore the job queue from the previous session (decks that still
    // exist), with what each job did if that was recorded.
    const QJsonArray jobs = jsonArrayFromSetting(s.value(QStringLiteral("jobs")));
    if (!jobs.isEmpty()) {
        restoreJobs(jobs);
    } else {
        QStringList restored;
        for (const QString& d : s.value(QStringLiteral("queue")).toStringList())
            if (QFileInfo::exists(d)) restored << d;
        if (!restored.isEmpty()) addDecks(restored);
    }

    // ... and the working setup itself: the plotted cases and what each
    // subplot shows, the 3D view's case and property, the decks open in the
    // editor, and which tab was in front. Picking up where the last session
    // left off is the point of all of it.
    restoreUiState(jsonFromSetting(s.value(QStringLiteral("ui"))));

    // The project the session belonged to, so Save keeps writing to it and
    // the title bar still names it.
    const QString proj = s.value(QStringLiteral("projectPath")).toString();
    if (!proj.isEmpty() && QFileInfo::exists(proj)) projectPath_ = proj;
    updateWindowTitle();
}

void FlowGuiWindow::saveSettings()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    s.setValue(QStringLiteral("simulator"),  currentSimulator());
    s.setValue(QStringLiteral("simulators"), simulators());
    s.setValue(QStringLiteral("ranks"),   ranksSpin_->value());
    s.setValue(QStringLiteral("threads"), threadsSpin_->value());
    s.setValue(QStringLiteral("outmode"), outdirMode_->currentIndex());
    s.setValue(QStringLiteral("outdir"),  outdirEdit_->text());
    s.setValue(QStringLiteral("extra"),   extraEdit_->text());
    s.setValue(QStringLiteral("tuning"),  tuningChk_->isChecked());
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("projectPath"), projectPath_);

    QStringList queue;                       // kept for older versions
    for (const Job& j : jobs_) queue << j.deck;
    s.setValue(QStringLiteral("queue"), queue);
    s.setValue(QStringLiteral("jobs"), settingFromJson(jobsState()));
    s.setValue(QStringLiteral("ui"),   settingFromJson(collectUiState()));
}

void FlowGuiWindow::closeEvent(QCloseEvent* ev)
{
    if (deckEd_ && deckEd_->hasUnsavedChanges()) {
        const auto a = QMessageBox::question(
            this, QLatin1String(kAppName),
            QStringLiteral("The deck editor has unsaved changes. Quit anyway?"));
        if (a != QMessageBox::Yes) { ev->ignore(); return; }
    }
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        const auto answer = QMessageBox::question(
            this, QLatin1String(kAppName),
            QStringLiteral("A simulation is still running. Stop it and quit?"));
        if (answer != QMessageBox::Yes) { ev->ignore(); return; }
        stopCurrentJob();
        proc_->waitForFinished(3000);
    }
    saveSettings();
    ev->accept();
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::dragEnterEvent(QDragEnterEvent* ev)
{
    if (ev->mimeData()->hasUrls()) {
        for (const QUrl& u : ev->mimeData()->urls()) {
            const QString f = u.toLocalFile();
            if (f.endsWith(QStringLiteral(".data"),   Qt::CaseInsensitive) ||
                f.endsWith(QStringLiteral(".smspec"), Qt::CaseInsensitive)) {
                ev->acceptProposedAction();
                return;
            }
        }
    }
}

void FlowGuiWindow::dropEvent(QDropEvent* ev)
{
    QStringList decks;
    QString smspec;
    for (const QUrl& u : ev->mimeData()->urls()) {
        const QString f = u.toLocalFile();
        if (f.endsWith(QStringLiteral(".data"), Qt::CaseInsensitive))
            decks << f;
        else if (f.endsWith(QStringLiteral(".smspec"), Qt::CaseInsensitive))
            smspec = f;
    }
    addDecks(decks);
#ifdef FLOWGUI_HAVE_SUMMARY
    // Dropping a finished run's SMSPEC opens it straight in the Results tab.
    if (!smspec.isEmpty() && summary_) {
        summary_->addCase(QFileInfo(smspec).completeBaseName(), smspec);
        summary_->activateCase(smspec);
        tabs_->setCurrentWidget(summary_);
    }
#endif
}

void FlowGuiWindow::onAddDecks()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select input deck(s)"),
        flowgui::startDir(QStringLiteral("deck"),
                          jobs_.isEmpty() ? QString() : jobs_.last().deck),
        QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)"));
    if (files.isEmpty()) return;
    flowgui::rememberDir(QStringLiteral("deck"), files.first());
    addDecks(files);
}

void FlowGuiWindow::addDecks(const QStringList& files)
{
    for (const QString& f : files) {
        Job j; j.deck = QDir::toNativeSeparators(f);
#ifdef FLOWGUI_HAVE_SUMMARY
        // If this deck already has finished output next to it, register the
        // case so its results are immediately available in the Results tab.
        if (summary_) {
            const QFileInfo di(f);
            const QString prev = di.absolutePath() + '/' + di.completeBaseName()
                + QStringLiteral("_run/") + di.completeBaseName() + QStringLiteral(".SMSPEC");
            if (QFileInfo::exists(prev))
                summary_->addCase(di.completeBaseName(), prev);
        }
#endif
        jobs_.push_back(j);
        const int r = jobTable_->rowCount();
        jobTable_->insertRow(r);
        jobTable_->setItem(r, ColDeck, new QTableWidgetItem(j.deck));
        jobTable_->setItem(r, ColStatus,  new QTableWidgetItem(QStringLiteral("queued")));
        auto* bar = new QProgressBar;
        bar->setRange(0, 100); bar->setValue(0); bar->setTextVisible(true);
        jobTable_->setCellWidget(r, ColProgress, bar);
        jobTable_->setItem(r, ColElapsed, new QTableWidgetItem(QStringLiteral("-")));
        jobTable_->setItem(r, ColEta,     new QTableWidgetItem(QStringLiteral("-")));
    }
}

void FlowGuiWindow::onBrowseOutdir()
{
    QFileDialog dlg(this, QStringLiteral("Select or create the output directory"),
                    flowgui::startDir(QStringLiteral("outdir"), outdirEdit_->text()));
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setOption(QFileDialog::ShowDirsOnly);
    dlg.setOption(QFileDialog::DontUseNativeDialog);

    // Renaming a freshly created folder can leave the "Directory:" box on the
    // old name: Qt only refreshes it when the box still holds exactly that
    // name, so anything else typed or selected in between makes it go stale.
    // Choosing then points at a folder that no longer exists - and the mkpath
    // below would helpfully create it. Track the renames instead: keep the box
    // on the folder just named, and never resurrect a name renamed away here.
    QHash<QString, QString> renamed;      // old absolute path -> new one
    auto* nameEdit = dlg.findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
    if (auto* model = dlg.findChild<QFileSystemModel*>()) {
        connect(model, &QFileSystemModel::fileRenamed, &dlg,
                [&renamed, nameEdit](const QString& path, const QString& oldName,
                                     const QString& newName) {
            renamed.insert(QDir::cleanPath(QDir(path).filePath(oldName)),
                           QDir::cleanPath(QDir(path).filePath(newName)));
            if (nameEdit) nameEdit->setText(newName);
        });
    }

    if (dlg.exec() == QDialog::Accepted && !dlg.selectedFiles().isEmpty()) {
        QString d = dlg.selectedFiles().first();
        if (const auto it = renamed.constFind(QDir::cleanPath(d));
            it != renamed.constEnd())
            d = it.value();               // the folder was renamed in this dialog
        if (!QDir().mkpath(d)) {
            QMessageBox::warning(this, QLatin1String(kAppName),
                QStringLiteral("Could not create directory:\n%1").arg(d));
            return;
        }
        outdirEdit_->setText(QDir::toNativeSeparators(d));
        outdirMode_->setCurrentIndex(1);
    }
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::appendLog(const QString& text)
{
    // Batch appends and flush at most every 100 ms: very chatty simulations
    // would otherwise spend the UI thread's time on per-chunk text layout.
    logPend_ += text;
    if (!logTimer_) {
        logTimer_ = new QTimer(this);
        logTimer_->setSingleShot(true);
        logTimer_->setInterval(100);
        connect(logTimer_, &QTimer::timeout, this, [this] { flushLog(); });
    }
    if (!logTimer_->isActive()) logTimer_->start();
}

void FlowGuiWindow::flushLog()
{
    if (logPend_.isEmpty()) return;
    QString clean;
    clean.swap(logPend_);
    clean.remove(QLatin1Char('\r'));

    // Append without disturbing the user's scroll position: stick to the
    // bottom only if the view is already at (or near) the bottom.
    auto* sb = logView_->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 4;
    QTextCursor c(logView_->document());
    c.movePosition(QTextCursor::End);
    c.insertText(clean);
    if (atBottom) sb->setValue(sb->maximum());
}

void FlowGuiWindow::setRunning(bool on)
{
    runBtn_->setEnabled(!on);
    runSelBtn_->setEnabled(!on);
    stopBtn_->setEnabled(on);
    if (skipBtn_) skipBtn_->setEnabled(on);
}

QString FlowGuiWindow::jobEta(const Job& j) const
{
    if (j.state != Job::Running || j.progressDays <= 0.0 || j.totalDays <= 0.0)
        return QStringLiteral("-");
    const double frac = j.progressDays / j.totalDays;
    if (frac <= 0.001) return QStringLiteral("-");
    const qint64 eta = static_cast<qint64>(jobTimer_.elapsed() * (1.0 - frac) / frac);
    return fmtDuration(eta);
}

void FlowGuiWindow::refreshRow(int i)
{
    if (i < 0 || i >= jobs_.size()) return;
    const Job& j = jobs_[i];
    static const char* names[] = { "queued", "running", "done", "FAILED", "stopped" };
    jobTable_->item(i, ColStatus)->setText(QLatin1String(names[j.state]));
    if (auto* bar = qobject_cast<QProgressBar*>(jobTable_->cellWidget(i, ColProgress))) {
        int pct = 0;
        if (j.state == Job::Done) pct = 100;
        else if (j.totalDays > 0) pct = int(100.0 * j.progressDays / j.totalDays);
        bar->setValue(qBound(0, pct, 100));
    }
    const qint64 el = (j.state == Job::Running) ? jobTimer_.elapsed() : j.elapsedMs;
    jobTable_->item(i, ColElapsed)->setText(el > 0 ? fmtDuration(el) : QStringLiteral("-"));
    jobTable_->item(i, ColEta)->setText(
        j.state == Job::Running ? jobEta(j)
        : j.state == Job::Failed ? QStringLiteral("exit %1").arg(j.exitCode)
        : QStringLiteral("-"));
}

void FlowGuiWindow::parseProgress(const QString& chunk)
{
    if (current_ < 0) return;
    // flow prints e.g. "Report step 49/247 at day 561/3312, date = 21-May-1999"
    static const QRegularExpression re(QStringLiteral(
        R"(Report step\s+(\d+)/(\d+)\s+at day\s+([0-9.]+)/([0-9.]+))"));
    lineBuf_ += chunk;
    int nl;
    while ((nl = lineBuf_.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = lineBuf_.left(nl);
        lineBuf_.remove(0, nl + 1);
        const auto m = re.match(line);
        if (m.hasMatch()) {
            Job& j = jobs_[current_];
            j.reportStep   = m.captured(1).toInt();
            j.reportTotal  = m.captured(2).toInt();
            j.progressDays = m.captured(3).toDouble();
            j.totalDays    = m.captured(4).toDouble();
        }
    }
    refreshRow(current_);
}

// ---------------------------------------------------------------------------
bool FlowGuiWindow::flushDeckEdits()
{
    // flow reads the decks from disk on every run, so unsaved editor changes
    // would silently run the OLD file: offer to write them out first.
    if (!deckEd_ || !deckEd_->hasUnsavedChanges()) return true;
    const auto a = QMessageBox::question(this, QLatin1String(kAppName),
        QStringLiteral("The deck editor has unsaved changes.\n"
                       "Save them before running? (flow reads the decks from "
                       "disk, so unsaved edits would not take effect.)"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (a == QMessageBox::Cancel) return false;
    if (a == QMessageBox::Save) {
        deckEd_->saveAllTabs();
        appendLog(QStringLiteral("deck editor changes saved before the run\n"));
    }
    return true;
}

void FlowGuiWindow::onRun(bool selectedOnly)
{
    if (proc_ && proc_->state() != QProcess::NotRunning) return;

    // Candidate rows: the selection, or the whole queue.
    QList<int> rows;
    if (selectedOnly) {
        QSet<int> uniq;
        const auto sel = jobTable_->selectionModel()->selectedIndexes();
        for (const auto& ix : sel) uniq.insert(ix.row());
        rows = QList<int>(uniq.begin(), uniq.end());
        std::sort(rows.begin(), rows.end());
        if (rows.isEmpty()) {
            QMessageBox::information(this, QLatin1String(kAppName),
                QStringLiteral("Select one or more decks in the queue first "
                               "(Ctrl/Shift-click for several)."));
            return;
        }
    } else {
        for (int i = 0; i < jobs_.size(); ++i) rows << i;
    }

    bool anyQueued = false, anyFinished = false;
    for (int r : std::as_const(rows)) {
        const Job& j = jobs_[r];
        anyQueued   |= (j.state == Job::Queued);
        anyFinished |= (j.state == Job::Done || j.state == Job::Failed
                        || j.state == Job::Stopped);
    }
    if (!anyQueued && !anyFinished) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("Add at least one input deck (*.DATA) to the queue first."));
        return;
    }
    if (!anyQueued) {   // everything selected already ran - confirmed re-run
        const auto a = QMessageBox::question(this, QLatin1String(kAppName),
            selectedOnly
                ? QStringLiteral("The selected job(s) have already run.\n"
                                 "Re-run them? Previous results in their output "
                                 "directories will be overwritten.")
                : QStringLiteral("All jobs in the queue have already run.\n"
                                 "Re-run them? Previous results in their output "
                                 "directories will be overwritten."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (a != QMessageBox::Yes) return;
        for (int i : std::as_const(rows)) {
            Job& j = jobs_[i];
            if (j.state != Job::Done && j.state != Job::Failed
                && j.state != Job::Stopped) continue;
            j.state        = Job::Queued;
            j.progressDays = 0.0;
            j.totalDays    = 0.0;
            j.reportStep   = 0;
            j.reportTotal  = 0;
            j.elapsedMs    = 0;
            j.exitCode     = 0;
            refreshRow(i);
        }
        appendLog(selectedOnly ? QStringLiteral("re-running the selected job(s)\n")
                               : QStringLiteral("re-running the queue\n"));
    }
    if (!flushDeckEdits()) return;

    // Mark this batch: only these rows are picked up by the runner.
    for (Job& j : jobs_) j.inRun = false;
    int batch = 0;
    for (int r : std::as_const(rows))
        if (jobs_[r].state == Job::Queued) { jobs_[r].inRun = true; ++batch; }
    if (selectedOnly)
        appendLog(QStringLiteral("running %1 selected job(s) of %2 in the queue\n")
                      .arg(batch).arg(jobs_.size()));

    // always re-resolve: the Simulator override may have changed since the
    // last run, and it must win over whatever executable ran previously
    exePath_ = resolveSimulator();
    if (exePath_.isEmpty() || !QFileInfo::exists(exePath_)) {
        const QString custom = currentSimulator();
        QMessageBox::critical(this, QLatin1String(kAppName),
            custom.isEmpty()
                ? QStringLiteral("No flow executable was found next to this program.\n"
                                 "Reinstall the package or place flow(.exe) beside the GUI.")
                : QStringLiteral("The simulator given in the Simulator field does "
                                 "not exist:\n%1").arg(custom));
        return;
    }
    const int cores = QThread::idealThreadCount();
    if (cores > 0 && ranksSpin_->value() * threadsSpin_->value() > cores)
        appendLog(QStringLiteral("note: %1 ranks x %2 threads oversubscribes the "
                                 "%3 logical cores of this machine.\n")
                      .arg(ranksSpin_->value()).arg(threadsSpin_->value()).arg(cores));
    aborted_ = false;
    setRunning(true);
    saveSettings();
    startNextJob();
}

void FlowGuiWindow::startNextJob()
{
    current_ = -1;
    if (!aborted_)
        for (int i = 0; i < jobs_.size(); ++i)
            if (jobs_[i].state == Job::Queued && jobs_[i].inRun) { current_ = i; break; }

    if (current_ < 0) {                       // queue finished
        int ok = 0, fail = 0;
        for (const Job& j : jobs_) {
            if (!j.inRun) continue;           // count this batch only
            if (j.state == Job::Done)   ++ok;
            if (j.state == Job::Failed) ++fail;
        }
        appendLog(aborted_ ? QStringLiteral("\n*** queue aborted ***\n")
                           : QStringLiteral("\n*** queue complete: %1 ok, %2 failed ***\n")
                                 .arg(ok).arg(fail));
        if (!aborted_) notifyQueueDone(ok, fail);
        setRunning(false);
        return;
    }

    Job& j = jobs_[current_];
    const QFileInfo deckInfo(j.deck);
    if (outdirMode_->currentIndex() == 1 && !outdirEdit_->text().isEmpty())
        j.outdir = outdirEdit_->text();
    else
        j.outdir = deckInfo.absolutePath() + '/' + deckInfo.completeBaseName() + "_run";
    if (!QDir().mkpath(j.outdir)) {
        appendLog(QStringLiteral("FAILED to create output directory %1\n").arg(j.outdir));
        j.state = Job::Failed;
        refreshRow(current_);
        startNextJob();
        return;
    }

    const int ranks   = ranksSpin_->value();
    const int threads = threadsSpin_->value();
    QString     program;
    QStringList args;
    if (ranks > 1) {
        program = QStringLiteral("mpiexec");
        args << QStringLiteral("-n") << QString::number(ranks) << exePath_;
    } else {
        program = exePath_;
    }
    args << j.deck << (QStringLiteral("--output-dir=") + j.outdir);
    // Always pass the thread count: without the option an OpenMP-enabled
    // flow defaults to 2 threads per process, so "1" must be explicit.
    args << (QStringLiteral("--threads-per-process=") + QString::number(threads));
    if (tuningChk_ && tuningChk_->isChecked())
        args << QStringLiteral("--enable-tuning=true");
    const QString extra = extraEdit_->text().trimmed();
    if (!extra.isEmpty())
        args << extra.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

    appendLog(QStringLiteral("\n==================== job %1 ====================\n"
                             "%2 %3\noutput directory: %4\n\n")
                  .arg(deckInfo.fileName(), program, args.join(QLatin1Char(' ')), j.outdir));

    j.state = Job::Running;
    j.progressDays = j.totalDays = 0.0;
    lineBuf_.clear();
    jobTimer_.start();
    refreshRow(current_);

#ifdef FLOWGUI_HAVE_SUMMARY
    if (summary_)
        summary_->addCase(deckInfo.completeBaseName(),
                          j.outdir + '/' + deckInfo.completeBaseName() + ".SMSPEC");
#endif

    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    proc_->setWorkingDirectory(deckInfo.absolutePath());
#if !defined(Q_OS_WIN)
    proc_->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    connect(proc_, &QProcess::readyRead, this, [this] {
        const QString text = QString::fromLocal8Bit(proc_->readAll());
        appendLog(text);
        parseProgress(text);
    });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        appendLog(QStringLiteral("FAILED to start: %1\n").arg(proc_->errorString()));
    });
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        if (current_ >= 0 && current_ < jobs_.size()) {
            Job& jj = jobs_[current_];
            jj.elapsedMs = jobTimer_.elapsed();
            jj.exitCode  = code;
            jj.state = (st == QProcess::CrashExit) ? Job::Stopped
                       : (code == 0) ? Job::Done : Job::Failed;
            if (jj.state == Job::Done) {
                const QFileInfo di(jj.deck);
                lastFinishedSmspec_ = jj.outdir + '/' + di.completeBaseName()
                                    + QStringLiteral(".SMSPEC");
                // The case was registered at job start, before flow had
                // written anything - load the now-existing files.
#ifdef FLOWGUI_HAVE_SUMMARY
                if (summary_) summary_->caseFinished(lastFinishedSmspec_);
#endif
#ifdef FLOWGUI_HAVE_3D
                if (viewer3D_) viewer3D_->caseFinished(lastFinishedSmspec_);
#endif
            }
            appendLog(QStringLiteral("\n---- job finished, exit code %1%2, %3 ----\n")
                          .arg(code)
                          .arg(st == QProcess::CrashExit ? QStringLiteral(" (terminated)") : QString())
                          .arg(fmtDuration(jj.elapsedMs)));
            refreshRow(current_);
        }
        proc_->deleteLater();
        proc_ = nullptr;
        startNextJob();
    });
    connect(proc_, &QProcess::started, this, [this] {
        exemptFromPowerThrottling(proc_->processId());
    });
    proc_->start(program, args);
}

void FlowGuiWindow::killCurrentTree()
{
    if (!proc_ || proc_->state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("taskkill"),
        { QStringLiteral("/PID"), QString::number(proc_->processId()),
          QStringLiteral("/T"), QStringLiteral("/F") });
#else
    ::kill(-static_cast<pid_t>(proc_->processId()), SIGKILL);
#endif
}

void FlowGuiWindow::stopCurrentJob()
{
    aborted_ = true;
    if (!proc_ || proc_->state() == QProcess::NotRunning) return;
    appendLog(QStringLiteral("\n*** stopping current job and aborting the queue ***\n"));
    killCurrentTree();
}

void FlowGuiWindow::skipCurrentJob()
{
    if (!proc_ || proc_->state() == QProcess::NotRunning) return;
    appendLog(QStringLiteral("\n*** skipping current job, continuing with the queue ***\n"));
    killCurrentTree();
    // aborted_ stays false: the finished handler marks this job Stopped and
    // startNextJob() moves on to the next queued deck.
}

void FlowGuiWindow::validateSelectedDeck()
{
    if (vproc_) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("A validation is already running."));
        return;
    }
    const int row = jobTable_->currentRow();
    if (row < 0 || row >= jobs_.size()) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("Select a deck in the queue to validate."));
        return;
    }
    if (!flushDeckEdits()) return;   // validate what is on disk, not a stale copy
    exePath_ = resolveSimulator();   // honor a changed Simulator override
    if (exePath_.isEmpty() || !QFileInfo::exists(exePath_)) return;

    const QString deck = jobs_[row].deck;
    const QString out  = QDir::temp().filePath(QStringLiteral("flowgui_validate"));
    QDir().mkpath(out);

    appendLog(QStringLiteral("\n==== validating %1 (parse + init only) ====\n")
                  .arg(QFileInfo(deck).fileName()));
    vproc_ = new QProcess(this);
    vproc_->setProcessChannelMode(QProcess::MergedChannels);
    vproc_->setWorkingDirectory(QFileInfo(deck).absolutePath());
    connect(vproc_, &QProcess::readyRead, this, [this] {
        appendLog(QString::fromLocal8Bit(vproc_->readAll()));
    });
    connect(vproc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        appendLog(QStringLiteral("validation FAILED to start: %1\n").arg(vproc_->errorString()));
    });
    connect(vproc_, &QProcess::finished, this, [this, deck](int code, QProcess::ExitStatus) {
        appendLog(code == 0
            ? QStringLiteral("==== %1: deck validates OK ====\n").arg(QFileInfo(deck).fileName())
            : QStringLiteral("==== %1: validation FAILED (exit %2) - see messages above ====\n")
                  .arg(QFileInfo(deck).fileName()).arg(code));
        vproc_->deleteLater();
        vproc_ = nullptr;
    });
    connect(vproc_, &QProcess::started, this, [this] {
        exemptFromPowerThrottling(vproc_->processId());
    });
    vproc_->start(exePath_, { deck,
                              QStringLiteral("--enable-dry-run=true"),
                              QStringLiteral("--output-dir=") + out,
                              QStringLiteral("--threads-per-process=1") });
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::openJobFolder(int row)
{
    if (row < 0 || row >= jobs_.size()) return;
    const QString d = jobs_[row].outdir.isEmpty()
        ? QFileInfo(jobs_[row].deck).absolutePath() : jobs_[row].outdir;
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
}

void FlowGuiWindow::viewJobFile(int row, const QString& ext)
{
    if (row < 0 || row >= jobs_.size()) return;
    const Job& j = jobs_[row];
    const QFileInfo deckInfo(j.deck);
    const QString prt = (j.outdir.isEmpty()
        ? deckInfo.absolutePath() : j.outdir) + '/'
        + deckInfo.completeBaseName() + '.' + ext;
    QFile f(prt);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("No %1 file yet at\n%2")
                .arg(ext, QDir::toNativeSeparators(prt)));
        return;
    }
    constexpr qint64 kMax = 8 * 1024 * 1024;   // show at most the last 8 MB
    if (f.size() > kMax) f.seek(f.size() - kMax);
    const QString text = QString::fromLocal8Bit(f.readAll());

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QFileInfo(prt).fileName());
    dlg->resize(900, 650);
    auto* lay = new QVBoxLayout(dlg);

    auto* view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setPlainText(text);
    view->moveCursor(QTextCursor::End);

    // search row: free text (Enter/Find = next match, wraps) and a
    // jump-to-problems button cycling through Error/Warning/Problem lines
    auto* srow = new QHBoxLayout;
    auto* searchEdit = new QLineEdit;
    searchEdit->setPlaceholderText(QStringLiteral("search in PRT..."));
    auto* bfind = new QPushButton(QStringLiteral("Find next"));
    auto* bprob = new QPushButton(QStringLiteral("Next problem"));
    srow->addWidget(searchEdit, 1);
    srow->addWidget(bfind);
    srow->addWidget(bprob);
    lay->addLayout(srow);
    lay->addWidget(view);

    auto findWrapped = [view](auto&& needle) {
        if (!view->find(needle)) {                 // wrap to the top once
            view->moveCursor(QTextCursor::Start);
            view->find(needle);
        }
    };
    auto doFind = [findWrapped, searchEdit] {
        const QString t = searchEdit->text();
        if (!t.isEmpty()) findWrapped(t);
    };
    connect(bfind, &QPushButton::clicked, view, doFind);
    connect(searchEdit, &QLineEdit::returnPressed, view, doFind);
    connect(bprob, &QPushButton::clicked, view, [findWrapped] {
        findWrapped(QRegularExpression(QStringLiteral("\\b(Error|Warning|Problem)\\b"),
                                       QRegularExpression::CaseInsensitiveOption));
    });

    dlg->show();
    searchEdit->setFocus();
}

// ---------------------------------------------------------------------------
// Projects: a human-readable .opmproj JSON file holding the deck queue, the
// run options (ranks/threads/output/extra args) and the Results-tab cases.
void FlowGuiWindow::updateWindowTitle()
{
    QString t = QStringLiteral("OPM Flow GUI");
    if (!projectPath_.isEmpty())
        t += QStringLiteral("  -  ") + QFileInfo(projectPath_).fileName();
    setWindowTitle(t);
}

void FlowGuiWindow::newProject()
{
    if (proc_) { QMessageBox::information(this, QLatin1String(kAppName),
        QStringLiteral("Stop the running queue first.")); return; }
    jobs_.clear();
    jobTable_->setRowCount(0);
    current_ = -1;
#ifdef FLOWGUI_HAVE_SUMMARY
    if (summary_) summary_->clearCases();
#endif
    projectPath_.clear();
    updateWindowTitle();
    appendLog(QStringLiteral("\nnew project - queue and cases cleared\n"));
}

void FlowGuiWindow::openProject()
{
    if (proc_) { QMessageBox::information(this, QLatin1String(kAppName),
        QStringLiteral("Stop the running queue first.")); return; }
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open project"),
        flowgui::startDir(QStringLiteral("project"), projectPath_),
        QStringLiteral("OPM Flow GUI project (*.opmproj);;All files (*)"));
    if (f.isEmpty()) return;
    if (readProject(f)) {
        projectPath_ = f;
        flowgui::rememberDir(QStringLiteral("project"), f);
        updateWindowTitle();
    }
}

void FlowGuiWindow::saveProject()
{
    if (projectPath_.isEmpty()) { saveProjectAs(); return; }
    if (writeProject(projectPath_))
        appendLog(QStringLiteral("project saved: %1\n")
                      .arg(QDir::toNativeSeparators(projectPath_)));
}

void FlowGuiWindow::saveProjectAs()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save project as"),
        flowgui::startDir(QStringLiteral("project"), projectPath_),
        QStringLiteral("OPM Flow GUI project (*.opmproj)"));
    if (f.isEmpty()) return;
    if (!f.endsWith(QStringLiteral(".opmproj"), Qt::CaseInsensitive))
        f += QStringLiteral(".opmproj");
    if (writeProject(f)) {
        projectPath_ = f;
        flowgui::rememberDir(QStringLiteral("project"), f);
        updateWindowTitle();
        appendLog(QStringLiteral("project saved: %1\n").arg(QDir::toNativeSeparators(f)));
    }
}

// The queue as it stands: the decks, plus where each finished job wrote and
// how it ended, so "Open folder" and "View PRT" still reach its output after
// a restart. A job that was still running is stored as queued - it is not
// running any more by the time this is read back.
QJsonArray FlowGuiWindow::jobsState() const
{
    static const char* kState[] = { "queued", "queued", "done", "failed", "stopped" };
    QJsonArray arr;
    for (const Job& j : jobs_) {
        QJsonObject o;
        o[QStringLiteral("deck")]   = QDir::fromNativeSeparators(j.deck);
        o[QStringLiteral("outdir")] = QDir::fromNativeSeparators(j.outdir);
        o[QStringLiteral("state")]  = QLatin1String(kState[j.state]);
        if (j.elapsedMs > 0) o[QStringLiteral("elapsedMs")] = double(j.elapsedMs);
        if (j.state == Job::Failed) o[QStringLiteral("exitCode")] = j.exitCode;
        arr.append(o);
    }
    return arr;
}

void FlowGuiWindow::restoreJobs(const QJsonArray& jobs)
{
    QStringList decks;
    QVector<Job> meta;
    for (const auto& v : jobs) {
        const QJsonObject o = v.toObject();
        const QString deck =
            QDir::toNativeSeparators(o[QStringLiteral("deck")].toString());
        if (deck.isEmpty() || !QFileInfo::exists(deck)) continue;
        Job j;
        j.deck      = deck;
        j.outdir    = QDir::toNativeSeparators(o[QStringLiteral("outdir")].toString());
        j.elapsedMs = qint64(o[QStringLiteral("elapsedMs")].toDouble(0));
        j.exitCode  = o[QStringLiteral("exitCode")].toInt(0);
        const QString st = o[QStringLiteral("state")].toString();
        j.state = st == QLatin1String("done")    ? Job::Done
                : st == QLatin1String("failed")  ? Job::Failed
                : st == QLatin1String("stopped") ? Job::Stopped
                                                 : Job::Queued;
        decks << deck;
        meta.append(j);
    }
    const int base = jobs_.size();
    addDecks(decks);                       // builds the rows
    for (int i = 0; i < meta.size() && base + i < jobs_.size(); ++i) {
        Job& j = jobs_[base + i];
        j.outdir    = meta[i].outdir;
        j.state     = meta[i].state;
        j.elapsedMs = meta[i].elapsedMs;
        j.exitCode  = meta[i].exitCode;
        refreshRow(base + i);
    }
}

QJsonObject FlowGuiWindow::collectUiState() const
{
    QJsonObject ui;
    ui[QStringLiteral("activeTab")] = tabs_ ? tabs_->currentIndex() : 0;
#ifdef FLOWGUI_HAVE_SUMMARY
    if (summary_) ui[QStringLiteral("summary")] = summary_->uiState();
#endif
#ifdef FLOWGUI_HAVE_3D
    if (viewer3D_) ui[QStringLiteral("viewer3d")] = viewer3D_->uiState();
#endif
    if (deckEd_) ui[QStringLiteral("deckEditor")] = deckEd_->uiState();
    return ui;
}

void FlowGuiWindow::restoreUiState(const QJsonObject& ui)
{
    if (ui.isEmpty()) return;
#ifdef FLOWGUI_HAVE_SUMMARY
    // The plot first: the 3D tab mirrors its case list, so the case a
    // restored 3D view asks for only exists once this has run.
    if (summary_)
        summary_->restoreUiState(ui[QStringLiteral("summary")].toObject());
#endif
#ifdef FLOWGUI_HAVE_3D
    if (viewer3D_)
        viewer3D_->restoreUiState(ui[QStringLiteral("viewer3d")].toObject());
#endif
    if (deckEd_)
        deckEd_->restoreUiState(ui[QStringLiteral("deckEditor")].toObject());
    if (tabs_ && ui.contains(QStringLiteral("activeTab"))) {
        const int t = ui[QStringLiteral("activeTab")].toInt(0);
        if (t >= 0 && t < tabs_->count()) tabs_->setCurrentIndex(t);
    }
}

bool FlowGuiWindow::writeProject(const QString& path)
{
    QJsonObject root;
    root[QStringLiteral("format")]  = QStringLiteral("opm-flow-gui-project");
    // 2 added the queue's per-job outcome and the "ui" section; a version 1
    // file still opens, and version 1 readers still find "decks" and "cases".
    root[QStringLiteral("version")] = 2;

    QJsonArray decks;
    for (const Job& j : jobs_) decks.append(QDir::fromNativeSeparators(j.deck));
    root[QStringLiteral("decks")] = decks;
    root[QStringLiteral("jobs")]  = jobsState();
    root[QStringLiteral("ui")]    = collectUiState();

    root[QStringLiteral("ranks")]      = ranksSpin_->value();
    root[QStringLiteral("threads")]    = threadsSpin_->value();
    root[QStringLiteral("outputMode")] = outdirMode_->currentIndex();
    root[QStringLiteral("outputDir")]  = QDir::fromNativeSeparators(outdirEdit_->text());
    root[QStringLiteral("extraOptions")] = extraEdit_->text();
    root[QStringLiteral("tuning")]     = tuningChk_->isChecked();
    root[QStringLiteral("simulator")]  = QDir::fromNativeSeparators(currentSimulator());
    QJsonArray sims;
    for (const QString& p : simulators()) sims.append(QDir::fromNativeSeparators(p));
    root[QStringLiteral("simulators")] = sims;

#ifdef FLOWGUI_HAVE_SUMMARY
    if (summary_) {
        QJsonArray cases;
        for (const auto& c : summary_->caseInfos()) {
            QJsonObject o;
            o[QStringLiteral("label")]   = c.label;
            o[QStringLiteral("path")]    = QDir::fromNativeSeparators(c.path);
            o[QStringLiteral("checked")] = c.checked;
            cases.append(o);
        }
        root[QStringLiteral("cases")] = cases;
    }
#endif

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QLatin1String(kAppName),
            QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        QMessageBox::warning(this, QLatin1String(kAppName),
            QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    return true;
}

bool FlowGuiWindow::readProject(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QLatin1String(kAppName),
            QStringLiteral("Could not open %1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    const QJsonObject root = doc.object();
    if (perr.error != QJsonParseError::NoError ||
        root[QStringLiteral("format")].toString() != QLatin1String("opm-flow-gui-project")) {
        QMessageBox::warning(this, QLatin1String(kAppName),
            QStringLiteral("%1 is not an OPM Flow GUI project file")
                .arg(QFileInfo(path).fileName()));
        return false;
    }

    // replace the queue
    jobs_.clear();
    jobTable_->setRowCount(0);
    current_ = -1;
    int missingDecks = 0;
    // Version 2 carries a per-job record (output directory and outcome);
    // version 1 only the deck paths.
    const QJsonArray jobArr = root[QStringLiteral("jobs")].toArray();
    QStringList deckFiles;
    if (!jobArr.isEmpty()) {
        for (const auto& v : jobArr)
            if (!QFileInfo::exists(v.toObject()[QStringLiteral("deck")].toString()))
                ++missingDecks;
    } else {
        for (const auto& v : root[QStringLiteral("decks")].toArray()) {
            const QString d = v.toString();
            if (QFileInfo::exists(d)) deckFiles << d; else ++missingDecks;
        }
    }

    // options
    ranksSpin_->setValue(root[QStringLiteral("ranks")].toInt(1));
    threadsSpin_->setValue(root[QStringLiteral("threads")].toInt(1));
    outdirMode_->setCurrentIndex(root[QStringLiteral("outputMode")].toInt(0));
    outdirEdit_->setText(QDir::toNativeSeparators(root[QStringLiteral("outputDir")].toString()));
    outdirEdit_->setEnabled(outdirMode_->currentIndex() == 1);
    extraEdit_->setText(root[QStringLiteral("extraOptions")].toString());
    tuningChk_->setChecked(root[QStringLiteral("tuning")].toBool(false));
    QStringList sims;
    for (const auto& v : root[QStringLiteral("simulators")].toArray())
        sims << QDir::toNativeSeparators(v.toString());
    if (sims.isEmpty()) sims = simulators();      // a version 1 file has none
    setSimulators(sims, QDir::toNativeSeparators(
                            root[QStringLiteral("simulator")].toString()));

    // The queue first: adding a deck auto-registers a case when the deck has
    // output beside it, which the restored case list below then overrides.
    if (!jobArr.isEmpty()) restoreJobs(jobArr);
    else                   addDecks(deckFiles);
    const int deckCount = jobs_.size();

    // Cases. A version 2 file carries them inside "ui" together with the rest
    // of the plot setup; "cases" at the root is what version 1 wrote (and is
    // still written, so an older flow-gui can read this file).
    int missingCases = 0;
    const QJsonObject ui = root[QStringLiteral("ui")].toObject();
    const QJsonArray rootCases = root[QStringLiteral("cases")].toArray();
    for (const auto& v : rootCases)
        if (!QFileInfo::exists(v.toObject()[QStringLiteral("path")].toString()))
            ++missingCases;
    if (!ui.isEmpty()) {
        restoreUiState(ui);
    } else {
#ifdef FLOWGUI_HAVE_SUMMARY
        if (summary_) {
            summary_->clearCases();
            for (const auto& v : rootCases) {
                const QJsonObject o = v.toObject();
                const QString p = o[QStringLiteral("path")].toString();
                if (!QFileInfo::exists(p)) continue;
                summary_->addCase(o[QStringLiteral("label")].toString(), p,
                                  o[QStringLiteral("checked")].toBool(true));
            }
        }
#endif
    }

    QString msg = QStringLiteral("\nproject loaded: %1 (%2 decks, %3 cases")
        .arg(QFileInfo(path).fileName())
        .arg(deckCount)
        .arg(rootCases.size() - missingCases);
    if (missingDecks) msg += QStringLiteral("; %1 missing deck(s) skipped").arg(missingDecks);
    if (missingCases) msg += QStringLiteral("; %1 missing case(s) skipped").arg(missingCases);
    appendLog(msg + QStringLiteral(")\n"));
    return true;
}

void FlowGuiWindow::notifyQueueDone(int okCount, int failCount)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) { QApplication::beep(); return; }
    if (!tray_) {
        QPixmap px(32, 32);
        px.fill(QColor(0x0b, 0x3d, 0x63));
        QPainter p(&px);
        p.setPen(Qt::white);
        p.drawText(px.rect(), Qt::AlignCenter, QStringLiteral("F"));
        p.end();
        tray_ = new QSystemTrayIcon(QIcon(px), this);
        // Clicking the notification brings the window up and opens the last
        // finished case in the Results tab.
        connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
            showNormal(); raise(); activateWindow();
#ifdef FLOWGUI_HAVE_SUMMARY
            if (summary_ && !lastFinishedSmspec_.isEmpty()) {
                summary_->addCase(QFileInfo(lastFinishedSmspec_).completeBaseName(),
                                  lastFinishedSmspec_);
                summary_->activateCase(lastFinishedSmspec_);
                tabs_->setCurrentWidget(summary_);
            }
#endif
        });
    }
    tray_->show();
    tray_->showMessage(QStringLiteral("OPM Flow"),
        QStringLiteral("Queue complete: %1 ok, %2 failed").arg(okCount).arg(failCount),
        failCount ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
        10000);
}
