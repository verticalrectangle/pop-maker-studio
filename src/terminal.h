#pragma once
#include <vterm.h>
#include <mutex>
#include <thread>
#include <sys/types.h>

struct TerminalState {
    VTerm*       vt         = nullptr;
    VTermScreen* vts        = nullptr;
    int          pty_master = -1;
    pid_t        child_pid  = -1;
    int          cols       = 80;
    int          rows       = 24;
    std::mutex   mu;
    std::thread  reader;
    bool         running    = false;
    char         input_buf[1024] = {};
};

void terminal_init   (TerminalState& t, int cols, int rows);
void terminal_destroy(TerminalState& t);
void terminal_write  (TerminalState& t, const char* data, size_t len);
bool terminal_alive  (const TerminalState& t);
