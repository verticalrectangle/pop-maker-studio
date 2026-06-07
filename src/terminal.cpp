#include "terminal.h"
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <vector>

static int cb_damage    (VTermRect, void*)              { return 1; }
static int cb_moverect  (VTermRect, VTermRect, void*)   { return 1; }
static int cb_movecursor(VTermPos, VTermPos, int, void*){ return 1; }
static int cb_settermprop(VTermProp, VTermValue*, void*){ return 1; }
static int cb_bell      (void*)                        { return 1; }
static int cb_resize    (int, int, void*)              { return 1; }
static int cb_sb_pushline(int, const VTermScreenCell*, void*) { return 1; }
static int cb_sb_popline (int, VTermScreenCell*, void*)       { return 1; }
static int cb_sb_clear   (void*)                              { return 1; }

static const VTermScreenCallbacks k_screen_cbs = {
    cb_damage, cb_moverect, cb_movecursor, cb_settermprop,
    cb_bell, cb_resize, cb_sb_pushline, cb_sb_popline, cb_sb_clear
};

// libvterm queues terminal responses (Primary/Secondary DA, cursor position
// report, device status, OSC color queries, etc.) and expects us to deliver
// them back to the pty. Without this, fish/zsh hang on startup waiting for
// the Primary DA reply and disable optional features (custom prompt glyphs,
// title queries, true-color probing) when they time out.
static void cb_output(const char* s, size_t len, void* user) {
    TerminalState* t = (TerminalState*)user;
    if (t->pty_master >= 0)
        (void)write(t->pty_master, s, len);
}

void terminal_init(TerminalState& t, int cols, int rows) {
    t.cols = cols;
    t.rows = rows;

    t.vt = vterm_new(rows, cols);
    if (!t.vt) return;
    vterm_set_utf8(t.vt, 1);
    vterm_output_set_callback(t.vt, cb_output, &t);
    t.vts = vterm_obtain_screen(t.vt);
    vterm_screen_set_callbacks(t.vts, &k_screen_cbs, nullptr);
    vterm_screen_set_damage_merge(t.vts, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(t.vts, 1);

    // Resolve shell and build env array in the parent, before any fork.
    // vfork() requires that the child only calls execve/_exit — no malloc,
    // no setenv. We prepare everything here so the child path is trivial.
    const char* shell = getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/sh";

    // Copy environ, replacing or appending TERM=xterm-256color.
    extern char** environ;
    std::vector<const char*> envp;
    bool term_found = false;
    for (char** e = environ; e && *e; ++e) {
        if (strncmp(*e, "TERM=", 5) == 0) {
            envp.push_back("TERM=xterm-256color");
            term_found = true;
        } else {
            envp.push_back(*e);
        }
    }
    if (!term_found) envp.push_back("TERM=xterm-256color");
    envp.push_back(nullptr);

    const char* argv[] = { shell, nullptr };

    struct winsize ws = {};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    // openpty + vfork avoids fork-safety issues with OpenGL/audio threads.
    // vfork suspends the parent until the child exec's, so all pointers
    // (shell, argv, envp) remain valid in the child without any copy.
    int master_fd = -1, slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) < 0) {
        vterm_free(t.vt);
        t.vt = nullptr; t.vts = nullptr;
        return;
    }
    fcntl(master_fd, F_SETFD, FD_CLOEXEC);

    t.child_pid = vfork();
    if (t.child_pid == 0) {
        // Child: only async-signal-safe / vfork-safe calls until execve.
        close(master_fd);
        setsid();
        ioctl(slave_fd, TIOCSCTTY, 0);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);
        execve(shell, (char* const*)argv, (char* const*)envp.data());
        _exit(127);
    }

    // Parent resumes here after child exec's.
    close(slave_fd);

    if (t.child_pid < 0) {
        close(master_fd);
        vterm_free(t.vt);
        t.vt = nullptr; t.vts = nullptr;
        return;
    }

    t.pty_master = master_fd;
    t.running    = true;

    TerminalState* tp = &t;
    t.reader = std::thread([tp]() {
        char buf[4096];
        while (tp->running) {
            ssize_t n = read(tp->pty_master, buf, sizeof(buf));
            if (n <= 0) break;
            std::lock_guard<std::mutex> lk(tp->mu);
            vterm_input_write(tp->vt, buf, (size_t)n);
            vterm_screen_flush_damage(tp->vts);
        }
        tp->running = false;
    });
}

void terminal_destroy(TerminalState& t) {
    t.running = false;

    // The reader thread is blocked in read() on the pty master. On Linux,
    // close() from another thread does NOT reliably unblock an in-progress
    // read() — the behavior is officially undefined. Closing first and
    // hoping join() returns is what caused app shutdown to hang.
    //
    // The reliable wake-up: kill the child. When the shell exits and the
    // slave side drops, read() on the master returns 0 (EOF), and the
    // reader's `if (n <= 0) break;` exits the loop cleanly.
    if (t.child_pid > 0) {
        kill(t.child_pid, SIGHUP);  // graceful — shell saves history, etc.

        bool reaped = false;
        for (int i = 0; i < 50; ++i) {
            int status;
            pid_t r = waitpid(t.child_pid, &status, WNOHANG);
            if (r != 0) { reaped = true; break; }   // exited or error
            usleep(10 * 1000);                       // 10 ms × 50 = 500 ms budget
        }
        if (!reaped) {
            kill(t.child_pid, SIGKILL);
            waitpid(t.child_pid, nullptr, 0);
        }
        t.child_pid = -1;
    }

    if (t.reader.joinable())
        t.reader.join();

    if (t.pty_master >= 0) {
        close(t.pty_master);
        t.pty_master = -1;
    }

    if (t.vt) {
        vterm_free(t.vt);
        t.vt  = nullptr;
        t.vts = nullptr;
    }
}

void terminal_write(TerminalState& t, const char* data, size_t len) {
    if (t.pty_master < 0 || !t.running) return;
    write(t.pty_master, data, len);
}

bool terminal_alive(const TerminalState& t) {
    return t.running && t.child_pid > 0;
}
