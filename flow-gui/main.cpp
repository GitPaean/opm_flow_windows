/*
  flow-gui - a small cross-platform (Windows / Linux) GUI front end for
  running OPM Flow simulations, inspired by the basic functionality of
  OPMRUN (https://github.com/OPM/opm-utilities/tree/master/opmrun):

    - queue up one or more input decks (*.DATA)
    - choose the flow executable, MPI rank count, OpenMP threads,
      output directory policy and extra command line options
    - run the queue sequentially with the live simulator log streamed
      into the window; abort the running job (kills the whole MPI tree)

  Toolkit: FLTK 1.3/1.4 (vcpkg `fltk` on Windows, libfltk1.3-dev on Linux).

  This file is part of the opm_flow_windows build harness and is licensed
  under the GNU General Public License v3+ like the OPM project itself.
*/

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

static const char* kAppTitle = "OPM Flow GUI";
static const char* kVersion  = "0.1.0";

// ---------------------------------------------------------------------------
// Cross-platform child process with merged stdout/stderr pipe and hard kill.
// On Windows the child is placed in a Job Object with KILL_ON_JOB_CLOSE so
// that terminating a run also terminates every MPI rank mpiexec spawned; on
// POSIX the child leads its own process group and we signal the whole group.
// ---------------------------------------------------------------------------
class ChildProcess
{
public:
    // argv[0] is the program. Returns false + message on spawn failure.
    bool start(const std::vector<std::string>& argv,
               const std::string& workdir,
               std::string& error)
    {
#ifdef _WIN32
        std::string cmdline;
        for (const auto& a : argv) {
            if (!cmdline.empty()) cmdline += ' ';
            // quote arguments containing spaces (paths); embedded quotes are
            // not expected in deck paths or options
            if (a.find(' ') != std::string::npos) cmdline += '"' + a + '"';
            else                                  cmdline += a;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) { error = "CreatePipe failed"; return false; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError  = wr;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<char> cl(cmdline.begin(), cmdline.end());
        cl.push_back('\0');

        job_ = CreateJobObjectA(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
        }

        const BOOL ok = CreateProcessA(
            nullptr, cl.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);

        CloseHandle(wr);   // child owns the write end now
        if (!ok) {
            CloseHandle(rd);
            error = "CreateProcess failed (" + std::to_string(GetLastError())
                  + ") for: " + cmdline;
            return false;
        }
        if (job_) AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        pipe_ = rd;
        proc_ = pi.hProcess;
        return true;
#else
        int fds[2];
        if (pipe(fds) != 0) { error = "pipe() failed"; return false; }

        const pid_t pid = fork();
        if (pid < 0) { error = "fork() failed"; close(fds[0]); close(fds[1]); return false; }

        if (pid == 0) {                    // child
            setpgid(0, 0);                 // own process group -> killable tree
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]); close(fds[1]);
            if (!workdir.empty()) { if (chdir(workdir.c_str()) != 0) _exit(127); }
            std::vector<char*> cargv;
            for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            std::perror("execvp");
            _exit(127);
        }
        close(fds[1]);
        pipe_ = fds[0];
        pid_  = pid;
        return true;
#endif
    }

    // Blocking read of the next output chunk; returns false on EOF.
    bool read(std::string& chunk)
    {
        char buf[4096];
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(pipe_, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
        chunk.assign(buf, buf + n);
        return true;
#else
        const ssize_t n = ::read(pipe_, buf, sizeof(buf));
        if (n <= 0) return false;
        chunk.assign(buf, buf + n);
        return true;
#endif
    }

    // Wait for exit; returns the exit code (-1 if it could not be determined).
    int wait()
    {
#ifdef _WIN32
        WaitForSingleObject(proc_, INFINITE);
        DWORD code = static_cast<DWORD>(-1);
        GetExitCodeProcess(proc_, &code);
        return static_cast<int>(code);
#else
        int status = 0;
        waitpid(pid_, &status, 0);
        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
#endif
    }

    // Kill the whole process tree (mpiexec + ranks).
    void kill()
    {
#ifdef _WIN32
        if (job_)  { CloseHandle(job_); job_ = nullptr; }   // KILL_ON_JOB_CLOSE
        else if (proc_) TerminateProcess(proc_, 1);
#else
        if (pid_ > 0) ::kill(-pid_, SIGKILL);
#endif
    }

    ~ChildProcess()
    {
#ifdef _WIN32
        if (pipe_) CloseHandle(pipe_);
        if (proc_) CloseHandle(proc_);
        if (job_)  CloseHandle(job_);
#else
        if (pipe_ >= 0) close(pipe_);
#endif
    }

private:
#ifdef _WIN32
    HANDLE pipe_ = nullptr;
    HANDLE proc_ = nullptr;
    HANDLE job_  = nullptr;
#else
    int   pipe_ = -1;
    pid_t pid_  = -1;
#endif
};

// ---------------------------------------------------------------------------
// The application window.
// ---------------------------------------------------------------------------
class FlowGui
{
public:
    FlowGui();
    void show() { win_->show(); }

private:
    // -- widgets --------------------------------------------------------
    std::unique_ptr<Fl_Double_Window> win_;
    Fl_Input*        exe_in_      = nullptr;
    Fl_Browser*      queue_       = nullptr;
    Fl_Spinner*      ranks_       = nullptr;
    Fl_Spinner*      threads_     = nullptr;
    Fl_Choice*       outdir_mode_ = nullptr;
    Fl_Input*        outdir_in_   = nullptr;
    Fl_Input*        extra_in_    = nullptr;
    Fl_Button*       run_btn_     = nullptr;
    Fl_Button*       stop_btn_    = nullptr;
    Fl_Text_Display* log_view_    = nullptr;
    Fl_Text_Buffer*  log_buf_     = nullptr;

    // -- run state ------------------------------------------------------
    std::thread              worker_;
    std::atomic<bool>        running_{false};
    std::atomic<bool>        abort_{false};
    ChildProcess*            child_ = nullptr;    // owned by worker
    std::mutex               child_mtx_;

    // pending log text handed from worker to GUI thread
    std::mutex               log_mtx_;
    std::string              log_pending_;

    // -- helpers --------------------------------------------------------
    static std::string defaultFlowExe();
    void append_log(const std::string& text);        // any thread
    void flush_log();                                 // GUI thread
    void set_running(bool on);                        // GUI thread
    void run_queue();                                 // worker thread

    // -- callbacks (static trampolines) ---------------------------------
    static void cb_add(Fl_Widget*, void* p);
    static void cb_remove(Fl_Widget*, void* p);
    static void cb_clear(Fl_Widget*, void* p);
    static void cb_browse_exe(Fl_Widget*, void* p);
    static void cb_browse_outdir(Fl_Widget*, void* p);
    static void cb_run(Fl_Widget*, void* p);
    static void cb_stop(Fl_Widget*, void* p);
    static void cb_clearlog(Fl_Widget*, void* p);
    static void cb_awake(void* p);                    // Fl::awake target
};

// Find a plausible default flow executable next to / above this program.
std::string FlowGui::defaultFlowExe()
{
#ifdef _WIN32
    const char* exe_name = "flow.exe";
#else
    const char* exe_name = "flow";
#endif
    // typical locations relative to a build harness checkout
    const char* candidates[] = {
        "build-mpi/opm-simulators/bin", "build/opm-simulators/bin", "."
    };
    std::error_code ec;
    for (int up = 0; up < 4; ++up) {
        fs::path base = fs::current_path(ec);
        for (int i = 0; i < up; ++i) base = base.parent_path();
        for (const char* c : candidates) {
            const fs::path p = base / c / exe_name;
            if (fs::exists(p, ec)) return p.string();
        }
    }
    return exe_name;   // hope for PATH
}

FlowGui::FlowGui()
{
    const int W = 900, H = 640;
    win_ = std::make_unique<Fl_Double_Window>(W, H, kAppTitle);

    int y = 10;

    // --- simulator executable row --------------------------------------
    exe_in_ = new Fl_Input(120, y, W - 240, 26, "Simulator:");
    exe_in_->value(defaultFlowExe().c_str());
    auto* bexe = new Fl_Button(W - 110, y, 100, 26, "Browse...");
    bexe->callback(cb_browse_exe, this);
    y += 34;

    // --- job queue ------------------------------------------------------
    queue_ = new Fl_Browser(120, y, W - 240, 110, "Job queue:");
    queue_->align(FL_ALIGN_LEFT_TOP);
    queue_->type(FL_HOLD_BROWSER);
    {
        auto* badd = new Fl_Button(W - 110, y,      100, 26, "Add deck...");
        auto* brem = new Fl_Button(W - 110, y + 32, 100, 26, "Remove");
        auto* bclr = new Fl_Button(W - 110, y + 64, 100, 26, "Clear");
        badd->callback(cb_add, this);
        brem->callback(cb_remove, this);
        bclr->callback(cb_clear, this);
    }
    y += 120;

    // --- options ---------------------------------------------------------
    ranks_ = new Fl_Spinner(120, y, 70, 26, "MPI ranks:");
    ranks_->minimum(1); ranks_->maximum(256); ranks_->value(1);

    threads_ = new Fl_Spinner(330, y, 70, 26, "OMP threads:");
    threads_->minimum(1); threads_->maximum(64); threads_->value(1);

    outdir_mode_ = new Fl_Choice(510, y, 170, 26, "Output:");
    outdir_mode_->add("next to deck (<deck>_run)");
    outdir_mode_->add("custom directory");
    outdir_mode_->value(0);
    y += 34;

    outdir_in_ = new Fl_Input(120, y, W - 240, 26, "Out dir:");
    outdir_in_->deactivate();
    auto* bout = new Fl_Button(W - 110, y, 100, 26, "Browse...");
    bout->callback(cb_browse_outdir, this);
    outdir_mode_->callback([](Fl_Widget* w, void* p) {
        auto* self = static_cast<FlowGui*>(p);
        if (static_cast<Fl_Choice*>(w)->value() == 1) self->outdir_in_->activate();
        else                                          self->outdir_in_->deactivate();
    }, this);
    y += 34;

    extra_in_ = new Fl_Input(120, y, W - 240, 26, "Extra options:");
    extra_in_->tooltip("additional flow command line options, e.g. --linear-solver=ilu0");
    y += 40;

    // --- run / stop / clear-log ------------------------------------------
    run_btn_  = new Fl_Button(120, y, 120, 30, "Run queue");
    stop_btn_ = new Fl_Button(250, y, 120, 30, "Stop job");
    auto* bcl = new Fl_Button(380, y, 120, 30, "Clear log");
    run_btn_->callback(cb_run, this);
    stop_btn_->callback(cb_stop, this);
    stop_btn_->deactivate();
    bcl->callback(cb_clearlog, this);
    y += 40;

    // --- log view ---------------------------------------------------------
    log_buf_  = new Fl_Text_Buffer();
    log_view_ = new Fl_Text_Display(10, y, W - 20, H - y - 10);
    log_view_->buffer(log_buf_);
    log_view_->textfont(FL_COURIER);
    log_view_->textsize(12);

    win_->resizable(log_view_);
    win_->end();

    append_log(std::string(kAppTitle) + " " + kVersion +
               " - queue OPM Flow simulations and watch them run.\n");
    flush_log();
}

// -- log plumbing -----------------------------------------------------------
void FlowGui::append_log(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lk(log_mtx_);
        log_pending_ += text;
    }
    Fl::awake(cb_awake, this);
}

void FlowGui::cb_awake(void* p)
{
    static_cast<FlowGui*>(p)->flush_log();
}

void FlowGui::flush_log()
{
    std::string t;
    {
        std::lock_guard<std::mutex> lk(log_mtx_);
        t.swap(log_pending_);
    }
    if (t.empty()) return;
    // normalize CRLF so the display does not show ^M artifacts
    std::string clean; clean.reserve(t.size());
    for (char c : t) if (c != '\r') clean.push_back(c);
    log_buf_->append(clean.c_str());
    log_view_->insert_position(log_buf_->length());
    log_view_->show_insert_position();
}

void FlowGui::set_running(bool on)
{
    if (on) { run_btn_->deactivate(); stop_btn_->activate(); }
    else    { run_btn_->activate();   stop_btn_->deactivate(); }
}

// -- queue callbacks ----------------------------------------------------------
void FlowGui::cb_add(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    Fl_Native_File_Chooser fc;
    fc.title("Select input deck(s)");
    fc.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
    fc.filter("Eclipse decks\t*.{DATA,data}");
    if (fc.show() == 0) {
        for (int i = 0; i < fc.count(); ++i)
            self->queue_->add(fc.filename(i));
    }
}

void FlowGui::cb_remove(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    const int line = self->queue_->value();
    if (line > 0) self->queue_->remove(line);
}

void FlowGui::cb_clear(Fl_Widget*, void* p)
{
    static_cast<FlowGui*>(p)->queue_->clear();
}

void FlowGui::cb_browse_exe(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    Fl_Native_File_Chooser fc;
    fc.title("Select the flow executable");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
#ifdef _WIN32
    fc.filter("Programs\t*.exe");
#endif
    if (fc.show() == 0) self->exe_in_->value(fc.filename());
}

void FlowGui::cb_browse_outdir(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    Fl_Native_File_Chooser fc;
    fc.title("Select output base directory");
    fc.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    if (fc.show() == 0) {
        self->outdir_in_->value(fc.filename());
        self->outdir_mode_->value(1);
        self->outdir_in_->activate();
    }
}

void FlowGui::cb_clearlog(Fl_Widget*, void* p)
{
    static_cast<FlowGui*>(p)->log_buf_->text("");
}

// -- running ---------------------------------------------------------------
void FlowGui::cb_run(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    if (self->running_) return;
    if (self->queue_->size() == 0) {
        fl_message("Add at least one input deck (*.DATA) to the queue first.");
        return;
    }
    self->abort_ = false;
    self->running_ = true;
    self->set_running(true);
    if (self->worker_.joinable()) self->worker_.join();
    self->worker_ = std::thread([self] { self->run_queue(); });
}

void FlowGui::cb_stop(Fl_Widget*, void* p)
{
    auto* self = static_cast<FlowGui*>(p);
    self->abort_ = true;
    std::lock_guard<std::mutex> lk(self->child_mtx_);
    if (self->child_) {
        self->append_log("\n*** stopping current job ***\n");
        self->child_->kill();
    }
}

void FlowGui::run_queue()
{
    // Snapshot the queue and options under the FLTK lock: widgets belong to
    // the GUI thread, so cross-thread reads must be bracketed by Fl::lock().
    Fl::lock();
    std::vector<std::string> decks;
    for (int i = 1; i <= queue_->size(); ++i) decks.push_back(queue_->text(i));

    const std::string exe     = exe_in_->value();
    const int         ranks   = static_cast<int>(ranks_->value());
    const int         threads = static_cast<int>(threads_->value());
    const bool        custom  = outdir_mode_->value() == 1;
    const std::string outbase = outdir_in_->value();
    const std::string extra   = extra_in_->value();
    Fl::unlock();

    int jobno = 0;
    for (const auto& deck : decks) {
        if (abort_) break;
        ++jobno;

        const fs::path deckp(deck);
        std::string outdir;
        if (custom && !outbase.empty()) {
            outdir = (fs::path(outbase) / deckp.stem()).string();
        } else {
            outdir = (deckp.parent_path() / (deckp.stem().string() + "_run")).string();
        }

        std::vector<std::string> argv;
        if (ranks > 1) {
            argv.push_back("mpiexec");
            argv.push_back("-n");
            argv.push_back(std::to_string(ranks));
        }
        argv.push_back(exe);
        argv.push_back(deck);
        argv.push_back("--output-dir=" + outdir);
        if (threads > 1)
            argv.push_back("--threads-per-process=" + std::to_string(threads));
        if (!extra.empty()) {
            // naive whitespace split of the extra options
            std::string cur;
            for (char c : extra) {
                if (c == ' ' || c == '\t') { if (!cur.empty()) { argv.push_back(cur); cur.clear(); } }
                else cur.push_back(c);
            }
            if (!cur.empty()) argv.push_back(cur);
        }

        std::string cmd_show;
        for (const auto& a : argv) { cmd_show += a; cmd_show += ' '; }
        append_log("\n==================== job " + std::to_string(jobno) + "/" +
                   std::to_string(decks.size()) + " ====================\n" +
                   cmd_show + "\n\n");

        auto child = std::make_unique<ChildProcess>();
        std::string err;
        if (!child->start(argv, deckp.parent_path().string(), err)) {
            append_log("FAILED to start: " + err + "\n");
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(child_mtx_);
            child_ = child.get();
        }

        std::string chunk;
        while (child->read(chunk)) append_log(chunk);
        const int code = child->wait();

        {
            std::lock_guard<std::mutex> lk(child_mtx_);
            child_ = nullptr;
        }
        append_log("\n---- job " + std::to_string(jobno) + " finished, exit code " +
                   std::to_string(code) + " ----\n");
    }

    if (abort_) append_log("\n*** queue aborted ***\n");
    else        append_log("\n*** queue complete ***\n");

    running_ = false;
    Fl::awake([](void* p) { static_cast<FlowGui*>(p)->set_running(false); }, this);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    // --version: print and exit (also used as a headless smoke test)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s %s\n", kAppTitle, kVersion);
            return 0;
        }
    }

    Fl::lock();   // enable FLTK multithread support (Fl::awake from workers)
    FlowGui gui;
    gui.show();
    return Fl::run();
}
