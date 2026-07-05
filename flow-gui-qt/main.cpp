/*
  flow-gui-qt - a cross-platform (Windows / Linux / macOS) Qt 6 GUI front
  end for running OPM Flow simulations. Functional parity with the FLTK
  flow-gui in this repository, inspired by the basic functionality of
  OPMRUN (https://github.com/OPM/opm-utilities/tree/master/opmrun):

    - queue up one or more input decks (*.DATA)
    - choose the flow executable, MPI rank count, OpenMP threads,
      output directory policy and extra command line options
    - run the queue sequentially with the live simulator log streamed
      into the window; stop the running job (kills the whole MPI tree)

  Settings (paths, ranks, options) persist between sessions via QSettings.

  This file is part of the opm_flow_windows build harness and is licensed
  under the GNU General Public License v3+ like the OPM project itself.
*/

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStyleHints>
#include <QTextCursor>
#include <QVBoxLayout>

#include <cstdio>
#include <cstring>

#if !defined(Q_OS_WIN)
  #include <signal.h>
  #include <unistd.h>
#endif

static const char* kAppName = "flow-gui-qt";
static const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------
class FlowGuiWindow : public QMainWindow
{
public:
    FlowGuiWindow();

protected:
    void closeEvent(QCloseEvent* ev) override;

private:
    // widgets
    QLineEdit*      exeEdit_      = nullptr;
    QListWidget*    queueList_    = nullptr;
    QSpinBox*       ranksSpin_    = nullptr;
    QSpinBox*       threadsSpin_  = nullptr;
    QComboBox*      outdirMode_   = nullptr;
    QLineEdit*      outdirEdit_   = nullptr;
    QPushButton*    outdirBrowse_ = nullptr;
    QLineEdit*      extraEdit_    = nullptr;
    QPushButton*    runBtn_       = nullptr;
    QPushButton*    stopBtn_      = nullptr;
    QPlainTextEdit* logView_      = nullptr;

    // run state
    QProcess*   proc_    = nullptr;
    QStringList pending_;         // decks still to run
    int         jobNo_   = 0;
    int         jobCount_ = 0;
    bool        aborted_ = false;

    // helpers
    static QString defaultFlowExe();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString& text);
    void setRunning(bool on);
    void startNextJob();
    void stopCurrentJob();

    // slots (connected via lambdas; no moc needed for a Q_OBJECT-free class)
    void onAddDecks();
    void onBrowseExe();
    void onBrowseOutdir();
    void onRun();
};

// Find a plausible default flow executable next to / above this program.
QString FlowGuiWindow::defaultFlowExe()
{
#ifdef Q_OS_WIN
    const QString exeName = QStringLiteral("flow.exe");
#else
    const QString exeName = QStringLiteral("flow");
#endif
    const char* candidates[] = {
        "build-mpi/opm-simulators/bin", "build/opm-simulators/bin", "."
    };
    QDir base(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 4; ++up) {
        for (const char* c : candidates) {
            const QString p = base.filePath(QString::fromLatin1(c) + '/' + exeName);
            if (QFileInfo::exists(p)) return QDir::cleanPath(p);
        }
        if (!base.cdUp()) break;
    }
    return exeName;   // hope for PATH
}

FlowGuiWindow::FlowGuiWindow()
{
    setWindowTitle(QStringLiteral("OPM Flow GUI (Qt)"));
    resize(950, 680);

    auto* central = new QWidget(this);
    auto* top = new QVBoxLayout(central);

    // --- simulator row ---------------------------------------------------
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Simulator:")));
        exeEdit_ = new QLineEdit;
        row->addWidget(exeEdit_, 1);
        auto* b = new QPushButton(QStringLiteral("Browse..."));
        connect(b, &QPushButton::clicked, this, [this] { onBrowseExe(); });
        row->addWidget(b);
        top->addLayout(row);
    }

    // --- job queue ---------------------------------------------------------
    {
        auto* box = new QGroupBox(QStringLiteral("Job queue"));
        auto* row = new QHBoxLayout(box);
        queueList_ = new QListWidget;
        queueList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        row->addWidget(queueList_, 1);

        auto* col = new QVBoxLayout;
        auto* badd = new QPushButton(QStringLiteral("Add deck..."));
        auto* brem = new QPushButton(QStringLiteral("Remove"));
        auto* bclr = new QPushButton(QStringLiteral("Clear"));
        connect(badd, &QPushButton::clicked, this, [this] { onAddDecks(); });
        connect(brem, &QPushButton::clicked, this, [this] {
            for (auto* it : queueList_->selectedItems()) delete it;
        });
        connect(bclr, &QPushButton::clicked, this, [this] { queueList_->clear(); });
        col->addWidget(badd); col->addWidget(brem); col->addWidget(bclr);
        col->addStretch(1);
        row->addLayout(col);
        top->addWidget(box);
    }

    // --- options ------------------------------------------------------------
    {
        auto* box  = new QGroupBox(QStringLiteral("Run options"));
        auto* grid = new QGridLayout(box);

        grid->addWidget(new QLabel(QStringLiteral("MPI ranks:")), 0, 0);
        ranksSpin_ = new QSpinBox;
        ranksSpin_->setRange(1, 256);
        grid->addWidget(ranksSpin_, 0, 1);

        grid->addWidget(new QLabel(QStringLiteral("OMP threads:")), 0, 2);
        threadsSpin_ = new QSpinBox;
        threadsSpin_->setRange(1, 64);
        grid->addWidget(threadsSpin_, 0, 3);

        grid->addWidget(new QLabel(QStringLiteral("Output:")), 0, 4);
        outdirMode_ = new QComboBox;
        outdirMode_->addItem(QStringLiteral("next to deck (<deck>_run)"));
        outdirMode_->addItem(QStringLiteral("custom directory"));
        grid->addWidget(outdirMode_, 0, 5);
        grid->setColumnStretch(5, 1);

        grid->addWidget(new QLabel(QStringLiteral("Out dir:")), 1, 0);
        outdirEdit_ = new QLineEdit;
        outdirEdit_->setEnabled(false);
        grid->addWidget(outdirEdit_, 1, 1, 1, 4);
        outdirBrowse_ = new QPushButton(QStringLiteral("Browse..."));
        grid->addWidget(outdirBrowse_, 1, 5);
        connect(outdirMode_, &QComboBox::currentIndexChanged, this, [this](int i) {
            outdirEdit_->setEnabled(i == 1);
        });
        connect(outdirBrowse_, &QPushButton::clicked, this, [this] { onBrowseOutdir(); });

        grid->addWidget(new QLabel(QStringLiteral("Extra options:")), 2, 0);
        extraEdit_ = new QLineEdit;
        extraEdit_->setToolTip(QStringLiteral(
            "additional flow command line options, e.g. --linear-solver=ilu0"));
        grid->addWidget(extraEdit_, 2, 1, 1, 5);

        top->addWidget(box);
    }

    // --- run / stop / clear ---------------------------------------------------
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

    // --- log ---------------------------------------------------------------------
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(200000);       // keep memory bounded
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    logView_->setFont(mono);
    top->addWidget(logView_, 1);

    setCentralWidget(central);

    loadSettings();
    if (exeEdit_->text().isEmpty()) exeEdit_->setText(defaultFlowExe());

    appendLog(QString::fromLatin1("%1 %2 - queue OPM Flow simulations and watch them run.\n")
                  .arg(QLatin1String(kAppName), QLatin1String(kVersion)));
}

// -- settings persistence ------------------------------------------------------
void FlowGuiWindow::loadSettings()
{
    QSettings s(QStringLiteral("OPM"), QLatin1String(kAppName));
    exeEdit_->setText(s.value(QStringLiteral("exe")).toString());
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
    s.setValue(QStringLiteral("exe"),     exeEdit_->text());
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

// -- log ---------------------------------------------------------------------
void FlowGuiWindow::appendLog(const QString& text)
{
    // appendPlainText adds a newline per call; insert raw instead
    logView_->moveCursor(QTextCursor::End);
    QString clean = text;
    clean.remove(QLatin1Char('\r'));
    logView_->insertPlainText(clean);
    logView_->moveCursor(QTextCursor::End);
}

void FlowGuiWindow::setRunning(bool on)
{
    runBtn_->setEnabled(!on);
    stopBtn_->setEnabled(on);
}

// -- callbacks -----------------------------------------------------------------
void FlowGuiWindow::onBrowseExe()
{
#ifdef Q_OS_WIN
    const QString filter = QStringLiteral("Programs (*.exe);;All files (*)");
#else
    const QString filter = QStringLiteral("All files (*)");
#endif
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select the flow executable"),
        QFileInfo(exeEdit_->text()).absolutePath(), filter);
    if (!f.isEmpty()) exeEdit_->setText(QDir::toNativeSeparators(f));
}

void FlowGuiWindow::onAddDecks()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select input deck(s)"), QString(),
        QStringLiteral("Eclipse decks (*.DATA *.data);;All files (*)"));
    for (const QString& f : files)
        queueList_->addItem(QDir::toNativeSeparators(f));
}

void FlowGuiWindow::onBrowseOutdir()
{
    // Use Qt's own directory dialog rather than the native one: it has a
    // guaranteed, always-visible "New Folder" button (top-right toolbar) on
    // every platform, and new folders can also be created by typing a name.
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

// -- running ---------------------------------------------------------------------
void FlowGuiWindow::onRun()
{
    if (proc_ && proc_->state() != QProcess::NotRunning) return;
    if (queueList_->count() == 0) {
        QMessageBox::information(this, QLatin1String(kAppName),
            QStringLiteral("Add at least one input deck (*.DATA) to the queue first."));
        return;
    }
    pending_.clear();
    for (int i = 0; i < queueList_->count(); ++i)
        pending_ << queueList_->item(i)->text();
    jobNo_    = 0;
    jobCount_ = pending_.size();
    aborted_  = false;
    setRunning(true);
    saveSettings();
    startNextJob();
}

void FlowGuiWindow::startNextJob()
{
    if (aborted_ || pending_.isEmpty()) {
        appendLog(aborted_ ? QStringLiteral("\n*** queue aborted ***\n")
                           : QStringLiteral("\n*** queue complete ***\n"));
        setRunning(false);
        return;
    }

    const QString deck = pending_.takeFirst();
    ++jobNo_;

    const QFileInfo deckInfo(deck);
    QString outdir;
    if (outdirMode_->currentIndex() == 1 && !outdirEdit_->text().isEmpty()) {
        outdir = outdirEdit_->text();
    } else {
        outdir = deckInfo.absolutePath() + '/' + deckInfo.completeBaseName() + "_run";
    }
    if (!QDir().mkpath(outdir)) {
        appendLog(QStringLiteral("FAILED to create output directory %1\n").arg(outdir));
        startNextJob();
        return;
    }

    const int ranks   = ranksSpin_->value();
    const int threads = threadsSpin_->value();

    QString     program;
    QStringList args;
    if (ranks > 1) {
        program = QStringLiteral("mpiexec");
        args << QStringLiteral("-n") << QString::number(ranks) << exeEdit_->text();
    } else {
        program = exeEdit_->text();
    }
    args << deck << (QStringLiteral("--output-dir=") + outdir);
    if (threads > 1)
        args << (QStringLiteral("--threads-per-process=") + QString::number(threads));
    const QString extra = extraEdit_->text().trimmed();
    if (!extra.isEmpty())
        args << extra.split(QRegularExpression(QStringLiteral("\\s+")),
                            Qt::SkipEmptyParts);

    appendLog(QStringLiteral("\n==================== job %1/%2 ====================\n"
                             "%3 %4\noutput directory: %5\n\n")
                  .arg(jobNo_).arg(jobCount_)
                  .arg(program, args.join(QLatin1Char(' ')), outdir));

    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    proc_->setWorkingDirectory(deckInfo.absolutePath());
#if !defined(Q_OS_WIN)
    // Own process group so "Stop job" can signal the whole MPI tree.
    proc_->setChildProcessModifier([] { ::setpgid(0, 0); });
#endif

    connect(proc_, &QProcess::readyRead, this, [this] {
        appendLog(QString::fromLocal8Bit(proc_->readAll()));
    });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        appendLog(QStringLiteral("FAILED to start: %1\n").arg(proc_->errorString()));
    });
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        appendLog(QStringLiteral("\n---- job %1 finished, exit code %2%3 ----\n")
                      .arg(jobNo_).arg(code)
                      .arg(st == QProcess::CrashExit
                               ? QStringLiteral(" (terminated)") : QString()));
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
    // Kill the whole tree (mpiexec + ranks): taskkill /T terminates children.
    QProcess::startDetached(QStringLiteral("taskkill"),
        { QStringLiteral("/PID"), QString::number(proc_->processId()),
          QStringLiteral("/T"), QStringLiteral("/F") });
#else
    // The child leads its own process group (see setChildProcessModifier).
    ::kill(-static_cast<pid_t>(proc_->processId()), SIGKILL);
#endif
    // QProcess::finished fires once the process exits; the queue then stops
    // because aborted_ is set.
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    // --version: print and exit (headless smoke test, no QApplication needed)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s %s\n", kAppName, kVersion);
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("OPM"));
    QApplication::setApplicationName(QLatin1String(kAppName));

    // Bright, platform-independent appearance: do not follow a dark system
    // color scheme; use the Fusion style with an explicit light palette.
    app.setStyle(QStringLiteral("Fusion"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QPalette pal;
    pal.setColor(QPalette::Window,          QColor(0xf4, 0xf6, 0xf8));
    pal.setColor(QPalette::WindowText,      Qt::black);
    pal.setColor(QPalette::Base,            Qt::white);
    pal.setColor(QPalette::AlternateBase,   QColor(0xec, 0xf0, 0xf4));
    pal.setColor(QPalette::Text,            Qt::black);
    pal.setColor(QPalette::Button,          QColor(0xe8, 0xec, 0xf0));
    pal.setColor(QPalette::ButtonText,      Qt::black);
    pal.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xe1));
    pal.setColor(QPalette::ToolTipText,     Qt::black);
    pal.setColor(QPalette::Highlight,       QColor(0x2f, 0x6f, 0xd0));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, QColor(0x80, 0x88, 0x90));
    pal.setColor(QPalette::Disabled, QPalette::Text,       QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0xa0, 0xa6));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0xa0, 0xa6));
    app.setPalette(pal);

    FlowGuiWindow win;
    win.show();
    return app.exec();
}
