#include "history.h"

static constexpr int MAX_HISTORY = 50;

static std::vector<TimelineSnapshot> g_history;
static int g_pos = -1;

static void capture(TimelineSnapshot& snap, const AppState& state) {
    snap.tracks         = state.tracks;
    snap.selected_track = state.selected_track;
    snap.selected_clip  = state.selected_clip;
    snap.subtitle_mode  = state.subtitle_mode;
    snap.subtitle_n     = state.subtitle_n;
    snap.style          = state.style;
    snap.font_weight    = state.font_weight;
    snap.format         = state.format;
    snap.lyrics_edits   = state.lyrics_edits;
}

static void restore(AppState& state, const TimelineSnapshot& snap) {
    state.tracks         = snap.tracks;
    state.selected_track = snap.selected_track;
    state.selected_clip  = snap.selected_clip;
    state.subtitle_mode  = snap.subtitle_mode;
    state.subtitle_n     = snap.subtitle_n;
    state.style          = snap.style;
    state.font_weight    = snap.font_weight;
    state.format         = snap.format;
    state.lyrics_edits   = snap.lyrics_edits;
}

void history_push(const AppState& state, const std::string& action) {
    // Discard any redo branch when a new action is recorded
    if (g_pos < (int)g_history.size() - 1)
        g_history.erase(g_history.begin() + g_pos + 1, g_history.end());

    TimelineSnapshot snap;
    snap.action = action;
    capture(snap, state);
    g_history.push_back(std::move(snap));

    if ((int)g_history.size() > MAX_HISTORY)
        g_history.erase(g_history.begin());

    g_pos = (int)g_history.size() - 1;
}

void history_undo(AppState& state) {
    if (g_pos <= 0) return;
    --g_pos;
    restore(state, g_history[g_pos]);
}

void history_redo(AppState& state) {
    if (g_pos >= (int)g_history.size() - 1) return;
    ++g_pos;
    restore(state, g_history[g_pos]);
}

bool history_can_undo() { return g_pos > 0; }
bool history_can_redo() { return g_pos < (int)g_history.size() - 1; }
int  history_pos()      { return g_pos; }

const std::vector<TimelineSnapshot>& history_entries() { return g_history; }

void history_jump(AppState& state, int idx) {
    if (idx < 0 || idx >= (int)g_history.size()) return;
    g_pos = idx;
    restore(state, g_history[g_pos]);
}

void history_clear() {
    g_history.clear();
    g_pos = -1;
}
