#pragma once
#include "app.h"
#include <string>

bool project_save(const AppState& state, const std::string& path);
bool project_load(AppState& state, const std::string& path);
