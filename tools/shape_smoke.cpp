// shape_smoke — verify the shape clip engine surface end-to-end through the
// C ABI: add_shape (preset) → get_shape_path → set_shape_path (freehand) →
// set_shape_style → set_shape_keyframes (morph) → set_clip_keyframes
// (draw-on) → save → load into a second engine → verify path + keys + style
// round-trip. Exit 0 = the shape engine core works standalone.
#include "../src/pms_engine.h"
#include "../src/shape.h"
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

using json = nlohmann::json;

static void fail(const std::string& msg) {
    fprintf(stderr, "shape smoke: FAIL — %s\n", msg.c_str());
    exit(1);
}
static void expect(bool cond, const std::string& msg) {
    if (!cond) fail(msg);
}

static json cmd(pms_engine* e, const std::string& method, const json& params) {
    json req = {{"id", "s"}, {"method", method}, {"params", params}};
    char* r = pms_command(e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    json reply = json::parse(out, nullptr, false);
    if (reply.is_discarded()) fail(method + ": unparsable reply: " + out.substr(0, 300));
    if (reply.contains("error")) fail(method + ": " + reply["error"].get<std::string>());
    return reply.value("result", json::object());
}

// Raw variant: returns the full reply (error included) for negative tests.
static json cmd_raw(pms_engine* e, const std::string& method, const json& params) {
    json req = {{"id", "s"}, {"method", method}, {"params", params}};
    char* r = pms_command(e, req.dump().c_str());
    std::string out = r ? r : "";
    pms_free(r);
    return json::parse(out, nullptr, false);
}

int main() {
    const char* root = "/tmp/pms-shape-smoke";
    std::string rm = "rm -rf " + std::string(root);
    system(rm.c_str());

    pms_engine* e = pms_create(nullptr, root, root);
    expect(e != nullptr, "pms_create");

    // New project + vertical format.
    cmd(e, "new_project", {{"force", true}});
    cmd(e, "set_format", {{"format", "vertical"}});
    cmd(e, "add_track", {{"name", "shapes"}});

    // ── 1. add_shape (star preset) ───────────────────────────────────────────
    json r1 = cmd(e, "add_shape", {{"track", 0}, {"start", 0.0}, {"end", 4.0},
                                    {"preset", "star"}, {"params", {5, 0.22}}});
    expect(r1["clip"] == 0, "add_shape returned clip 0");
    expect(r1["preset"] == "star", "add_shape echoed preset");

    // ── 2. get_shape_path — star should be closed with 10 points (5-star) ────
    json r2 = cmd(e, "get_shape_path", {{"track", 0}, {"clip", 0}, {"t", 0.0}});
    expect(r2["closed"] == true, "star path is closed");
    expect((int)r2["points"].size() == 10, "star has 10 points (5 outer + 5 inner)");

    // ── 3. set_shape_path (freehand triangle) ────────────────────────────────
    json pts = json::array({{{"x", 0.5}, {"y", 0.1}}, {{"x", 0.9}, {"y", 0.9}}, {{"x", 0.1}, {"y", 0.9}}});
    cmd(e, "set_shape_path", {{"track", 0}, {"clip", 0}, {"points", pts}, {"closed", true}});
    json r3 = cmd(e, "get_shape_path", {{"track", 0}, {"clip", 0}, {"t", 0.0}});
    expect((int)r3["points"].size() == 3, "freehand triangle has 3 points");
    expect(r3["closed"] == true, "freehand triangle is closed");

    // ── 4. set_shape_style ───────────────────────────────────────────────────
    cmd(e, "set_shape_style", {{"track", 0}, {"clip", 0},
                                {"fill_on", true}, {"fill_col", {1.0, 0.2, 0.3, 1.0}},
                                {"stroke_on", true}, {"stroke_width", 0.012},
                                {"grad_mode", 1}, {"grad_angle", 90.0},
                                {"glow_on", true}, {"glow_radius", 0.03}});

    // ── 5. set_shape_keyframes (morph triangle → square) ─────────────────────
    json sq = json::array({{{"x", 0.1}, {"y", 0.1}}, {{"x", 0.9}, {"y", 0.1}},
                           {{"x", 0.9}, {"y", 0.9}}, {{"x", 0.1}, {"y", 0.9}}});
    json keys = json::array({{{"t", 0.0}, {"points", pts}, {"closed", true}},
                              {{"t", 2.0}, {"points", sq}, {"closed", true}}});
    json r5 = cmd(e, "set_shape_keyframes", {{"track", 0}, {"clip", 0}, {"keys", keys}});
    expect(r5["key_count"] == 2, "2 morph keyframes set");

    // Verify morph at t=1.0 produces 4 points (resampled to larger count).
    json r5b = cmd(e, "get_shape_path", {{"track", 0}, {"clip", 0}, {"t", 1.0}});
    expect((int)r5b["points"].size() == 4, "morph at t=1.0 resamples to 4 points");

    // ── 6. set_clip_keyframes (draw-on reveal) ───────────────────────────────
    json kf = json::array();
    kf.push_back({{"t", 0.0}, {"v", 0.0}});
    kf.push_back({{"t", 1.0}, {"v", 1.0}});
    json r6 = cmd(e, "set_clip_keyframes", {{"track", 0}, {"clip", 0},
                                             {"prop", "shape_stroke_length"}, {"keys", kf}});
    expect(r6["key_count"] == 2, "2 draw-on keyframes set");

    // ── 7. set_shape_color_keyframes + get_shape_style (lerped eval) ────────
    json ck = json::array();
    ck.push_back({{"t", 0.0}, {"v", {1.0, 0.0, 0.0, 1.0}}, {"interp", "linear"}});
    ck.push_back({{"t", 2.0}, {"v", {0.0, 0.0, 1.0, 1.0}}, {"interp", "linear"}});
    json r7c = cmd(e, "set_shape_color_keyframes", {{"track", 0}, {"clip", 0},
                                                    {"prop", "fill_col"}, {"keys", ck}});
    expect(r7c["key_count"] == 2, "2 colour keyframes set");
    json r7s = cmd(e, "get_shape_style", {{"track", 0}, {"clip", 0}, {"t", 1.0}});
    float fr = r7s["fill_col"][0].get<float>();
    float fb = r7s["fill_col"][2].get<float>();
    expect(fabsf(fr - 0.5f) < 0.05f && fabsf(fb - 0.5f) < 0.05f,
           "fill_col lerps red->blue at t=1 (got r=" + std::to_string(fr) +
           " b=" + std::to_string(fb) + ")");

    // ── 8. Kaleidoscope: static via set_clip_prop, keys via set_clip_keyframes
    cmd(e, "set_clip_prop", {{"track", 0}, {"clip", 0},
                              {"prop", "shape_mirror_fold"}, {"value", 6.0}});
    json r8 = cmd(e, "get_shape_style", {{"track", 0}, {"clip", 0}, {"t", 0.0}});
    expect(r8["mirror_fold"] == 6, "static mirror fold = 6");
    expect(r8["mirror_reflect"] == true, "mirror reflect defaults on");
    json fk = json::array();
    fk.push_back({{"t", 0.0}, {"v", 2.0}});
    fk.push_back({{"t", 2.0}, {"v", 8.0}, {"interp", "hold"}});
    cmd(e, "set_clip_keyframes", {{"track", 0}, {"clip", 0},
                                   {"prop", "shape_mirror_fold"}, {"keys", fk}});
    json r8b = cmd(e, "get_shape_style", {{"track", 0}, {"clip", 0}, {"t", 2.0}});
    expect(r8b["mirror_fold"] == 8, "keyframed mirror fold evaluates to 8 at t=2");

    // ── 8b. Shapes occupy rows: overlapping add_shape on the same track is
    // rejected; a disjoint slot and a different track both succeed. ──────────
    json r8o = cmd_raw(e, "add_shape", {{"track", 0}, {"start", 1.0}, {"end", 3.0},
                                        {"preset", "circle"}});
    expect(r8o.contains("error"), "overlapping add_shape rejected on same track");
    json r8p = cmd(e, "add_shape", {{"track", 0}, {"start", 4.0}, {"end", 6.0},
                                    {"preset", "circle"}});
    expect(r8p["clip"] == 1, "disjoint add_shape succeeds on same track");
    cmd(e, "add_track", {{"name", "shapes 2"}, {"position", 1}});
    json r8q = cmd(e, "add_shape", {{"track", 1}, {"start", 1.0}, {"end", 3.0},
                                    {"preset", "circle"}});
    expect(r8q["clip"] == 0, "same window succeeds on another track");
    // move_clip into an occupied slot is also rejected (clip 1 → clip 0's window).
    json r8m = cmd_raw(e, "move_clip", {{"track", 0}, {"clip", 1}, {"start", 1.0}});
    expect(r8m.contains("error"), "move_clip into occupied slot rejected");

    // ── 9. Save → load into second engine → verify round-trip ────────────────
    std::string path = std::string(root) + "/shape_test.pms";
    cmd(e, "save_project", {{"path", path}});

    pms_engine* e2 = pms_create(nullptr, "/tmp/pms-shape-smoke2", "/tmp/pms-shape-smoke2");
    json r7 = cmd(e2, "load_project", {{"path", path}});
    json r7p = cmd(e2, "get_shape_path", {{"track", 0}, {"clip", 0}, {"t", 0.0}});
    expect((int)r7p["points"].size() == 3, "loaded project has 3-point base path");
    expect(r7p["closed"] == true, "loaded path is closed");
    expect(r7p["key_count"] == 2, "loaded project has 2 morph keyframes");

    // Morph at t=1.0 in the loaded project should also produce 4 points.
    json r7b = cmd(e2, "get_shape_path", {{"track", 0}, {"clip", 0}, {"t", 1.0}});
    expect((int)r7b["points"].size() == 4, "loaded morph at t=1.0 resamples to 4 points");

    // Colour keys + kaleidoscope survive the round-trip.
    json r9 = cmd(e2, "get_shape_style", {{"track", 0}, {"clip", 0}, {"t", 1.0}});
    expect((int)r9["color_key_counts"]["fill_col"] == 2,
           "loaded project keeps 2 fill_col colour keys");
    float lfr = r9["fill_col"][0].get<float>();
    expect(fabsf(lfr - 0.5f) < 0.05f, "loaded fill_col lerps at t=1");
    expect((int)r9["mirror_fold"] >= 2, "loaded project keeps keyframed mirror fold");

    pms_destroy(e2);
    pms_destroy(e);

    // ── 10. Direct: ColorPropTrack eval semantics ────────────────────────────
    {
        ColorPropTrack ct;
        float red[4]  = {1.f, 0.f, 0.f, 1.f};
        float blue[4] = {0.f, 0.f, 1.f, 1.f};
        float out[4];
        ct.set(0.f, red,  InterpType::Linear);
        ct.set(2.f, blue, InterpType::Linear);
        ct.eval(1.f, red, out);
        expect(fabsf(out[0] - 0.5f) < 1e-3f && fabsf(out[2] - 0.5f) < 1e-3f,
               "ColorPropTrack lerp midpoint");
        ct.eval(-1.f, red, out);
        expect(fabsf(out[0] - 1.f) < 1e-3f, "ColorPropTrack clamps before first key");
        ColorPropTrack empty;
        empty.eval(0.5f, blue, out);
        expect(fabsf(out[2] - 1.f) < 1e-3f, "empty ColorPropTrack returns base");
    }

    // ── 11. Direct: radial replication geometry ──────────────────────────────
    {
        ShapePath tri;
        tri.closed = true;
        tri.pts = {{0.5f, 0.1f, 0.008f}, {0.9f, 0.9f, 0.008f}, {0.1f, 0.9f, 0.008f}};
        ShapeGeometry g = shape_tessellate(tri, 1.f, 1.f, 0.f, 1000, 1000,
                                           500.f, 500.f, 200.f, 200.f, 1.f, 0.f);
        size_t n = g.fill.size();
        expect(n == 3, "triangle tessellates to 3 fill verts");
        ShapeGeometry rot = shape_radial_replicate(g, 500.f, 500.f, 3, false);
        expect(rot.fill.size() == n * 3, "fold=3 replicates fill verts x3");
        expect(fabsf(rot.fill[0].x - g.fill[0].x) < 1e-3f &&
               fabsf(rot.fill[0].y - g.fill[0].y) < 1e-3f, "replica 0 is identity");
        const float c = cosf(2.f * 3.14159265f / 3.f), s = sinf(2.f * 3.14159265f / 3.f);
        float dx = g.fill[0].x - 500.f, dy = g.fill[0].y - 500.f;
        // rotate-only replica 1: v' = rot120(v - c) + c
        expect(fabsf(rot.fill[n].x - (500.f + dx * c - dy * s)) < 1e-2f &&
               fabsf(rot.fill[n].y - (500.f + dx * s + dy * c)) < 1e-2f,
               "rotate-only replica 1 matches rot120");
        // mirrored replica 1: dy flips before rotation (winding swap leaves
        // the first vert of each triangle in place)
        ShapeGeometry mir = shape_radial_replicate(g, 500.f, 500.f, 3, true);
        expect(fabsf(mir.fill[n].x - (500.f + dx * c + dy * s)) < 1e-2f &&
               fabsf(mir.fill[n].y - (500.f + dx * s - dy * c)) < 1e-2f,
               "mirrored replica 1 matches reflect+rot120");
        ShapeGeometry same = shape_radial_replicate(g, 500.f, 500.f, 1, true);
        expect(same.fill.size() == n, "fold=1 is a no-op");
    }

    printf("shape smoke: PASS — preset, freehand, style, morph, draw-on, colour keys, kaleidoscope, save/load round-trip\n");
    return 0;
}
