// panel_agent.cpp — chat UI for the in-app agent. Rendering only: all loop
// state lives in agent_harness.cpp and is snapshotted per frame.
#include "panel_agent.h"
#include "../agent_harness.h"
#include "theme.h"
#include <imgui.h>
#include <cstring>

extern ImFont* g_font_mono;

static char s_input[4096] = {};
static int  s_detail_row  = -1;   // row index with expanded detail, -1 = none
static size_t s_last_rows = 0;    // autoscroll when new rows arrive

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
        ImGui::PushID(i);
        switch (r.role) {
            case AgentRole::User:
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 160, 255, 255));
                ImGui::TextWrapped("> %s", r.text.c_str());
                ImGui::PopStyleColor();
                break;
            case AgentRole::Assistant: {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xc0, 0xca, 0xf5, 255));
                std::string t = r.text;
                if (r.streaming) t += "\xe2\x96\x8d";  // ▍ caret
                ImGui::TextWrapped("%s", t.c_str());
                ImGui::PopStyleColor();
                break;
            }
            case AgentRole::Tool:
            case AgentRole::Error: {
                bool is_err = (r.role == AgentRole::Error);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    is_err ? IM_COL32(247, 118, 142, 255)
                           : IM_COL32(100, 116, 160, 255));
                ImGui::TextWrapped("%s", r.text.c_str());
                ImGui::PopStyleColor();
                if (!r.detail.empty() && ImGui::IsItemClicked())
                    s_detail_row = (s_detail_row == i) ? -1 : i;
                if (s_detail_row == i) {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x1e, 0x1e, 0x2c, 255));
                    ImGui::BeginChild("##detail", {panel_w - 24.f, 140.f},
                                      ImGuiChildFlags_Borders);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 160, 190, 255));
                    ImGui::TextWrapped("%s", r.detail.c_str());
                    ImGui::PopStyleColor();
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                break;
            }
            case AgentRole::Info:
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(110, 110, 140, 255));
                ImGui::TextWrapped("%s", r.text.c_str());
                ImGui::PopStyleColor();
                break;
            case AgentRole::Image:
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(125, 207, 255, 255));
                ImGui::TextWrapped("\xf0\x9f\x96\xbc %s", r.text.c_str());
                ImGui::PopStyleColor();
                break;
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

    if (rows.size() != s_last_rows ||
        (!rows.empty() && rows.back().streaming)) {
        ImGui::SetScrollHereY(1.f);
        s_last_rows = rows.size();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
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
    bool submit = ImGui::InputText("##agent_in", s_input, sizeof(s_input),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
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
    }
    if (submit && !running && s_input[0]) {
        agent_send(s_input);
        s_input[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopFont();
}
