// panel_agent.cpp — chat UI for the in-app agent. Rendering only: all loop
// state lives in agent_harness.cpp and is snapshotted per frame.
#include "panel_agent.h"
#include "../agent_harness.h"
#include "theme.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern ImFont* g_font_mono;

static char s_input[4096] = {};
static int  s_detail_row  = -1;   // row index with expanded detail, -1 = none
static size_t s_last_rows = 0;    // autoscroll when new rows arrive

// ── Prompt history (Up/Down recalls past prompts, shell-style) ──────────────
static std::vector<std::string> s_history;
static int         s_hist_pos = -1;    // index into s_history, -1 = fresh line
static std::string s_draft;            // unsent text stashed while browsing
static bool        s_refocus  = false; // refocus the input next frame
static int         s_ctx_sel0 = 0;     // input selection captured at right-click
static int         s_ctx_sel1 = 0;     // (byte offsets into s_input)

// ── Log text selection (terminal-style drag + copy) ─────────────────────────
// Anchor/head are (row, byte offset into the row's display text). Display
// text is rebuilt every frame; offsets on a streaming row stay valid because
// its text only grows at the end.
struct VisLine { int row; int b0, b1; ImVec2 pos; };  // byte range + screen pos
static std::vector<VisLine>     s_vis;   // visual lines, rebuilt every frame
static std::vector<std::string> s_disp;  // display text per row, rebuilt every frame
static int  s_sel_ar = -1, s_sel_ao = 0; // anchor row / byte offset
static int  s_sel_hr = -1, s_sel_ho = 0; // head row / byte offset
static bool s_selecting = false;
static int  s_press_row = -1;            // row under the initial press, -1 = margin

static bool sel_has() {
    return s_sel_ar >= 0 && s_sel_hr >= 0 &&
           !(s_sel_ar == s_sel_hr && s_sel_ao == s_sel_ho);
}

static void sel_clear() {
    s_sel_ar = s_sel_hr = -1;
    s_selecting = false;
}

static void sel_norm(int& r0, int& o0, int& r1, int& o1) {
    r0 = s_sel_ar; o0 = s_sel_ao; r1 = s_sel_hr; o1 = s_sel_ho;
    if (r0 > r1 || (r0 == r1 && o0 > o1)) { std::swap(r0, r1); std::swap(o0, o1); }
}

static std::string sel_text() {
    if (s_disp.empty() || !sel_has()) return {};
    int r0, o0, r1, o1;
    sel_norm(r0, o0, r1, o1);
    r0 = std::clamp(r0, 0, (int)s_disp.size() - 1);
    r1 = std::clamp(r1, 0, (int)s_disp.size() - 1);
    std::string out;
    for (int r = r0; r <= r1; ++r) {
        const std::string& d = s_disp[(size_t)r];
        int a = (r == r0) ? std::clamp(o0, 0, (int)d.size()) : 0;
        int b = (r == r1) ? std::clamp(o1, 0, (int)d.size()) : (int)d.size();
        if (b > a) out.append(d, (size_t)a, (size_t)(b - a));
        if (r < r1) out += '\n';
    }
    return out;
}

// Map a mouse position to (row, byte offset) over this frame's visual lines.
// Returns true when the position fell exactly on a line band — used to tell a
// click on a row apart from a drag started in the margin.
static bool map_mouse(ImVec2 mp, float line_h, int* out_row, int* out_off) {
    int idx = -1;
    for (int i = 0; i < (int)s_vis.size(); ++i) {
        if (s_vis[(size_t)i].pos.y <= mp.y) idx = i;
        else break;
    }
    if (idx < 0) { *out_row = s_vis[0].row; *out_off = s_vis[0].b0; return false; }
    const VisLine& L = s_vis[(size_t)idx];
    if (mp.y >= L.pos.y + line_h) { *out_row = L.row; *out_off = L.b1; return false; }
    *out_row = L.row;
    const std::string& d = s_disp[(size_t)L.row];
    const char* base = d.c_str();
    const char* p    = base + L.b0;
    const char* end  = base + L.b1;
    float x = L.pos.x;
    int off = L.b1;
    while (p < end) {
        unsigned int c;
        int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        float w = ImGui::CalcTextSize(p, p + n).x;
        if (mp.x < x + w * 0.5f) { off = (int)(p - base); break; }
        x += w;
        p += n;
    }
    *out_off = off;
    return true;
}

// Draw one transcript row word-wrapped line by line. Replicates TextWrapped
// layout but records the byte range and screen position of every visual line
// so mouse selection can hit-test exact characters, and paints the selection
// highlight behind the selected byte ranges.
static void draw_row_wrapped(int row, const std::string& d, ImU32 col) {
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImFont*     font   = ImGui::GetFont();
    const float fsize  = ImGui::GetFontSize();
    const float line_h = ImGui::GetTextLineHeight();
    const float wrap_w = std::max(ImGui::GetContentRegionAvail().x, 16.f);

    int r0 = 0, o0 = 0, r1 = 0, o1 = 0;
    const bool has = sel_has();
    if (has) sel_norm(r0, o0, r1, o1);

    ImGui::PushStyleColor(ImGuiCol_Text, col);
    // Zero spacing packs the lines at line_h intervals, like TextWrapped did.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.f, 0.f});

    const char* base = d.c_str();
    const char* end  = base + d.size();
    const char* p    = base;
    for (;;) {
        const char* line_end = font->CalcWordWrapPosition(fsize, p, end, wrap_w);
        // line_end == p is legitimate for blank lines (cursor sits on '\n');
        // anything else would stall the loop, so force one codepoint.
        if (line_end == p && p < end && *p != '\n') {
            unsigned int c;
            int n = ImTextCharFromUtf8(&c, p, end);
            line_end = p + std::max(n, 1);
        }
        ImVec2 pos = ImGui::GetCursorScreenPos();
        s_vis.push_back({row, (int)(p - base), (int)(line_end - base), pos});

        if (has && row >= r0 && row <= r1) {
            int a = (row == r0) ? std::max((int)(p - base), o0) : (int)(p - base);
            int b = (row == r1) ? std::min((int)(line_end - base), o1)
                                : (int)(line_end - base);
            if (b > a) {
                float x0 = pos.x + ImGui::CalcTextSize(p, base + a).x;
                float x1 = pos.x + ImGui::CalcTextSize(p, base + b).x;
                dl->AddRectFilled({x0, pos.y}, {x1, pos.y + line_h},
                                  IM_COL32(0x3d, 0x59, 0xa1, 150));
            }
        }

        if (line_end > p) ImGui::TextUnformatted(p, line_end);
        else              ImGui::TextUnformatted("");  // keep blank-line height
        if (line_end >= end) break;
        p = ImTextCalcWordWrapNextLineStart(line_end, end);
        if (p >= end) break;
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void draw_agent_log(AppState& state, float panel_w, float panel_h) {
    (void)state;

    ImGui::PushFont(g_font_mono);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x16, 0x16, 0x20, 255));
    // Padding must be pushed before BeginChild to apply to this child.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.f, 8.f});
    ImGui::BeginChild("##agent_log", {panel_w, panel_h},
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    std::vector<AgentRow> rows = agent_rows_snapshot();
    s_vis.clear();
    s_disp.clear();
    s_disp.reserve(rows.size());
    bool mouse_in_detail = false;

    if (rows.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(110, 110, 140, 255));
        ImGui::TextUnformatted("");
        ImGui::TextWrapped("  Agent ready. It edits this project with the same "
                           "tools external agents use. Add your API key in "
                           "File > Settings if you haven't.");
        ImGui::PopStyleColor();
    }

    for (int i = 0; i < (int)rows.size(); ++i) {
        const AgentRow& r = rows[i];
        ImU32 col = 0;
        std::string d;
        switch (r.role) {
            case AgentRole::User:
                col = IM_COL32(200, 160, 255, 255);
                d = "> " + r.text;
                break;
            case AgentRole::Assistant:
                col = IM_COL32(0xc0, 0xca, 0xf5, 255);
                d = r.text;
                if (r.streaming) d += "\xe2\x96\x8d";  // ▍ caret
                break;
            case AgentRole::Tool:
                col = IM_COL32(100, 116, 160, 255);
                d = r.text;
                break;
            case AgentRole::Error:
                col = IM_COL32(247, 118, 142, 255);
                d = r.text;
                break;
            case AgentRole::Info:
                col = IM_COL32(110, 110, 140, 255);
                d = r.text;
                break;
            case AgentRole::Image:
                col = IM_COL32(125, 207, 255, 255);
                d = "\xf0\x9f\x96\xbc " + r.text;
                break;
        }
        s_disp.push_back(std::move(d));

        ImGui::PushID(i);
        draw_row_wrapped(i, s_disp[(size_t)i], col);
        if (!r.detail.empty() && s_detail_row == i) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x1e, 0x1e, 0x2c, 255));
            ImGui::BeginChild("##detail", {panel_w - 24.f, 140.f},
                              ImGuiChildFlags_Borders);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 160, 190, 255));
            ImGui::TextWrapped("%s", r.detail.c_str());
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) mouse_in_detail = true;
        }
        ImGui::PopID();
        ImGui::Dummy({0.f, 2.f});
    }

    // "thinking" shimmer while waiting for the first token of a turn
    if (agent_running() && (rows.empty() || !rows.back().streaming)) {
        float t = (float)ImGui::GetTime();
        int a = (int)(120.f + 80.f * sinf(t * 5.f));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 130, 220, a));
        ImGui::TextUnformatted("  \xc2\xb7\xc2\xb7 thinking");
        ImGui::PopStyleColor();
    }

    // ── Mouse selection: drag selects text, plain click toggles detail ──────
    {
        ImGuiIO& io = ImGui::GetIO();
        const float line_h = ImGui::GetTextLineHeight();
        const bool hovered = ImGui::IsWindowHovered();
        // Don't steal clicks that grabbed this child's scrollbar (handled in
        // BeginChild, so its ActiveId is already set by the time we run).
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        const bool scrollbar_active =
            GImGui->ActiveId != 0 &&
            GImGui->ActiveId == ImGui::GetWindowScrollbarID(win, ImGuiAxis_Y);

        if (hovered && !mouse_in_detail && !scrollbar_active && !s_vis.empty() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool exact = map_mouse(io.MousePos, line_h, &s_sel_ar, &s_sel_ao);
            s_sel_hr = s_sel_ar;
            s_sel_ho = s_sel_ao;
            s_selecting = true;
            s_press_row = exact ? s_sel_ar : -1;
        }
        if (s_selecting) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !s_vis.empty())
                map_mouse(io.MousePos, line_h, &s_sel_hr, &s_sel_ho);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                s_selecting = false;
                if (!sel_has()) {
                    sel_clear();
                    if (s_press_row >= 0 && s_press_row < (int)rows.size() &&
                        !rows[(size_t)s_press_row].detail.empty())
                        s_detail_row = (s_detail_row == s_press_row) ? -1
                                                                     : s_press_row;
                }
            }
        }

        // Right-click: context menu
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##agent_log_ctx");
        if (ImGui::BeginPopup("##agent_log_ctx")) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x1e, 0x1f, 0x2e, 245));
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel_has())) {
                std::string t = sel_text();
                if (!t.empty()) ImGui::SetClipboardText(t.c_str());
                sel_clear();
            }
            if (ImGui::MenuItem("Select All", nullptr, false, !s_disp.empty())) {
                s_sel_ar = 0;
                s_sel_ao = 0;
                s_sel_hr = (int)s_disp.size() - 1;
                s_sel_ho = (int)s_disp.back().size();
            }
            if (ImGui::MenuItem("Copy All", nullptr, false, !s_disp.empty())) {
                std::string all;
                for (size_t k = 0; k < s_disp.size(); ++k) {
                    all += s_disp[k];
                    if (k + 1 < s_disp.size()) all += '\n';
                }
                ImGui::SetClipboardText(all.c_str());
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }

        // Ctrl+C / Ctrl+Shift+C copy the selection while hovering the log
        if (hovered && sel_has() && io.KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            std::string t = sel_text();
            if (!t.empty()) ImGui::SetClipboardText(t.c_str());
            sel_clear();
        }
    }

    if (!s_selecting &&
        (rows.size() != s_last_rows || (!rows.empty() && rows.back().streaming))) {
        ImGui::SetScrollHereY(1.f);
        s_last_rows = rows.size();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Up/Down cycles through past prompts; the in-progress line is stashed and
// restored when cycling back past the newest entry.
static int input_history_cb(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    const int prev = s_hist_pos;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (s_history.empty()) return 0;
        if (s_hist_pos < 0) {
            s_draft.assign(data->Buf, (size_t)data->BufTextLen);
            s_hist_pos = (int)s_history.size() - 1;
        } else if (s_hist_pos > 0) {
            --s_hist_pos;
        }
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (s_hist_pos < 0) return 0;
        if (++s_hist_pos >= (int)s_history.size()) s_hist_pos = -1;
    }
    if (prev != s_hist_pos) {
        const std::string& t = (s_hist_pos < 0) ? s_draft
                                                : s_history[(size_t)s_hist_pos];
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, t.c_str(), t.c_str() + t.size());
    }
    return 0;
}

void draw_agent_input(AppState& state, float panel_w) {
    (void)state;
    (void)panel_w;
    bool running = agent_running();
    float send_w = 64.f, clear_w = 60.f;
    ImGui::PushFont(g_font_mono);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x1e, 0x1e, 0x2c, 255));
    // Size off the content region so the row ends at the same window padding
    // on the right as it starts with on the left (two 6 px SameLine gaps).
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                            send_w - clear_w - 12.f);
    if (s_refocus) { ImGui::SetKeyboardFocusHere(); s_refocus = false; }
    bool submit = ImGui::InputText("##agent_in", s_input, sizeof(s_input),
                                   ImGuiInputTextFlags_EnterReturnsTrue |
                                   ImGuiInputTextFlags_CallbackHistory,
                                   input_history_cb);
    ImGui::PopStyleColor();
    const ImGuiID input_id = ImGui::GetItemID();

    // Right-click context menu. Selection/cursor must be captured at click
    // time: opening the popup moves focus and the live state goes stale.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        s_ctx_sel0 = s_ctx_sel1 = (int)strlen(s_input);
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(input_id)) {
            if (st->HasSelection()) {
                s_ctx_sel0 = std::min(st->GetSelectionStart(), st->GetSelectionEnd());
                s_ctx_sel1 = std::max(st->GetSelectionStart(), st->GetSelectionEnd());
            } else {
                s_ctx_sel0 = s_ctx_sel1 = st->GetCursorPos();
            }
        }
        ImGui::OpenPopup("##agent_in_ctx");
    }
    if (ImGui::BeginPopup("##agent_in_ctx")) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0x1e, 0x1f, 0x2e, 245));
        const int len = (int)strlen(s_input);
        int a = std::clamp(s_ctx_sel0, 0, len);
        int b = std::clamp(s_ctx_sel1, 0, len);
        if (a > b) std::swap(a, b);
        const bool has_sel = b > a;
        // Push the externally edited buffer into the (possibly still live)
        // widget state and park the cursor at 'cur'.
        auto apply = [&](int cur) {
            if (ImGuiInputTextState* st = ImGui::GetInputTextState(input_id)) {
                st->WantReloadUserBuf = true;
                st->ReloadSelectionStart = st->ReloadSelectionEnd = cur;
                s_refocus = true;
            }
        };
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, has_sel)) {
            std::string sub(s_input + a, (size_t)(b - a));
            ImGui::SetClipboardText(sub.c_str());
            memmove(s_input + a, s_input + b, (size_t)(len - b) + 1);
            apply(a);
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, has_sel)) {
            std::string sub(s_input + a, (size_t)(b - a));
            ImGui::SetClipboardText(sub.c_str());
        }
        const char* cb = ImGui::GetClipboardText();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, cb && *cb)) {
            std::string out(s_input, (size_t)a);
            out += cb;
            out.append(s_input + b);
            // Single-line input: control characters would render as garbage.
            for (char& ch : out)
                if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            if (out.size() >= sizeof(s_input)) out.resize(sizeof(s_input) - 1);
            memcpy(s_input, out.c_str(), out.size() + 1);
            apply(std::min(a + (int)strlen(cb), (int)out.size()));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", nullptr, false, len > 0)) {
            if (ImGuiInputTextState* st = ImGui::GetInputTextState(input_id)) {
                st->ReloadUserBufAndSelectAll();
                s_refocus = true;
            }
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    ImGui::SameLine(0.f, 6.f);
    if (running) {
        if (ImGui::Button("Stop", {send_w, 0.f})) agent_stop();
    } else {
        if (ImGui::Button("Send", {send_w, 0.f})) submit = true;
    }
    ImGui::SameLine(0.f, 6.f);
    if (!running && ImGui::Button("Clear", {clear_w, 0.f})) {
        agent_clear();
        s_detail_row = -1;
        s_last_rows  = 0;
        sel_clear();
    }
    if (submit && !running && s_input[0]) {
        if (s_history.empty() || s_history.back() != s_input)
            s_history.emplace_back(s_input);
        s_hist_pos = -1;
        s_draft.clear();
        agent_send(s_input);
        s_input[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopFont();
}
