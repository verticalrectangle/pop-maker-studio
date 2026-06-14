#pragma once
#include <string>
#include <vector>
struct AppState;
void ui_splash(AppState& state);
void ui_setup(AppState& state);
void ui_model_download_modal(AppState& state);
void ui_studio(AppState& state);

// ── Home / launcher ───────────────────────────────────────────────────────────
void ui_home(AppState& state);                         // recent-projects front page
void enter_new_project(AppState& state);               // blank project → studio
std::vector<std::string> recent_projects_list();       // newest first, existing only
void recent_projects_push(const std::string& path);    // call on save / open
