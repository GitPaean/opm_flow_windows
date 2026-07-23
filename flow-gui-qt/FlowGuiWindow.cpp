/*
  FlowGuiWindow implementation. Part of the opm_flow_windows harness;
  GPL v3+ (see repository LICENSE).
*/
#include "FlowGuiWindow.h"
#ifdef FLOWGUI_HAVE_SUMMARY
  #include "SummaryPlotWidget.h"
#endif

#include <QApplication>
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
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
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

#if !defined(Q_OS_WIN)
  #include <signal.h>
  #include <unistd.h>
#endif

static const char* kAppName = "flow-gui-qt";
static const char* kVersion = "0.2.0";

namespace {
enum Col { ColDeck = 0, ColStatus, ColProgress, ColElapsed, ColEta, ColCount };

QString fmtDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

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
    setWindowTitle(QStringLiteral("OPM Flow GUI (Qt)"));
    resize(1000, 720);
    setAcceptDrops(true);

    tabs_ = new QTabWidget(this);

    // ================= Run tab ==============================================
    auto* runPage = new QWidget;
    auto* top = new QVBoxLayout(runPage);

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
        auto* bopen = new QPushButton(QStringLiteral("Open folder"));
        auto* bprt  = new QPushButton(QStringLiteral("View PRT"));
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
        connect(bprt,  &QPushButton::clicked, this, [this] { viewJobPrt(jobTable_->currentRow()); });
        connect(jobTable_, &QTableWidget::cellDoubleClicked, this,
                [this](int row, int) { openJobFolder(row); });
        for (auto* b : { badd, brem, bclr, bopen, bprt }) col->addWidget(b);
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
        grid->addWidget(extraEdit_, 2, 1, 1, 5);
        top->addWidget(box);
    }

    // --- run / stop / clear-log ---------------------------------------------
    {
        auto* row = new QHBoxLayout;
        runBtn_  = new QPushButton(QStringLiteral("Run queue"));
        stopBtn_ = new QPushButton(QStringLiteral("Stop job"));
        auto* bclear = new QPushButton(QStringLiteral("Clear log"));
        stopBtn_->setEnabled(false);
        connect(runBtn_,  &QPushButton::clicked, this, [this] { onRun(); });
        connect(stopBtn_, &QPushButton::clicked, this, [this] { stopCurrentJob(); });
        connect(bclear,   &QPushButton::clicked, this, [this] { logView_->clear(); });
        row->addWidget(runBtn_); row->addWidget(stopBtn_); row->addWidget(bclear);
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

    // ================= Results tab ==========================================
#ifdef FLOWGUI_HAVE_SUMMARY
    summary_ = new SummaryPlotWidget;
    tabs_->addTab(summary_, QStringLiteral("Results"));
#endif

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

    loadSettings();
    exePath_ = findFlowExe();

    appendLog(QString::fromLatin1("%1 %2 - queue OPM Flow simulations and watch them run.\n")
                  .arg(QLatin1String(kAppName), QLatin1String(kVersion)));
    if (exePath_.isEmpty())
        appendLog(QStringLiteral("WARNING: no flow executable found next to this "
                                 "program - simulations cannot be started.\n"));
    else
        appendLog(QStringLiteral("simulator: %1\n").arg(QDir::toNativeSeparators(exePath_)));
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::loadSettings()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    ranksSpin_->setValue(s.value(QStringLiteral("ranks"), 1).toInt());
    threadsSpin_->setValue(s.value(QStringLiteral("threads"), 1).toInt());
    outdirMode_->setCurrentIndex(s.value(QStringLiteral("outmode"), 0).toInt());
    outdirEdit_->setText(s.value(QStringLiteral("outdir")).toString());
    outdirEdit_->setEnabled(outdirMode_->currentIndex() == 1);
    extraEdit_->setText(s.value(QStringLiteral("extra")).toString());
}

void FlowGuiWindow::saveSettings()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    s.setValue(QStringLiteral("ranks"),   ranksSpin_->value());
    s.setValue(QStringLiteral("threads"), threadsSpin_->value());
    s.setValue(QStringLiteral("outmode"), outdirMode_->currentIndex());
    s.setValue(QStringLiteral("outdir"),  outdirEdit_->text());
    s.setValue(QStringLiteral("extra"),   extraEdit_->text());
}

void FlowGuiWindow::closeEvent(QCloseEvent* ev)
{
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
        for (const QUrl& u : ev->mimeData()->urls())
            if (u.toLocalFile().endsWith(QStringLiteral(".data"), Qt::CaseInsensitive)) {
                ev->acceptProposedAction();
                return;
            }
    }
}

void FlowGuiWindow::dropEvent(QDropEvent* ev)
{
    QStringList files;
    for (const QUrl& u : ev->mimeData()->urls()) {
        const QString f = u.toLocalFile();
        if (f.endsWith(QStringLiteral(".data"), Qt::CaseInsensitive)) files << f;
    }
    addDecks(files);
}

void FlowGuiWindow::onAddDecks()
{
    addDecks(QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select input deck(s)"), QString(),
        QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)")));
}

void FlowGuiWindow::addDecks(const QStringList& files)
{
    for (const QString& f : files) {
        Job j; j.deck = QDir::toNativeSeparators(f);
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
                    outdirEdit_->text());
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setOption(QFileDialog::ShowDirsOnly);
    dlg.setOption(QFileDialog::DontUseNativeDialog);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedFiles().isEmpty()) {
        const QString d = dlg.selectedFiles().first();
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
    QString clean = text;
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
    stopBtn_->setEnabled(on);
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
void FlowGuiWindow::onRun()
{
    if (proc_ && proc_->state() != QProcess::NotRunning) return;
    bool anyQueued = false;
    for (const Job& j : jobs_) anyQueued |= (j.state == Job::Queued);
    if (!anyQueued) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("Add at least one input deck (*.DATA) to the queue first."));
        return;
    }
    if (exePath_.isEmpty() || !QFileInfo::exists(exePath_)) {
        exePath_ = findFlowExe();
        if (exePath_.isEmpty()) {
            QMessageBox::critical(this, QLatin1String(kAppName),
                QStringLiteral("No flow executable was found next to this program.\n"
                               "Reinstall the package or place flow(.exe) beside the GUI."));
            return;
        }
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
            if (jobs_[i].state == Job::Queued) { current_ = i; break; }

    if (current_ < 0) {                       // queue finished
        int ok = 0, fail = 0;
        for (const Job& j : jobs_) {
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
    proc_->start(program, args);
}

void FlowGuiWindow::stopCurrentJob()
{
    aborted_ = true;
    if (!proc_ || proc_->state() == QProcess::NotRunning) return;
    appendLog(QStringLiteral("\n*** stopping current job ***\n"));
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("taskkill"),
        { QStringLiteral("/PID"), QString::number(proc_->processId()),
          QStringLiteral("/T"), QStringLiteral("/F") });
#else
    ::kill(-static_cast<pid_t>(proc_->processId()), SIGKILL);
#endif
}

// ---------------------------------------------------------------------------
void FlowGuiWindow::openJobFolder(int row)
{
    if (row < 0 || row >= jobs_.size()) return;
    const QString d = jobs_[row].outdir.isEmpty()
        ? QFileInfo(jobs_[row].deck).absolutePath() : jobs_[row].outdir;
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
}

void FlowGuiWindow::viewJobPrt(int row)
{
    if (row < 0 || row >= jobs_.size()) return;
    const Job& j = jobs_[row];
    const QFileInfo deckInfo(j.deck);
    const QString prt = (j.outdir.isEmpty()
        ? deckInfo.absolutePath() : j.outdir) + '/' + deckInfo.completeBaseName() + ".PRT";
    QFile f(prt);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("No PRT file yet at\n%1").arg(QDir::toNativeSeparators(prt)));
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
    lay->addWidget(view);
    dlg->show();
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
    }
    tray_->show();
    tray_->showMessage(QStringLiteral("OPM Flow"),
        QStringLiteral("Queue complete: %1 ok, %2 failed").arg(okCount).arg(failCount),
        failCount ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
        10000);
}
