#pragma once
#include "app.h"
#include <string>
#include <vector>

struct TimelineSnapshot {
    std::string        action;
    std::vector<Track> tracks;
    int                selected_track = -1;
    int                selected_clip  = -1;
    SubtitleMode       subtitle_mode  = SubtitleMode::Word;
    int                subtitle_n     = 3;
    AnimStyle          style          = AnimStyle::Block;
    int                font_weight    = 900;
    OutputFormat       format         = OutputFormat::Vertical;
    std::map<int, std::string> lyrics_edits;
};

// Push the current state onto the history stack with a descriptive action name.
// Call this AFTER the mutation so the snapshot captures the resulting state.
void history_push(const AppState& state, const std::string& action);

// Step backward / forward through the history.
void history_undo(AppState& state);
void history_redo(AppState& state);

bool history_can_undo();
bool history_can_redo();

// Current cursor index into the history list.
int history_pos();

// All recorded snapshots (oldest first).
const std::vector<TimelineSnapshot>& history_entries();

// Jump directly to any entry (Photoshop-style click-to-restore).
void history_jump(AppState& state, int idx);

// Clear all history (call on project close).
void history_clear();
