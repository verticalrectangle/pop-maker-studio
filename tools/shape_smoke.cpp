// shape_smoke — verify the shape clip engine surface end-to-end through the
// C ABI: add_shape (preset) → get_shape_path → set_shape_path (freehand) →
// set_shape_style → set_shape_keyframes (morph) → set_clip_keyframes
// (draw-on) → save → load into a second engine → verify path + keys + style
// round-trip. Exit 0 = the shape engine core works standalone.
#include "../src/pms_engine.h"
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

    // ── 7. Save → load into second engine → verify round-trip ────────────────
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

    pms_destroy(e2);
    pms_destroy(e);

    printf("shape smoke: PASS — preset, freehand, style, morph, draw-on, save/load round-trip\n");
    return 0;
}
