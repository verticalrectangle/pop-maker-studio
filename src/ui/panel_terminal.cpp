// panel_terminal.cpp — embedded terminal panel (libvterm + pty)
#include "panel_terminal.h"
#include "terminal.h"
#include "globals.h"
#include <imgui.h>
#include <vterm.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdint>

extern ImFont* g_font_mono;

static TerminalState s_term;
static bool          s_initialized = false;
static bool          s_focused     = false;


// Tokyo Night palette — designed for low eye strain and readable contrast.
// libvterm's default ANSI palette has very dark blues that disappear against
// a near-black background (fish renders paths in ANSI blue, which was the
// "blue on black" complaint). We override the 16 standard ANSI entries plus
// the default fg/bg here; 256-color indices >= 16 still flow through
// libvterm's built-in conversion.
static constexpr ImU32 kTermBg = IM_COL32(0x1a, 0x1b, 0x26, 255);
static constexpr ImU32 kTermFg = IM_COL32(0xc0, 0xca, 0xf5, 255);
static const ImU32 kTermPalette[16] = {
    IM_COL32(0x15, 0x16, 0x1e, 255), // 0  black
    IM_COL32(0xf7, 0x76, 0x8e, 255), // 1  red
    IM_COL32(0x9e, 0xce, 0x6a, 255), // 2  green
    IM_COL32(0xe0, 0xaf, 0x68, 255), // 3  yellow
    IM_COL32(0x7a, 0xa2, 0xf7, 255), // 4  blue
    IM_COL32(0xbb, 0x9a, 0xf7, 255), // 5  magenta
    IM_COL32(0x7d, 0xcf, 0xff, 255), // 6  cyan
    IM_COL32(0xa9, 0xb1, 0xd6, 255), // 7  white
    IM_COL32(0x41, 0x48, 0x68, 255), // 8  bright black
    IM_COL32(0xff, 0x7a, 0x93, 255), // 9  bright red
    IM_COL32(0xb9, 0xf2, 0x7c, 255), // 10 bright green
    IM_COL32(0xff, 0x9e, 0x64, 255), // 11 bright yellow
    IM_COL32(0x7d, 0xa6, 0xff, 255), // 12 bright blue
    IM_COL32(0xbb, 0x9a, 0xf7, 255), // 13 bright magenta
    IM_COL32(0x0d, 0xb9, 0xd7, 255), // 14 bright cyan
    IM_COL32(0xc0, 0xca, 0xf5, 255), // 15 bright white
};

static ImU32 vcolor_to_u32(VTermScreen* vts, VTermColor col, bool is_bg) {
    if (VTERM_COLOR_IS_DEFAULT_BG(&col)) return kTermBg;
    if (VTERM_COLOR_IS_DEFAULT_FG(&col)) return kTermFg;
    if (VTERM_COLOR_IS_INDEXED(&col) && col.indexed.idx < 16)
        return kTermPalette[col.indexed.idx];
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

    int init_cols = (int)((panel_w - 8.f) / cell_w);
    int init_rows = (int)(output_h / cell_h);
    if (init_cols < 10) init_cols = 10;
    if (init_rows < 2)  init_rows = 2;
    ensure_init(init_cols, init_rows);
    // Resize vterm + pty whenever the panel dimensions change.  The mutex in
    // terminal_resize keeps this safe against the reader thread.
    terminal_resize(s_term, init_cols, init_rows);

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
        bool wrote_something = false;

        // Printable characters
        int char_count = io.InputQueueCharacters.Size;
        for (int i = 0; i < char_count; i++) {
            ImWchar cp = io.InputQueueCharacters[i];
            char utf8[8] = {};
            int n = cp_to_utf8((uint32_t)cp, utf8);
            terminal_write(s_term, utf8, (size_t)n);
        }
        io.InputQueueCharacters.clear();
        if (char_count > 0) wrote_something = true;

        // Special keys
        auto wp = [&](ImGuiKey k, const char* seq, int len, bool repeat = true) {
            if (ImGui::IsKeyPressed(k, repeat)) {
                terminal_write(s_term, seq, (size_t)len);
                wrote_something = true;
            }
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
                    wrote_something = true;
                }
            }
        }

        if (wrote_something) s_term.scroll_offset = 0;
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kTermBg);
    ImGui::BeginChild("##term_cells", {panel_w, output_h}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      origin = ImGui::GetCursorScreenPos();

    // Clickable area to acquire focus. AllowOverlap so the Restart button
    // (drawn on top of this when the shell has exited) actually receives
    // clicks — otherwise this full-area InvisibleButton swallows them.
    ImGui::SetCursorScreenPos(origin);
    ImGui::SetNextItemAllowOverlap();
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

        // Mouse wheel: scroll through scrollback history
        if (ImGui::IsWindowHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.f) {
                int max_scroll = (int)s_term.scrollback.size();
                s_term.scroll_offset = std::clamp(
                    s_term.scroll_offset + (int)(wheel * 3.f), 0, max_scroll);
                ImGui::GetIO().MouseWheel = 0.f;
            }
        }

        VTermPos cursor_pos = {0, 0};
        vterm_state_get_cursorpos(vterm_obtain_state(s_term.vt), &cursor_pos);

        int sb_size = (int)s_term.scrollback.size();
        if (s_term.scroll_offset > sb_size) s_term.scroll_offset = sb_size;
        bool in_history = (s_term.scroll_offset > 0);

        for (int row = 0; row < s_term.rows; ++row) {
            // virtual_row: position in the combined [scrollback | screen] space
            int virtual_row = sb_size - s_term.scroll_offset + row;

            for (int col = 0; col < s_term.cols; ++col) {
                VTermScreenCell cell = {};

                if (virtual_row < sb_size) {
                    const auto& sbrow = s_term.scrollback[(size_t)virtual_row];
                    if (col < (int)sbrow.cells.size())
                        cell = sbrow.cells[col];
                } else {
                    VTermPos pos = {virtual_row - sb_size, col};
                    vterm_screen_get_cell(s_term.vts, pos, &cell);
                }

                float cx = origin.x + col * cell_w;
                float cy = origin.y + row * cell_h;

                ImU32 bg = vcolor_to_u32(s_term.vts, cell.bg, true);
                bool  is_cursor = !in_history &&
                                  (row == cursor_pos.row && col == cursor_pos.col);
                if (is_cursor && s_focused) {
                    dl->AddRectFilled({cx, cy}, {cx + cell_w, cy + cell_h},
                                      IM_COL32(200, 200, 255, 200));
                } else if (bg != kTermBg) {
                    dl->AddRectFilled({cx, cy}, {cx + cell_w, cy + cell_h}, bg);
                }

                if (cell.chars[0] && cell.chars[0] != ' ') {
                    char utf8[8] = {};
                    cp_to_utf8(cell.chars[0], utf8);
                    ImU32 fg = (is_cursor && s_focused)
                        ? kTermBg
                        : vcolor_to_u32(s_term.vts, cell.fg, false);
                    dl->AddText({cx, cy}, fg, utf8);
                }
            }
        }

        // Blinking unfocused cursor outline (live view only)
        if (!s_focused && !in_history) {
            float cx = origin.x + cursor_pos.col * cell_w;
            float cy = origin.y + cursor_pos.row * cell_h;
            float t  = (float)ImGui::GetTime();
            int   a  = (int)(140.f + 80.f * sinf(t * 4.f));
            dl->AddRect({cx, cy}, {cx + cell_w, cy + cell_h},
                        IM_COL32(180, 180, 255, a), 0.f, 0, 1.f);
        }

        // Scrollbar (shown whenever there is scrollback history)
        if (sb_size > 0) {
            float total = (float)(sb_size + s_term.rows);
            float thumb_h = std::max(4.f, output_h * (float)s_term.rows / total);
            float thumb_top = output_h * (float)(sb_size - s_term.scroll_offset) / total;
            float sb_x = origin.x + panel_w - 5.f;
            dl->AddRectFilled({sb_x, origin.y}, {sb_x + 4.f, origin.y + output_h},
                              IM_COL32(40, 40, 60, 120));
            dl->AddRectFilled({sb_x, origin.y + thumb_top},
                              {sb_x + 4.f, origin.y + thumb_top + thumb_h},
                              IM_COL32(150, 140, 220, in_history ? 220 : 140));
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
