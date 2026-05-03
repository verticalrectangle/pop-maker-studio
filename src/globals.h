#pragma once
#include <string>

// Globals set by GLFW callbacks in main.cpp, read by UI screens
extern std::string g_dropped_file;    // path from GLFW drop callback
extern std::string g_pipeline_script; // path to ml_pipeline.py
