#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <filesystem>

#include "app.h"
#include "paths.h"
#include "video.h"
#include "fx_shader.h"
#include "render.h"
#include "generated/fx_type_list.h"
#include "globals.h"
#include "stb_image_write.h"
#include "portrait_preview.h"
namespace fs = std::filesystem;

// Definitions of globals declared in globals.h
std::string g_dropped_file;
ImVec2      g_drop_pos;
std::string g_managed_dir;

static void glfw_drop_callback(GLFWwindow* win, int count, const char** paths) {
    if (count > 0) {
        g_dropped_file = paths[0];
        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        g_drop_pos = {(float)mx, (float)my};
    }
}

static void glfw_error_callback(int err, const char* desc) {
    fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

// Check for the whisper ggml model in the bundled models/ directory.
bool models_detect() {
    fs::path mp = fs::path(app_models_dir()) / "ggml-large-v3-turbo-q5_0.bin";
    return fs::exists(mp);
}

// Dump FX preview PNGs for every registered effect and exit.
static void dump_fx_previews(const char* out_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(out_dir);

    glfwSetErrorCallback([](int, const char* d){ fprintf(stderr, "GLFW: %s\n", d); });
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(256, 256, "fx_dump", nullptr, nullptr);
    if (!win) { glfwTerminate(); fprintf(stderr, "window failed\n"); return; }
    glfwMakeContextCurrent(win);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(win, false);
    ImGui_ImplOpenGL3_Init("#version 330");

    render_init_fonts();
    fx_shader_init();

    // FX preview thumbnails are 108x192 (portrait_preview dimensions)
    const int W = 108, H = 192;

    // Existing hand-written effects
    static const struct { FXType ft; const char* name; } kBuiltin[] = {
        { FXType::Glitch,    "glitch"     },
        { FXType::ZoomPunch, "zoom_punch" },
        { FXType::LightLeak, "light_leak" },
        { FXType::VHS,       "vhs"        },
        { FXType::Datamosh,  "datamosh"   },
        { FXType::ChromaKey, "chroma_key" },
    };
    for (auto& b : kBuiltin) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        uintptr_t tex = video_fx_preview_texture(b.ft, 0.5f);
        ImGui::Render();
        std::vector<uint8_t> px((size_t)W * H * 3);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        // Flip vertically (GL origin is bottom-left)
        std::vector<uint8_t> flipped((size_t)W * H * 3);
        for (int y = 0; y < H; ++y)
            memcpy(flipped.data() + y*W*3, px.data() + (H-1-y)*W*3, W*3);
        std::string path = std::string(out_dir) + "/" + b.name + ".png";
        stbi_write_png(path.c_str(), W, H, 3, flipped.data(), W*3);
        printf("  %s\n", path.c_str());
    }

    // Upload portrait source to a GL texture for generated effect rendering
    GLuint src_tex = 0;
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // portrait_preview_rgb is RGB; upload as RGBA (GL_RGB internal format)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, W, H, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, portrait_preview_rgb);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Registry names for output filenames (same order as k_gen_fx_types)
    static const char* kGenNames[] = {
#include "generated/fx_gen_names.h"
    };

    // Generated effects — rendered via fx_preview_gen_effect
    for (int i = 0; i < k_gen_fx_count; ++i) {
        uintptr_t out = fx_preview_gen_effect(k_gen_fx_types[i], (uintptr_t)src_tex, W, H, 0.5f);
        // Read back RGBA from the output texture
        std::vector<uint8_t> px((size_t)W * H * 4);
        glBindTexture(GL_TEXTURE_2D, (GLuint)out);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // Convert RGBA → RGB and flip vertically
        std::vector<uint8_t> flipped((size_t)W * H * 3);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t* s = px.data() + ((H-1-y)*W + x) * 4;
                uint8_t* d = flipped.data() + (y*W + x) * 3;
                d[0]=s[0]; d[1]=s[1]; d[2]=s[2];
            }
        std::string name = (i < (int)(sizeof(kGenNames)/sizeof(kGenNames[0]))) ? kGenNames[i] : std::to_string(i);
        std::string path = std::string(out_dir) + "/" + name + ".png";
        stbi_write_png(path.c_str(), W, H, 3, flipped.data(), W*3);
        printf("  %s\n", path.c_str());
    }

    glDeleteTextures(1, &src_tex);

    fx_shader_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    printf("done\n");
}

int main(int argc, char** argv) {
    // --dump-fx-previews <dir>: render all FX previews to PNGs and exit
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--dump-fx-previews") {
            dump_fx_previews(argv[i+1]);
            return 0;
        }
    }

    // Set managed dir
    if (const char* home = getenv("HOME"))
        g_managed_dir = (fs::path(home) / ".local" / "share" / "pop-maker-studio").string();

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
    _exit(0); // skip C++ static destructors — detached threads are already signalled via g_shutdown
}
