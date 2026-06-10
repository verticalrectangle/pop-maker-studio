#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <deque>
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
#include "ui/canvas.h"
#include "ui/panel_media.h"  // bin_add for drain_bin_pending()
namespace fs = std::filesystem;

// Definitions of globals declared in globals.h
std::string g_dropped_file;
std::string g_managed_dir;

// Drop queues. Single-file drops follow the legacy g_dropped_file path so the
// existing readers (screen_studio, screen_upload, panel_terminal) handle
// hover-track placement as before. Multi-file drops bypass that entirely and
// go straight to the bin, because "place all 5 files at the playhead on one
// track" is essentially never what the user wants — it just stacks them.
static std::deque<std::string> g_dropped_queue;   // single-file: one path per frame
static std::deque<std::string> g_bin_pending;     // multi-file: drained into AppState.bin

static void glfw_drop_callback(GLFWwindow*, int count, const char** paths) {
    if (count <= 0 || !paths) return;
    if (count == 1) {
        if (paths[0] && paths[0][0])
            g_dropped_queue.emplace_back(paths[0]);
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (paths[i] && paths[i][0])
            g_bin_pending.emplace_back(paths[i]);
    }
}

static void drain_dropped_queue() {
    if (!g_dropped_file.empty() || g_dropped_queue.empty()) return;
    g_dropped_file = std::move(g_dropped_queue.front());
    g_dropped_queue.pop_front();
}

// Drain the multi-file pending list into the project bin. Needs AppState
// access, so this lives in main.cpp where state is in scope and is called
// from the main loop right after drain_dropped_queue.
static void drain_bin_pending(AppState& state) {
    while (!g_bin_pending.empty()) {
        bin_add(state, g_bin_pending.front());
        g_bin_pending.pop_front();
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

    // If ffmpeg dies mid-export (vaapi hiccup, encoder error, user kill, ...)
    // our next write() to its stdin pipe would send SIGPIPE and the default
    // handler silently terminates this process — losing the in-flight render
    // and leaving the user with a truncated mp4. Ignore SIGPIPE so write()
    // returns EPIPE and the render loop can log + clean up properly.
    signal(SIGPIPE, SIG_IGN);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // NavEnableKeyboard intentionally off: it reroutes arrow keys to whichever
    // widget the nav cursor drifts onto (typically a slider), which then eats
    // the arrows we want for playhead seeking.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    AppState state;
    app_init(state);
    state.models_ready = models_detect();

    // Vsync is on during normal interactive use (smooth UI, low CPU). While an
    // export is running we let the main loop free-run so render_tick_gl isn't
    // capped at the display refresh — otherwise the export's wall-clock speed
    // is gated by the monitor (e.g. 60 Hz), and changing the libx264 preset
    // has no observable effect because ffmpeg is starved on stdin.
    bool vsync_on = true;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        drain_dropped_queue();   // single-file drops: feed one path per frame
        drain_bin_pending(state); // multi-file drops: dump all into the bin

        bool want_vsync = !state.render.running;
        if (want_vsync != vsync_on) {
            glfwSwapInterval(want_vsync ? 1 : 0);
            vsync_on = want_vsync;
        }

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

        // "source: canvas" snapshot — grab the preview rect from the back
        // buffer now that the full frame is drawn (no-op unless armed).
        canvas_capture_after_render(state);

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
