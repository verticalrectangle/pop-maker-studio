#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <filesystem>

#if defined(__APPLE__)
#  include <mach-o/getsect.h>
#  include <mach-o/ldsyms.h>
#elif defined(PYTHON_EMBED_CARRAY)
#  include "python311_embedded.h"
#else
// Linux / ELF — symbols injected by objcopy
extern "C" {
    extern const unsigned char _binary_python311_tar_gz_start[];
    extern const unsigned char _binary_python311_tar_gz_end[];
}
#endif

#include "app.h"
#include "globals.h"
#include "ml_pipeline_embedded.h"
#include "ml_prefetch_embedded.h"
#include "ml_setup_embedded.h"
#include "beat_detect_embedded.h"
#include "envelope_extract_embedded.h"

namespace fs = std::filesystem;

// Definitions of globals declared in globals.h
std::string g_dropped_file;
std::string g_pipeline_script;
std::string g_prefetch_script;
std::string g_setup_script;
std::string g_beat_detect_script;
std::string g_envelope_script;
std::string g_managed_dir;

static void glfw_drop_callback(GLFWwindow*, int count, const char** paths) {
    if (count > 0)
        g_dropped_file = paths[0];
}

static void glfw_error_callback(int err, const char* desc) {
    fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

static std::string extract_embedded(const unsigned char* data, unsigned int size,
                                     const char* filename) {
    fs::path tmp = fs::temp_directory_path() / filename;
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (f) { fwrite(data, 1, size, f); fclose(f); }
    return tmp.string();
}

// Resolve HuggingFace hub cache directory, mirroring the priority order used
// by huggingface_hub: HF_HUB_CACHE > HF_HOME/hub > XDG_CACHE_HOME/huggingface/hub
// > ~/.cache/huggingface/hub
static fs::path hf_hub_cache_dir() {
    auto env = [](const char* k) -> std::string {
        const char* v = getenv(k); return v ? v : "";
    };
    if (auto v = env("HF_HUB_CACHE");      !v.empty()) return fs::path(v);
    if (auto v = env("HF_HOME");            !v.empty()) return fs::path(v) / "hub";
    if (auto v = env("XDG_CACHE_HOME");     !v.empty()) return fs::path(v) / "huggingface" / "hub";
    const char* home = getenv("HOME");
    return home ? fs::path(home) / ".cache" / "huggingface" / "hub" : fs::path{};
}

// Resolve torch hub directory: TORCH_HOME > XDG_CACHE_HOME/torch > ~/.cache/torch
static fs::path torch_hub_dir() {
    auto env = [](const char* k) -> std::string {
        const char* v = getenv(k); return v ? v : "";
    };
    if (auto v = env("TORCH_HOME"); !v.empty()) return fs::path(v) / "hub";
    if (auto v = env("XDG_CACHE_HOME"); !v.empty()) return fs::path(v) / "torch" / "hub";
    const char* home = getenv("HOME");
    return home ? fs::path(home) / ".cache" / "torch" / "hub" : fs::path{};
}

bool models_detect() {
    // WhisperX uses faster-whisper: look for any Systran/faster-whisper-* model dir.
    bool whisper_ok = false;
    fs::path hf_hub = hf_hub_cache_dir();
    if (!hf_hub.empty() && fs::exists(hf_hub)) {
        for (auto& entry : fs::directory_iterator(hf_hub)) {
            if (entry.path().filename().string()
                    .rfind("models--Systran--faster-whisper", 0) == 0) {
                whisper_ok = true;
                break;
            }
        }
    }

    // Demucs: look for any .th checkpoint in torch hub.
    bool demucs_ok = false;
    fs::path checkpoints = torch_hub_dir() / "checkpoints";
    if (fs::exists(checkpoints)) {
        for (auto& entry : fs::directory_iterator(checkpoints)) {
            if (entry.path().extension() == ".th") {
                demucs_ok = true;
                break;
            }
        }
    }

    return whisper_ok && demucs_ok;
}

const unsigned char* python311_tar_data() {
#if defined(__APPLE__)
    unsigned long sz = 0;
    return (const unsigned char*)getsectiondata(&_mh_execute_header,
                                                "__DATA", "__python_tar", &sz);
#elif defined(PYTHON_EMBED_CARRAY)
    return python311_zip;
#else
    return _binary_python311_tar_gz_start;
#endif
}

size_t python311_tar_size() {
#if defined(__APPLE__)
    unsigned long sz = 0;
    getsectiondata(&_mh_execute_header, "__DATA", "__python_tar", &sz);
    return (size_t)sz;
#elif defined(PYTHON_EMBED_CARRAY)
    return (size_t)python311_zip_size;
#else
    return (size_t)(_binary_python311_tar_gz_end - _binary_python311_tar_gz_start);
#endif
}

std::string managed_python_detect() {
    const char* home = getenv("HOME");
    if (!home) return "";
    fs::path py = fs::path(home) / ".local" / "share" / "pop-maker-studio"
                                 / "venv" / "bin" / "python3";
    return fs::exists(py) ? py.string() : "";
}

int main(int, char**) {
    // Set managed dir
    if (const char* home = getenv("HOME"))
        g_managed_dir = (fs::path(home) / ".local" / "share" / "pop-maker-studio").string();

    g_pipeline_script = extract_embedded(ml_pipeline_py,  ml_pipeline_py_size,
                                          "pop_maker_ml_pipeline.py");
    g_prefetch_script = extract_embedded(ml_prefetch_py,  ml_prefetch_py_size,
                                          "pop_maker_ml_prefetch.py");
    g_setup_script    = extract_embedded(ml_setup_py,          ml_setup_py_size,
                                          "pop_maker_ml_setup.py");
    g_beat_detect_script = extract_embedded(beat_detect_py,   beat_detect_py_size,
                                          "pop_maker_beat_detect.py");
    g_envelope_script    = extract_embedded(envelope_extract_py, envelope_extract_py_size,
                                          "pop_maker_envelope_extract.py");

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Pop Maker Studio", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwSetWindowSizeLimits(window, 1000, 640, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glfwSetDropCallback(window, glfw_drop_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    AppState state;
    app_init(state);
    state.models_ready = models_detect();
    // Prefer managed venv over hardcoded default if it exists
    if (auto mp = managed_python_detect(); !mp.empty())
        state.python_path = mp;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app_frame(state);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    app_shutdown(state);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
