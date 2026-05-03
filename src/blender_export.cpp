#include "blender_export.h"
#include <fstream>
#include <sstream>
#include <cmath>

// Map our AnimStyle enum to lyric-video-blender's style name strings
static const char* style_to_lvb(AnimStyle s) {
    switch (s) {
        case AnimStyle::Fade:       return "fade";
        case AnimStyle::Glitch:     return "glitch";
        case AnimStyle::Typewriter: return "typewriter";
        case AnimStyle::Bounce:     return "bounce";
        case AnimStyle::Scale:      return "scale";
        case AnimStyle::Slide:      return "slide_up";
        case AnimStyle::Stack:      return "drift";
        case AnimStyle::Block:      return "pop";
        default:                    return "fade";
    }
}

// Escape a string for inclusion in a Python string literal
static std::string py_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "\\'";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

bool blender_export_script(const AppState& state, const std::string& out_path) {
    std::ofstream f(out_path);
    if (!f) return false;

    const char* style_name = style_to_lvb(state.style);

    // Font weight → Blender font path heuristic
    // lyric-video-blender uses a custom font path property; we leave it as
    // empty string so the addon uses whatever font is configured in Blender.
    const std::string font_path;

    f << "# Pop Maker Studio — Blender export\n";
    f << "# Generated automatically. Run this script inside Blender with the\n";
    f << "# lyric-video-blender addon enabled (Blender 3.0+).\n";
    f << "# File: Text Editor → Run Script\n\n";

    f << "import bpy\n\n";

    f << "# ── Scene setup ──────────────────────────────────────────────────────\n";
    f << "props = bpy.context.scene.lvb_props\n\n";

    // Style
    f << "props.default_style = '" << style_name << "'\n";

    // Font weight → font size scale approximation
    float font_scale = (state.font_weight >= 900) ? 1.2f :
                       (state.font_weight >= 700) ? 1.0f : 0.85f;
    f << "props.font_size = " << (int)(120.f * font_scale) << "\n";

    // Video background if present
    if (state.video_loaded && !state.video_path.empty()) {
        f << "\n# ── Background video ─────────────────────────────────────────────\n";
        f << "bpy.ops.sequencer.movie_strip_add(\n";
        f << "    filepath='" << py_escape(state.video_path) << "',\n";
        f << "    frame_start=1, channel=1)\n";
    }

    // TikTok render setup if vertical format
    if (state.format == OutputFormat::Vertical) {
        f << "\n# ── TikTok render preset ─────────────────────────────────────────\n";
        f << "bpy.ops.lvb.setup_tiktok_render()\n";
    }

    // ── Lyric lines ───────────────────────────────────────────────────────────
    f << "\n# ── Clear existing LVB lines and add ours ────────────────────────────\n";
    f << "# Remove any existing LVB lines\n";
    f << "lvb_lines = props.lyric_lines\n";
    f << "lvb_lines.clear()\n\n";

    for (size_t li = 0; li < state.lines.size(); ++li) {
        const auto& line = state.lines[li];
        if (line.words.empty()) continue;

        std::string text = line.full_text();
        float start      = line.start_time();
        float end        = line.end_time() + 0.3f;  // small hold after last word

        f << "# Line " << (li + 1) << "\n";
        f << "item = lvb_lines.add()\n";
        f << "item.text = '" << py_escape(text) << "'\n";
        f << "item.start_time = " << start << "\n";
        f << "item.end_time   = " << end   << "\n";
        f << "item.style = '" << style_name << "'\n\n";
    }

    // ── Generate animation ────────────────────────────────────────────────────
    f << "# ── Generate animated text objects ──────────────────────────────────\n";
    f << "bpy.ops.lvb.generate_animation()\n\n";

    // ── Render output path ────────────────────────────────────────────────────
    if (!state.out_mp4.empty()) {
        f << "# ── Output path ─────────────────────────────────────────────────\n";
        f << "bpy.context.scene.render.filepath = '";
        f << py_escape(state.out_mp4) << "'\n";
        f << "bpy.context.scene.render.image_settings.file_format = 'FFMPEG'\n";
        f << "bpy.context.scene.render.ffmpeg.format = 'MPEG4'\n";
        f << "bpy.context.scene.render.ffmpeg.codec = 'H264'\n\n";
    }

    f << "print('[PMS] Blender scene ready — press Render Animation (Ctrl+F12)')\n";
    return true;
}
