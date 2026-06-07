// panel_terminal.cpp — embedded terminal panel (libvterm + pty)
#include "panel_terminal.h"
#include "terminal.h"
#include "globals.h"
#include <imgui.h>
#include <vterm.h>
#include <cstring>
#include <cstdio>
#include <cstdint>

extern ImFont* g_font_mono;

static TerminalState s_term;
static bool          s_initialized = false;
static bool          s_focused     = false;


static ImU32 vcolor_to_u32(VTermScreen* vts, VTermColor col, bool is_bg) {
    if (VTERM_COLOR_IS_DEFAULT_BG(&col)) return IM_COL32(10, 10, 14, 255);
    if (VTERM_COLOR_IS_DEFAULT_FG(&col)) return IM_COL32(220, 220, 220, 255);
    vterm_screen_convert_color_to_rgb(vts, &col);
    (void)is_bg;
    return IM_COL32(col.rgb.red, col.rgb.green, col.rgb.blue, 255);
}

// Encode a Unicode codepoint to UTF-8; returns byte count.
static int cp_to_utf8(uint32_t cp, char out[8]) {
    if (cp < 0x80)        { out[0]=(char)cp; return 1; }
    if (cp < 0x800)       { out[0]=(char)(0xC0|(cp>>6));  out[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000)     { out[0]=(char)(0xE0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&0x3F));  out[2]=(char)(0x80|(cp&0x3F)); return 3; }
    out[0]=(char)(0xF0|(cp>>18)); out[1]=(char)(0x80|((cp>>12)&0x3F));
    out[2]=(char)(0x80|((cp>>6)&0x3F)); out[3]=(char)(0x80|(cp&0x3F)); return 4;
}

static void ensure_init(int cols, int rows) {
    if (!s_initialized) {
        terminal_init(s_term, cols, rows);
        s_initialized = true;
    }
}

void terminal_panel_shutdown() {
    if (s_initialized) {
        terminal_destroy(s_term);
        s_initialized = false;
    }
}

void terminal_inject_path(const std::string& path) {
    if (!s_initialized) return;
    terminal_write(s_term, path.c_str(), path.size());
    terminal_write(s_term, " ", 1);
}

bool terminal_is_focused() { return s_focused; }

void draw_terminal_panel(AppState& state, float panel_w, float panel_h) {
    (void)state;

    ImGui::PushFont(g_font_mono);
    float cell_w = ImGui::CalcTextSize("M").x;
    float cell_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    // Reserve 1px top border + bottom padding
    float output_h = panel_h - 2.f;
    if (output_h < cell_h) output_h = cell_h;

    // The vterm grid is locked at first init — libvterm resizes are unreliable
    // here (race-prone with the reader thread), so we size once and never call
    // vterm_set_size again. The visible viewport may be larger or smaller than
    // the grid; BeginChild's clip rect handles overflow.
    int init_cols = (int)((panel_w - 8.f) / cell_w);
    int init_rows = (int)(output_h / cell_h);
    if (init_cols < 10) init_cols = 10;
    if (init_rows < 2)  init_rows = 2;
    ensure_init(init_cols, init_rows);

    // ── Dismiss focus when clicking outside ──────────────────────────────────
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 mp = ImGui::GetIO().MousePos;
        ImVec2 ws = ImGui::GetWindowSize();
        bool inside = mp.x >= wp.x && mp.x < wp.x + ws.x &&
                      mp.y >= wp.y && mp.y < wp.y + ws.y;
        if (!inside) s_focused = false;
    }

    // ── Keyboard input when focused ───────────────────────────────────────────
    if (s_focused && terminal_alive(s_term)) {
        ImGuiIO& io = ImGui::GetIO();

        // Printable characters
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            ImWchar cp = io.InputQueueCharacters[i];
            char utf8[8] = {};
            int n = cp_to_utf8((uint32_t)cp, utf8);
            terminal_write(s_term, utf8, (size_t)n);
        }
        io.InputQueueCharacters.clear();

        // Special keys
        auto wp = [&](ImGuiKey k, const char* seq, int len, bool repeat = true) {
            if (ImGui::IsKeyPressed(k, repeat))
                terminal_write(s_term, seq, (size_t)len);
        };
        wp(ImGuiKey_Enter,     "\r",       1, false);
        wp(ImGuiKey_Backspace, "\x7f",     1);
        wp(ImGuiKey_Tab,       "\t",       1);
        wp(ImGuiKey_Escape,    "\x1b",     1, false);
        wp(ImGuiKey_Delete,    "\x1b[3~",  4);
        wp(ImGuiKey_UpArrow,   "\x1b[A",   3);
        wp(ImGuiKey_DownArrow, "\x1b[B",   3);
        wp(ImGuiKey_RightArrow,"\x1b[C",   3);
        wp(ImGuiKey_LeftArrow, "\x1b[D",   3);
        wp(ImGuiKey_Home,      "\x1b[H",   3, false);
        wp(ImGuiKey_End,       "\x1b[F",   3, false);
        wp(ImGuiKey_PageUp,    "\x1b[5~",  4, false);
        wp(ImGuiKey_PageDown,  "\x1b[6~",  4, false);

        // Ctrl+letter (A=1 … Z=26)
        if (io.KeyCtrl) {
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z; k++) {
                if (ImGui::IsKeyPressed((ImGuiKey)k, true)) {
                    char ctrl = (char)(1 + (k - ImGuiKey_A));
                    terminal_write(s_term, &ctrl, 1);
                }
            }
        }
    }

    // ── OS file drop onto terminal ────────────────────────────────────────────
    // The terminal claims drops only while focused — the studio handler is
    // gated the same way, so there's exactly one owner per drop.
    if (!g_dropped_file.empty() && s_initialized && s_focused) {
        terminal_inject_path(g_dropped_file);
        g_dropped_file.clear();
    }

    // ── Render cell grid ──────────────────────────────────────────────────────
    ImGui::PushFont(g_font_mono);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10, 10, 14, 255));
    ImGui::BeginChild("##term_cells", {panel_w, output_h}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      origin = ImGui::GetCursorScreenPos();

    // Clickable area to acquire focus
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##term_focus_btn", {panel_w, output_h});
    if (ImGui::IsItemClicked())
        s_focused = true;

    if (!terminal_alive(s_term)) {
        // Shell exited — show status text
        ImGui::SetCursorScreenPos({origin.x + 8.f, origin.y + 4.f});
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 80, 80, 255));
        ImGui::TextUnformatted("Terminal closed.");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 12.f);
        if (ImGui::SmallButton("Restart")) {
            terminal_destroy(s_term);
            s_initialized = false;
            ensure_init(init_cols, init_rows);
        }
    } else {
        std::lock_guard<std::mutex> lk(s_term.mu);

        VTermPos cursor_pos = {0, 0};
        vterm_state_get_cursorpos(vterm_obtain_state(s_term.vt), &cursor_pos);

        for (int row = 0; row < s_term.rows; ++row) {
            for (int col = 0; col < s_term.cols; ++col) {
                VTermScreenCell cell;
                VTermPos pos = {row, col};
                vterm_screen_get_cell(s_term.vts, pos, &cell);

                float cx = origin.x + col * cell_w;
                float cy = origin.y + row * cell_h;

                // Background
                ImU32 bg = vcolor_to_u32(s_term.vts, cell.bg, true);
                bool  is_cursor = (row == cursor_pos.row && col == cursor_pos.col);
                if (is_cursor && s_focused) {
                    dl->AddRectFilled({cx, cy}, {cx + cell_w, cy + cell_h},
                                      IM_COL32(200, 200, 255, 200));
                } else if (bg != IM_COL32(10, 10, 14, 255)) {
                    dl->AddRectFilled({cx, cy}, {cx + cell_w, cy + cell_h}, bg);
                }

                // Character
                if (cell.chars[0] && cell.chars[0] != ' ') {
                    char utf8[8] = {};
                    cp_to_utf8(cell.chars[0], utf8);
                    ImU32 fg = (is_cursor && s_focused)
                        ? IM_COL32(10, 10, 14, 255)
                        : vcolor_to_u32(s_term.vts, cell.fg, false);
                    dl->AddText({cx, cy}, fg, utf8);
                }
            }
        }

        // Blinking unfocused cursor outline
        if (!s_focused) {
            float cx = origin.x + cursor_pos.col * cell_w;
            float cy = origin.y + cursor_pos.row * cell_h;
            float t  = (float)ImGui::GetTime();
            int   a  = (int)(140.f + 80.f * sinf(t * 4.f));
            dl->AddRect({cx, cy}, {cx + cell_w, cy + cell_h},
                        IM_COL32(180, 180, 255, a), 0.f, 0, 1.f);
        }
    }

    // Focus ring
    if (s_focused) {
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsz  = ImGui::GetWindowSize();
        dl->AddRect(wpos, {wpos.x + wsz.x, wpos.y + wsz.y},
                    IM_COL32(120, 80, 255, 120), 0.f, 0, 1.f);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
