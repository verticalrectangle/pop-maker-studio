#pragma once
#include <vterm.h>
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
#include <sys/types.h>

struct ScrollbackRow {
    std::vector<VTermScreenCell> cells;
};

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

    std::deque<ScrollbackRow> scrollback;
    int scroll_offset = 0;  // lines above live screen; 0 = live view
    static constexpr int kMaxScrollback = 2000;
};

void terminal_init   (TerminalState& t, int cols, int rows);
void terminal_destroy(TerminalState& t);
void terminal_resize (TerminalState& t, int cols, int rows);
void terminal_write  (TerminalState& t, const char* data, size_t len);
bool terminal_alive  (const TerminalState& t);
