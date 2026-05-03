#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <string>
#include <filesystem>

#include "app.h"
#include "globals.h"

namespace fs = std::filesystem;

// Definitions of globals declared in globals.h
std::string g_dropped_file;
std::string g_pipeline_script;

static void glfw_drop_callback(GLFWwindow*, int count, const char** paths) {
    if (count > 0)
        g_dropped_file = paths[0];
}

static void glfw_error_callback(int err, const char* desc) {
    fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

int main(int, char** argv) {
    // Locate ml_pipeline.py next to the executable
    fs::path exe_dir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
    g_pipeline_script = (exe_dir / "ml_pipeline.py").string();
    if (!fs::exists(g_pipeline_script)) {
        // Fallback: same directory as source root (dev mode)
        g_pipeline_script = (exe_dir.parent_path().parent_path() / "ml_pipeline.py").string();
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    // OpenGL 3.3 core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Pop Maker Studio", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    glfwSetDropCallback(window, glfw_drop_callback);

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    AppState state;
    app_init(state);  // loads fonts, applies theme

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
