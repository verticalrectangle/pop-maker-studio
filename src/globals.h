#pragma once
#include <string>
#include <cstddef>

// Globals set by GLFW callbacks in main.cpp, read by UI screens
extern std::string g_dropped_file;     // path from GLFW drop callback
extern std::string g_pipeline_script;  // path to ml_pipeline.py  (extracted from binary)
extern std::string g_prefetch_script;  // path to ml_prefetch.py  (extracted from binary)
extern std::string g_setup_script;     // path to ml_setup.py     (extracted from binary)
extern std::string g_envelope_script;       // path to envelope_extract.py
extern std::string g_rembg_script;          // path to rembg_remove.py
extern std::string g_noise_reduce_script;   // path to noise_reduce.py
extern std::string g_managed_dir;      // ~/.local/share/pop-maker-studio

// Returns true if faster-whisper and Demucs weights are present on disk.
bool models_detect();

// Returns path to managed venv python3 if the managed venv exists, else "".
std::string managed_python_detect();

// Embedded Python 3.11 tarball.
// Access via python311_tar_data() / python311_tar_size() which abstract over
// the platform-specific embedding method (ELF objcopy / Mach-O sectcreate / C array).
const unsigned char* python311_tar_data();
size_t               python311_tar_size();
