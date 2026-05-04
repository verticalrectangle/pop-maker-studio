#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>

#include "app.h"
#include "globals.h"
#include "ml_pipeline_embedded.h"
#include "ml_prefetch_embedded.h"

namespace fs = std::filesystem;

// Definitions of globals declared in globals.h
std::string g_dropped_file;
std::string g_pipeline_script;
std::string g_prefetch_script;

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

int main(int, char**) {
    g_pipeline_script = extract_embedded(ml_pipeline_py,  ml_pipeline_py_size,
                                          "pop_maker_ml_pipeline.py");
    g_prefetch_script = extract_embedded(ml_prefetch_py,  ml_prefetch_py_size,
                                          "pop_maker_ml_prefetch.py");

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Pop Maker Studio", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

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
